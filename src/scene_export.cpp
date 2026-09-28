#include "rws/scene_export.hpp"
#include "rws/world_recovery.hpp"

#include "rws/decoded.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace rws {
namespace {

// CSF coordinates are centimetre-scale. glTF uses metres; applying this at
// export keeps a full map inside Blender's practical default clipping range.
constexpr float scene_scale = 0.01F;

struct FrameTransform {
    std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Vec3 position{};
};

struct Uv {
    float u{}, v{};
};

struct MaterialRecord {
    std::string name;
    std::array<std::uint8_t, 4> color{190, 190, 190, 255};
    std::string base_texture;
    std::string lightmap_texture;
    std::uint64_t owner_offset{};
    std::uint32_t slot{};
    std::string surface_name;
    std::optional<std::uint32_t> surface_id;
};

struct PrimitiveRecord {
    std::size_t material{};
    std::vector<std::uint32_t> indices;
};

struct MeshRecord {
    std::string name;
    std::string kind;
    std::uint64_t owner_offset{};
    std::uint64_t source_offset{};
    std::optional<std::size_t> world_index;
    std::optional<std::size_t> sector_index;
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Uv> uv0;
    std::vector<Uv> uv1;
    std::vector<PrimitiveRecord> primitives;
};

struct PrototypeRecord {
    std::vector<std::size_t> meshes;
    FrameTransform original_root;
};

struct BufferView {
    std::uint64_t offset{}, length{};
    std::uint32_t target{};
};
struct Accessor {
    std::size_t view{};
    std::uint32_t component_type{};
    std::uint64_t count{};
    std::string type;
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    bool has_bounds{};
};
struct GltfPrimitive {
    std::size_t position{}, normal{}, uv0{}, uv1{}, indices{}, material{};
    bool has_normal{}, has_uv0{}, has_uv1{};
};
struct GltfMesh {
    std::string name;
    std::vector<GltfPrimitive> primitives;
};

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    if (offset > bytes.size() || bytes.size() - static_cast<std::size_t>(offset) < 4)
        throw std::runtime_error("Scene source array is truncated");
    const auto i = static_cast<std::size_t>(offset);
    return std::to_integer<std::uint32_t>(bytes[i]) |
           (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[i + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[i + 3]) << 24U);
}

float read_f32(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    return std::bit_cast<float>(read_u32(bytes, offset));
}

FrameTransform frame_transform(const FrameInfo& frame) {
    return {{{frame.rotation[0], frame.rotation[3], frame.rotation[6], frame.rotation[1],
              frame.rotation[4], frame.rotation[7], frame.rotation[2], frame.rotation[5],
              frame.rotation[8]}},
            frame.position};
}

Vec3 transform_point(const FrameTransform& transform, const Vec3 value) {
    const auto& m = transform.rotation;
    return {m[0] * value.x + m[1] * value.y + m[2] * value.z + transform.position.x,
            m[3] * value.x + m[4] * value.y + m[5] * value.z + transform.position.y,
            m[6] * value.x + m[7] * value.y + m[8] * value.z + transform.position.z};
}

Vec3 normalize(const Vec3 value) {
    const auto length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 1.0e-12F) return {};
    return {value.x / length, value.y / length, value.z / length};
}

Vec3 transform_direction(const FrameTransform& transform, const Vec3 value) {
    const auto& m = transform.rotation;
    return normalize({m[0] * value.x + m[1] * value.y + m[2] * value.z,
                      m[3] * value.x + m[4] * value.y + m[5] * value.z,
                      m[6] * value.x + m[7] * value.y + m[8] * value.z});
}

Vec3 inverse_transform_point(const FrameTransform& transform, const Vec3 value) {
    const auto& m = transform.rotation;
    const Vec3 translated{value.x - transform.position.x, value.y - transform.position.y,
                          value.z - transform.position.z};
    const float c00 = m[4] * m[8] - m[5] * m[7];
    const float c01 = m[2] * m[7] - m[1] * m[8];
    const float c02 = m[1] * m[5] - m[2] * m[4];
    const float determinant =
        m[0] * c00 + m[1] * (m[5] * m[6] - m[3] * m[8]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::abs(determinant) < 1.0e-8F) return translated;
    const float inverse = 1.0F / determinant;
    return {
        (c00 * translated.x + c01 * translated.y + c02 * translated.z) * inverse,
        ((m[5] * m[6] - m[3] * m[8]) * translated.x + (m[0] * m[8] - m[2] * m[6]) * translated.y +
         (m[2] * m[3] - m[0] * m[5]) * translated.z) *
            inverse,
        ((m[3] * m[7] - m[4] * m[6]) * translated.x + (m[1] * m[6] - m[0] * m[7]) * translated.y +
         (m[0] * m[4] - m[1] * m[3]) * translated.z) *
            inverse};
}

Vec3 inverse_transform_direction(const FrameTransform& transform, const Vec3 value) {
    FrameTransform without_translation = transform;
    without_translation.position = {};
    return normalize(inverse_transform_point(without_translation, value));
}

Vec3 export_point(const Vec3 value) {
    return {value.x * scene_scale, value.y * scene_scale, value.z * scene_scale};
}

FrameTransform compose(const FrameTransform& parent, const FrameTransform& local) {
    FrameTransform result;
    for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 3; ++column) {
            result.rotation[row * 3 + column] = 0.0F;
            for (unsigned k = 0; k < 3; ++k)
                result.rotation[row * 3 + column] +=
                    parent.rotation[row * 3 + k] * local.rotation[k * 3 + column];
        }
    result.position = transform_point(parent, local.position);
    return result;
}

std::string hex_offset(const std::uint64_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '\"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20)
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(character) << std::dec;
            else
                output << character;
        }
    }
    return output.str();
}

std::vector<MaterialRecord> decode_materials(const Chunk* list_chunk,
                                             const std::span<const std::byte> bytes,
                                             const std::uint64_t owner_offset,
                                             const std::string& name_prefix,
                                             const std::array<std::uint8_t, 4> fallback) {
    std::vector<MaterialRecord> result;
    const auto list =
        list_chunk ? decode_material_list(*list_chunk, bytes) : DecodeResult<MaterialListInfo>{};
    const auto count = list && list.value->material_count > 0
                           ? static_cast<std::size_t>(list.value->material_count)
                           : 1U;
    result.resize(count);
    std::vector<const Chunk*> material_chunks;
    if (list_chunk)
        for (const auto& child : list_chunk->children)
            if (child.type == 0x07) material_chunks.push_back(&child);
    std::size_t next_material{};
    for (std::size_t slot = 0; slot < count; ++slot) {
        auto& item = result[slot];
        item.name = name_prefix + "_mat_" + std::to_string(slot);
        item.color = fallback;
        item.owner_offset = owner_offset;
        item.slot = static_cast<std::uint32_t>(slot);
        const auto remap = list && slot < list.value->remap.size() ? list.value->remap[slot] : -1;
        if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
            const auto stable_name = item.name;
            item = result[static_cast<std::size_t>(remap)];
            item.name = stable_name;
            item.slot = static_cast<std::uint32_t>(slot);
            continue;
        }
        if (next_material >= material_chunks.size()) continue;
        const auto& chunk = *material_chunks[next_material++];
        if (const auto decoded = decode_material(chunk, bytes)) item.color = decoded.value->color;
        if (const auto* texture_chunk = find_child(chunk, 0x06)) {
            const auto texture = decode_texture(*texture_chunk, bytes);
            if (texture) item.base_texture = texture.value->name;
        }
        if (const auto* extension = find_child(chunk, 0x03)) {
            if (const auto* pyro = find_child(*extension, 0xFFFFFF00U)) {
                const auto metadata = decode_pyro_extension(*pyro, 0x07, bytes);
                if (metadata) {
                    item.surface_name = std::string(metadata.value->object_name());
                    item.surface_id = metadata.value->material_surface_type();
                }
            }
            if (const auto* effects_chunk = find_child(*extension, 0x120)) {
                const auto effects = decode_material_effects(*effects_chunk, 0x07, bytes);
                if (effects && effects.value->has_dual_texture)
                    item.lightmap_texture = effects.value->dual_texture.name;
            }
        }
    }
    return result;
}

