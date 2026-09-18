#include "rws/world_recovery.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <sstream>

namespace rws {
namespace {

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::uint64_t offset) noexcept {
    const auto i = static_cast<std::size_t>(offset);
    return std::to_integer<std::uint32_t>(bytes[i]) |
        (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U) |
        (std::to_integer<std::uint32_t>(bytes[i + 2]) << 16U) |
        (std::to_integer<std::uint32_t>(bytes[i + 3]) << 24U);
}

std::uint16_t read_u16(const std::span<const std::byte> bytes, const std::uint64_t offset) noexcept {
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

void find_worlds(const std::vector<Chunk>& chunks, std::vector<const Chunk*>& output) {
    for (const auto& chunk : chunks) {
        if (chunk.type == 0x0B) output.push_back(&chunk);
        find_worlds(chunk.children, output);
    }
}

} // namespace

const char* world_recovery_status_name(const WorldRecoveryStatus status) noexcept {
    switch (status) {
    case WorldRecoveryStatus::complete: return "complete";
    case WorldRecoveryStatus::partial: return "partial";
    case WorldRecoveryStatus::failed: return "failed";
    }
    return "failed";
}

DecodeResult<TriangleInfo> decode_recovered_world_triangle(
    const RecoveredWorldSector& sector, const std::int32_t index,
    const std::span<const std::byte> bytes) {
    if (index < 0 || index >= sector.triangle_count)
        return {std::nullopt, "World Sector triangle index is out of range"};
    const auto offset = sector.triangles_offset + static_cast<std::uint64_t>(index) * 8U;
    if (offset > bytes.size() || bytes.size() - offset < 8U)
        return {std::nullopt, "World Sector triangle is outside the file"};
    TriangleInfo result;
    // RpWorldSector serializes three vertex indices followed by a local material.
    result.vertices = {read_u16(bytes, offset), read_u16(bytes, offset + 2U),
                       read_u16(bytes, offset + 4U)};
    result.material = read_u16(bytes, offset + 6U);
    return {result, {}};
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
        if (materials) result.material_count = materials.value->material_count;
        else result.diagnostics.push_back("World Material List: " + materials.error);
    } else {
        result.diagnostics.emplace_back("World has no Material List");
    }

    std::uint32_t texcoord_sets = (result.header.format >> 16U) & 0xFFU;
    if (texcoord_sets == 0)
        texcoord_sets = (result.header.format & 0x80U) ? 2U :
                        ((result.header.format & 0x04U) ? 1U : 0U);
    if (texcoord_sets > 8U) {
        result.diagnostics.emplace_back("World has more than 8 texture coordinate sets");
        return result;
    }

    if (world.payload_offset > bytes.size()) {
        result.diagnostics.emplace_back("World payload begins outside the file");
        return result;
    }
    const auto scan_end = world.payload_offset +
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
            result.diagnostics.push_back(offset_message(
                "Truncated or undersized World Sector Struct candidate", candidate));
            continue;
        }
        const auto triangle_count = std::bit_cast<std::int32_t>(read_u32(bytes, data + 4U));
        const auto vertex_count = std::bit_cast<std::int32_t>(read_u32(bytes, data + 8U));
        std::uint64_t expected = 44U;
        bool valid_size = checked_array(expected, vertex_count, 12U);
        if (valid_size && (result.header.format & 0x10U)) valid_size = checked_array(expected, vertex_count, 4U);
        if (valid_size && (result.header.format & 0x08U)) valid_size = checked_array(expected, vertex_count, 4U);
        if (valid_size) {
            if (vertex_count < 0 || static_cast<std::uint64_t>(vertex_count) >
                    std::numeric_limits<std::uint64_t>::max() / (8U * std::max(1U, texcoord_sets))) {
                valid_size = false;
            } else {
                valid_size = checked_add(expected, static_cast<std::uint64_t>(vertex_count) *
                    static_cast<std::uint64_t>(texcoord_sets) * 8U);
            }
        }
        if (valid_size) valid_size = checked_array(expected, triangle_count, 8U);
        if (!valid_size || expected != struct_size) {
            ++result.invalid_candidates;
            result.diagnostics.push_back(offset_message(
                "World Sector Struct layout does not match its counts", candidate));
            continue;
        }
        const auto range_end = data + struct_size;
        if (candidate < last_end) {
            ++result.duplicate_or_overlapping_ranges;
            result.diagnostics.push_back(offset_message(
                "Duplicate or overlapping World Sector candidate", candidate));
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
            const auto material = static_cast<std::int64_t>(sector.material_window_base) +
                                  triangle.value->material;
            if (material < 0 || material >= result.material_count)
                ++result.invalid_material_references;
        }
        result.recovered_vertices += vertex_count;
        result.recovered_triangles += triangle_count;
        result.sectors.push_back(std::move(sector));
        last_end = range_end;
        candidate = range_end - 1U;
    }

    const bool counts_match = result.sectors.size() ==
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
    if (result.sectors.size() != static_cast<std::size_t>(std::max(0, result.header.world_sector_count)))
        result.diagnostics.emplace_back("Recovered World Sector count differs from the World header");
    if (result.recovered_triangles != result.header.triangle_count)
        result.diagnostics.emplace_back("Recovered triangle count differs from the World header");
    if (result.recovered_vertices != result.header.vertex_count)
        result.diagnostics.emplace_back("Recovered vertex count differs from the World header");
    if (result.invalid_triangles != 0)
        result.diagnostics.emplace_back("One or more recovered triangles use invalid vertex indices");
    if (result.invalid_material_references != 0)
        result.diagnostics.emplace_back("One or more recovered triangles use invalid material references");
    return result;
}

std::vector<RecoveredWorld> recover_worlds(const std::vector<Chunk>& chunks,
                                           const std::span<const std::byte> bytes) {
    std::vector<const Chunk*> worlds;
    find_worlds(chunks, worlds);
    std::vector<RecoveredWorld> result;
    result.reserve(worlds.size());
    for (const auto* world : worlds) result.push_back(recover_world(*world, bytes));
    return result;
}

} // namespace rws
