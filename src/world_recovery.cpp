#include "rws/world_recovery.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <sstream>

namespace rws {
namespace {

std::uint32_t read_u32(const std::span<const std::byte> bytes,
                       const std::uint64_t offset) noexcept {
    const auto i = static_cast<std::size_t>(offset);
    return std::to_integer<std::uint32_t>(bytes[i]) |
           (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[i + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[i + 3]) << 24U);
}

std::uint16_t read_u16(const std::span<const std::byte> bytes,
                       const std::uint64_t offset) noexcept {
    const auto i = static_cast<std::size_t>(offset);
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[i]) |
                                      (std::to_integer<std::uint16_t>(bytes[i + 1]) << 8U));
}

float read_f32(const std::span<const std::byte> bytes, const std::uint64_t offset) noexcept {
    return std::bit_cast<float>(read_u32(bytes, offset));
}

bool checked_add(std::uint64_t& value, const std::uint64_t add) noexcept {
    if (add > std::numeric_limits<std::uint64_t>::max() - value) return false;
    value += add;
    return true;
}

bool checked_array(std::uint64_t& value, const std::int32_t count,
                   const std::uint64_t stride) noexcept {
    if (count < 0) return false;
    const auto unsigned_count = static_cast<std::uint64_t>(count);
    if (stride != 0 && unsigned_count > std::numeric_limits<std::uint64_t>::max() / stride)
        return false;
    return checked_add(value, unsigned_count * stride);
}

std::string offset_message(const char* message, const std::uint64_t offset) {
    std::ostringstream out;
    out << message << " at 0x" << std::hex << offset;
    return out.str();
}

