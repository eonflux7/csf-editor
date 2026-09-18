#pragma once

#include "rws/decoded.hpp"
#include "rws/chunk.hpp"

#include <cstddef>
#include <filesystem>
#include <span>

namespace rws {

// Exports one geometry in its local frame. Materials are emitted as symbolic
// `material_N` groups; texture image extraction is a separate concern.
void export_geometry_obj(const GeometryInfo& geometry, std::span<const std::byte> bytes,
                         const std::filesystem::path& output_path);

struct CollisionExportStats {
    std::uint64_t worlds{}, sectors{}, vertices{}, triangles{}, skipped_triangles{}, materials{};
};
[[nodiscard]] CollisionExportStats export_collision_obj(
    const std::vector<Chunk>& chunks, std::span<const std::byte> bytes,
    const std::filesystem::path& output_path);

} // namespace rws
