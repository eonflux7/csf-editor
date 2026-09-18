#include "rws/document.hpp"
#include "rws/decoded.hpp"

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
               "enabled, animated, scene-registered");
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
        for (int i = 0; i < 4; ++i) append_u32(bytes, 0); // one triangle
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
        constexpr std::uint32_t texture_payload_size = 52;
        constexpr std::uint32_t effects_payload_size = 88;
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
        append_u32(payload, 1); append_u32(payload, 0);
        append_u32(payload, 1); append_u32(payload, 0);
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
        append_u32(payload, 1); append_u32(payload, 0);
        append_u32(payload, 1); append_u32(payload, 0);
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
    return 0;
}