float& component(Vec3& value, const std::size_t axis) noexcept {
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

float get_component(const Vec3 value, const std::size_t axis) noexcept {
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

bool valid_bounds(const Vec3 inf, const Vec3 sup) noexcept {
    return std::isfinite(inf.x) && std::isfinite(inf.y) && std::isfinite(inf.z) &&
           std::isfinite(sup.x) && std::isfinite(sup.y) && std::isfinite(sup.z) && inf.x <= sup.x &&
           inf.y <= sup.y && inf.z <= sup.z;
}

void find_worlds(const std::vector<Chunk>& chunks, std::vector<const Chunk*>& output) {
    for (const auto& chunk : chunks) {
        if (chunk.type == 0x0B) output.push_back(&chunk);
        find_worlds(chunk.children, output);
    }
}

void recover_topology(RecoveredWorld& world, const Chunk& chunk,
                      const std::span<const std::byte> bytes) {
    struct Token {
        std::uint64_t offset{};
        bool plane{};
        std::size_t index{};
    };
    const auto scan_end =
        chunk.payload_offset +
        std::min<std::uint64_t>(chunk.available_size, bytes.size() - chunk.payload_offset);
    std::vector<Token> tokens;
    tokens.reserve(world.sectors.size());
    for (std::size_t i = 0; i < world.sectors.size(); ++i)
        tokens.push_back({world.sectors[i].chunk_offset, false, i});
    for (std::uint64_t candidate = chunk.payload_offset; candidate + 48U <= scan_end; ++candidate) {
        if (read_u32(bytes, candidate) != 0x0AU ||
            read_u32(bytes, candidate + 8U) != world.library_id)
            continue;
        if (std::any_of(world.sectors.begin(), world.sectors.end(), [&](const auto& sector) {
                return candidate >= sector.chunk_offset && candidate < sector.range_end;
            })) {
            ++world.topology_stats.overlapping_candidates;
            world.topology_diagnostics.push_back(
                offset_message("Plane candidate overlaps a recovered leaf range", candidate));
            continue;
        }
        if (read_u32(bytes, candidate + 12U) != 0x01U ||
            read_u32(bytes, candidate + 20U) != world.library_id) {
            ++world.topology_stats.invalid_candidates;
            continue;
        }
        const auto size = read_u32(bytes, candidate + 16U);
        if (size < 24U || candidate + 24U + size > scan_end) {
            ++world.topology_stats.truncated_candidates;
            world.topology_diagnostics.push_back(
                offset_message("Truncated Plane Struct candidate", candidate));
            continue;
        }
        const auto data = candidate + 24U;
        RecoveredWorldPlane plane;
        plane.chunk_offset = candidate;
        plane.struct_offset = candidate + 12U;
        plane.axis = std::bit_cast<std::int32_t>(read_u32(bytes, data));
        plane.split = read_f32(bytes, data + 4U);
        plane.left_is_world_sector = read_u32(bytes, data + 8U) != 0U;
        plane.right_is_world_sector = read_u32(bytes, data + 12U) != 0U;
        plane.left_value = read_f32(bytes, data + 16U);
        plane.right_value = read_f32(bytes, data + 20U);
        if ((plane.axis != 0 && plane.axis != 4 && plane.axis != 8) ||
            !std::isfinite(plane.split) || !std::isfinite(plane.left_value) ||
            !std::isfinite(plane.right_value)) {
            ++world.topology_stats.invalid_candidates;
            world.topology_diagnostics.push_back(offset_message("Invalid Plane fields", candidate));
            continue;
        }
        const auto index = world.planes.size();
        world.planes.push_back(plane);
        tokens.push_back({candidate, true, index});
        candidate += 47U;
    }
    std::sort(tokens.begin(), tokens.end(), [](const Token& a, const Token& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        return a.plane && !b.plane;
    });
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i - 1].offset == tokens[i].offset) ++world.topology_stats.duplicate_candidates;
    }
    if (tokens.empty()) {
        world.topology_diagnostics.emplace_back("No validated Plane or leaf topology candidates");
        world.topology_stats.unlinked_sectors = world.sectors.size();
        return;
    }
    std::size_t cursor{};
    constexpr float tolerance = 0.01F;
    std::function<std::optional<std::size_t>(bool, std::optional<std::size_t>, std::optional<bool>,
                                             Vec3, Vec3, std::size_t)>
        consume;
    consume = [&](const bool expect_sector, const std::optional<std::size_t> parent,
                  const std::optional<bool> left, Vec3 inf, Vec3 sup,
                  const std::size_t depth) -> std::optional<std::size_t> {
        if (cursor >= tokens.size()) {
            ++world.topology_stats.truncated_candidates;
            return std::nullopt;
        }
        const auto token = tokens[cursor];
        if (token.plane == expect_sector) {
            ++world.topology_stats.ambiguous_candidates;
            world.topology_diagnostics.push_back(offset_message(
                "BSP child kind does not match physical preorder node", token.offset));
            return std::nullopt;
        }
        ++cursor;
        const auto node_index = world.topology_nodes.size();
        world.topology_nodes.push_back(
            {token.plane ? RecoveredWorldNode::Kind::plane : RecoveredWorldNode::Kind::sector,
             token.index, parent, left, inf, sup, depth});
        world.topology_stats.maximum_depth = std::max(world.topology_stats.maximum_depth, depth);
        if (!token.plane) {
            ++world.topology_stats.linked_sectors;
            const auto& sector = world.sectors[token.index];
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (get_component(sector.bounding_box_inf, axis) <
                        get_component(inf, axis) - tolerance ||
                    get_component(sector.bounding_box_sup, axis) >
                        get_component(sup, axis) + tolerance) {
                    ++world.topology_stats.bounds_conflicts;
                    world.topology_diagnostics.push_back(offset_message(
                        "Leaf bounds conflict with validated BSP bounds", sector.chunk_offset));
                    break;
                }
            }
            world.topology_nodes[node_index].bounding_box_inf = sector.bounding_box_inf;
            world.topology_nodes[node_index].bounding_box_sup = sector.bounding_box_sup;
            return node_index;
        }
        auto& plane = world.planes[token.index];
        const auto axis = static_cast<std::size_t>(plane.axis / 4);
        Vec3 left_inf = inf, left_sup = sup, right_inf = inf, right_sup = sup;
        component(left_sup, axis) = plane.left_value;
        component(right_inf, axis) = plane.right_value;
        if (!valid_bounds(left_inf, left_sup) || !valid_bounds(right_inf, right_sup) ||
            plane.left_value > get_component(sup, axis) + tolerance ||
            plane.right_value < get_component(inf, axis) - tolerance) {
            ++world.topology_stats.bounds_conflicts;
            world.topology_diagnostics.push_back(
                offset_message("Plane child bounds are invalid", plane.chunk_offset));
            return node_index;
        }
        plane.left_node =
            consume(plane.left_is_world_sector, node_index, true, left_inf, left_sup, depth + 1U);
        plane.right_node = consume(plane.right_is_world_sector, node_index, false, right_inf,
                                   right_sup, depth + 1U);
        return node_index;
    };
    world.topology_root = consume(world.header.root_is_world_sector, std::nullopt, std::nullopt,
                                  world.header.bounding_box_inf, world.header.bounding_box_sup, 0U);
    world.topology_stats.unreachable_nodes = tokens.size() - cursor;
    world.topology_stats.unlinked_sectors =
        world.sectors.size() -
        std::min<std::uint64_t>(world.sectors.size(), world.topology_stats.linked_sectors);
    const bool count_match =
        world.planes.size() ==
            static_cast<std::size_t>(std::max(0, world.header.plane_sector_count)) &&
        world.topology_stats.linked_sectors ==
            static_cast<std::uint64_t>(std::max(0, world.header.world_sector_count));
    const bool clean = world.topology_root && cursor == tokens.size() && count_match &&
                       world.topology_stats.invalid_candidates == 0 &&
                       world.topology_stats.ambiguous_candidates == 0 &&
                       world.topology_stats.duplicate_candidates == 0 &&
                       world.topology_stats.overlapping_candidates == 0 &&
                       world.topology_stats.truncated_candidates == 0 &&
                       world.topology_stats.bounds_conflicts == 0;
    world.topology_status =
        clean ? WorldTopologyStatus::complete
              : (world.topology_root ? WorldTopologyStatus::partial : WorldTopologyStatus::failed);
    if (!count_match)
        world.topology_diagnostics.emplace_back(
            "Recovered Plane/leaf totals differ from the World header");
    if (world.topology_stats.unreachable_nodes != 0)
        world.topology_diagnostics.emplace_back(
            "One or more validated BSP candidates are unreachable");
}

} // namespace

