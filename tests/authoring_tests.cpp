#include "csf/authoring.hpp"
#include "csf/mod_project.hpp"

#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::cerr << "CHECK failed: " #value " at " << __FILE__ << ':' << __LINE__ << '\n'; std::abort(); } } while (false)

namespace {

void u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value)); out.push_back(static_cast<std::byte>(value >> 8U));
}
void u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>(value >> shift));
}
void entry(std::vector<std::byte>& out, std::uint32_t next, std::uint32_t value,
           std::int16_t identifier, std::uint16_t type) {
    u32(out,next); u32(out,value); u16(out,static_cast<std::uint16_t>(identifier)); u16(out,type);
}
void string(std::vector<std::byte>& out, std::string_view value) {
    u32(out,static_cast<std::uint32_t>(value.size()+1));
    for (char c : value) out.push_back(static_cast<std::byte>(c));
    out.push_back(std::byte{0});
}
std::vector<std::byte> sample() {
    std::vector<std::byte> out;
    for (char c : std::string("CSFFBS")) out.push_back(static_cast<std::byte>(c));
    out.push_back(std::byte{0x12}); out.push_back(std::byte{0x34});
    u32(out,1); u32(out,8); u32(out,3); u32(out,1);
    entry(out,1,0,0,0); entry(out,2,42,-1,3);
    entry(out,3,0,1,0); entry(out,4,std::bit_cast<std::uint32_t>(1.5F),-1,4);
    entry(out,5,0,2,0); entry(out,6,0,-1,5);
    entry(out,7,0,2,0); entry(out,8,0,-1,5);
    string(out,".FLAGS"); string(out,".ANGULO"); string(out,"FILE"); string(out,"old.dds");
    out.push_back(std::byte{0xde}); out.push_back(std::byte{0xad});
    return out;
}
void write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    CHECK(out.good());
}
std::vector<std::byte> read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate); CHECK(in.good());
    std::vector<std::byte> out(static_cast<std::size_t>(in.tellg())); in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()),static_cast<std::streamsize>(out.size())); CHECK(in.good());
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const auto bytes = sample();
    CHECK(csf::sha256(std::as_bytes(std::span("abc",3))) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        auto edit = csf::EditSession::from_document(csf::Document::from_bytes(bytes));
        CHECK(csf::authorable_fields(edit.document()).size() == 4);
        CHECK(edit.serialize() == bytes);
        CHECK(!edit.dirty());
        CHECK(edit.set_integer(1,-7).valid);
        auto changed = edit.serialize();
        std::size_t differences{};
        for (std::size_t i=0;i<bytes.size();++i) differences += bytes[i] != changed[i];
        CHECK(differences <= 4);
        CHECK(edit.set_real(3,2.25F).valid);
        CHECK(!edit.set_real(3,std::numeric_limits<float>::infinity()).valid);
        CHECK(edit.undo()); CHECK(edit.undo());
        CHECK(edit.serialize() == bytes);
        CHECK(edit.redo()); CHECK(edit.redo());
    }
    {
        auto edit = csf::EditSession::from_document(csf::Document::from_bytes(bytes));
        CHECK(edit.set_string(5,"new.dds").valid);
        CHECK(edit.document().header().string_count == 2);
        CHECK(edit.document().entries()[5].raw_value_or_size == 1);
        CHECK(edit.document().entries()[7].raw_value_or_size == 0);
        CHECK(edit.document().trailing_bytes().size() == 2);
        CHECK(edit.undo()); CHECK(edit.serialize() == bytes);
        CHECK(edit.redo());
        CHECK(edit.document().string(1)->display_utf8() == "new.dds");
    }
    {
        auto edit = csf::EditSession::from_document(csf::Document::from_bytes(bytes));
        CHECK(edit.set_string(5,"global.dds",true).valid);
        CHECK(edit.document().header().string_count == 1);
        CHECK(edit.document().string(0)->display_utf8() == "global.dds");
        CHECK(edit.undo()); CHECK(edit.serialize() == bytes);
    }

    const auto root = std::filesystem::temp_directory_path() /
        ("csf-authoring-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto source_root = root / "source";
    const auto workspace = root / "project";
    const auto source = source_root / "Maps" / "Test.scn";
    const auto authored = root / "authored" / "Test.scn";
    write(source,bytes);
    {
        auto edit = csf::EditSession::load(source);
        CHECK(edit.set_integer(1,99,"test count").valid);
        const auto saved = edit.save_copy(authored);
        CHECK(saved.validation.passed);
        CHECK(std::filesystem::is_regular_file(saved.manifest_path));
        CHECK(read(authored) == edit.serialize());
        bool refused{};
        try { (void)edit.save_copy(source); } catch (const std::exception&) { refused=true; }
        CHECK(refused);

        auto project = csf::ModProject::create(workspace,source_root,"Synthetic","test");
        project.add_file({"Maps/Test.scn",authored,saved.manifest_path,saved.source_sha256,
                          saved.output_sha256,{"entry:1"}});
        project.save();
        CHECK(project.manifest_json().find(source_root.string()) == std::string::npos);
        const auto loaded = csf::ModProject::load(workspace);
        CHECK(loaded.validate().passed);
        const auto staging = root / "staging";
        CHECK(loaded.build(staging).passed);
        CHECK(read(staging/"files/Maps/Test.scn") == read(authored));
        const auto first_manifest = read(staging/"mod-project.json");
        CHECK(loaded.build(staging).passed);
        CHECK(read(staging/"mod-project.json") == first_manifest);
        CHECK(!std::filesystem::exists(staging/"files/unchanged.bin"));
        write(source_root/"fallback.bin",std::as_bytes(std::span("x",1)));
        CHECK(loaded.resolve_overlay(staging,"fallback.bin") == source_root/"fallback.bin");

        CHECK(argc == 2);
        csf::PakOptions pak_options;
        pak_options.pakman_cli = argv[1];
        const auto packaged = loaded.package(staging,root/"Synthetic Mod.pak",pak_options);
        CHECK(std::filesystem::is_regular_file(packaged.archive_path));
        CHECK(std::filesystem::is_regular_file(packaged.manifest_path));
        CHECK(!packaged.archive_sha256.empty());
        const auto archive = read(packaged.archive_path);
        const std::string archive_text(reinterpret_cast<const char*>(archive.data()),archive.size());
        CHECK(archive_text.find("Maps/Test.scn") != std::string::npos);
        CHECK(archive_text.find("mod-project.json") == std::string::npos);
        bool package_refused{};
        try { (void)loaded.package(staging,packaged.archive_path,pak_options); }
        catch (const std::exception&) { package_refused=true; }
        CHECK(package_refused);
        pak_options.overwrite = true;
        CHECK(!loaded.package(staging,packaged.archive_path,pak_options).archive_sha256.empty());
        {
            // A bare archive name has no parent directory to create.
            const auto previous_directory = std::filesystem::current_path();
            std::filesystem::current_path(root);
            bool bare_packaged{};
            try { bare_packaged = !loaded.package(staging,"Bare.pak",pak_options).archive_sha256.empty(); }
            catch (const std::exception&) {}
            std::filesystem::current_path(previous_directory);
            CHECK(bare_packaged);
            CHECK(std::filesystem::is_regular_file(root/"Bare.pak"));
        }

        auto other = csf::ModProject::create(root/"other",source_root,"Other","test");
        other.add_file({"Maps/Test.scn",authored,saved.manifest_path,saved.source_sha256,
                        saved.output_sha256,{"entry:1"}});
        CHECK(csf::ModProject::conflicts(loaded,other).size() == 1);

        const auto install = root / "test-install";
        std::filesystem::create_directories(install/"Maps");
        write(install/"Maps/Test.scn",bytes);
        const auto pak_dry = loaded.deploy_package(packaged.archive_path,install,
                                                    "Mods/Synthetic Mod.pak",pak_options,true);
        CHECK(pak_dry.dry_run && !std::filesystem::exists(install/"Mods/Synthetic Mod.pak"));
        const auto pak_deployed = loaded.deploy_package(packaged.archive_path,install,
                                                         "Mods/Synthetic Mod.pak",pak_options,false);
        CHECK(read(install/"Mods/Synthetic Mod.pak") == read(packaged.archive_path));
        csf::ModProject::rollback(pak_deployed.manifest_path);
        CHECK(!std::filesystem::exists(install/"Mods/Synthetic Mod.pak"));
        auto tampered_archive = read(packaged.archive_path);
        tampered_archive.back() ^= std::byte{1};
        write(packaged.archive_path,tampered_archive);
        bool tampered_refused{};
        try { (void)loaded.deploy_package(packaged.archive_path,install,
                                          "Mods/Synthetic Mod.pak",pak_options,true); }
        catch (const std::exception&) { tampered_refused=true; }
        CHECK(tampered_refused);
        const auto dry = loaded.deploy(staging,install,true);
        CHECK(dry.dry_run && read(install/"Maps/Test.scn") == bytes);
        const auto deployed = loaded.deploy(staging,install,false);
        CHECK(read(install/"Maps/Test.scn") == read(authored));
        csf::ModProject::rollback(deployed.manifest_path);
        CHECK(read(install/"Maps/Test.scn") == bytes);
    }
    auto stale = csf::ModProject::load(workspace);
    auto modified = bytes; modified.back() = std::byte{0xbe}; write(source,modified);
    CHECK(!stale.validate().passed);
    std::filesystem::remove_all(root);
    std::cout << "Authoring and mod staging tests passed\n";
}
