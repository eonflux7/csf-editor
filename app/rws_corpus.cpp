#include "rws/document.hpp"
#include "rws/decoded.hpp"
#include "rws/world_recovery.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct Stats { std::uint64_t count{}, bytes{}, truncated{}; };

void collect(const std::vector<rws::Chunk>& chunks, std::map<std::uint32_t, Stats>& stats) {
    for (const auto& chunk : chunks) {
        auto& value = stats[chunk.type];
        ++value.count;
        value.bytes += chunk.available_size;
        value.truncated += chunk.truncated ? 1U : 0U;
        collect(chunk.children, stats);
    }
}

bool is_rws(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".rws";
}

struct RootKey {
    std::uint32_t type{}, stamp{};
    auto operator<=>(const RootKey&) const = default;
};

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: rws-corpus <directory>\n";
        return 2;
    }
    const std::filesystem::path root(argv[1]);
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
        std::cerr << "rws-corpus: directory not found: " << root.string() << '\n';
        return 1;
    }

    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
        if (error) { std::cerr << "warning: " << error.message() << '\n'; error.clear(); continue; }
        if (it->is_regular_file(error) && is_rws(it->path())) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());

    std::map<RootKey, Stats> roots;
    std::map<std::uint32_t, Stats> chunks;
    std::uint64_t total_bytes{}, warning_files{}, failed_files{}, instance_files{}, total_instances{};
    std::uint64_t complete_worlds{}, partial_worlds{}, failed_worlds{};
    std::map<std::uint32_t, std::uint64_t> instance_prototypes;
    std::cout << "file\tbytes\troot\tversion/build\tinstances\tdiagnostics\tworld-sectors\tworld-triangles\tworld-vertices\tworld-recovery\n";
    for (const auto& path : files) {
        try {
            const auto document = rws::Document::load(path);
            total_bytes += document.bytes().size();
            warning_files += document.diagnostics().empty() ? 0U : 1U;
            instance_files += document.scene_instances().empty() ? 0U : 1U;
            total_instances += document.scene_instances().size();
            for (const auto& instance : document.scene_instances())
                ++instance_prototypes[instance.prototype_id];
            collect(document.chunks(), chunks);
            const auto worlds = rws::recover_worlds(document.chunks(), document.bytes());
            std::cout << std::filesystem::relative(path, root, error).string() << '\t'
                      << document.bytes().size() << '\t';
            if (!document.chunks().empty()) {
                const auto& first = document.chunks().front();
                auto& value = roots[{first.type, first.library_id}];
                ++value.count; value.bytes += document.bytes().size(); value.truncated += first.truncated ? 1U : 0U;
                const auto version = rws::decode_library_id(first.library_id);
                std::cout << "0x" << std::hex << first.type << std::dec << " " << rws::chunk_name(first.type)
                          << '\t' << version.major << '.' << version.minor << '.' << version.revision << '.'
                          << version.binary << '/' << version.build;
            } else {
                std::cout << "none\tunknown";
            }
            std::uint64_t recovered_sectors{}, declared_sectors{}, recovered_triangles{},
                          declared_triangles{}, recovered_vertices{}, declared_vertices{};
            auto file_status = rws::WorldRecoveryStatus::complete;
            for (const auto& world : worlds) {
                recovered_sectors += world.sectors.size();
                declared_sectors += std::max(0, world.header.world_sector_count);
                recovered_triangles += world.recovered_triangles;
                declared_triangles += std::max(0, world.header.triangle_count);
                recovered_vertices += world.recovered_vertices;
                declared_vertices += std::max(0, world.header.vertex_count);
                if (world.status == rws::WorldRecoveryStatus::failed)
                    file_status = rws::WorldRecoveryStatus::failed;
                else if (world.status == rws::WorldRecoveryStatus::partial &&
                         file_status != rws::WorldRecoveryStatus::failed)
                    file_status = rws::WorldRecoveryStatus::partial;
                if (world.status == rws::WorldRecoveryStatus::complete) ++complete_worlds;
                else if (world.status == rws::WorldRecoveryStatus::partial) ++partial_worlds;
                else ++failed_worlds;
            }
            std::cout << '\t' << document.scene_instances().size()
                      << '\t' << document.diagnostics().size() << '\t';
            if (worlds.empty()) {
                std::cout << "-\t-\t-\tnone\n";
            } else {
                std::cout << recovered_sectors << '/' << declared_sectors << '\t'
                          << recovered_triangles << '/' << declared_triangles << '\t'
                          << recovered_vertices << '/' << declared_vertices << '\t'
                          << rws::world_recovery_status_name(file_status) << '\n';
            }
        } catch (const std::exception& exception) {
            ++failed_files;
            std::cerr << path.string() << ": " << exception.what() << '\n';
        }
    }

    std::cout << "\nCorpus: " << files.size() << " files, " << total_bytes << " bytes, "
              << warning_files << " files with diagnostics, " << failed_files << " failed loads\n";
    std::cout << "CSF scene instances: " << total_instances << " records in " << instance_files
              << " files\n";
    std::cout << "World recovery: " << complete_worlds << " complete, " << partial_worlds
              << " partial, " << failed_worlds << " failed\n";
    if (!instance_prototypes.empty()) {
        std::cout << "Instance prototypes:\n";
        for (const auto& [prototype, count] : instance_prototypes)
            std::cout << "  " << prototype << "\t" << count << '\n';
    }
    std::cout << "\nRoot formats:\n";
    for (const auto& [key, value] : roots) {
        const auto version = rws::decode_library_id(key.stamp);
        std::cout << "  0x" << std::hex << std::setw(8) << std::setfill('0') << key.type << std::dec
                  << std::setfill(' ') << "  " << std::left << std::setw(28) << rws::chunk_name(key.type)
                  << std::right << " files=" << value.count << " bytes=" << value.bytes
                  << " RW=" << version.major << '.' << version.minor << '.' << version.revision << '.'
                  << version.binary << " build=" << version.build << '\n';
    }
    std::cout << "\nChunk inventory:\n";
    for (const auto& [type, value] : chunks) {
        std::cout << "  0x" << std::hex << std::setw(8) << std::setfill('0') << type << std::dec
                  << std::setfill(' ') << "  " << std::left << std::setw(32) << rws::chunk_name(type)
                  << std::right << " count=" << value.count << " payload=" << value.bytes
                  << " truncated=" << value.truncated << '\n';
    }
    return failed_files == 0 ? 0 : 1;
}