const char* world_recovery_status_name(const WorldRecoveryStatus status) noexcept {
    switch (status) {
    case WorldRecoveryStatus::complete:
        return "complete";
    case WorldRecoveryStatus::partial:
        return "partial";
    case WorldRecoveryStatus::failed:
        return "failed";
    }
    return "failed";
}

const char* world_topology_status_name(const WorldTopologyStatus status) noexcept {
    switch (status) {
    case WorldTopologyStatus::complete:
        return "complete";
    case WorldTopologyStatus::partial:
        return "partial";
    case WorldTopologyStatus::failed:
        return "failed";
    }
    return "failed";
}

DecodeResult<Vec3> decode_recovered_world_vertex(const RecoveredWorldSector& sector,
                                                 const std::int32_t index,
                                                 const std::span<const std::byte> bytes) {
    if (index < 0 || index >= sector.vertex_count)
        return {std::nullopt, "World Sector vertex index is out of range"};
    const auto unsigned_index = static_cast<std::uint64_t>(index);
    if (unsigned_index > (std::numeric_limits<std::uint64_t>::max() - sector.vertices_offset) / 12U)
        return {std::nullopt, "World Sector vertex offset overflows"};
    const auto offset = sector.vertices_offset + unsigned_index * 12U;
    if (offset > bytes.size() || bytes.size() - offset < 12U)
        return {std::nullopt, "World Sector vertex is outside the file"};
    return {
        Vec3{read_f32(bytes, offset), read_f32(bytes, offset + 4U), read_f32(bytes, offset + 8U)},
        {}};
}

DecodeResult<TriangleInfo> decode_recovered_world_triangle(const RecoveredWorldSector& sector,
                                                           const std::int32_t index,
                                                           const std::span<const std::byte> bytes) {
    if (index < 0 || index >= sector.triangle_count)
        return {std::nullopt, "World Sector triangle index is out of range"};
    const auto unsigned_index = static_cast<std::uint64_t>(index);
    if (unsigned_index > (std::numeric_limits<std::uint64_t>::max() - sector.triangles_offset) / 8U)
        return {std::nullopt, "World Sector triangle offset overflows"};
    const auto offset = sector.triangles_offset + unsigned_index * 8U;
    if (offset > bytes.size() || bytes.size() - offset < 8U)
        return {std::nullopt, "World Sector triangle is outside the file"};
    TriangleInfo result;
    // RpWorldSector serializes three vertex indices followed by a local material.
    result.vertices = {read_u16(bytes, offset), read_u16(bytes, offset + 2U),
                       read_u16(bytes, offset + 4U)};
    result.material = read_u16(bytes, offset + 6U);
    return {result, {}};
}

