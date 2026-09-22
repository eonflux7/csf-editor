#pragma once

#include "rws/decoded.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rws {

enum class WorldRecoveryStatus { complete, partial, failed };
enum class WorldTopologyStatus { complete, partial, failed };

struct RecoveredWorldPlane {
    std::uint64_t chunk_offset{};
    std::uint64_t struct_offset{};
    std::int32_t axis{};
    float split{};
    bool left_is_world_sector{}, right_is_world_sector{};
    float left_value{}, right_value{};
    std::optional<std::size_t> left_node, right_node;
};

struct RecoveredWorldNode {
    enum class Kind : std::uint8_t { plane, sector } kind{};
    std::size_t value_index{};
    std::optional<std::size_t> parent;
    std::optional<bool> is_left_child;
    Vec3 bounding_box_inf, bounding_box_sup;
    std::size_t depth{};
};

struct WorldTopologyStats {
    std::uint64_t invalid_candidates{}, ambiguous_candidates{}, duplicate_candidates{};
    std::uint64_t overlapping_candidates{}, truncated_candidates{}, unreachable_nodes{};
    std::uint64_t cycles{}, multiple_parents{}, bounds_conflicts{};
    std::uint64_t linked_sectors{}, unlinked_sectors{};
    std::size_t maximum_depth{};
};

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
    std::vector<RecoveredWorldPlane> planes;
    std::vector<RecoveredWorldNode> topology_nodes;
    std::optional<std::size_t> topology_root;
    WorldTopologyStats topology_stats;
    WorldTopologyStatus topology_status{WorldTopologyStatus::failed};
    std::vector<std::string> topology_diagnostics;
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

struct RecoveredWorldTriangle {
    std::size_t sector_index{};
    std::int32_t triangle_index{};
    std::uint64_t source_offset{};
    std::int32_t material_slot{};
    std::array<std::uint16_t, 3> vertex_indices{};
    std::array<Vec3, 3> vertices{};
};

struct CollisionRay {
    Vec3 origin, direction;
};
struct CollisionClipPlane {
    bool enabled{};
    std::uint8_t axis{};
    bool keep_greater{true};
    float position{};
};
struct CollisionHit {
    std::size_t world_index{}, sector_index{};
    std::int32_t triangle_index{}, material_slot{};
    std::uint64_t sector_offset{}, triangle_offset{};
    float distance{};
    Vec3 position, geometric_normal;
    std::array<float, 3> barycentric{};
};
struct Measurement {
    float distance{};
    Vec3 absolute_delta;
};

[[nodiscard]] RecoveredWorld recover_world(const Chunk& world, std::span<const std::byte> bytes);
[[nodiscard]] std::vector<RecoveredWorld> recover_worlds(const std::vector<Chunk>& chunks,
                                                         std::span<const std::byte> bytes);
[[nodiscard]] DecodeResult<TriangleInfo>
decode_recovered_world_triangle(const RecoveredWorldSector& sector, std::int32_t index,
                                std::span<const std::byte> bytes);
[[nodiscard]] DecodeResult<Vec3> decode_recovered_world_vertex(const RecoveredWorldSector& sector,
                                                               std::int32_t index,
                                                               std::span<const std::byte> bytes);
[[nodiscard]] DecodeResult<RecoveredWorldTriangle>
decode_recovered_world_triangle_resolved(const RecoveredWorld& world, std::size_t sector_index,
                                         std::int32_t triangle_index,
                                         std::span<const std::byte> bytes);
[[nodiscard]] std::optional<CollisionHit>
pick_collision_worlds(std::span<const RecoveredWorld> worlds, std::span<const std::byte> bytes,
                      const CollisionRay& ray, std::span<const CollisionClipPlane> clips = {});
// Walls crossed by the ray within `max_distance`: surface hits closer together
// than `merge_distance` merge, and each pair of faces counts as one wall.
[[nodiscard]] int count_collision_walls(std::span<const RecoveredWorld> worlds,
                                        std::span<const std::byte> bytes, const CollisionRay& ray,
                                        float max_distance, float merge_distance,
                                        std::span<const CollisionClipPlane> clips = {});
[[nodiscard]] bool collision_point_visible(Vec3 point,
                                           std::span<const CollisionClipPlane> clips) noexcept;
[[nodiscard]] Measurement measure_points(Vec3 a, Vec3 b) noexcept;
[[nodiscard]] const char* world_recovery_status_name(WorldRecoveryStatus status) noexcept;
[[nodiscard]] const char* world_topology_status_name(WorldTopologyStatus status) noexcept;

} // namespace rws
