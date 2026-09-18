#include "rws/document.hpp"
#include "rws/decoded.hpp"
#include "rws/world_recovery.hpp"
#include "rws/scene_export.hpp"
#include "rws/obj_export.hpp"

#include <bit>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

void append_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void append_header(std::vector<std::byte>& bytes, const std::uint32_t type,
                   const std::uint32_t size, const std::uint32_t stamp = 0x1C020037) {
    append_u32(bytes, type);
    append_u32(bytes, size);
    append_u32(bytes, stamp);
}

void append_f32(std::vector<std::byte>& bytes, const float value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void append_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>(value >> 8U));
}

void append_world_sector(std::vector<std::byte>& output, const std::uint32_t format,
                         const std::int32_t material_base, const std::int32_t vertices,
                         const std::vector<std::array<std::uint16_t, 4>>& triangles) {
    std::vector<std::byte> data;
    append_u32(data, static_cast<std::uint32_t>(material_base));
    append_u32(data, static_cast<std::uint32_t>(triangles.size()));
    append_u32(data, static_cast<std::uint32_t>(vertices));
    for (const float value : {-1.0F, -2.0F, -3.0F, 4.0F, 5.0F, 6.0F}) append_f32(data, value);
    append_u32(data, 0); append_u32(data, 0);
    if (vertices > 0) {
        for (std::int32_t i = 0; i < vertices; ++i) {
            append_f32(data, static_cast<float>(i)); append_f32(data, 0); append_f32(data, 0);
        }
        if (format & 0x10U)
            for (std::int32_t i = 0; i < vertices; ++i) append_u32(data, 0x007F0000U);
        if (format & 0x08U)
            for (std::int32_t i = 0; i < vertices; ++i) append_u32(data, 0xFFFFFFFFU);
        auto uv_sets = (format >> 16U) & 0xFFU;
        if (uv_sets == 0) uv_sets = (format & 0x80U) ? 2U : ((format & 0x04U) ? 1U : 0U);
        for (std::uint32_t set = 0; set < uv_sets; ++set)
            for (std::int32_t i = 0; i < vertices; ++i) {
                append_f32(data, static_cast<float>(i)); append_f32(data, static_cast<float>(set));
            }
    }
    for (const auto& triangle : triangles)
        for (const auto word : triangle) append_u16(data, word);
    append_header(output, 0x09, static_cast<std::uint32_t>(12U + data.size()));
    append_header(output, 0x01, static_cast<std::uint32_t>(data.size()));
    output.insert(output.end(), data.begin(), data.end());
}