void append_clumps(const std::vector<Chunk>& chunks, const std::span<const std::byte> bytes,
                   std::vector<MeshRecord>& meshes, std::vector<MaterialRecord>& materials,
                   SceneExportStats& stats, std::map<std::uint32_t, PrototypeRecord>& prototypes) {
    for (const auto& clump : chunks) {
        if (clump.type != 0x10) continue;
        ++stats.clumps;
        const auto mesh_begin = meshes.size();
        std::optional<std::uint32_t> prototype_id;
        const auto* frame_list = find_child(clump, 0x0E);
        const auto* geometry_list = find_child(clump, 0x1A);
        if (!frame_list || !geometry_list) {
            ++stats.skipped;
            continue;
        }
        const auto frames = decode_frame_list(*frame_list, bytes);
        if (!frames) {
            ++stats.skipped;
            continue;
        }
        std::vector<const Chunk*> geometries;
        for (const auto& child : geometry_list->children)
            if (child.type == 0x0F) geometries.push_back(&child);

        std::vector<FrameTransform> world_frames(frames.value->frames.size());
        std::vector<std::uint8_t> states(frames.value->frames.size());
        auto resolve = [&](auto&& self, const std::size_t index) -> bool {
            if (index >= world_frames.size() || states[index] == 1) return false;
            if (states[index] == 2) return true;
            states[index] = 1;
            const auto& frame = frames.value->frames[index];
            const auto local = frame_transform(frame);
            if (frame.parent >= 0) {
                const auto parent = static_cast<std::size_t>(frame.parent);
                if (!self(self, parent)) return false;
                world_frames[index] = compose(world_frames[parent], local);
            } else
                world_frames[index] = local;
            states[index] = 2;
            return true;
        };

        std::size_t atomic_ordinal{};
        for (const auto& atomic_chunk : clump.children) {
            if (atomic_chunk.type != 0x14) continue;
            if (!prototype_id) {
                const auto* extension = find_child(atomic_chunk, 0x03);
                const auto* pyro = extension ? find_child(*extension, 0xFFFFFF00U) : nullptr;
                const auto metadata = pyro ? decode_pyro_extension(*pyro, 0x14, bytes)
                                           : DecodeResult<PyroExtensionInfo>{};
                if (metadata) {
                    if (const auto index = metadata.value->atomic_object_index())
                        prototype_id = 1000U + *index;
                }
            }
            const auto ordinal = atomic_ordinal++;
            const auto atomic = decode_atomic(atomic_chunk, bytes);
            if (!atomic || atomic.value->frame_index < 0 || atomic.value->geometry_index < 0 ||
                static_cast<std::size_t>(atomic.value->geometry_index) >= geometries.size() ||
                !resolve(resolve, static_cast<std::size_t>(atomic.value->frame_index))) {
                ++stats.skipped;
                continue;
            }
            const auto& geometry_chunk =
                *geometries[static_cast<std::size_t>(atomic.value->geometry_index)];
            const auto geometry = decode_geometry(geometry_chunk, bytes);
            if (!geometry || geometry.value->triangle_layout == TriangleLayout::unknown) {
                ++stats.skipped;
                continue;
            }
            const auto morph = std::find_if(
                geometry.value->morph_targets.begin(), geometry.value->morph_targets.end(),
                [](const MorphTargetInfo& item) { return item.has_vertices; });
            if (morph == geometry.value->morph_targets.end()) {
                ++stats.skipped;
                continue;
            }

            MeshRecord mesh;
            mesh.kind = "clump_atomic";
            mesh.owner_offset = clump.offset;
            mesh.source_offset = geometry_chunk.offset;
            mesh.name = "clump_" + hex_offset(clump.offset) + "_atomic_" + std::to_string(ordinal) +
                        "_geometry_" + hex_offset(geometry_chunk.offset);
            const auto local_materials =
                decode_materials(find_child(geometry_chunk, 0x08), bytes, geometry_chunk.offset,
                                 mesh.name, {190, 190, 190, 255});
            const auto material_base = materials.size();
            materials.insert(materials.end(), local_materials.begin(), local_materials.end());
            mesh.primitives.resize(local_materials.size());
            for (std::size_t i = 0; i < mesh.primitives.size(); ++i)
                mesh.primitives[i].material = material_base + i;

            const auto& transform =
                world_frames[static_cast<std::size_t>(atomic.value->frame_index)];
            mesh.positions.reserve(static_cast<std::size_t>(geometry.value->vertex_count));
            for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
                const auto offset = morph->vertices_offset + static_cast<std::uint64_t>(i) * 12U;
                mesh.positions.push_back(export_point(transform_point(
                    transform, {read_f32(bytes, offset), read_f32(bytes, offset + 4),
                                read_f32(bytes, offset + 8)})));
            }
            if (morph->has_normals) {
                mesh.normals.reserve(mesh.positions.size());
                for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
                    const auto offset = morph->normals_offset + i * 12U;
                    mesh.normals.push_back(transform_direction(
                        transform, {read_f32(bytes, offset), read_f32(bytes, offset + 4),
                                    read_f32(bytes, offset + 8)}));
                }
            }
            for (std::size_t set = 0;
                 set < std::min<std::size_t>(2, geometry.value->texcoord_offsets.size()); ++set) {
                auto& output = set == 0 ? mesh.uv0 : mesh.uv1;
                output.reserve(mesh.positions.size());
                for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
                    const auto offset = geometry.value->texcoord_offsets[set] + i * 8U;
                    output.push_back({read_f32(bytes, offset), read_f32(bytes, offset + 4)});
                }
            }
            for (std::int32_t i = 0; i < geometry.value->triangle_count; ++i) {
                const auto triangle = decode_triangle(*geometry.value, i, bytes);
                if (!triangle) continue;
                const auto slot =
                    std::min<std::size_t>(triangle.value->material, mesh.primitives.size() - 1);
                auto& indices = mesh.primitives[slot].indices;
                for (const auto index : triangle.value->vertices)
                    indices.push_back(index);
            }
            mesh.primitives.erase(
                std::remove_if(mesh.primitives.begin(), mesh.primitives.end(),
                               [](const PrimitiveRecord& item) { return item.indices.empty(); }),
                mesh.primitives.end());
            if (mesh.primitives.empty()) {
                ++stats.skipped;
                continue;
            }
            stats.vertices += mesh.positions.size();
            for (const auto& primitive : mesh.primitives)
                stats.triangles += primitive.indices.size() / 3U;
            ++stats.atomic_instances;
            meshes.push_back(std::move(mesh));
        }
        if (prototype_id && !prototypes.contains(*prototype_id)) {
            PrototypeRecord prototype;
            const auto root = std::find_if(frames.value->frames.begin(), frames.value->frames.end(),
                                           [](const FrameInfo& frame) { return frame.parent < 0; });
            if (root != frames.value->frames.end()) {
                const auto index = static_cast<std::size_t>(root - frames.value->frames.begin());
                if (resolve(resolve, index)) prototype.original_root = world_frames[index];
            }
            for (auto index = mesh_begin; index < meshes.size(); ++index)
                prototype.meshes.push_back(index);
            prototypes.emplace(*prototype_id, std::move(prototype));
        }
    }
}

FrameTransform instance_transform(const SceneInstance& instance) {
    return {{{instance.rotation[0], instance.rotation[3], instance.rotation[6],
              instance.rotation[1], instance.rotation[4], instance.rotation[7],
              instance.rotation[2], instance.rotation[5], instance.rotation[8]}},
            instance.position};
}

void append_instances(const std::span<const SceneInstance> instances,
                      const std::map<std::uint32_t, PrototypeRecord>& prototypes,
                      std::vector<MeshRecord>& meshes, SceneExportStats& stats) {
    for (const auto& instance : instances) {
        const auto found = prototypes.find(instance.prototype_id);
        if (found == prototypes.end() || found->second.meshes.empty()) {
            ++stats.unresolved_instances;
            continue;
        }
        auto transform = instance_transform(instance);
        transform.position = export_point(transform.position);
        auto original_root = found->second.original_root;
        original_root.position = export_point(original_root.position);
        std::vector<MeshRecord> copies;
        copies.reserve(found->second.meshes.size());
        for (const auto prototype_index : found->second.meshes) {
            MeshRecord copy = meshes[prototype_index];
            copy.kind = "csf_instance";
            copy.owner_offset = instance.offset;
            const auto prototype_name = instance.prototype_name.empty()
                                            ? "prototype_" + std::to_string(instance.prototype_id)
                                            : instance.prototype_name;
            copy.name = prototype_name + "_instance_" + std::to_string(instance.instance_id) + "_" +
                        hex_offset(instance.offset);
            for (auto& position : copy.positions) {
                position =
                    transform_point(transform, inverse_transform_point(original_root, position));
            }
            for (auto& normal : copy.normals) {
                normal = transform_direction(transform,
                                             inverse_transform_direction(original_root, normal));
            }
            stats.vertices += copy.positions.size();
            for (const auto& primitive : copy.primitives)
                stats.triangles += primitive.indices.size() / 3U;
            ++stats.atomic_instances;
            copies.push_back(std::move(copy));
        }
        meshes.insert(meshes.end(), std::make_move_iterator(copies.begin()),
                      std::make_move_iterator(copies.end()));
        ++stats.custom_instances;
    }
}