DecodeResult<RecoveredWorldTriangle> decode_recovered_world_triangle_resolved(
    const RecoveredWorld& world, const std::size_t sector_index, const std::int32_t triangle_index,
    const std::span<const std::byte> bytes) {
    if (sector_index >= world.sectors.size())
        return {std::nullopt, "Recovered World Sector index is out of range"};
    const auto& sector = world.sectors[sector_index];
    const auto triangle = decode_recovered_world_triangle(sector, triangle_index, bytes);
    if (!triangle) return {std::nullopt, triangle.error};
    RecoveredWorldTriangle result;
    result.sector_index = sector_index;
    result.triangle_index = triangle_index;
    result.source_offset =
        sector.triangles_offset + static_cast<std::uint64_t>(triangle_index) * 8U;
    const auto resolved_material =
        static_cast<std::int64_t>(sector.material_window_base) + triangle.value->material;
    if (resolved_material < 0 || resolved_material >= world.material_count)
        return {std::nullopt, "World Sector triangle material is out of range"};
    result.material_slot = static_cast<std::int32_t>(resolved_material);
    result.vertex_indices = triangle.value->vertices;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto vertex = decode_recovered_world_vertex(sector, result.vertex_indices[i], bytes);
        if (!vertex) return {std::nullopt, vertex.error};
        result.vertices[i] = *vertex.value;
    }
    return {result, {}};
}

bool collision_point_visible(const Vec3 point,
                             const std::span<const CollisionClipPlane> clips) noexcept {
    const std::array<float, 3> values{point.x, point.y, point.z};
    for (const auto& clip : clips) {
        if (!clip.enabled || clip.axis >= values.size()) continue;
        if (clip.keep_greater ? values[clip.axis] < clip.position
                              : values[clip.axis] > clip.position)
            return false;
    }
    return true;
}

Measurement measure_points(const Vec3 a, const Vec3 b) noexcept {
    Measurement result;
    result.absolute_delta = {std::abs(b.x - a.x), std::abs(b.y - a.y), std::abs(b.z - a.z)};
    result.distance = std::sqrt(result.absolute_delta.x * result.absolute_delta.x +
                                result.absolute_delta.y * result.absolute_delta.y +
                                result.absolute_delta.z * result.absolute_delta.z);
    return result;
}

namespace {

// Slab test: does the ray reach the box within [0, limit]?
bool ray_box_hit(const CollisionRay& ray, const Vec3 inf, const Vec3 sup, const float limit) {
    const auto axis = [](const Vec3 value, const std::size_t i) {
        return i == 0 ? value.x : (i == 1 ? value.y : value.z);
    };
    float near_value = 0.0F, far_value = limit;
    for (std::size_t i = 0; i < 3; ++i) {
        const float origin = axis(ray.origin, i), direction = axis(ray.direction, i);
        if (std::abs(direction) < 1.0e-12F) {
            if (origin < axis(inf, i) || origin > axis(sup, i)) return false;
            continue;
        }
        float a = (axis(inf, i) - origin) / direction;
        float b = (axis(sup, i) - origin) / direction;
        if (a > b) std::swap(a, b);
        near_value = std::max(near_value, a);
        far_value = std::min(far_value, b);
        if (near_value > far_value) return false;
    }
    return far_value >= 0.0F;
}

struct RayTriangleHit {
    float distance{}, u{}, v{};
};

// Möller-Trumbore, both faces.
std::optional<RayTriangleHit> ray_triangle_hit(const CollisionRay& ray, const Vec3 a, const Vec3 b,
                                               const Vec3 c) {
    const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 p{ray.direction.y * e2.z - ray.direction.z * e2.y,
                 ray.direction.z * e2.x - ray.direction.x * e2.z,
                 ray.direction.x * e2.y - ray.direction.y * e2.x};
    const float determinant = e1.x * p.x + e1.y * p.y + e1.z * p.z;
    if (std::abs(determinant) < 1.0e-10F) return std::nullopt;
    const float inverse = 1.0F / determinant;
    const Vec3 offset{ray.origin.x - a.x, ray.origin.y - a.y, ray.origin.z - a.z};
    const float u = (offset.x * p.x + offset.y * p.y + offset.z * p.z) * inverse;
    if (u < -1.0e-6F || u > 1.000001F) return std::nullopt;
    const Vec3 q{offset.y * e1.z - offset.z * e1.y, offset.z * e1.x - offset.x * e1.z,
                 offset.x * e1.y - offset.y * e1.x};
    const float v =
        (ray.direction.x * q.x + ray.direction.y * q.y + ray.direction.z * q.z) * inverse;
    if (v < -1.0e-6F || u + v > 1.000001F) return std::nullopt;
    const float distance = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inverse;
    if (distance <= 0.0F) return std::nullopt;
    return RayTriangleHit{distance, u, v};
}

Vec3 point_along(const CollisionRay& ray, const float distance) {
    return {ray.origin.x + ray.direction.x * distance, ray.origin.y + ray.direction.y * distance,
            ray.origin.z + ray.direction.z * distance};
}

} // namespace

