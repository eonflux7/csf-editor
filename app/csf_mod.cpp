#include "csf/authoring.hpp"
#include "csf/mod_project.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void usage() {
    std::cerr
        << "Usage:\n"
           "  csf-mod edit <source> <new-output> [--int <entry> <value> | --real <entry> <value>\n"
           "                                      | --string <entry> <value>\n"
           "                                      | --global-string <entry> <value>]...\n"
           "  csf-mod audit <resource-root>\n"
           "  csf-mod init <workspace> <source-root> <name>\n"
           "  csf-mod add <workspace> <game-relative-path> <authored-file> [change-manifest]\n"
           "              [--target <semantic-target>]...\n"
           "  csf-mod validate <workspace>\n"
           "  csf-mod build <workspace> <staging-directory>\n"
           "  csf-mod package <workspace> <staging-directory> <archive.pak>\n"
           "                  [--pakman <pakman-cli>] [--type stored|compressed]\n"
           "                  [--platform pc|ps2|xbox|ps2-prototype] [--overwrite]\n"
           "  csf-mod conflicts <workspace-a> <workspace-b>\n"
           "  csf-mod deploy-pak <workspace> <archive.pak> <test-install> <relative-pak-path>\n"
           "                     [--pakman <pakman-cli>] [--apply]\n"
           "  csf-mod deploy-loose <workspace> <staging-directory> <test-install> [--apply]\n"
           "  csf-mod rollback <deployment.state>\n";
}

std::uint32_t u32(const std::string_view text) {
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid unsigned integer: " + std::string(text));
    return value;
}

std::int32_t i32(const std::string_view text) {
    std::int32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid integer: " + std::string(text));
    return value;
}

float real(const std::string_view text) {
    float value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid real: " + std::string(text));
    return value;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    const auto size = input.tellg();
    if (size < 0) throw std::runtime_error("Cannot determine file size");
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (!input && !result.empty()) throw std::runtime_error("Cannot read complete file");
    return result;
}