void append_world(const std::vector<Chunk>& chunks, const std::span<const std::byte> bytes,
                  std::vector<MeshRecord>& meshes, std::vector<MaterialRecord>& materials,
                  SceneExportStats& stats, const bool preserve_valid_faces) {
    const auto world_it = std::find_if(chunks.begin(), chunks.end(),
                                       [](const Chunk& chunk) { return chunk.type == 0x0B; });
    if (world_it == chunks.end()) return;
    const auto& world_chunk = *world_it;
    const auto recovered = recover_world(world_chunk, bytes);
    if (recovered.sectors.empty()) {
        ++stats.skipped;
        return;
    }
    stats.recovered_world_sectors += recovered.sectors.size();
    stats.recovered_world_vertices += static_cast<std::uint64_t>(recovered.recovered_vertices);
    stats.recovered_world_triangles += static_cast<std::uint64_t>(recovered.recovered_triangles);
    const auto world_materials =
        decode_materials(find_child(world_chunk, 0x08), bytes, world_chunk.offset,
                         "world_" + hex_offset(world_chunk.offset), {155, 158, 150, 255});
    const auto material_base = materials.size();
    materials.insert(materials.end(), world_materials.begin(), world_materials.end());
    for (std::size_t sector_index = 0; sector_index < recovered.sectors.size(); ++sector_index) {
        const auto& sector = recovered.sectors[sector_index];
        const auto triangle_count = sector.triangle_count;
        const auto vertex_count = sector.vertex_count;
        const auto uv_sets = sector.texcoord_sets;
        MeshRecord mesh;
        mesh.kind = "world_sector";
        mesh.owner_offset = world_chunk.offset;
        mesh.source_offset = sector.chunk_offset;
        mesh.world_index = 0;
        mesh.sector_index = sector_index;
        mesh.name = "world_" + hex_offset(world_chunk.offset) + "_sector_" +
                    hex_offset(sector.chunk_offset);
        mesh.positions.reserve(static_cast<std::size_t>(vertex_count));
        for (std::int32_t i = 0; i < vertex_count; ++i) {
            const auto offset = sector.vertices_offset + static_cast<std::uint64_t>(i) * 12U;
            mesh.positions.push_back(
                export_point({read_f32(bytes, offset), read_f32(bytes, offset + 4),
                              read_f32(bytes, offset + 8)}));
        }
        if (sector.normals_offset != 0) {
            mesh.normals.reserve(mesh.positions.size());
            for (std::int32_t i = 0; i < vertex_count; ++i) {
                const auto offset = sector.normals_offset + static_cast<std::uint64_t>(i) * 4U;
                const auto component = [&](const std::uint64_t at) {
                    return static_cast<float>(static_cast<std::int8_t>(
                        std::to_integer<std::uint8_t>(bytes[static_cast<std::size_t>(at)])));
                };
                mesh.normals.push_back(
                    normalize({component(offset), component(offset + 1U), component(offset + 2U)}));
            }
        }
        for (std::size_t set = 0; set < std::min<std::uint32_t>(2, uv_sets); ++set) {
            auto& output = set == 0 ? mesh.uv0 : mesh.uv1;
            output.reserve(mesh.positions.size());
            const auto set_offset = sector.texcoord_offsets[set];
            for (std::int32_t i = 0; i < vertex_count; ++i) {
                const auto offset = set_offset + static_cast<std::uint64_t>(i) * 8U;
                output.push_back({read_f32(bytes, offset), read_f32(bytes, offset + 4)});
            }
        }
        struct WorldTriangle {
            std::array<std::uint16_t, 3> indices{};
            std::size_t material{};
        };
        using PositionBits = std::array<std::uint32_t, 3>;
        using TrianglePositionKey = std::array<PositionBits, 3>;
        std::vector<WorldTriangle> triangles;
        std::map<TrianglePositionKey, std::size_t> triangle_by_position;
        for (std::int32_t i = 0; i < triangle_count; ++i) {
            const auto decoded_triangle = decode_recovered_world_triangle(sector, i, bytes);
            if (!decoded_triangle) {
                ++stats.skipped;
                continue;
            }
            const auto indices = decoded_triangle.value->vertices;
            if (indices[0] >= mesh.positions.size() || indices[1] >= mesh.positions.size() ||
                indices[2] >= mesh.positions.size()) {
                ++stats.skipped;
                continue;
            }
            const auto source_a = decode_recovered_world_vertex(sector, indices[0], bytes);
            const auto source_b = decode_recovered_world_vertex(sector, indices[1], bytes);
            const auto source_c = decode_recovered_world_vertex(sector, indices[2], bytes);
            if (!source_a || !source_b || !source_c) {
                ++stats.skipped;
                continue;
            }
            const auto& a = *source_a.value;
            const auto& b = *source_b.value;
            const auto& c = *source_c.value;
            const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
            const Vec3 ac{c.x - a.x, c.y - a.y, c.z - a.z};
            const Vec3 cross{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z,
                             ab.x * ac.y - ab.y * ac.x};
            if (cross.x * cross.x + cross.y * cross.y + cross.z * cross.z <= 1.0e-20F) {
                ++stats.skipped;
                continue;
            }
            const auto resolved_material = static_cast<std::int64_t>(sector.material_window_base) +
                                           decoded_triangle.value->material;
            if (resolved_material < 0 ||
                resolved_material >= static_cast<std::int64_t>(world_materials.size())) {
                ++stats.skipped;
                continue;
            }
            const auto material = static_cast<std::size_t>(resolved_material);
            TrianglePositionKey key{};
            for (std::size_t vertex = 0; vertex < 3; ++vertex) {
                const auto& position = mesh.positions[indices[vertex]];
                key[vertex] = {std::bit_cast<std::uint32_t>(position.x),
                               std::bit_cast<std::uint32_t>(position.y),
                               std::bit_cast<std::uint32_t>(position.z)};
            }
            std::sort(key.begin(), key.end());
            const WorldTriangle triangle{indices, static_cast<std::size_t>(material)};
            if (const auto duplicate = triangle_by_position.find(key);
                duplicate != triangle_by_position.end() && !preserve_valid_faces) {
                // RenderWare's less-or-equal depth test makes the later coplanar
                // face win. Retain that face explicitly because glTF material
                // grouping otherwise reorders both faces and causes z-fighting.
                triangles[duplicate->second] = triangle;
            } else {
                triangle_by_position.emplace(key, triangles.size());
                triangles.push_back(triangle);
            }
        }
        std::map<std::size_t, std::vector<std::uint32_t>> grouped;
        if (mesh.normals.empty()) {
            mesh.normals.assign(mesh.positions.size(), {});
            for (const auto& triangle : triangles) {
                const auto& a = mesh.positions[triangle.indices[0]];
                const auto& b = mesh.positions[triangle.indices[1]];
                const auto& c = mesh.positions[triangle.indices[2]];
                const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z}, ac{c.x - a.x, c.y - a.y, c.z - a.z};
                const Vec3 face{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z,
                                ab.x * ac.y - ab.y * ac.x};
                for (const auto index : triangle.indices) {
                    mesh.normals[index].x += face.x;
                    mesh.normals[index].y += face.y;
                    mesh.normals[index].z += face.z;
                }
            }
            for (auto& normal : mesh.normals)
                normal = normalize(normal);
        }
        for (const auto& triangle : triangles) {
            auto& output = grouped[triangle.material];
            output.insert(output.end(), triangle.indices.begin(), triangle.indices.end());
        }
        for (auto& [material, indices] : grouped)
            mesh.primitives.push_back({material_base + material, std::move(indices)});
        if (!mesh.primitives.empty()) {
            stats.vertices += mesh.positions.size();
            for (const auto& primitive : mesh.primitives)
                stats.triangles += primitive.indices.size() / 3U;
            ++stats.world_sectors;
            meshes.push_back(std::move(mesh));
        }
    }
}

template <typename T>
std::pair<std::size_t, std::uint64_t> append_binary(std::vector<std::byte>& binary,
                                                    const std::vector<T>& values) {
    while (binary.size() % 4U)
        binary.push_back(std::byte{});
    const auto offset = binary.size();
    const auto* begin = reinterpret_cast<const std::byte*>(values.data());
    binary.insert(binary.end(), begin, begin + values.size() * sizeof(T));
    return {offset, values.size() * sizeof(T)};
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create " + path.string());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) throw std::runtime_error("Failed while writing " + path.string());
}