std::optional<CollisionHit> pick_collision_worlds(const std::span<const RecoveredWorld> worlds,
                                                  const std::span<const std::byte> bytes,
                                                  const CollisionRay& ray,
                                                  const std::span<const CollisionClipPlane> clips) {
    float closest = std::numeric_limits<float>::max();
    std::optional<CollisionHit> hit;
    for (std::size_t wi = 0; wi < worlds.size(); ++wi) {
        const auto& world = worlds[wi];
        for (std::size_t si = 0; si < world.sectors.size(); ++si) {
            const auto& sector = world.sectors[si];
            if (!ray_box_hit(ray, sector.bounding_box_inf, sector.bounding_box_sup, closest)) continue;
            for (std::int32_t ti = 0; ti < sector.triangle_count; ++ti) {
                const auto triangle =
                    decode_recovered_world_triangle_resolved(world, si, ti, bytes);
                if (!triangle) continue;
                const auto& a = triangle.value->vertices[0];
                const auto& b = triangle.value->vertices[1];
                const auto& c = triangle.value->vertices[2];
                const auto crossing = ray_triangle_hit(ray, a, b, c);
                if (!crossing || crossing->distance >= closest) continue;
                const auto position = point_along(ray, crossing->distance);
                if (!collision_point_visible(position, clips)) continue;
                const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
                Vec3 normal{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                            e1.x * e2.y - e1.y * e2.x};
                const float length =
                    std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                if (length <= 1.0e-10F) continue;
                normal = {normal.x / length, normal.y / length, normal.z / length};
                closest = crossing->distance;
                hit = CollisionHit{wi,
                                   si,
                                   ti,
                                   triangle.value->material_slot,
                                   sector.chunk_offset,
                                   triangle.value->source_offset,
                                   crossing->distance,
                                   position,
                                   normal,
                                   {1.0F - crossing->u - crossing->v, crossing->u, crossing->v}};
            }
        }
    }
    return hit;
}

int count_collision_walls(const std::span<const RecoveredWorld> worlds,
                          const std::span<const std::byte> bytes, const CollisionRay& ray,
                          const float max_distance, const float merge_distance,
                          const std::span<const CollisionClipPlane> clips) {
    if (!(max_distance > 0.0F)) return 0;
    std::vector<float> crossings;
    for (const auto& world : worlds) {
        for (std::size_t si = 0; si < world.sectors.size(); ++si) {
            const auto& sector = world.sectors[si];
            if (!ray_box_hit(ray, sector.bounding_box_inf, sector.bounding_box_sup, max_distance))
                continue;
            for (std::int32_t ti = 0; ti < sector.triangle_count; ++ti) {
                const auto triangle =
                    decode_recovered_world_triangle_resolved(world, si, ti, bytes);
                if (!triangle) continue;
                const auto& t = triangle.value->vertices;
                const auto crossing = ray_triangle_hit(ray, t[0], t[1], t[2]);
                if (!crossing || crossing->distance >= max_distance) continue;
                if (!collision_point_visible(point_along(ray, crossing->distance), clips)) continue;
                crossings.push_back(crossing->distance);
            }
        }
    }
    // Hits on shared edges or coplanar layers count once; each wall has two faces,
    // while a one-sided surface (terrain, a single plane) still counts as a wall.
    std::ranges::sort(crossings);
    int surfaces = 0;
    float last = -std::numeric_limits<float>::max();
    for (const float distance : crossings) {
        if (distance - last > merge_distance) ++surfaces;
        last = distance;
    }
    return (surfaces + 1) / 2;
}