int report(const csf::OverlayReport& value) {
    for (const auto& diagnostic : value.diagnostics)
        std::cout << csf::overlay_diagnostic_kind_name(diagnostic.kind) << '\t'
                  << diagnostic.relative_path.generic_string() << '\t' << diagnostic.message << '\n';
    std::cout << (value.passed ? "validation passed" : "validation failed") << '\n';
    return value.passed ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) try {
    if (argc < 2) { usage(); return 1; }
    const std::string_view command = argv[1];
    if (command == "audit") {
        if (argc != 3) { usage(); return 1; }
        std::size_t exact{}, structurally_invalid{}, differences{};
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(
                 argv[2], std::filesystem::directory_options::skip_permission_denied, error), end;
             it != end; it.increment(error)) {
            if (error) { error.clear(); continue; }
            if (!it->is_regular_file(error)) continue;
            const auto bytes = read_bytes(it->path());
            if (!csf::Document::sniff(bytes)) continue;
            const auto document = csf::Document::from_bytes(bytes);
            if (document.has_errors()) { ++structurally_invalid; continue; }
            const auto serialized = csf::EditSession::from_document(document).serialize();
            if (serialized == bytes) ++exact;
            else { ++differences; std::cout << "difference\t" << it->path().generic_string() << '\n'; }
        }
        std::cout << "exact\t" << exact << "\nstructurally-invalid\t" << structurally_invalid
                  << "\ndifferences\t" << differences << '\n';
        return differences == 0 ? 0 : 2;
    }
    if (command == "edit") {
        if (argc < 5) { usage(); return 1; }
        auto session = csf::EditSession::load(argv[2]);
        const auto supported = csf::authorable_fields(session.document());
        for (int i = 4; i < argc;) {
            const std::string_view operation = argv[i++];
            if (i + 1 >= argc) throw std::runtime_error("Edit operation requires entry and value");
            const auto entry = u32(argv[i++]);
            const auto policy = std::ranges::find_if(supported, [&](const csf::EditableField& field) {
                return field.entry_index == entry;
            });
            if (policy == supported.end())
                throw std::runtime_error("Entry is not in the reviewed authoring field set");
            csf::EditValidation validation;
            if (operation == "--int") {
                const auto value = i32(argv[i++]);
                if (policy->category == csf::AuthoringCategory::script_flag && value != 0 && value != 1)
                    throw std::runtime_error("Script flags accept only 0 or 1");
                validation = session.set_integer(entry, value, policy->meaning);
            }
            else if (operation == "--real") validation = session.set_real(entry, real(argv[i++]), policy->meaning);
            else if (operation == "--string") validation = session.set_string(entry, argv[i++], false, policy->meaning);
            else if (operation == "--global-string") validation = session.set_string(entry, argv[i++], true, policy->meaning);
            else throw std::runtime_error("Unknown edit operation: " + std::string(operation));
            if (!validation.valid || validation.blocking) throw std::runtime_error(validation.message);
        }
        const auto saved = session.save_copy(argv[3]);
        std::cout << "saved\t" << saved.output_path.generic_string() << "\nmanifest\t"
                  << saved.manifest_path.generic_string() << "\nsha256\t" << saved.output_sha256 << '\n';
        return 0;
    }
    if (command == "init") {
        if (argc != 5) { usage(); return 1; }
        const auto project = csf::ModProject::create(argv[2], argv[3], argv[4], "0.1.0");
        std::cout << "created\t" << project.workspace_root.generic_string() << '\n';
        return 0;
    }
    if (command == "add") {
        if (argc < 5) { usage(); return 1; }
        auto project = csf::ModProject::load(argv[2]);
        csf::ModFile file;
        file.relative_path = argv[3];
        file.authored_path = std::filesystem::absolute(argv[4]);
        int i = 5;
        if (i < argc && std::string_view(argv[i]) != "--target")
            file.change_manifest = std::filesystem::absolute(argv[i++]);
        while (i < argc) {
            if (std::string_view(argv[i++]) != "--target" || i >= argc)
                throw std::runtime_error("Expected --target <semantic-target>");
            file.semantic_targets.emplace_back(argv[i++]);
        }
        const auto source = project.source_root / file.relative_path;
        if (std::filesystem::is_regular_file(source)) file.source_sha256 = csf::sha256(read_bytes(source));
        file.output_sha256 = csf::sha256(read_bytes(file.authored_path));
        project.add_file(std::move(file));
        project.save();
        std::cout << "added\t" << argv[3] << '\n';
        return 0;
    }
    if (command == "validate") {
        if (argc != 3) { usage(); return 1; }
        return report(csf::ModProject::load(argv[2]).validate());
    }
    if (command == "build") {
        if (argc != 4) { usage(); return 1; }
        return report(csf::ModProject::load(argv[2]).build(argv[3]));
    }
    if (command == "package") {
        if (argc < 5) { usage(); return 1; }
        csf::PakOptions options;
        for (int i = 5; i < argc;) {
            const std::string_view option = argv[i++];
            if (option == "--overwrite") options.overwrite = true;
            else {
                if (i >= argc) throw std::runtime_error(std::string(option) + " requires a value");
                if (option == "--pakman") options.pakman_cli = argv[i++];
                else if (option == "--type") options.type = argv[i++];
                else if (option == "--platform") options.platform = argv[i++];
                else throw std::runtime_error("Unknown package option: " + std::string(option));
            }
        }
        const auto result = csf::ModProject::load(argv[2]).package(argv[3], argv[4], options);
        std::cout << "archive\t" << result.archive_path.generic_string()
                  << "\nmanifest\t" << result.manifest_path.generic_string()
                  << "\nsha256\t" << result.archive_sha256 << '\n';
        return 0;
    }
    if (command == "conflicts") {
        if (argc != 4) { usage(); return 1; }
        const auto conflicts = csf::ModProject::conflicts(csf::ModProject::load(argv[2]),
                                                          csf::ModProject::load(argv[3]));
        for (const auto& conflict : conflicts)
            std::cout << conflict.relative_path.generic_string() << '\t' << conflict.reason << '\n';
        return conflicts.empty() ? 0 : 3;
    }
    if (command == "deploy-pak") {
        if (argc < 6) { usage(); return 1; }
        bool apply{};
        csf::PakOptions options;
        for (int i = 6; i < argc;) {
            const std::string_view option = argv[i++];
            if (option == "--apply") apply = true;
            else if (option == "--pakman") {
                if (i >= argc) throw std::runtime_error("--pakman requires a value");
                options.pakman_cli = argv[i++];
            } else throw std::runtime_error("Unknown deploy-pak option: " + std::string(option));
        }
        const auto result = csf::ModProject::load(argv[2]).deploy_package(
            argv[3], argv[4], argv[5], options, !apply);
        for (const auto& file : result.files)
            std::cout << (apply ? "deploy-pak" : "would-deploy-pak") << '\t'
                      << file.relative_path.generic_string() << '\n';
        if (apply) std::cout << "rollback-manifest\t" << result.manifest_path.generic_string() << '\n';
        return 0;
    }
    if (command == "deploy-loose") {
        if (argc != 5 && argc != 6) { usage(); return 1; }
        const bool apply = argc == 6 && std::string_view(argv[5]) == "--apply";
        if (argc == 6 && !apply) throw std::runtime_error("Only --apply confirms deployment");
        const auto result = csf::ModProject::load(argv[2]).deploy(argv[3], argv[4], !apply);
        for (const auto& file : result.files)
            std::cout << (apply ? "deploy" : "would-deploy") << '\t'
                      << file.relative_path.generic_string() << '\n';
        if (apply) std::cout << "rollback-manifest\t" << result.manifest_path.generic_string() << '\n';
        return 0;
    }
    if (command == "rollback") {
        if (argc != 3) { usage(); return 1; }
        csf::ModProject::rollback(argv[2]);
        std::cout << "rollback complete\n";
        return 0;
    }
    usage();
    return 1;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
}
