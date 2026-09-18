#pragma once

#include "rws/decoded.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rws {

enum class WorldRecoveryStatus { complete, partial, failed };

struct RecoveredWorldSector {
    std::uint64_t chunk_offset{};
    std::uint64_t struct_offset{};
    std::uint64_t range_end{};
    std::int32_t material_window_base{};
    std::int32_t triangle_count{};
    std::int32_t vertex_count{};
    Vec3 bounding_box_inf;
    Vec3 bounding_box_sup;
    bool collision_sector_present{};
    std::uint32_t texcoord_sets{};
    std::uint64_t vertices_offset{};
    std::uint64_t normals_offset{};
    std::uint64_t prelight_offset{};
    std::vector<std::uint64_t> texcoord_offsets;
    std::uint64_t triangles_offset{};
};

struct RecoveredWorld {
    std::uint64_t world_offset{};
    std::uint32_t library_id{};
    WorldInfo header;
    std::int32_t material_count{};
    std::vector<RecoveredWorldSector> sectors;
    std::int64_t recovered_vertices{};
    std::int64_t recovered_triangles{};
    std::uint64_t invalid_candidates{};
    std::uint64_t invalid_triangles{};
    std::uint64_t invalid_material_references{};
    std::uint64_t duplicate_or_overlapping_ranges{};
    std::uint64_t truncated_candidates{};
    std::vector<std::string> diagnostics;
    WorldRecoveryStatus status{WorldRecoveryStatus::failed};
};

[[nodiscard]] RecoveredWorld recover_world(const Chunk& world,
                                           std::span<const std::byte> bytes);
[[nodiscard]] std::vector<RecoveredWorld> recover_worlds(
    const std::vector<Chunk>& chunks, std::span<const std::byte> bytes);
[[nodiscard]] DecodeResult<TriangleInfo> decode_recovered_world_triangle(
    const RecoveredWorldSector& sector, std::int32_t index,
    std::span<const std::byte> bytes);
[[nodiscard]] const char* world_recovery_status_name(WorldRecoveryStatus status) noexcept;

} // namespace rws