RecoveredWorld recover_world(const Chunk& world, const std::span<const std::byte> bytes) {
    RecoveredWorld result;
    result.world_offset = world.offset;
    result.library_id = world.library_id;
    if (world.type != 0x0B) {
        result.diagnostics.emplace_back("Recovery target is not a World chunk");
        return result;
    }
    const auto decoded = decode_world(world, bytes);
    if (!decoded) {
        result.diagnostics.push_back(decoded.error);
        return result;
    }
    result.header = *decoded.value;

    if (const auto* material_list = find_child(world, 0x08)) {
        const auto materials = decode_material_list(*material_list, bytes);
        if (materials)
            result.material_count = materials.value->material_count;
        else
            result.diagnostics.push_back("World Material List: " + materials.error);
    } else {
        result.diagnostics.emplace_back("World has no Material List");
    }

    std::uint32_t texcoord_sets = (result.header.format >> 16U) & 0xFFU;
    if (texcoord_sets == 0)
        texcoord_sets =
            (result.header.format & 0x80U) ? 2U : ((result.header.format & 0x04U) ? 1U : 0U);
    if (texcoord_sets > 8U) {
        result.diagnostics.emplace_back("World has more than 8 texture coordinate sets");
        return result;
    }

    if (world.payload_offset > bytes.size()) {
        result.diagnostics.emplace_back("World payload begins outside the file");
        return result;
    }
    const auto scan_end =
        world.payload_offset +
        std::min<std::uint64_t>(world.available_size, bytes.size() - world.payload_offset);
    std::uint64_t last_end = world.payload_offset;
    for (std::uint64_t candidate = world.payload_offset; candidate + 24U <= scan_end; ++candidate) {
        if (read_u32(bytes, candidate) != 0x09U ||
            read_u32(bytes, candidate + 8U) != world.library_id)
            continue;
        if (read_u32(bytes, candidate + 12U) != 0x01U ||
            read_u32(bytes, candidate + 20U) != world.library_id) {
            ++result.invalid_candidates;
            continue;
        }

        const auto struct_size = static_cast<std::uint64_t>(read_u32(bytes, candidate + 16U));
        const auto data = candidate + 24U;
        if (struct_size < 44U || struct_size > scan_end - data) {
            ++result.invalid_candidates;
            ++result.truncated_candidates;
            result.diagnostics.push_back(
                offset_message("Truncated or undersized World Sector Struct candidate", candidate));
            continue;
        }
        const auto triangle_count = std::bit_cast<std::int32_t>(read_u32(bytes, data + 4U));
        const auto vertex_count = std::bit_cast<std::int32_t>(read_u32(bytes, data + 8U));
        std::uint64_t expected = 44U;
        bool valid_size = checked_array(expected, vertex_count, 12U);
        if (valid_size && (result.header.format & 0x10U))
            valid_size = checked_array(expected, vertex_count, 4U);
        if (valid_size && (result.header.format & 0x08U))
            valid_size = checked_array(expected, vertex_count, 4U);
        if (valid_size) {
            if (vertex_count < 0 || static_cast<std::uint64_t>(vertex_count) >
                                        std::numeric_limits<std::uint64_t>::max() /
                                            (8U * std::max(1U, texcoord_sets))) {
                valid_size = false;
            } else {
                valid_size =
                    checked_add(expected, static_cast<std::uint64_t>(vertex_count) *
                                              static_cast<std::uint64_t>(texcoord_sets) * 8U);
            }
        }
        if (valid_size) valid_size = checked_array(expected, triangle_count, 8U);
        if (!valid_size || expected != struct_size) {
            ++result.invalid_candidates;
            result.diagnostics.push_back(
                offset_message("World Sector Struct layout does not match its counts", candidate));
            continue;
        }
        const auto range_end = data + struct_size;
        if (candidate < last_end) {
            ++result.duplicate_or_overlapping_ranges;
            result.diagnostics.push_back(
                offset_message("Duplicate or overlapping World Sector candidate", candidate));
            continue;
        }

        RecoveredWorldSector sector;
        sector.chunk_offset = candidate;
        sector.struct_offset = candidate + 12U;
        sector.range_end = range_end;
        sector.material_window_base = std::bit_cast<std::int32_t>(read_u32(bytes, data));
        sector.triangle_count = triangle_count;
        sector.vertex_count = vertex_count;
        sector.bounding_box_inf = {read_f32(bytes, data + 12U), read_f32(bytes, data + 16U),
                                   read_f32(bytes, data + 20U)};
        sector.bounding_box_sup = {read_f32(bytes, data + 24U), read_f32(bytes, data + 28U),
                                   read_f32(bytes, data + 32U)};
        sector.collision_sector_present = read_u32(bytes, data + 36U) != 0U;
        if (!valid_bounds(sector.bounding_box_inf, sector.bounding_box_sup)) {
            ++result.invalid_candidates;
            result.diagnostics.push_back(
                offset_message("World Sector bounds are invalid", candidate));
            candidate = range_end - 1U;
            continue;
        }
        sector.texcoord_sets = texcoord_sets;
        sector.vertices_offset = data + 44U;
        auto cursor = sector.vertices_offset + static_cast<std::uint64_t>(vertex_count) * 12U;
        if (result.header.format & 0x10U) {
            sector.normals_offset = cursor;
            cursor += static_cast<std::uint64_t>(vertex_count) * 4U;
        }
        if (result.header.format & 0x08U) {
            sector.prelight_offset = cursor;
            cursor += static_cast<std::uint64_t>(vertex_count) * 4U;
        }
        for (std::uint32_t set = 0; set < texcoord_sets; ++set) {
            sector.texcoord_offsets.push_back(cursor);
            cursor += static_cast<std::uint64_t>(vertex_count) * 8U;
        }
        sector.triangles_offset = cursor;

        for (std::int32_t i = 0; i < triangle_count; ++i) {
            const auto triangle = decode_recovered_world_triangle(sector, i, bytes);
            if (!triangle || triangle.value->vertices[0] >= vertex_count ||
                triangle.value->vertices[1] >= vertex_count ||
                triangle.value->vertices[2] >= vertex_count) {
                ++result.invalid_triangles;
                continue;
            }
            const auto material =
                static_cast<std::int64_t>(sector.material_window_base) + triangle.value->material;
            if (material < 0 || material >= result.material_count)
                ++result.invalid_material_references;
        }
        result.recovered_vertices += vertex_count;
        result.recovered_triangles += triangle_count;
        result.sectors.push_back(std::move(sector));
        last_end = range_end;
        candidate = range_end - 1U;
    }

    const bool counts_match =
        result.sectors.size() ==
            static_cast<std::size_t>(std::max(0, result.header.world_sector_count)) &&
        result.recovered_vertices == result.header.vertex_count &&
        result.recovered_triangles == result.header.triangle_count;
    if (result.sectors.empty()) {
        result.status = WorldRecoveryStatus::failed;
        result.diagnostics.emplace_back("No usable World Sectors were recovered");
    } else if (counts_match && result.invalid_triangles == 0 &&
               result.invalid_material_references == 0) {
        result.status = WorldRecoveryStatus::complete;
    } else {
        result.status = WorldRecoveryStatus::partial;
    }
    if (result.sectors.size() !=
        static_cast<std::size_t>(std::max(0, result.header.world_sector_count)))
        result.diagnostics.emplace_back(
            "Recovered World Sector count differs from the World header");
    if (result.recovered_triangles != result.header.triangle_count)
        result.diagnostics.emplace_back("Recovered triangle count differs from the World header");
    if (result.recovered_vertices != result.header.vertex_count)
        result.diagnostics.emplace_back("Recovered vertex count differs from the World header");
    if (result.invalid_triangles != 0)
        result.diagnostics.emplace_back(
            "One or more recovered triangles use invalid vertex indices");
    if (result.invalid_material_references != 0)
        result.diagnostics.emplace_back(
            "One or more recovered triangles use invalid material references");
    recover_topology(result, world, bytes);
    return result;
}

std::vector<RecoveredWorld> recover_worlds(const std::vector<Chunk>& chunks,
                                           const std::span<const std::byte> bytes) {
    std::vector<const Chunk*> worlds;
    find_worlds(chunks, worlds);
    std::vector<RecoveredWorld> result;
    result.reserve(worlds.size());
    for (const auto* world : worlds)
        result.push_back(recover_world(*world, bytes));
    return result;
}

} // namespace rws
