#include "rws/obj_export.hpp"
#include "rws/world_recovery.hpp"

#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rws {
namespace {

std::uint32_t read_u32(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("OBJ source array is truncated");
    const auto i = static_cast<std::size_t>(offset);
    return std::to_integer<std::uint32_t>(bytes[i]) |
           (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[i + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[i + 3]) << 24U);
}

float read_f32(const std::span<const std::byte> bytes, const std::uint64_t offset) {
    return std::bit_cast<float>(read_u32(bytes, offset));
}

const Chunk* find_at(const std::vector<Chunk>& chunks, const std::uint64_t offset) {
    for (const auto& chunk : chunks) {
        if (chunk.offset == offset) return &chunk;
        if (const auto* nested = find_at(chunk.children, offset)) return nested;
    }
    return nullptr;
}

struct SurfaceMetadata {
    std::string name;
    std::optional<std::uint32_t> id;
};

std::string json_escape(const std::string& value) {
    std::string output;
    for (const char c : value) {
        if (c == '"' || c == '\\') output.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20) output.push_back(c);
    }
    return output;
}

std::vector<SurfaceMetadata>
read_surfaces(const Chunk* world, const std::span<const std::byte> bytes, const std::size_t count) {
    std::vector<SurfaceMetadata> result(count);
    const auto* list = world ? find_child(*world, 0x08) : nullptr;
    const auto decoded =
        list ? decode_material_list(*list, bytes) : DecodeResult<MaterialListInfo>{};
    std::vector<const Chunk*> materials;
    if (list)
        for (const auto& child : list->children)
            if (child.type == 0x07) materials.push_back(&child);
    std::size_t next{};
    for (std::size_t slot = 0; slot < count; ++slot) {
        const auto remap =
            decoded && slot < decoded.value->remap.size() ? decoded.value->remap[slot] : -1;
        if (remap >= 0 && static_cast<std::size_t>(remap) < slot) {
            result[slot] = result[remap];
            continue;
        }
        if (next >= materials.size()) continue;
        const auto* extension = find_child(*materials[next++], 0x03);
        const auto* pyro = extension ? find_child(*extension, 0xFFFFFF00U) : nullptr;
        const auto metadata =
            pyro ? decode_pyro_extension(*pyro, 0x07, bytes) : DecodeResult<PyroExtensionInfo>{};
        if (metadata) {
            result[slot].name = std::string(metadata.value->object_name());
            result[slot].id = metadata.value->material_surface_type();
        }
    }
    return result;
}

} // namespace