// Writes the entries of "materials" (the caller has opened the array) and,
// when textures resolve, closes it and writes "images" and "textures", whose
// arrays the caller closes like any other: texture i samples image i.
void write_materials(std::ostringstream& json, const std::vector<MaterialRecord>& materials,
                     const SceneTextureResolver& resolve_texture) {
    // Images and textures in first-use order, one per distinct URI.
    std::vector<std::string> image_uris;
    std::map<std::string, std::optional<SceneTexture>> resolved;
    const auto texture_of = [&](const std::string& name) -> const std::optional<SceneTexture>& {
        static const std::optional<SceneTexture> none;
        if (!resolve_texture || name.empty()) return none;
        auto [it, added] = resolved.try_emplace(name);
        if (added) it->second = resolve_texture(name);
        return it->second;
    };
    const auto image_index = [&](const std::string& uri) {
        const auto found = std::ranges::find(image_uris, uri);
        if (found != image_uris.end()) return static_cast<std::size_t>(found - image_uris.begin());
        image_uris.push_back(uri);
        return image_uris.size() - 1;
    };
    for (std::size_t i = 0; i < materials.size(); ++i) {
        const auto& material = materials[i];
        const auto& base = texture_of(material.base_texture);
        const auto& lightmap = texture_of(material.lightmap_texture);
        json << "    {\"name\": \"" << json_escape(material.name) << "\", \"doubleSided\": true, ";
        if (base && base->alpha == SceneTexture::Alpha::mask)
            json << "\"alphaMode\": \"MASK\", \"alphaCutoff\": 0.5, ";
        else if (base && base->alpha == SceneTexture::Alpha::blend)
            json << "\"alphaMode\": \"BLEND\", ";
        json << "\"pbrMetallicRoughness\": {\"baseColorFactor\": [" << material.color[0] / 255.0F
             << ',' << material.color[1] / 255.0F << ',' << material.color[2] / 255.0F << ','
             << material.color[3] / 255.0F << "], ";
        if (base) json << "\"baseColorTexture\": {\"index\": " << image_index(base->uri) << ", \"texCoord\": 0}, ";
        json << "\"metallicFactor\": 0, \"roughnessFactor\": 1}, \"extras\": {"
             << "\"rws_owner_offset\": \"" << hex_offset(material.owner_offset)
             << "\", \"rws_material_slot\": " << material.slot << ", \"rws_surface_name\": \""
             << json_escape(material.surface_name) << "\""
             << ", \"rws_surface_id\": ";
        if (material.surface_id)
            json << *material.surface_id;
        else
            json << "null";
        json << ", \"rws_base_texture\": \"" << json_escape(material.base_texture)
             << "\", \"rws_lightmap_texture\": \"" << json_escape(material.lightmap_texture) << '"';
        if (lightmap) json << ", \"lightmap_uri\": \"" << json_escape(lightmap->uri) << '"';
        json << "}}" << (i + 1 == materials.size() ? "\n" : ",\n");
    }
    if (!image_uris.empty()) {
        // Texture i samples image i with the default sampler (repeat, mipmaps).
        json << "  ],\n  \"images\": [\n";
        for (std::size_t i = 0; i < image_uris.size(); ++i)
            json << "    {\"uri\": \"" << json_escape(image_uris[i]) << "\"}" << (i + 1 == image_uris.size() ? "\n" : ",\n");
        json << "  ],\n  \"textures\": [\n";
        for (std::size_t i = 0; i < image_uris.size(); ++i)
            json << "    {\"source\": " << i << '}' << (i + 1 == image_uris.size() ? "\n" : ",\n");
    }
}

} // namespace

