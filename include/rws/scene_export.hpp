#pragma once

#include "rws/document.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace rws {

struct SceneExportStats {
    std::uint64_t clumps{};
    std::uint64_t atomic_instances{};
    std::uint64_t custom_instances{};
    std::uint64_t unresolved_instances{};
    std::uint64_t world_sectors{};
    std::uint64_t recovered_world_sectors{};
    std::uint64_t recovered_world_vertices{};
    std::uint64_t recovered_world_triangles{};
    std::uint64_t materials{};
    std::uint64_t vertices{};
    std::uint64_t triangles{};
    std::uint64_t skipped{};
};

// Exports the assembled, Y-up scene to glTF 2.0. Clump frame transforms are
// baked into vertex positions, World Sectors remain separate nodes, and UV0/
// UV1 are exported as TEXCOORD_0/TEXCOORD_1. A sibling manifest records the
// original RWS offsets and base/lightmap texture names for round-trip tooling.
[[nodiscard]] SceneExportStats export_scene_gltf(
    const std::vector<Chunk>& chunks, std::span<const SceneInstance> instances,
    std::span<const std::byte> bytes,
    const std::filesystem::path& output_path);

// Exports every Atomic belonging to one Clump, with the same layout and
// manifest as the whole-scene export.
[[nodiscard]] SceneExportStats export_clump_gltf(
    const Chunk& clump, std::span<const std::byte> bytes,
    const std::filesystem::path& output_path);

} // namespace rws