void export_geometry_obj(const GeometryInfo& geometry, const std::span<const std::byte> bytes,
                         const std::filesystem::path& output_path) {
    const MorphTargetInfo* morph = nullptr;
    for (const auto& candidate : geometry.morph_targets) {
        if (candidate.has_vertices) {
            morph = &candidate;
            break;
        }
    }
    if (!morph) throw std::runtime_error("Geometry has no exportable vertex morph target");
    if (geometry.triangle_layout == TriangleLayout::unknown)
        throw std::runtime_error("Geometry triangle word order is ambiguous");

    std::ofstream output(output_path, std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create OBJ: " + output_path.string());
    output << "# Exported by CSF Mission Editor\n" << std::setprecision(9);
    for (std::int32_t i = 0; i < geometry.vertex_count; ++i) {
        const auto offset = morph->vertices_offset + static_cast<std::uint64_t>(i) * 12;
        output << "v " << read_f32(bytes, offset) << ' ' << read_f32(bytes, offset + 4) << ' '
               << read_f32(bytes, offset + 8) << '\n';
    }
    const bool has_uv = !geometry.texcoord_offsets.empty();
    if (has_uv) {
        for (std::int32_t i = 0; i < geometry.vertex_count; ++i) {
            const auto offset =
                geometry.texcoord_offsets.front() + static_cast<std::uint64_t>(i) * 8;
            output << "vt " << read_f32(bytes, offset) << ' '
                   << (1.0F - read_f32(bytes, offset + 4)) << '\n';
        }
    }
    if (morph->has_normals) {
        for (std::int32_t i = 0; i < geometry.vertex_count; ++i) {
            const auto offset = morph->normals_offset + static_cast<std::uint64_t>(i) * 12;
            output << "vn " << read_f32(bytes, offset) << ' ' << read_f32(bytes, offset + 4) << ' '
                   << read_f32(bytes, offset + 8) << '\n';
        }
    }

    std::uint16_t active_material = 0xFFFFU;
    for (std::int32_t i = 0; i < geometry.triangle_count; ++i) {
        const auto triangle = decode_triangle(geometry, i, bytes);
        if (!triangle) throw std::runtime_error(triangle.error);
        if (triangle.value->material != active_material) {
            active_material = triangle.value->material;
            output << "usemtl material_" << active_material << '\n';
        }
        output << "f";
        for (const auto index : triangle.value->vertices) {
            const auto obj_index = static_cast<std::uint32_t>(index) + 1U;
            output << ' ' << obj_index;
            if (has_uv || morph->has_normals) {
                output << '/';
                if (has_uv) output << obj_index;
                if (morph->has_normals) output << '/' << obj_index;
            }
        }
        output << '\n';
    }
    if (!output) throw std::runtime_error("Failed while writing OBJ: " + output_path.string());
}

CollisionExportStats export_collision_obj(const std::vector<Chunk>& chunks,
                                          const std::span<const std::byte> bytes,
                                          const std::filesystem::path& requested_path) {
    auto output_path = requested_path;
    if (output_path.extension() != ".obj") output_path.replace_extension(".obj");
    if (output_path.has_parent_path())
        std::filesystem::create_directories(output_path.parent_path());
    auto mtl_path = output_path;
    mtl_path.replace_extension(".mtl");
    auto manifest_path = output_path;
    manifest_path.replace_extension(".manifest.json");
    const auto worlds = recover_worlds(chunks, bytes);
    if (worlds.empty()) throw std::runtime_error("No World collision geometry was found");
    std::ofstream output(output_path, std::ios::trunc);
    std::ofstream mtl(mtl_path, std::ios::trunc);
    std::ofstream manifest(manifest_path, std::ios::trunc);
    if (!output || !mtl || !manifest)
        throw std::runtime_error("Cannot create collision OBJ export set");
    output << "# Collision-only export; source RWS axes and units are preserved\nmtllib "
           << mtl_path.filename().string() << '\n'
           << std::setprecision(9);
    CollisionExportStats stats;
    std::uint64_t vertex_base = 1;
    std::vector<std::vector<SurfaceMetadata>> surfaces;
    surfaces.reserve(worlds.size());
    for (const auto& world : worlds)
        surfaces.push_back(
            read_surfaces(find_at(chunks, world.world_offset), bytes,
                          static_cast<std::size_t>(std::max(0, world.material_count))));
    manifest << "{\n  \"format\": \"rws-man-collision-obj-manifest-v1\",\n"
             << "  \"coordinate_system\": \"source RWS axes and units\",\n  \"sectors\": [\n";
    bool first_sector = true;
    for (std::size_t wi = 0; wi < worlds.size(); ++wi) {
        const auto& world = worlds[wi];
        ++stats.worlds;
        stats.materials += static_cast<std::uint64_t>(std::max(0, world.material_count));
        for (std::size_t si = 0; si < world.sectors.size(); ++si) {
            const auto& sector = world.sectors[si];
            std::vector<Vec3> vertices;
            vertices.reserve(static_cast<std::size_t>(std::max(0, sector.vertex_count)));
            for (std::int32_t vi = 0; vi < sector.vertex_count; ++vi) {
                const auto decoded = decode_recovered_world_vertex(sector, vi, bytes);
                if (!decoded) continue;
                vertices.push_back(*decoded.value);
                output << "v " << decoded.value->x << ' ' << decoded.value->y << ' '
                       << decoded.value->z << '\n';
            }
            output << "g world_" << wi << "_sector_" << si << "_0x" << std::hex
                   << sector.chunk_offset << std::dec << '\n';
            std::int32_t active_material = -1;
            std::uint64_t emitted{};
            for (std::int32_t ti = 0; ti < sector.triangle_count; ++ti) {
                const auto triangle =
                    decode_recovered_world_triangle_resolved(world, si, ti, bytes);
                if (!triangle) {
                    ++stats.skipped_triangles;
                    continue;
                }
                const auto& a = triangle.value->vertices[0];
                const auto& b = triangle.value->vertices[1];
                const auto& c = triangle.value->vertices[2];
                const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z}, ac{c.x - a.x, c.y - a.y, c.z - a.z};
                const Vec3 normal{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z,
                                  ab.x * ac.y - ab.y * ac.x};
                if (normal.x * normal.x + normal.y * normal.y + normal.z * normal.z <= 1.0e-20F) {
                    ++stats.skipped_triangles;
                    continue;
                }
                if (triangle.value->material_slot != active_material) {
                    active_material = triangle.value->material_slot;
                    output << "usemtl world_" << wi << "_collision_material_" << active_material
                           << '\n';
                }
                output << "f";
                for (const auto index : triangle.value->vertex_indices)
                    output << ' ' << vertex_base + index;
                output << '\n';
                ++emitted;
            }
            if (!first_sector) manifest << ",\n";
            first_sector = false;
            manifest << "    {\"world_index\": " << wi << ", \"world_offset\": \"0x" << std::hex
                     << world.world_offset << "\", \"sector_index\": " << std::dec << si
                     << ", \"sector_offset\": \"0x" << std::hex << sector.chunk_offset << std::dec
                     << "\", \"vertices\": " << vertices.size() << ", \"triangles\": " << emitted
                     << '}';
            vertex_base += vertices.size();
            stats.vertices += vertices.size();
            stats.triangles += emitted;
            ++stats.sectors;
        }
    }
    manifest << "\n  ],\n  \"materials\": [\n";
    bool first_material = true;
    for (std::size_t wi = 0; wi < surfaces.size(); ++wi)
        for (std::size_t material = 0; material < surfaces[wi].size(); ++material) {
            if (!first_material) manifest << ",\n";
            first_material = false;
            manifest << "    {\"world_index\": " << wi << ", \"slot\": " << material
                     << ", \"obj_name\": \"world_" << wi << "_collision_material_" << material
                     << "\", \"raw_surface_name\": \"" << json_escape(surfaces[wi][material].name)
                     << "\", \"surface_id\": ";
            if (surfaces[wi][material].id)
                manifest << *surfaces[wi][material].id;
            else
                manifest << "null";
            manifest << '}';
            const auto r = ((material * 97U + wi * 17U + 61U) % 191U + 48U) / 255.0;
            const auto g = ((material * 57U + wi * 29U + 101U) % 191U + 48U) / 255.0;
            const auto b = ((material * 31U + wi * 43U + 149U) % 191U + 48U) / 255.0;
            mtl << "newmtl world_" << wi << "_collision_material_" << material << "\nKd " << r
                << ' ' << g << ' ' << b << "\nKa 0 0 0\nKs 0 0 0\nd 1\n\n";
        }
    manifest << "\n  ],\n  \"statistics\": {\"worlds\": " << stats.worlds
             << ", \"sectors\": " << stats.sectors << ", \"vertices\": " << stats.vertices
             << ", \"triangles\": " << stats.triangles
             << ", \"skipped_triangles\": " << stats.skipped_triangles << "}\n}\n";
    if (!output || !mtl || !manifest)
        throw std::runtime_error("Failed while writing collision OBJ export set");
    return stats;
}

} // namespace rws