static SceneExportStats export_gltf(const std::vector<Chunk>& chunks,
                                    const std::span<const SceneInstance> instances,
                                    const std::span<const std::byte> bytes,
                                    const std::filesystem::path& requested_path,
                                    const bool preserve_world_faces,
                                    const SceneTextureResolver& resolve_texture = {}) {
    auto output_path = requested_path;
    if (output_path.extension() != ".gltf") output_path.replace_extension(".gltf");
    if (output_path.has_parent_path())
        std::filesystem::create_directories(output_path.parent_path());
    auto bin_path = output_path;
    bin_path.replace_extension(".bin");
    auto manifest_path = output_path;
    manifest_path.replace_extension(".manifest.json");

    SceneExportStats stats;
    std::vector<MeshRecord> meshes;
    std::vector<MaterialRecord> materials;
    std::map<std::uint32_t, PrototypeRecord> prototypes;
    append_clumps(chunks, bytes, meshes, materials, stats, prototypes);
    append_instances(instances, prototypes, meshes, stats);
    append_world(chunks, bytes, meshes, materials, stats, preserve_world_faces);
    stats.materials = materials.size();
    if (meshes.empty()) throw std::runtime_error("No exportable Clump or World geometry was found");

    std::vector<std::byte> binary;
    std::vector<BufferView> views;
    std::vector<Accessor> accessors;
    std::vector<GltfMesh> gltf_meshes;
    for (const auto& mesh : meshes) {
        const auto [position_offset, position_length] = append_binary(binary, mesh.positions);
        views.push_back({position_offset, position_length, 34962});
        std::array<float, 3> minimum{std::numeric_limits<float>::max(),
                                     std::numeric_limits<float>::max(),
                                     std::numeric_limits<float>::max()};
        std::array<float, 3> maximum{-minimum[0], -minimum[1], -minimum[2]};
        for (const auto& item : mesh.positions) {
            minimum[0] = std::min(minimum[0], item.x);
            minimum[1] = std::min(minimum[1], item.y);
            minimum[2] = std::min(minimum[2], item.z);
            maximum[0] = std::max(maximum[0], item.x);
            maximum[1] = std::max(maximum[1], item.y);
            maximum[2] = std::max(maximum[2], item.z);
        }
        const auto position_accessor = accessors.size();
        accessors.push_back(
            {views.size() - 1, 5126, mesh.positions.size(), "VEC3", minimum, maximum, true});
        std::size_t normal_accessor{}, uv0_accessor{}, uv1_accessor{};
        if (mesh.normals.size() == mesh.positions.size()) {
            const auto [offset, length] = append_binary(binary, mesh.normals);
            views.push_back({offset, length, 34962});
            normal_accessor = accessors.size();
            accessors.push_back({views.size() - 1, 5126, mesh.normals.size(), "VEC3"});
        }
        if (!mesh.uv0.empty()) {
            const auto [offset, length] = append_binary(binary, mesh.uv0);
            views.push_back({offset, length, 34962});
            uv0_accessor = accessors.size();
            accessors.push_back({views.size() - 1, 5126, mesh.uv0.size(), "VEC2"});
        }
        if (!mesh.uv1.empty()) {
            const auto [offset, length] = append_binary(binary, mesh.uv1);
            views.push_back({offset, length, 34962});
            uv1_accessor = accessors.size();
            accessors.push_back({views.size() - 1, 5126, mesh.uv1.size(), "VEC2"});
        }
        GltfMesh output_mesh{mesh.name};
        for (const auto& primitive : mesh.primitives) {
            const auto [offset, length] = append_binary(binary, primitive.indices);
            views.push_back({offset, length, 34963});
            const auto index_accessor = accessors.size();
            accessors.push_back({views.size() - 1, 5125, primitive.indices.size(), "SCALAR"});
            output_mesh.primitives.push_back({position_accessor, normal_accessor, uv0_accessor,
                                              uv1_accessor, index_accessor, primitive.material,
                                              mesh.normals.size() == mesh.positions.size(),
                                              !mesh.uv0.empty(), !mesh.uv1.empty()});
        }
        gltf_meshes.push_back(std::move(output_mesh));
    }

    std::ofstream bin_output(bin_path, std::ios::binary | std::ios::trunc);
    if (!bin_output) throw std::runtime_error("Cannot create " + bin_path.string());
    bin_output.write(reinterpret_cast<const char*>(binary.data()),
                     static_cast<std::streamsize>(binary.size()));
    if (!bin_output) throw std::runtime_error("Failed while writing " + bin_path.string());

    std::ostringstream json;
    json << std::setprecision(9)
         << "{\n  \"asset\": {\"version\": \"2.0\", \"generator\": \"CSF Mission Editor\", "
         << "\"extras\": {\"rws_units_per_meter\": 100}},\n"
         << "  \"scene\": 0,\n  \"scenes\": [{\"name\": \"RWS Scene\", \"nodes\": [";
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        if (i) json << ',';
        json << i;
    }
    json << "]}],\n  \"nodes\": [\n";
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const auto& mesh = meshes[i];
        json << "    {\"name\": \"" << json_escape(mesh.name) << "\", \"mesh\": " << i
             << ", \"extras\": {\"rws_kind\": \"" << mesh.kind << "\", \"rws_owner_offset\": \""
             << hex_offset(mesh.owner_offset) << "\", \"rws_source_offset\": \""
             << hex_offset(mesh.source_offset) << "\"";
        if (mesh.world_index) json << ", \"rws_world_index\": " << *mesh.world_index;
        if (mesh.sector_index) json << ", \"rws_sector_index\": " << *mesh.sector_index;
        json << "}}" << (i + 1 == meshes.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"meshes\": [\n";
    for (std::size_t i = 0; i < gltf_meshes.size(); ++i) {
        const auto& mesh = gltf_meshes[i];
        json << "    {\"name\": \"" << json_escape(mesh.name) << "\", \"primitives\": [";
        for (std::size_t p = 0; p < mesh.primitives.size(); ++p) {
            const auto& primitive = mesh.primitives[p];
            if (p) json << ',';
            json << "{\"attributes\": {\"POSITION\": " << primitive.position;
            if (primitive.has_normal) json << ", \"NORMAL\": " << primitive.normal;
            if (primitive.has_uv0) json << ", \"TEXCOORD_0\": " << primitive.uv0;
            if (primitive.has_uv1) json << ", \"TEXCOORD_1\": " << primitive.uv1;
            json << "}, \"indices\": " << primitive.indices
                 << ", \"material\": " << primitive.material << ", \"mode\": 4}";
        }
        json << "]}" << (i + 1 == gltf_meshes.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"materials\": [\n";
    write_materials(json, materials, resolve_texture);
    json << "  ],\n  \"buffers\": [{\"uri\": \"" << json_escape(bin_path.filename().string())
         << "\", \"byteLength\": " << binary.size() << "}],\n  \"bufferViews\": [\n";
    for (std::size_t i = 0; i < views.size(); ++i) {
        const auto& view = views[i];
        json << "    {\"buffer\": 0, \"byteOffset\": " << view.offset
             << ", \"byteLength\": " << view.length << ", \"target\": " << view.target << '}'
             << (i + 1 == views.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"accessors\": [\n";
    for (std::size_t i = 0; i < accessors.size(); ++i) {
        const auto& accessor = accessors[i];
        json << "    {\"bufferView\": " << accessor.view
             << ", \"byteOffset\": 0, \"componentType\": " << accessor.component_type
             << ", \"count\": " << accessor.count << ", \"type\": \"" << accessor.type << '\"';
        if (accessor.has_bounds)
            json << ", \"min\": [" << accessor.minimum[0] << ',' << accessor.minimum[1] << ','
                 << accessor.minimum[2] << "], \"max\": [" << accessor.maximum[0] << ','
                 << accessor.maximum[1] << ',' << accessor.maximum[2] << ']';
        json << '}' << (i + 1 == accessors.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    write_text(output_path, json.str());

    std::ostringstream manifest;
    manifest << "{\n  \"format\": \"rws-man-scene-manifest-v1\",\n  \"gltf\": \""
             << json_escape(output_path.filename().string())
             << "\",\n  \"coordinate_system\": \"Y-up; Clump transforms baked\",\n"
             << "  \"unit_conversion\": {\"rws_units_per_meter\": 100, "
                "\"gltf_meters_per_rws_unit\": 0.01},\n"
             << "  \"uv_sets\": {\"TEXCOORD_0\": \"base texture\", \"TEXCOORD_1\": \"lightmap\"},\n"
             << "  \"objects\": [\n";
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const auto& mesh = meshes[i];
        manifest << "    {\"node\": " << i << ", \"name\": \"" << json_escape(mesh.name)
                 << "\", \"kind\": \"" << mesh.kind << "\", \"owner_offset\": \""
                 << hex_offset(mesh.owner_offset) << "\", \"source_offset\": \""
                 << hex_offset(mesh.source_offset) << "\", \"vertices\": " << mesh.positions.size()
                 << ", \"triangles\": ";
        std::size_t triangles{};
        for (const auto& primitive : mesh.primitives)
            triangles += primitive.indices.size() / 3U;
        manifest << triangles;
        if (mesh.world_index) manifest << ", \"world_index\": " << *mesh.world_index;
        if (mesh.sector_index) manifest << ", \"sector_index\": " << *mesh.sector_index;
        manifest << '}' << (i + 1 == meshes.size() ? "\n" : ",\n");
    }
    manifest << "  ],\n  \"materials\": [\n";
    for (std::size_t i = 0; i < materials.size(); ++i) {
        const auto& material = materials[i];
        manifest << "    {\"gltf_material\": " << i << ", \"name\": \""
                 << json_escape(material.name) << "\", \"owner_offset\": \""
                 << hex_offset(material.owner_offset) << "\", \"slot\": " << material.slot
                 << ", \"surface_name\": \"" << json_escape(material.surface_name)
                 << "\", \"surface_id\": ";
        if (material.surface_id)
            manifest << *material.surface_id;
        else
            manifest << "null";
        manifest << ", \"base_texture\": \"" << json_escape(material.base_texture)
                 << "\", \"lightmap_texture\": \"" << json_escape(material.lightmap_texture)
                 << "\"}" << (i + 1 == materials.size() ? "\n" : ",\n");
    }
    manifest << "  ]\n}\n";
    write_text(manifest_path, manifest.str());
    return stats;
}

SceneExportStats export_scene_gltf(const std::vector<Chunk>& chunks,
                                   const std::span<const SceneInstance> instances,
                                   const std::span<const std::byte> bytes,
                                   const std::filesystem::path& output_path,
                                   const SceneTextureResolver& textures) {
    return export_gltf(chunks, instances, bytes, output_path, false, textures);
}

SceneExportStats export_clump_gltf(const Chunk& clump, const std::span<const std::byte> bytes,
                                   const std::filesystem::path& output_path,
                                   const SceneTextureResolver& textures) {
    if (clump.type != 0x10) throw std::runtime_error("Selected chunk is not a Clump");
    // Chunk objects only describe offsets and hierarchy; payload bytes remain
    // in the shared span, so this temporary one-root document is inexpensive.
    return export_scene_gltf(std::vector<Chunk>{clump}, {}, bytes, output_path, textures);
}

SceneExportStats export_collision_gltf(const std::vector<Chunk>& chunks,
                                       const std::span<const std::byte> bytes,
                                       const std::filesystem::path& output_path) {
    std::vector<Chunk> worlds;
    for (const auto& chunk : chunks)
        if (chunk.type == 0x0B) worlds.push_back(chunk);
    if (worlds.empty()) throw std::runtime_error("No World collision geometry was found");
    return export_gltf(worlds, {}, bytes, output_path, true);
}

std::optional<std::uint32_t> clump_prototype_id(const Chunk& clump,
                                                const std::span<const std::byte> bytes) {
    for (const auto& atomic : clump.children) {
        if (atomic.type != 0x14) continue;
        const auto* extension = find_child(atomic, 0x03);
        const auto* pyro = extension ? find_child(*extension, 0xFFFFFF00U) : nullptr;
        if (!pyro) continue;
        const auto metadata = decode_pyro_extension(*pyro, 0x14, bytes);
        if (metadata)
            if (const auto index = metadata.value->atomic_object_index()) return 1000U + *index;
    }
    return std::nullopt;
}

std::vector<std::array<Vec3, 3>> placed_clump_triangles(const Chunk& clump,
                                                         const std::span<const std::byte> bytes,
                                                         const SceneInstance& instance) {
    std::vector<MeshRecord> meshes;
    std::vector<MaterialRecord> materials;
    SceneExportStats stats;
    std::map<std::uint32_t, PrototypeRecord> prototypes;
    append_clumps({clump}, bytes, meshes, materials, stats, prototypes);
    // The export pipeline works in scaled units; undo the scale at the end.
    auto placement = instance_transform(instance);
    placement.position = export_point(placement.position);
    FrameTransform root;
    if (!prototypes.empty()) {
        root = prototypes.begin()->second.original_root;
        root.position = export_point(root.position);
    }
    std::vector<std::array<Vec3, 3>> triangles;
    for (const auto& mesh : meshes)
        for (const auto& primitive : mesh.primitives)
            for (std::size_t i = 0; i + 2 < primitive.indices.size(); i += 3) {
                std::array<Vec3, 3> triangle;
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto point = transform_point(
                        placement, inverse_transform_point(root, mesh.positions[primitive.indices[i + c]]));
                    triangle[c] = {point.x / scene_scale, point.y / scene_scale, point.z / scene_scale};
                }
                triangles.push_back(triangle);
            }
    return triangles;
}


namespace {

using Matrix = std::array<float, 16>;  // column-major, as RenderWare's and glTF's

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float sum{};
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = sum;
        }
    return r;
}

// The inverse of a rotation-and-translation matrix.
Matrix rigid_inverse(const Matrix& m) {
    Matrix r{};
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) r[c * 4 + row] = m[row * 4 + c];
    for (int row = 0; row < 3; ++row)
        r[12 + row] = -(r[row] * m[12] + r[4 + row] * m[13] + r[8 + row] * m[14]);
    r[15] = 1;
    return r;
}

// Rotation (x, y, z, w) and translation of a matrix, its axes normalised
// (Biped bind matrices carry float noise in their scale).
std::pair<std::array<float, 4>, Vec3> decompose(const Matrix& m) {
    std::array<float, 9> r{};
    for (int c = 0; c < 3; ++c) {
        const float length = std::sqrt(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]);
        for (int row = 0; row < 3; ++row) r[c * 3 + row] = length > 0 ? m[c * 4 + row] / length : 0.0F;
    }
    // r is column-major 3x3: element (row, column) = r[column * 3 + row].
    const auto at = [&](const int row, const int column) { return r[column * 3 + row]; };
    std::array<float, 4> q{};
    const float trace = at(0, 0) + at(1, 1) + at(2, 2);
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0F) * 2;
        q = {(at(2, 1) - at(1, 2)) / s, (at(0, 2) - at(2, 0)) / s, (at(1, 0) - at(0, 1)) / s, 0.25F * s};
    } else if (at(0, 0) > at(1, 1) && at(0, 0) > at(2, 2)) {
        const float s = std::sqrt(1.0F + at(0, 0) - at(1, 1) - at(2, 2)) * 2;
        q = {0.25F * s, (at(0, 1) + at(1, 0)) / s, (at(0, 2) + at(2, 0)) / s, (at(2, 1) - at(1, 2)) / s};
    } else if (at(1, 1) > at(2, 2)) {
        const float s = std::sqrt(1.0F + at(1, 1) - at(0, 0) - at(2, 2)) * 2;
        q = {(at(0, 1) + at(1, 0)) / s, 0.25F * s, (at(1, 2) + at(2, 1)) / s, (at(0, 2) - at(2, 0)) / s};
    } else {
        const float s = std::sqrt(1.0F + at(2, 2) - at(0, 0) - at(1, 1)) * 2;
        q = {(at(0, 2) + at(2, 0)) / s, (at(1, 2) + at(2, 1)) / s, 0.25F * s, (at(1, 0) - at(0, 1)) / s};
    }
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (auto& value : q) value /= length;
    return {q, {m[12] * scene_scale, m[13] * scene_scale, m[14] * scene_scale}};
}