std::vector<std::byte> make_world(const std::uint32_t format,
                                  const std::int32_t declared_sectors,
                                  const std::int32_t declared_triangles,
                                  const std::int32_t declared_vertices,
                                  const std::int32_t materials,
                                  const std::vector<std::vector<std::byte>>& sectors,
                                  const std::int32_t declared_planes = 0,
                                  const bool root_is_sector = true) {
    std::vector<std::byte> payload;
    append_header(payload, 0x01, 64);
    append_u32(payload, root_is_sector ? 1U : 0U);
    append_f32(payload, 0); append_f32(payload, 0); append_f32(payload, 0);
    append_u32(payload, static_cast<std::uint32_t>(declared_triangles));
    append_u32(payload, static_cast<std::uint32_t>(declared_vertices));
    append_u32(payload, static_cast<std::uint32_t>(declared_planes));
    append_u32(payload, static_cast<std::uint32_t>(declared_sectors));
    append_u32(payload, 0); append_u32(payload, format);
    for (const float value : {4.0F, 5.0F, 6.0F, -1.0F, -2.0F, -3.0F}) append_f32(payload, value);
    std::vector<std::byte> material_struct;
    append_u32(material_struct, static_cast<std::uint32_t>(materials));
    for (std::int32_t i = 0; i < materials; ++i) append_u32(material_struct, 0xFFFFFFFFU);
    append_header(payload, 0x08, static_cast<std::uint32_t>(12U + material_struct.size()));
    append_header(payload, 0x01, static_cast<std::uint32_t>(material_struct.size()));
    payload.insert(payload.end(), material_struct.begin(), material_struct.end());
    for (const auto& sector : sectors) payload.insert(payload.end(), sector.begin(), sector.end());
    std::vector<std::byte> bytes;
    append_header(bytes, 0x0B, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

void append_plane(std::vector<std::byte>& output, const std::int32_t axis,
                  const bool left_sector, const bool right_sector,
                  const float split, const float left_value, const float right_value) {
    append_header(output, 0x0A, 36);
    append_header(output, 0x01, 24);
    append_u32(output, static_cast<std::uint32_t>(axis)); append_f32(output, split);
    append_u32(output, left_sector ? 1U : 0U); append_u32(output, right_sector ? 1U : 0U);
    append_f32(output, left_value); append_f32(output, right_value);
}

void write_f32(std::vector<std::byte>& bytes, const std::size_t offset, const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes[offset + shift / 8U] = static_cast<std::byte>((bits >> shift) & 0xFFU);
}

} // namespace

int main() {
    {
        const auto version = rws::decode_library_id(0x1C020037);
        assert(version.encoded_version == 0x37002);
        assert(version.major == 3 && version.minor == 7 && version.revision == 0 && version.binary == 2);
        assert(version.build == 55);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x10, 16);
        append_header(bytes, 0x01, 4);
        append_u32(bytes, 7);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        assert(document.chunks().size() == 1);
        assert(document.chunks()[0].children.size() == 1);
        assert(document.chunks()[0].children[0].type == 0x01);
        assert(document.diagnostics().empty());
        const auto clump = rws::decode_clump(document.chunks()[0], document.bytes());
        assert(clump && clump.value->atomics == 7);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x0B, 100);
        append_u32(bytes, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        assert(document.chunks().size() == 1);
        assert(document.chunks()[0].truncated);
        assert(!document.diagnostics().empty());
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x01, 0); // ordinary stream prefix establishes the stamp
        for (int i = 0; i < 5; ++i) bytes.push_back(std::byte{0x55}); // game-specific records
        const auto world_offset = bytes.size();
        append_header(bytes, 0x0B, 44); // one Struct child, declared 32 bytes beyond EOF
        append_header(bytes, 0x01, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        assert(document.chunks().size() == 2);
        assert(document.chunks()[1].offset == world_offset);
        assert(document.chunks()[1].type == 0x0B && document.chunks()[1].truncated);
        assert(document.chunks()[1].children.size() == 1);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x01, 0);
        append_header(bytes, 0x16FC0, 99);
        append_u32(bytes, 1001); append_u32(bytes, 42); append_f32(bytes, 5000.0F);
        append_u32(bytes, 0); append_u32(bytes, 0); append_u32(bytes, 0x641);
        append_header(bytes, 0x0D, 64);
        append_header(bytes, 0x01, 52);
        for (int i = 0; i < 9; ++i) append_f32(bytes, i % 4 == 0 ? 1.0F : 0.0F);
        append_f32(bytes, 10.0F); append_f32(bytes, 20.0F); append_f32(bytes, 30.0F);
        append_u32(bytes, 3);
        append_u32(bytes, 7);
        for (const char character : std::string("ARBOL_3"))
            bytes.push_back(static_cast<std::byte>(character));
        const auto world_offset = bytes.size();
        append_header(bytes, 0x0B, 44);
        append_header(bytes, 0x01, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        assert(document.scene_instances().size() == 1);
        const auto& instance = document.scene_instances().front();
        assert(instance.prototype_id == 1001 && instance.instance_id == 42);
        assert(instance.prototype_name == "ARBOL_3" && instance.physical_size == 123);
        assert(instance.maximum_visibility_distance == 5000.0F);
        assert(instance.minimum_visibility_distance == 0.0F);
        assert(instance.visibility_fade_range == 0.0F);
        assert(rws::has_scene_instance_flag(instance.flags, rws::SceneInstanceFlag::enabled));
        assert(rws::has_scene_instance_flag(instance.flags, rws::SceneInstanceFlag::animated));
        assert(rws::scene_instance_flag_names(instance.flags) ==
               "enabled, mipmapped, animated, scene-registered");
        assert(instance.position.x == 10.0F && instance.position.y == 20.0F && instance.position.z == 30.0F);
        assert(document.chunks().size() == 2 && document.chunks()[1].offset == world_offset);
    }
    {
        constexpr std::uint32_t struct_size = 120;
        std::vector<std::byte> bytes;
        append_header(bytes, 0x0F, 12 + struct_size);
        append_header(bytes, 0x01, struct_size);
        append_u32(bytes, 0x12); // positions + normals
        append_u32(bytes, 1);    // triangles
        append_u32(bytes, 3);    // vertices
        append_u32(bytes, 1);    // morph targets
        for (int i = 0; i < 2; ++i) append_u32(bytes, 0); // one eight-byte triangle
        append_f32(bytes, 0); append_f32(bytes, 0); append_f32(bytes, 0); append_f32(bytes, 1);
        append_u32(bytes, 1); append_u32(bytes, 1);
        for (int i = 0; i < 18; ++i) append_f32(bytes, 0); // positions + normals
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto geometry = rws::decode_geometry(document.chunks()[0], document.bytes());
        assert(geometry);
        assert(geometry.value->vertex_count == 3);
        assert(geometry.value->triangle_count == 1);
        assert(geometry.value->computed_size == struct_size);
        assert(geometry.value->morph_targets[0].has_vertices);
        assert(geometry.value->morph_targets[0].has_normals);
    }
    {
        constexpr std::uint32_t struct_size = 68;
        std::vector<std::byte> bytes;
        append_header(bytes, 0x0F, 12 + struct_size);
        append_header(bytes, 0x01, struct_size);
        append_u32(bytes, 0x00020002); // positions + two explicit UV sets
        append_u32(bytes, 0);         // triangles
        append_u32(bytes, 1);         // vertices
        append_u32(bytes, 1);         // morph targets
        append_f32(bytes, 0.1F); append_f32(bytes, 0.2F); // UV1
        append_f32(bytes, 0.3F); append_f32(bytes, 0.4F); // UV2
        append_f32(bytes, 0); append_f32(bytes, 0); append_f32(bytes, 0); append_f32(bytes, 1);
        append_u32(bytes, 1); append_u32(bytes, 0);
        append_f32(bytes, 0); append_f32(bytes, 0); append_f32(bytes, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto geometry = rws::decode_geometry(document.chunks()[0], document.bytes());
        assert(geometry && geometry.value->texcoord_sets == 2);
        assert(geometry.value->texcoord_offsets.size() == 2);
        assert(geometry.value->texcoord_offsets[1] - geometry.value->texcoord_offsets[0] == 8);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x050E, 32);
        append_u32(bytes, 0); // flags: triangle list
        append_u32(bytes, 1); // mesh count
        append_u32(bytes, 3); // total indices
        append_u32(bytes, 3); // entry indices
        append_u32(bytes, 2); // material
        append_u32(bytes, 0); append_u32(bytes, 1); append_u32(bytes, 2);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto mesh = rws::decode_bin_mesh(document.chunks()[0], document.bytes());
        assert(mesh && mesh.value->meshes.size() == 1);
        assert(mesh.value->total_indices == 3);
        assert(mesh.value->meshes[0].material_index == 2);
    }
    {
        constexpr std::uint32_t struct_size = 58;
        constexpr std::uint32_t tree_size = 12 + struct_size;
        std::vector<std::byte> bytes;
        append_header(bytes, 0x011D, 4 + 12 + tree_size);
        append_u32(bytes, 0x37002);
        append_header(bytes, 0x2C, tree_size);
        append_header(bytes, 0x01, struct_size);
        append_u32(bytes, 1);
        append_f32(bytes, -1); append_f32(bytes, -2); append_f32(bytes, -3);
        append_f32(bytes, 4); append_f32(bytes, 5); append_f32(bytes, 6);
        append_u32(bytes, 3); append_u32(bytes, 1);
        bytes.push_back(std::byte{1}); bytes.push_back(std::byte{0xFF});
        bytes.push_back(std::byte{2}); bytes.push_back(std::byte{0}); append_f32(bytes, 10);
        bytes.push_back(std::byte{0}); bytes.push_back(std::byte{0xFF});
        bytes.push_back(std::byte{7}); bytes.push_back(std::byte{0}); append_f32(bytes, 20);
        bytes.push_back(std::byte{2}); bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{1}); bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0}); bytes.push_back(std::byte{0});
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto collision = rws::decode_collision_tree(document.chunks()[0], document.bytes());
        assert(collision && collision.value->version == 0x37002);
        assert(collision.value->triangle_count == 3 && collision.value->split_count == 1);
        assert(collision.value->bounding_box_inf.y == -2.0F && collision.value->bounding_box_sup.z == 6.0F);
        assert(collision.value->splits[0].left.type == 1 && collision.value->splits[0].left.index == 2);
        assert(collision.value->triangle_map.size() == 3 && collision.value->triangle_map[0] == 2);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x24, 60);
        append_u32(bytes, 2);
        append_u32(bytes, 0x10); append_u32(bytes, 0x11223344); append_u32(bytes, 72);
        for (std::uint8_t i = 0; i < 16; ++i) bytes.push_back(static_cast<std::byte>(i));
        append_u32(bytes, 0x0B); append_u32(bytes, 0x55667788); append_u32(bytes, 84);
        for (std::uint8_t i = 16; i < 32; ++i) bytes.push_back(static_cast<std::byte>(i));
        append_header(bytes, 0x10, 0);
        append_header(bytes, 0x0B, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto contents = rws::decode_table_of_contents(document.chunks()[0], document.bytes());
        assert(contents && contents.value->entries.size() == 2);
        assert(contents.value->entries[0].chunk_type == 0x10);
        assert(contents.value->entries[0].object_id == 0x11223344);
        assert(contents.value->entries[0].offset == 72 && contents.value->entries[0].guid[15] == 15);
        assert(contents.value->entries[1].chunk_type == 0x0B && contents.value->entries[1].offset == 84);
    }
    {
        constexpr std::uint32_t texture_payload_size = 64;
        constexpr std::uint32_t effects_payload_size = 100;
        std::vector<std::byte> bytes;
        append_header(bytes, 0x120, effects_payload_size);
        append_u32(bytes, 4); // dual-pass effect
        append_u32(bytes, 4); // dual-pass slot
        append_u32(bytes, 1); // source: zero
        append_u32(bytes, 3); // destination: source color
        append_u32(bytes, 1); // embedded texture present
        append_header(bytes, 0x06, texture_payload_size);
        append_header(bytes, 0x01, 4); append_u32(bytes, 0x00011106);
        append_header(bytes, 0x02, 8);
        for (const char character : std::string("TEST_Lm\0", 8)) bytes.push_back(static_cast<std::byte>(character));
        append_header(bytes, 0x02, 4); append_u32(bytes, 0);
        append_header(bytes, 0x03, 0);
        append_u32(bytes, 0); // unused second effect slot
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto effects = rws::decode_material_effects(document.chunks()[0], 0x07, document.bytes());
        assert(effects && effects.value->effect_type == 4 && effects.value->slot_type == 4);
        assert(effects.value->has_dual_texture && effects.value->dual_texture.name == "TEST_Lm");
        assert(effects.value->source_blend == 1 && effects.value->destination_blend == 3);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x011E, 32);
        append_u32(bytes, 0x100); append_u32(bytes, 42); append_u32(bytes, 1);
        append_u32(bytes, 3); append_u32(bytes, 36);
        append_u32(bytes, 7); append_u32(bytes, 0); append_u32(bytes, 3);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto hierarchy = rws::decode_hanim(document.chunks()[0], document.bytes());
        assert(hierarchy && hierarchy.value->nodes.size() == 1);
        assert(hierarchy.value->hierarchy_id == 42);
        assert(hierarchy.value->nodes[0].node_id == 7);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x0116, 186);
        bytes.push_back(std::byte{2}); // bones
        bytes.push_back(std::byte{2}); // used bones
        bytes.push_back(std::byte{2}); // max weights
        bytes.push_back(std::byte{0});
        bytes.push_back(std::byte{0}); bytes.push_back(std::byte{1});
        for (int i = 0; i < 2; ++i) append_u32(bytes, 0x00000100U); // four packed indices per vertex
        for (int i = 0; i < 8; ++i) append_f32(bytes, i % 4 == 0 ? 1.0F : 0.0F);
        for (int i = 0; i < 32; ++i) append_f32(bytes, i % 5 == 0 ? 1.0F : 0.0F);
        append_u32(bytes, 1); append_u32(bytes, 0); append_u32(bytes, 0);
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto skin = rws::decode_skin(document.chunks()[0], 2, document.bytes());
        assert(skin && skin.value->bone_count == 2 && skin.value->used_bones.size() == 2);
        assert(skin.value->vertex_count == 2 && skin.value->trailing_split_bytes == 0);
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0x011F, 51);
        append_u32(bytes, 2);
        append_u32(bytes, 4); bytes.push_back(std::byte{'i'}); bytes.push_back(std::byte{'d'});
        bytes.push_back(std::byte{'\0'}); bytes.push_back(std::byte{'\0'});
        append_u32(bytes, 1); append_u32(bytes, 1); append_u32(bytes, 42);
        append_u32(bytes, 6); bytes.push_back(std::byte{'l'}); bytes.push_back(std::byte{'a'});
        bytes.push_back(std::byte{'b'}); bytes.push_back(std::byte{'e'}); bytes.push_back(std::byte{'l'});
        bytes.push_back(std::byte{'\0'});
        append_u32(bytes, 3); append_u32(bytes, 1);
        append_u32(bytes, 5); bytes.push_back(std::byte{'t'}); bytes.push_back(std::byte{'e'});
        bytes.push_back(std::byte{'s'}); bytes.push_back(std::byte{'t'}); bytes.push_back(std::byte{'\0'});
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto data = rws::decode_user_data(document.chunks()[0], document.bytes());
        assert(data && data.value->arrays.size() == 2);
        assert(data.value->arrays[0].integers[0] == 42);
        assert(data.value->arrays[1].strings[0] == "test");
    }
    {
        rws::PyroExtensionInfo metadata;
        metadata.owner_type = 0x14;
        metadata.words = {0x002A0009U};
        assert(metadata.atomic_object_index() == 9);
        metadata.words[0] = 0x002AFFFFU;
        assert(!metadata.atomic_object_index());
    }
    {
        std::vector<std::byte> bytes;
        append_header(bytes, 0xFFFFFF00U, 15);
        append_u32(bytes, 1); // World Sector schema version
        append_u32(bytes, 1); // per-vertex byte array present
        bytes.push_back(std::byte{0x10}); bytes.push_back(std::byte{0x20});
        bytes.push_back(std::byte{0x30});
        append_u32(bytes, 0x09); // first four bytes swallowed from the next header
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto metadata = rws::decode_pyro_extension(
            document.chunks()[0], 0x09, document.bytes());
        assert(metadata && metadata.value->present);
        assert(metadata.value->world_sector_vertex_bytes.size() == 3);
        assert(metadata.value->world_sector_vertex_bytes[2] == 0x30);
    }
    {
        std::vector<std::byte> payload;
        append_u32(payload, 2); // material schema version
        append_u32(payload, 1); // optional metadata record present
        for (const auto value : {1U, 0U, 4U, 7U, 2U}) append_u32(payload, value);
        append_u32(payload, 7);
        for (const char character : std::string("Cemento"))
            payload.push_back(static_cast<std::byte>(character));
        std::vector<std::byte> bytes;
        append_header(bytes, 0xFFFFFF00U, static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto metadata = rws::decode_pyro_extension(document.chunks()[0], 0x07, document.bytes());
        assert(metadata && metadata.value->version == 2 && metadata.value->present);
        assert(metadata.value->words.size() == 6 && metadata.value->strings[0] == "Cemento");
    }
    {
        std::vector<std::byte> payload;
        append_u32(payload, 0x18); append_u32(payload, 0x00010017);
        append_u32(payload, 0x0B); append_u32(payload, 3); append_u32(payload, 0x11);
        append_u32(payload, 0x11); append_u32(payload, 5); append_f32(payload, 3.0F);
        append_u32(payload, 5); append_f32(payload, 5.75F);
        append_u32(payload, 8); for (int i = 0; i < 12; ++i) append_f32(payload, i % 5 == 0 ? 1.0F : 0.0F);
        for (float value : {0.0F, 0.2F, 0.2F}) { append_u32(payload, 5); append_f32(payload, value); }
        append_u32(payload, 1); payload.push_back(std::byte{0x12}); payload.push_back(std::byte{0});
        append_u32(payload, 1); payload.push_back(std::byte{0x34}); payload.push_back(std::byte{0});
        append_u32(payload, 5); append_f32(payload, 3.0F);
        append_u32(payload, 9); append_u32(payload, 6);
        append_f32(payload, 1); append_f32(payload, 2); append_f32(payload, 3);
        append_u32(payload, 7); append_f32(payload, 0); append_f32(payload, 0); append_f32(payload, 0); append_f32(payload, 1);
        for (float value : {1.0F, 0.25F, 0.5F}) { append_u32(payload, 5); append_f32(payload, value); }
        append_u32(payload, 6); append_f32(payload, 1); append_f32(payload, 0); append_f32(payload, 0);
        append_u32(payload, 3); append_u32(payload, 3);
        append_u32(payload, 6); append_f32(payload, -1); append_f32(payload, 0.25F); append_f32(payload, 0.5F);
        std::vector<std::byte> bytes;
        append_header(bytes, 0x907, static_cast<std::uint32_t>(12 + payload.size()), 0x1C020018);
        append_header(bytes, 1, static_cast<std::uint32_t>(payload.size()), 0x1C020018);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto body = rws::decode_physics_body_def(document.chunks()[0], document.bytes());
        assert(body && body.value->volume.kind == 0x11 && body.value->mass == 3.0F);
        assert(body.value->volume.cylinder_radius == 3.0F);
        assert(body.value->volume.cylinder_half_height == 5.75F);
        assert(body.value->volume.fatness == 0.0F);
        assert(body.value->volume.friction == 0.2F && body.value->volume.restitution == 0.2F);
        assert(body.value->volume.flags == 0x12 && body.value->volume.collision_group == 0x34);
        assert(body.value->principal_inertia.x == 1.0F && body.value->flags == 3);
        assert(body.value->linear_damping == 0.25F && body.value->angular_damping == 0.5F);
        assert(body.value->finite_rotation_axis.x == 1.0F);
        assert(rws::has_physics_body_flag(body.value->flags,
                                          rws::PhysicsBodyFlag::finite_rotation_axis));
        assert(rws::physics_body_flag_names(body.value->flags) ==
               "finite-rotation axis, oriented inertia");
        assert(body.value->center_of_mass.x == -1.0F);
    }
    {
        std::vector<std::byte> payload;
        append_u32(payload, 0x18); append_u32(payload, 0x00010017);
        append_u32(payload, 0x0B); append_u32(payload, 3); append_u32(payload, 0x13);
        append_u32(payload, 0x13); append_u32(payload, 0x00010015);
        append_u32(payload, 3); append_u32(payload, 1);
        append_u32(payload, 0x00010017);
        append_u32(payload, 0x0B); append_u32(payload, 3); append_u32(payload, 0x0E);
        append_u32(payload, 0x0E);
        append_u32(payload, 8);
        for (int i = 0; i < 12; ++i) append_f32(payload, i % 5 == 0 ? 1.0F : 0.0F);
        for (float value : {2.0F, 0.3F, 0.1F}) { append_u32(payload, 5); append_f32(payload, value); }
        append_u32(payload, 1); append_u16(payload, 0);
        append_u32(payload, 1); append_u16(payload, 0);
        append_u32(payload, 5); append_f32(payload, 12.0F);
        append_u32(payload, 6); append_f32(payload, 1.0F); append_f32(payload, 2.0F);
        append_f32(payload, 3.0F);
        append_u32(payload, 9); append_u32(payload, 6);
        append_f32(payload, 4.0F); append_f32(payload, 5.0F); append_f32(payload, 6.0F);
        append_u32(payload, 7); append_f32(payload, 0.0F); append_f32(payload, 0.0F);
        append_f32(payload, 0.0F); append_f32(payload, 1.0F);
        append_u32(payload, 8);
        for (int i = 0; i < 12; ++i) append_f32(payload, i % 5 == 0 ? 1.0F : 0.0F);
        for (float value : {0.0F, 0.4F, 0.2F}) { append_u32(payload, 5); append_f32(payload, value); }
        append_u32(payload, 1); append_u16(payload, 0);
        append_u32(payload, 1); append_u16(payload, 0);
        append_u32(payload, 5); append_f32(payload, 12.0F);
        append_u32(payload, 9); append_u32(payload, 6);
        append_f32(payload, 4.0F); append_f32(payload, 5.0F); append_f32(payload, 6.0F);
        append_u32(payload, 7); append_f32(payload, 0.0F); append_f32(payload, 0.0F);
        append_f32(payload, 0.0F); append_f32(payload, 1.0F);
        for (float value : {1.0F, 0.0F, 0.0F}) { append_u32(payload, 5); append_f32(payload, value); }
        append_u32(payload, 6); append_f32(payload, 0.0F); append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        append_u32(payload, 3); append_u32(payload, 0);
        append_u32(payload, 6); append_f32(payload, 0.0F); append_f32(payload, 0.0F);
        append_f32(payload, 0.0F);
        std::vector<std::byte> bytes;
        append_header(bytes, 0x907, static_cast<std::uint32_t>(12 + payload.size()), 0x1C020018);
        append_header(bytes, 1, static_cast<std::uint32_t>(payload.size()), 0x1C020018);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto body = rws::decode_physics_body_def(document.chunks()[0], document.bytes());
        assert(body && body.value->volume.kind == 0x13 && body.value->volume.children.size() == 1);
        assert(body.value->volume.trilist_mass == 12.0F);
        assert(body.value->volume.trilist_center_of_mass->y == 2.0F);
        assert(body.value->volume.trilist_principal_inertia->z == 6.0F);
        assert((*body.value->volume.trilist_inertia_orientation)[3] == 1.0F);
    }
    {
        std::vector<std::byte> first, second;
        append_world_sector(first, 0, 0, 3, {{{0, 1, 2, 0}}});
        append_world_sector(second, 0, 1, 3, {{{2, 1, 0, 0}}});
        auto bytes = make_world(0, 2, 2, 6, 2, {first, second});
        const auto original = bytes;
        const auto document = rws::Document::from_bytes(std::move(bytes));
        const auto worlds = rws::recover_worlds(document.chunks(), document.bytes());
        assert(worlds.size() == 1);
        const auto& world = worlds.front();
        assert(world.status == rws::WorldRecoveryStatus::complete);
        assert(world.sectors.size() == 2 && world.recovered_triangles == 2 &&
               world.recovered_vertices == 6 && world.material_count == 2);
        assert(world.sectors[1].material_window_base == 1);
        assert(std::equal(original.begin(), original.end(), document.bytes().begin()));
    }
    {
        constexpr std::uint32_t format = 0x00020018U; // normals, prelight, two UV sets
        std::vector<std::byte> sector;
        append_world_sector(sector, format, 0, 3, {{{0, 1, 2, 0}}});
        const auto document = rws::Document::from_bytes(make_world(format, 1, 1, 3, 1, {sector}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        assert(world.status == rws::WorldRecoveryStatus::complete && world.sectors.size() == 1);
        const auto& recovered = world.sectors.front();
        assert(recovered.normals_offset != 0 && recovered.prelight_offset != 0);
        assert(recovered.texcoord_offsets.size() == 2);
        assert(recovered.prelight_offset - recovered.normals_offset == 12);
        assert(recovered.texcoord_offsets[1] - recovered.texcoord_offsets[0] == 24);
        assert(recovered.triangles_offset - recovered.texcoord_offsets[1] == 24);
    }
    {
        std::vector<std::byte> sector;
        append_world_sector(sector, 0, 2, 3, {{{0, 1, 9, 0}}});
        const auto document = rws::Document::from_bytes(make_world(0, 2, 2, 6, 1, {sector}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        assert(world.status == rws::WorldRecoveryStatus::partial);
        assert(world.sectors.size() == 1 && world.invalid_triangles == 1);
        assert(world.invalid_material_references == 0); // invalid vertices take precedence
        assert(!world.diagnostics.empty());
    }
    {
        std::vector<std::byte> malformed;
        append_world_sector(malformed, 0, 0, -1, {});
        const auto document = rws::Document::from_bytes(make_world(0, 1, 0, 0, 1, {malformed}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        assert(world.status == rws::WorldRecoveryStatus::failed);
        assert(world.sectors.empty() && world.invalid_candidates == 1);
    }
    {
        std::vector<std::byte> sector;
        append_world_sector(sector, 0, 1, 3, {{{0, 1, 2, 0}}});
        const auto document = rws::Document::from_bytes(make_world(0, 1, 1, 3, 1, {sector}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        assert(world.status == rws::WorldRecoveryStatus::partial);
        assert(world.invalid_material_references == 1 && world.invalid_triangles == 0);
    }
    {
        std::vector<std::byte> sector;
        append_world_sector(sector, 0, 0, 3, {{{0, 1, 2, 0}}});
        // A complete header signature inside vertex bytes must not be scanned after
        // the enclosing sector's exact Struct range has been accepted.
        const auto write_u32 = [&](const std::size_t offset, const std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                sector[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xFFU);
        };
        write_u32(68, 0x09); write_u32(76, 0x1C020037);
        write_u32(80, 0x01); write_u32(88, 0x1C020037);
        const auto document = rws::Document::from_bytes(make_world(0, 1, 1, 3, 1, {sector}));
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        assert(world.status == rws::WorldRecoveryStatus::complete);
        assert(world.sectors.size() == 1 && world.invalid_candidates == 0);
    }
    {
        std::vector<std::byte> plane, left, right;
        append_plane(plane, 0, true, true, 1.0F, 4.0F, -1.0F);
        append_world_sector(left, 0, 0, 3, {{{0, 1, 2, 0}}});
        append_world_sector(right, 0, 0, 3, {{{0, 1, 2, 0}}});
        const auto bytes = make_world(0, 2, 2, 6, 1, {plane, left, right}, 1, false);
        const auto original = bytes;
        const auto document = rws::Document::from_bytes(bytes);
        const auto world = rws::recover_world(document.chunks()[0], document.bytes());
        assert(world.topology_status == rws::WorldTopologyStatus::complete);
        assert(world.planes.size() == 1 && world.topology_nodes.size() == 3);
        assert(world.topology_root == 0 && world.planes[0].left_node == 1 && world.planes[0].right_node == 2);
        assert(world.topology_nodes[1].parent == 0 && world.topology_nodes[1].is_left_child == true);
        assert(world.topology_nodes[2].parent == 0 && world.topology_nodes[2].is_left_child == false);
        assert(world.topology_stats.maximum_depth == 1 && world.topology_stats.linked_sectors == 2);
        assert(std::equal(original.begin(), original.end(), document.bytes().begin()));
    }
    {
        std::vector<std::byte> root, branch, a, b, c;
        append_plane(root, 0, false, true, 1.0F, 4.0F, -1.0F);
        append_plane(branch, 8, true, true, 0.0F, 6.0F, -3.0F);
        append_world_sector(a,0,0,3,{{{0,1,2,0}}});
        append_world_sector(b,0,0,3,{{{0,1,2,0}}});
        append_world_sector(c,0,0,3,{{{0,1,2,0}}});
        const auto document=rws::Document::from_bytes(make_world(0,3,3,9,1,{root,branch,a,b,c},2,false));
        const auto world=rws::recover_world(document.chunks()[0],document.bytes());
        assert(world.topology_status==rws::WorldTopologyStatus::complete);
        assert(world.topology_nodes.size()==5&&world.topology_stats.maximum_depth==2);
        assert(world.topology_nodes[4].parent==0&&world.topology_nodes[4].is_left_child==false);
    }
    {
        std::vector<std::byte> invalid, sector;
        append_plane(invalid, 3, true, true, std::numeric_limits<float>::infinity(), 4.0F, -1.0F);
        append_world_sector(sector,0,0,3,{{{0,1,2,0}}});
        const auto document=rws::Document::from_bytes(make_world(0,1,1,3,1,{invalid,sector},1,false));
        const auto world=rws::recover_world(document.chunks()[0],document.bytes());
        assert(world.status==rws::WorldRecoveryStatus::complete&&world.sectors.size()==1);
        assert(world.topology_status==rws::WorldTopologyStatus::failed);
        assert(world.topology_stats.invalid_candidates==1||world.topology_stats.ambiguous_candidates==1);
    }
    {
        std::vector<std::byte> sector;
        append_world_sector(sector, 0, 0, 3, {{{0, 1, 2, 0}}});
        // Positions begin 68 bytes into this synthetic Atomic Section.
        for (const auto [offset, value] : std::array<std::pair<std::size_t, float>, 9>{{
            {68,0.0F},{72,0.0F},{76,0.0F},{80,1.0F},{84,0.0F},{88,0.0F},
            {92,0.0F},{96,1.0F},{100,0.0F}}})
            write_f32(sector, offset, value);
        const auto bytes = make_world(0, 1, 1, 3, 1, {sector});
        const auto original = bytes;
        const auto document = rws::Document::from_bytes(bytes);
        const auto worlds = rws::recover_worlds(document.chunks(), document.bytes());
        const auto vertex = rws::decode_recovered_world_vertex(worlds[0].sectors[0], 2, document.bytes());
        assert(vertex && vertex.value->x == 0 && vertex.value->y == 1);
        assert(!rws::decode_recovered_world_vertex(worlds[0].sectors[0], -1, document.bytes()));
        assert(!rws::decode_recovered_world_vertex(worlds[0].sectors[0], 3, document.bytes()));
        const auto triangle = rws::decode_recovered_world_triangle_resolved(worlds[0], 0, 0, document.bytes());
        assert(triangle && triangle.value->source_offset == worlds[0].sectors[0].triangles_offset);
        assert(!rws::decode_recovered_world_triangle_resolved(worlds[0], 1, 0, document.bytes()));
        const rws::CollisionRay ray{{0.25F,0.25F,1.0F},{0,0,-1}};
        const auto hit = rws::pick_collision_worlds(worlds, document.bytes(), ray);
        assert(hit && hit->sector_index == 0 && hit->triangle_index == 0 && hit->material_slot == 0);
        assert(std::abs(hit->position.z) < 1.0e-6F && hit->geometric_normal.z > 0.99F);
        assert(std::abs(hit->barycentric[0] - 0.5F) < 1.0e-5F);
        const std::array clips{rws::CollisionClipPlane{true, 0, true, 0.5F}};
        assert(!rws::pick_collision_worlds(worlds, document.bytes(), ray, clips));
        const auto measurement = rws::measure_points({0,0,0},{3,4,12});
        assert(measurement.distance == 13 && measurement.absolute_delta.y == 4);
        assert(std::equal(original.begin(), original.end(), document.bytes().begin()));

        const auto directory = std::filesystem::temp_directory_path() / "rws-man-s02-tests";
        std::filesystem::create_directories(directory);
        const auto gltf = directory / "collision.gltf";
        const auto obj = directory / "collision.obj";
        const auto gltf_stats = rws::export_collision_gltf(document.chunks(), document.bytes(), gltf);
        const auto obj_stats = rws::export_collision_obj(document.chunks(), document.bytes(), obj);
        assert(gltf_stats.triangles == 1 && obj_stats.triangles == 1 && obj_stats.skipped_triangles == 0);
        const auto read_text = [](const std::filesystem::path& path) {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        const auto gltf_text = read_text(gltf), obj_text = read_text(obj);
        assert(gltf_text.find("rws_source_offset") != std::string::npos);
        assert(gltf_text.find("rws_sector_index") != std::string::npos);
        assert(gltf_text.find("NORMAL") != std::string::npos);
        assert(obj_text.find("world_0_sector_0") != std::string::npos);
        assert(obj_text.find("usemtl world_0_collision_material_0") != std::string::npos);
        assert(std::equal(original.begin(), original.end(), document.bytes().begin()));
        std::filesystem::remove_all(directory);
    }
    return 0;
}