// Local matrices under `parents` (frame -> parent frame, -1 for a root) from world ones.
std::vector<Matrix> local_matrices(const std::vector<std::int32_t>& parents, const std::vector<Matrix>& world) {
    std::vector<Matrix> local(world.size());
    for (std::size_t i = 0; i < world.size(); ++i) {
        const auto parent = parents[i];
        local[i] = parent >= 0 && static_cast<std::size_t>(parent) < world.size()
                       ? multiply(rigid_inverse(world[static_cast<std::size_t>(parent)]), world[i])
                       : world[i];
    }
    return local;
}

struct CharacterMesh {
    MeshRecord mesh;
    std::optional<std::size_t> frame;  // rigid: hangs under this frame's node
    std::vector<std::array<std::uint8_t, 4>> joints;
    std::vector<std::array<float, 4>> weights;
};

} // namespace

CharacterExportStats export_character_gltf(const Chunk& clump, const std::span<const std::byte> bytes,
                                           const std::filesystem::path& output_path,
                                           std::vector<CharacterClip> clips,
                                           const SceneTextureResolver& textures,
                                           const JointNamer& joint_name, const JointParent& joint_parent,
                                           const bool include_meshes) {
    if (clump.type != 0x10) throw std::runtime_error("Selected chunk is not a Clump");
    const auto* frame_list = find_child(clump, 0x0E);
    const auto* geometry_list = find_child(clump, 0x1A);
    if (!frame_list || !geometry_list) throw std::runtime_error("Clump has no Frame List or Geometry List");
    const auto frames = decode_frame_list(*frame_list, bytes);
    const auto binding = decode_hanim_binding(*frame_list, bytes);
    if (!frames || !binding) throw std::runtime_error("Clump has no decodable HAnim hierarchy");
    const auto frame_count = frames.value->frames.size();
    std::vector<const Chunk*> geometries;
    for (const auto& child : geometry_list->children)
        if (child.type == 0x0F) geometries.push_back(&child);
    const auto find_skin = [](const Chunk& geometry) -> const Chunk* {
        for (const auto& child : geometry.children)
            if (child.type == 0x03)
                if (const auto* skin = find_child(child, 0x116)) return skin;
        return find_child(geometry, 0x116);
    };

    // Static world frames from the Frame List, for the skinned Atomic's bind.
    std::vector<Matrix> static_world(frame_count);
    for (std::size_t i = 0; i < frame_count; ++i) {
        const auto& f = frames.value->frames[i];
        const Matrix local{f.rotation[0], f.rotation[1], f.rotation[2], 0, f.rotation[3], f.rotation[4],
                           f.rotation[5], 0,  f.rotation[6], f.rotation[7], f.rotation[8], 0,
                           f.position.x,  f.position.y,  f.position.z,  1};
        // Frame Lists store parents before children.
        static_world[i] = f.parent >= 0 && static_cast<std::size_t>(f.parent) < i
                              ? multiply(static_world[static_cast<std::size_t>(f.parent)], local)
                              : local;
    }

    // The skinned Atomic: its Skin's inverse binds give the bind pose.
    std::optional<SkinBindPose> bind;
    std::vector<Matrix> inverse_bind;
    std::vector<std::size_t> joint_frames;  // skin bone -> frame
    std::vector<CharacterMesh> meshes;
    std::vector<MaterialRecord> materials;
    for (const auto& atomic_chunk : clump.children) {
        if (atomic_chunk.type != 0x14) continue;
        const auto atomic = decode_atomic(atomic_chunk, bytes);
        if (!atomic || atomic.value->frame_index < 0 || atomic.value->geometry_index < 0 ||
            static_cast<std::size_t>(atomic.value->frame_index) >= frame_count ||
            static_cast<std::size_t>(atomic.value->geometry_index) >= geometries.size())
            continue;
        const auto& geometry_chunk = *geometries[static_cast<std::size_t>(atomic.value->geometry_index)];
        const auto geometry = decode_geometry(geometry_chunk, bytes);
        if (!geometry) continue;
        const auto* skin_chunk = find_skin(geometry_chunk);
        const auto skin = skin_chunk ? decode_skin(*skin_chunk, geometry.value->vertex_count, bytes) : DecodeResult<SkinInfo>{};
        const auto frame = static_cast<std::size_t>(atomic.value->frame_index);
        if (skin && !bind) {
            inverse_bind = decode_inverse_bind_matrices(*skin.value, bytes);
            const auto recovered = recover_skin_bind_pose(*frames.value, *binding.value, inverse_bind, static_world[frame]);
            if (!recovered) throw std::runtime_error("Skin bind pose: " + recovered.error);
            bind = *recovered.value;
            for (std::size_t b = 0; b < inverse_bind.size(); ++b) {
                if (b >= binding.value->matrix_to_frame.size() || binding.value->matrix_to_frame[b] < 0)
                    throw std::runtime_error("Skin bone " + std::to_string(b) + " has no frame");
                joint_frames.push_back(static_cast<std::size_t>(binding.value->matrix_to_frame[b]));
            }
        }
        if (!include_meshes || geometry.value->triangle_layout == TriangleLayout::unknown) continue;
        const auto morph = std::ranges::find_if(geometry.value->morph_targets, [](const auto& m) { return m.has_vertices; });
        if (morph == geometry.value->morph_targets.end()) continue;
        CharacterMesh item;
        auto& mesh = item.mesh;
        mesh.name = "geometry_" + std::to_string(atomic.value->geometry_index);
        const auto local_materials =
            decode_materials(find_child(geometry_chunk, 0x08), bytes, geometry_chunk.offset, mesh.name, {190, 190, 190, 255});
        const auto material_base = materials.size();
        materials.insert(materials.end(), local_materials.begin(), local_materials.end());
        mesh.primitives.resize(local_materials.size());
        for (std::size_t i = 0; i < mesh.primitives.size(); ++i) mesh.primitives[i].material = material_base + i;
        for (std::int32_t i = 0; i < geometry.value->vertex_count; ++i) {
            const auto offset = morph->vertices_offset + static_cast<std::uint64_t>(i) * 12U;
            mesh.positions.push_back(export_point({read_f32(bytes, offset), read_f32(bytes, offset + 4), read_f32(bytes, offset + 8)}));
            if (morph->has_normals) {
                const auto n = morph->normals_offset + static_cast<std::uint64_t>(i) * 12U;
                mesh.normals.push_back(normalize({read_f32(bytes, n), read_f32(bytes, n + 4), read_f32(bytes, n + 8)}));
            }
            if (!geometry.value->texcoord_offsets.empty()) {
                const auto uv = geometry.value->texcoord_offsets[0] + static_cast<std::uint64_t>(i) * 8U;
                mesh.uv0.push_back({read_f32(bytes, uv), read_f32(bytes, uv + 4)});
            }
            if (skin) {
                std::array<std::uint8_t, 4> joints{};
                std::array<float, 4> weights{};
                float total{};
                for (std::size_t j = 0; j < 4; ++j) {
                    joints[j] = std::to_integer<std::uint8_t>(bytes[skin.value->vertex_indices_offset + static_cast<std::uint64_t>(i) * 4 + j]);
                    weights[j] = read_f32(bytes, skin.value->vertex_weights_offset + static_cast<std::uint64_t>(i) * 16 + j * 4);
                    if (weights[j] <= 0) weights[j] = 0, joints[j] = 0;
                    total += weights[j];
                }
                if (total <= 0) weights = {1, 0, 0, 0};
                else for (auto& w : weights) w /= total;
                item.joints.push_back(joints);
                item.weights.push_back(weights);
            }
        }
        for (std::int32_t i = 0; i < geometry.value->triangle_count; ++i) {
            const auto triangle = decode_triangle(*geometry.value, i, bytes);
            if (!triangle) continue;
            auto& indices = mesh.primitives[std::min<std::size_t>(triangle.value->material, mesh.primitives.size() - 1)].indices;
            for (const auto index : triangle.value->vertices) indices.push_back(index);
        }
        std::erase_if(mesh.primitives, [](const PrimitiveRecord& p) { return p.indices.empty(); });
        if (mesh.primitives.empty()) continue;
        if (!skin) item.frame = frame;
        meshes.push_back(std::move(item));
    }
    if (!bind) throw std::runtime_error("Clump has no skinned Atomic");
    for (const auto& item : meshes)
        if (!item.joints.empty())
            for (const auto& joints : item.joints)
                for (const auto j : joints)
                    if (j >= joint_frames.size()) throw std::runtime_error("Skin vertex names bone " + std::to_string(j));

    CharacterExportStats stats;
    stats.joints = joint_frames.size();
    stats.meshes = meshes.size();
    stats.materials = materials.size();

    // Clips: poses sampled through the viewport's evaluator, made frame-local.
    struct SampledClip {
        std::string name;
        std::vector<float> times;
        std::vector<std::vector<Matrix>> local;  // [sample][frame]
        std::vector<bool> animated;              // frames the clip moves
    };
    std::vector<SampledClip> sampled;
    // The glTF hierarchy: the Frame List's, with the joints `joint_parent` moves.
    // Poses are world ones made local, so moving a joint changes no pose.
    std::vector<std::int32_t> parents(frame_count);
    for (std::size_t i = 0; i < frame_count; ++i) parents[i] = frames.value->frames[i].parent;
    if (joint_parent) {
        const auto& ids = binding.value->frame_node_ids;
        const auto frame_of = [&](const std::int32_t id) -> std::int32_t {
            const auto it = std::ranges::find(ids, id);
            return it == ids.end() ? -1 : static_cast<std::int32_t>(it - ids.begin());
        };
        for (std::size_t i = 0; i < frame_count && i < ids.size(); ++i) {
            if (ids[i] < 0) continue;
            const auto parent_id = joint_parent(ids[i]);
            if (!parent_id) continue;
            const auto parent = frame_of(*parent_id);
            // A new parent must not be the joint itself or below it.
            bool cycle = parent < 0;
            for (auto p = parent; p >= 0 && !cycle; p = parents[static_cast<std::size_t>(p)]) cycle = p == static_cast<std::int32_t>(i);
            if (cycle) throw std::runtime_error("Joint " + std::to_string(ids[i]) + " cannot hang under " + std::to_string(*parent_id));
            parents[i] = parent;
        }
    }
    const auto bind_local = local_matrices(parents, bind->world);
    for (auto& clip : clips) {
        if (!clip.clip.valid()) {
            stats.skipped_clips.push_back(clip.name + ": not a supported animation");
            continue;
        }
        if (!map_animation_tracks(clip.clip, *binding.value, frame_count).compatible) {
            stats.skipped_clips.push_back(clip.name + ": its tracks do not fit this skeleton");
            continue;
        }
        SampledClip out;
        out.name = clip.name;
        out.animated.assign(frame_count, false);
        for (const auto& track : clip.clip.tracks)
            if (track.frame_index && *track.frame_index >= 0 && static_cast<std::size_t>(*track.frame_index) < frame_count)
                out.animated[static_cast<std::size_t>(*track.frame_index)] = true;
        const auto duration = std::max(clip.clip.duration, 0.0F);
        const auto samples = std::max<std::size_t>(2, static_cast<std::size_t>(std::ceil(duration * 30.0F)) + 1);
        for (std::size_t s = 0; s < samples; ++s) {
            const auto time = duration * static_cast<float>(s) / static_cast<float>(samples - 1);
            // The last sample is the clip's end, not a wrap to its start.
            const auto pose = evaluate_pose(clip.clip, *frames.value, bind->local, time, clip.loop && s + 1 < samples);
            if (pose.world.size() != frame_count) break;
            out.times.push_back(time);
            out.local.push_back(local_matrices(parents, pose.world));
        }
        if (out.times.size() < 2) {
            stats.skipped_clips.push_back(clip.name + ": its poses could not be evaluated");
            continue;
        }
        sampled.push_back(std::move(out));
    }
    stats.clips = sampled.size();

    // Buffers.
    std::vector<std::byte> binary;
    std::vector<BufferView> views;
    std::vector<Accessor> accessors;
    const auto add = [&]<class T>(const std::vector<T>& values, const std::uint32_t component, const std::string& type,
                                  const std::uint32_t target) {
        const auto [offset, length] = append_binary(binary, values);
        views.push_back({offset, length, target});
        Accessor accessor;
        accessor.view = views.size() - 1;
        accessor.component_type = component;
        accessor.count = values.size();
        accessor.type = type;
        accessors.push_back(accessor);
        return accessors.size() - 1;
    };

    // Nodes: frames first (index = frame), then one per mesh.
    std::ostringstream json;
    json << std::setprecision(9);
    json << "{\n  \"asset\": {\"version\": \"2.0\", \"generator\": \"CSF Mission Editor\"},\n  \"scene\": 0,\n";
    const auto name_of = [&](const std::size_t frame) {
        const auto& ids = binding.value->frame_node_ids;
        if (frame < ids.size() && ids[frame] >= 0) {
            if (joint_name) {
                auto name = joint_name(ids[frame]);
                if (!name.empty()) return name;
            }
            return "bone_" + std::to_string(ids[frame]);
        }
        return "frame_" + std::to_string(frame);
    };
    std::vector<std::vector<std::size_t>> children(frame_count);
    for (std::size_t i = 0; i < frame_count; ++i) {
        const auto parent = parents[i];
        if (parent >= 0 && static_cast<std::size_t>(parent) < frame_count) children[static_cast<std::size_t>(parent)].push_back(i);
    }
    for (std::size_t m = 0; m < meshes.size(); ++m)
        if (meshes[m].frame) children[*meshes[m].frame].push_back(frame_count + m);

    // Mesh data.
    std::vector<std::string> mesh_json;
    for (const auto& item : meshes) {
        const auto& mesh = item.mesh;
        std::string attributes;
        auto position = add(mesh.positions, 5126, "VEC3", 34962);
        auto& bounds = accessors[position];
        bounds.has_bounds = true;
        bounds.minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        bounds.maximum = {-bounds.minimum[0], -bounds.minimum[1], -bounds.minimum[2]};
        for (const auto& p : mesh.positions)
            for (int a = 0; a < 3; ++a) {
                const float v = a == 0 ? p.x : a == 1 ? p.y : p.z;
                bounds.minimum[a] = std::min(bounds.minimum[a], v);
                bounds.maximum[a] = std::max(bounds.maximum[a], v);
            }
        attributes = "\"POSITION\": " + std::to_string(position);
        if (!mesh.normals.empty()) attributes += ", \"NORMAL\": " + std::to_string(add(mesh.normals, 5126, "VEC3", 34962));
        if (!mesh.uv0.empty()) attributes += ", \"TEXCOORD_0\": " + std::to_string(add(mesh.uv0, 5126, "VEC2", 34962));
        if (!item.joints.empty()) {
            attributes += ", \"JOINTS_0\": " + std::to_string(add(item.joints, 5121, "VEC4", 34962));
            attributes += ", \"WEIGHTS_0\": " + std::to_string(add(item.weights, 5126, "VEC4", 34962));
        }
        std::string primitives;
        for (const auto& primitive : mesh.primitives) {
            const auto indices = add(primitive.indices, 5125, "SCALAR", 34963);
            stats.triangles += primitive.indices.size() / 3;
            primitives += std::string(primitives.empty() ? "" : ", ") + "{\"attributes\": {" + attributes +
                          "}, \"indices\": " + std::to_string(indices) + ", \"material\": " + std::to_string(primitive.material) +
                          ", \"mode\": 4}";
        }
        mesh_json.push_back("{\"name\": \"" + json_escape(mesh.name) + "\", \"primitives\": [" + primitives + "]}");
    }

    // Inverse binds, scaled to metres like everything else.
    std::vector<Matrix> scaled_inverse = inverse_bind;
    for (auto& m : scaled_inverse)
        for (int row = 0; row < 3; ++row) m[12 + row] *= scene_scale;
    const auto inverse_accessor = add(scaled_inverse, 5126, "MAT4", 0);

    // Animation samplers: per clip, per animated frame, translation and rotation.
        std::vector<std::string> animation_json;
    for (const auto& clip : sampled) {
        const auto input = add(clip.times, 5126, "SCALAR", 0);
        auto& time_accessor = accessors[input];
        time_accessor.has_bounds = true;
        time_accessor.minimum = {clip.times.front(), 0, 0};
        time_accessor.maximum = {clip.times.back(), 0, 0};
        std::string samplers, channels;
        std::size_t sampler{};
        for (std::size_t f = 0; f < frame_count; ++f) {
            if (!clip.animated[f]) continue;
            std::vector<Vec3> translations;
            std::vector<std::array<float, 4>> rotations;
            for (const auto& pose : clip.local) {
                auto [q, t] = decompose(pose[f]);
                // Keep neighbouring samples on the same hemisphere so LINEAR blends the short way.
                if (!rotations.empty()) {
                    const auto& prev = rotations.back();
                    if (prev[0] * q[0] + prev[1] * q[1] + prev[2] * q[2] + prev[3] * q[3] < 0)
                        for (auto& v : q) v = -v;
                }
                translations.push_back(t);
                rotations.push_back(q);
            }
            const auto t_out = add(translations, 5126, "VEC3", 0);
            const auto r_out = add(rotations, 5126, "VEC4", 0);
            for (const auto& [output, path] : {std::pair{t_out, "translation"}, std::pair{r_out, "rotation"}}) {
                samplers += std::string(samplers.empty() ? "" : ", ") + "{\"input\": " + std::to_string(input) +
                            ", \"output\": " + std::to_string(output) + ", \"interpolation\": \"LINEAR\"}";
                channels += std::string(channels.empty() ? "" : ", ") + "{\"sampler\": " + std::to_string(sampler++) +
                            ", \"target\": {\"node\": " + std::to_string(f) + ", \"path\": \"" + path + "\"}}";
            }
        }
        animation_json.push_back("{\"name\": \"" + json_escape(clip.name) + "\", \"samplers\": [" + samplers +
                                 "], \"channels\": [" + channels + "]}");
    }

    // Scene roots: parentless frames, and the skinned meshes (glTF ignores their node transform).
    std::vector<std::size_t> roots;
    for (std::size_t i = 0; i < frame_count; ++i)
        if (parents[i] < 0) roots.push_back(i);
    json << "  \"scenes\": [{\"nodes\": [";
    for (std::size_t m = 0; m < meshes.size(); ++m)
        if (!meshes[m].frame) roots.push_back(frame_count + m);
    for (std::size_t i = 0; i < roots.size(); ++i) json << (i ? ", " : "") << roots[i];
    json << "]}],\n  \"nodes\": [\n";
    for (std::size_t i = 0; i < frame_count; ++i) {
        const auto [q, t] = decompose(bind_local[i]);
        json << "    {\"name\": \"" << json_escape(name_of(i)) << "\", \"rotation\": [" << q[0] << ", " << q[1] << ", " << q[2]
             << ", " << q[3] << "], \"translation\": [" << t.x << ", " << t.y << ", " << t.z << ']';
        if (!children[i].empty()) {
            json << ", \"children\": [";
            for (std::size_t c = 0; c < children[i].size(); ++c) json << (c ? ", " : "") << children[i][c];
            json << ']';
        }
        json << "},\n";
    }
    for (std::size_t m = 0; m < meshes.size(); ++m) {
        json << "    {\"name\": \"" << json_escape(meshes[m].mesh.name) << "\", \"mesh\": " << m;
        if (!meshes[m].frame) json << ", \"skin\": 0";
        json << "},\n";
    }
    // Remove the last ",\n" of the node list.
    auto text = json.str();
    text.resize(text.size() - 2);
    json.str("");
    json << text << "\n  ],\n  \"skins\": [{\"name\": \"skeleton\", \"inverseBindMatrices\": " << inverse_accessor
         << ", \"joints\": [";
    for (std::size_t b = 0; b < joint_frames.size(); ++b) json << (b ? ", " : "") << joint_frames[b];
    json << "]}]";
    if (!meshes.empty()) {
        json << ",\n  \"meshes\": [\n";
        for (std::size_t m = 0; m < mesh_json.size(); ++m) json << "    " << mesh_json[m] << (m + 1 < mesh_json.size() ? ",\n" : "\n");
        json << "  ],\n  \"materials\": [\n";
        write_materials(json, materials, textures);
        json << "  ]";
    }
    if (!animation_json.empty()) {
        json << ",\n  \"animations\": [\n";
        for (std::size_t a = 0; a < animation_json.size(); ++a)
            json << "    " << animation_json[a] << (a + 1 < animation_json.size() ? ",\n" : "\n");
        json << "  ]";
    }
    auto gltf_path = output_path;
    if (gltf_path.extension() != ".gltf") gltf_path.replace_extension(".gltf");
    if (gltf_path.has_parent_path()) std::filesystem::create_directories(gltf_path.parent_path());
    auto bin_path = gltf_path;
    bin_path.replace_extension(".bin");
    json << ",\n  \"buffers\": [{\"uri\": \"" << json_escape(bin_path.filename().string()) << "\", \"byteLength\": " << binary.size()
         << "}],\n  \"bufferViews\": [\n";
    for (std::size_t i = 0; i < views.size(); ++i) {
        json << "    {\"buffer\": 0, \"byteOffset\": " << views[i].offset << ", \"byteLength\": " << views[i].length;
        if (views[i].target) json << ", \"target\": " << views[i].target;
        json << '}' << (i + 1 == views.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"accessors\": [\n";
    for (std::size_t i = 0; i < accessors.size(); ++i) {
        const auto& a = accessors[i];
        json << "    {\"bufferView\": " << a.view << ", \"componentType\": " << a.component_type << ", \"count\": " << a.count
             << ", \"type\": \"" << a.type << '"';
        if (a.has_bounds) {
            const int n = a.type == "SCALAR" ? 1 : 3;
            json << ", \"min\": [";
            for (int k = 0; k < n; ++k) json << (k ? ", " : "") << a.minimum[k];
            json << "], \"max\": [";
            for (int k = 0; k < n; ++k) json << (k ? ", " : "") << a.maximum[k];
            json << ']';
        }
        json << '}' << (i + 1 == accessors.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    std::ofstream bin(bin_path, std::ios::binary | std::ios::trunc);
    bin.write(reinterpret_cast<const char*>(binary.data()), static_cast<std::streamsize>(binary.size()));
    if (!bin) throw std::runtime_error("Cannot write " + bin_path.string());
    write_text(gltf_path, json.str());
    return stats;
}

} // namespace rws
