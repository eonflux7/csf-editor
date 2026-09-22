#include "app_state.hpp"
#include "app_util.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "ui/theme.hpp"

#include "rws/animation.hpp"
#include "rws/decoded.hpp"
#include "rws/obj_export.hpp"
#include "rws/physics_inspection.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <string>

namespace rwsman::ui {
namespace {

void draw_vec3(const char* label, const rws::Vec3& value) {
    ImGui::Text("%s: %.4f, %.4f, %.4f", label, value.x, value.y, value.z);
}

const char* physics_volume_kind_name(const std::uint32_t kind) {
    switch (kind) {
    case 0x0E:
        return "Sphere";
    case 0x0F:
        return "Capsule";
    case 0x10:
        return "Box";
    case 0x11:
        return "Cylinder";
    case 0x13:
        return "Trilist";
    default:
        return "Unknown";
    }
}

void draw_physics_volume(const rws::PhysicsVolumeInfo& volume, const char* label) {
    if (!ImGui::TreeNode(&volume, "%s: %s (0x%X)", label, physics_volume_kind_name(volume.kind),
                         volume.kind))
        return;
    ImGui::Text("Version: %u | collision group: %u | flags: 0x%X", volume.version,
                volume.collision_group, volume.flags);
    ImGui::Text("Fatness: %.6g | friction: %.6g | restitution: %.6g", volume.fatness,
                volume.friction, volume.restitution);
    ImGui::Text("Local matrix: [%.4g %.4g %.4g | %.4g]", volume.matrix[0], volume.matrix[3],
                volume.matrix[6], volume.matrix[9]);
    ImGui::Text("              [%.4g %.4g %.4g | %.4g]", volume.matrix[1], volume.matrix[4],
                volume.matrix[7], volume.matrix[10]);
    ImGui::Text("              [%.4g %.4g %.4g | %.4g]", volume.matrix[2], volume.matrix[5],
                volume.matrix[8], volume.matrix[11]);
    if (volume.capsule_half_height) {
        ImGui::Text("Radius: %.6g | half-height: %.6g", volume.fatness,
                    *volume.capsule_half_height);
    } else if (volume.box_half_extents) {
        draw_vec3("Half-extents", *volume.box_half_extents);
    } else if (volume.cylinder_radius && volume.cylinder_half_height) {
        ImGui::Text("Radius: %.6g | half-height: %.6g", *volume.cylinder_radius,
                    *volume.cylinder_half_height);
    }
    if (volume.trilist_mass) {
        ImGui::Text("Cached mass: %.6g", *volume.trilist_mass);
        draw_vec3("Center of mass", *volume.trilist_center_of_mass);
        draw_vec3("Principal inertia", *volume.trilist_principal_inertia);
        const auto& orientation = *volume.trilist_inertia_orientation;
        ImGui::Text("Inertia orientation: %.5g, %.5g, %.5g, %.5g", orientation[0], orientation[1],
                    orientation[2], orientation[3]);
    }
    for (std::size_t index = 0; index < volume.children.size(); ++index) {
        const auto child_label = "Child " + std::to_string(index);
        draw_physics_volume(volume.children[index], child_label.c_str());
    }
    ImGui::TreePop();
}

} // namespace

void draw_typed_details(AppState& state, const rws::Chunk& chunk,
                        const std::uint32_t parent_type) {
    auto& document = *state.document;
    auto& status = state.status;
    const auto bytes = document.bytes();
    switch (chunk.type) {
    case 0x06: {
        const auto decoded = rws::decode_texture(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Name: %s", decoded.value->name.c_str());
        ImGui::Text("Mask: %s", decoded.value->mask_name.c_str());
        ImGui::Text("Filter: %u | address U/V: %u/%u | packed: 0x%08X", decoded.value->filter_mode,
                    decoded.value->address_u, decoded.value->address_v,
                    decoded.value->filter_addressing);
        break;
    }
    case 0x08: {
        const auto decoded = rws::decode_material_list(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Materials: %d | remap entries: %zu", decoded.value->material_count,
                    decoded.value->remap.size());
        break;
    }
    case 0x07: {
        const auto decoded = rws::decode_material(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("RGBA: %u, %u, %u, %u | textured: %s", value.color[0], value.color[1],
                    value.color[2], value.color[3], value.textured ? "yes" : "no");
        ImGui::Text("Surface: ambient %.3f, specular %.3f, diffuse %.3f", value.ambient,
                    value.specular, value.diffuse);
        break;
    }
    case 0x09: {
        const auto decoded = rws::decode_world_sector(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("Vertices: %d | triangles: %d | material base: %d", value.vertex_count,
                    value.triangle_count, value.material_window_base);
        draw_vec3("Bounds min", value.bounding_box_inf);
        draw_vec3("Bounds max", value.bounding_box_sup);
        break;
    }
    case 0x0A: {
        const auto decoded = rws::decode_plane_sector(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("Axis: %d | split: %.4f | left %.4f (%s) | right %.4f (%s)", value.axis,
                    value.split, value.left_value, value.left_is_world_sector ? "leaf" : "branch",
                    value.right_value, value.right_is_world_sector ? "leaf" : "branch");
        break;
    }
    case 0x0B: {
        const auto decoded = rws::decode_world(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("Vertices: %d | triangles: %d | planes: %d | leaves: %d", value.vertex_count,
                    value.triangle_count, value.plane_sector_count, value.world_sector_count);
        ImGui::Text("Format: 0x%08X | root is %s", value.format,
                    value.root_is_world_sector ? "world sector" : "plane sector");
        draw_vec3("Bounds max", value.bounding_box_sup);
        draw_vec3("Bounds min", value.bounding_box_inf);
        break;
    }
    case 0x0E: {
        const auto decoded = rws::decode_frame_list(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Frames: %zu", decoded.value->frames.size());
        if (ImGui::BeginTable("frames", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Index");
            ImGui::TableSetupColumn("Parent");
            ImGui::TableSetupColumn("Position");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < decoded.value->frames.size(); ++i) {
                const auto& frame = decoded.value->frames[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%zu", i);
                ImGui::TableNextColumn();
                ImGui::Text("%d", frame.parent);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f, %.3f, %.3f", frame.position.x, frame.position.y,
                            frame.position.z);
            }
            ImGui::EndTable();
        }
        break;
    }
    case 0x0F: {
        const auto decoded = rws::decode_geometry(chunk, bytes);
        if (!decoded) {
            ImGui::TextColored(color(Token::error), "%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("Vertices: %d | triangles: %d | morph targets: %d | UV sets: %u",
                    value.vertex_count, value.triangle_count, value.morph_target_count,
                    value.texcoord_sets);
        ImGui::Text("Format: 0x%08X | Struct bytes: %llu (validated)", value.format,
                    static_cast<unsigned long long>(value.computed_size));
        for (std::size_t i = 0; i < value.morph_targets.size(); ++i) {
            const auto& morph = value.morph_targets[i];
            ImGui::Text("Morph %zu: radius %.3f | vertices %s | normals %s", i, morph.sphere.radius,
                        morph.has_vertices ? "yes" : "no", morph.has_normals ? "yes" : "no");
        }
        const char* layout =
            value.triangle_layout == rws::TriangleLayout::stream_order   ? "RenderWare stream"
            : value.triangle_layout == rws::TriangleLayout::memory_order ? "memory order"
                                                                         : "ambiguous";
        ImGui::Text("Triangle layout: %s | materials: %d", layout, value.material_count);
        if (value.triangle_layout != rws::TriangleLayout::unknown &&
            ImGui::Button("Export this geometry to OBJ")) {
            try {
                std::ostringstream suffix;
                suffix << ".geometry_" << std::hex << chunk.offset << ".obj";
                auto output = with_stem_suffix(document.source_path(), suffix.str(), false);
                // Exports never replace an existing file unless the policy says so.
                if (state.settings.export_policy == ExportPolicy::new_files_only) output = unique_output_path(output);
                rws::export_geometry_obj(value, bytes, output);
                state.notify(LogLevel::ok, "Exported " + path_utf8(output), output.parent_path());
            } catch (const std::exception& error) {
                state.notify(LogLevel::error, std::string("Geometry export failed: ") + error.what());
            }
        }
        break;
    }
    case 0x10: {
        const auto decoded = rws::decode_clump(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Atomics: %d | lights: %d | cameras: %d", decoded.value->atomics,
                    decoded.value->lights, decoded.value->cameras);
        break;
    }
    case 0x1B: {
        const auto clip = rws::decode_animation(chunk, bytes);
        ImGui::Text("Version: 0x%X | interpolator: %u", clip.version, clip.interpolation_type);
        ImGui::Text("Layout: %s", rws::animation_layout_name(clip.layout));
        ImGui::Text("Duration: %.6g s | keys: %zu/%u | tracks: %zu", clip.duration,
                    clip.keyframes.size(), clip.declared_keyframe_count, clip.tracks.size());
        ImGui::Text("Record bytes: %u | logical previous stride: %u | flags: 0x%X",
                    clip.serialized_record_size, clip.logical_record_stride, clip.flags);
        static std::filesystem::path active_source;
        static float time{}, speed{1};
        static bool playing{}, loop{true};
        if (active_source != document.source_path()) {
            active_source = document.source_path();
            time = 0;
            playing = false;
        }
        if (playing && clip.duration > 0) {
            time += ImGui::GetIO().DeltaTime * speed;
            if (loop)
                time = std::fmod(std::max(0.0F, time), clip.duration);
            else if (time >= clip.duration) {
                time = clip.duration;
                playing = false;
            }
        }
        if (ImGui::Button(playing ? "Pause" : "Play")) playing = !playing;
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            time = 0;
            playing = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Step -")) {
            time = std::max(0.0F, time - 1.0F / 30.0F);
            playing = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Step +")) {
            time = std::min(clip.duration, time + 1.0F / 30.0F);
            playing = false;
        }
        ImGui::Checkbox("Loop", &loop);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::SliderFloat("Speed", &speed, 0.05F, 4.0F, "%.2fx");
        ImGui::SliderFloat("Timeline", &time, 0.0F, std::max(clip.duration, 0.001F), "%.3f s");
        const auto path = rws::extract_root_motion(clip, 32);
        if (path.size() > 1) {
            const auto &a = path.front(), &b = path.back();
            ImGui::Text("Root motion: (%.4g, %.4g, %.4g) -> (%.4g, %.4g, %.4g)", a.x, a.y, a.z, b.x,
                        b.y, b.z);
        }
        if (ImGui::TreeNode("Tracks and source-stable keys")) {
            for (const auto& track : clip.tracks) {
                if (ImGui::TreeNode(
                        &track, "Track %d (%zu keys)%s", track.track_index, track.keyframes.size(),
                        track.node_id ? ((" | node " + std::to_string(*track.node_id)).c_str())
                                      : "")) {
                    for (auto index : track.keyframes) {
                        const auto& key = clip.keyframes[index];
                        ImGui::Text("#%u @0x%llX  t=%.5g  prev=%d", index,
                                    static_cast<unsigned long long>(key.source_offset), key.time,
                                    key.previous_keyframe);
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        for (const auto& diagnostic : clip.diagnostics)
            ImGui::TextColored(diagnostic.severity == rws::AnimationDiagnostic::Severity::error
                                   ? color(Token::error)
                                   : color(Token::warn),
                               "%s @0x%llX: %s", diagnostic.code.c_str(),
                               static_cast<unsigned long long>(diagnostic.offset),
                               diagnostic.message.c_str());
        break;
    }
    case 0x14: {
        const auto decoded = rws::decode_atomic(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Frame index: %d | geometry index: %d | flags: 0x%08X",
                    decoded.value->frame_index, decoded.value->geometry_index,
                    decoded.value->flags);
        break;
    }
    case 0x1F: {
        const auto decoded = rws::decode_right_to_render(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Pipeline plugin: 0x%08X (%s) | extra data: 0x%08X", decoded.value->plugin_id,
                    rws::chunk_name(decoded.value->plugin_id).data(), decoded.value->extra_data);
        break;
    }
    case 0x24: {
        const auto decoded = rws::decode_table_of_contents(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Entries: %zu", decoded.value->entries.size());
        for (const auto& entry : decoded.value->entries) {
            ImGui::BulletText("%s (0x%X) at 0x%08X | object ID 0x%08X",
                              rws::chunk_name(entry.chunk_type).data(), entry.chunk_type,
                              entry.offset, entry.object_id);
        }
        break;
    }
    case 0x11E: {
        const auto decoded = rws::decode_hanim(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("HAnim version: 0x%08X | hierarchy ID: %d | nodes: %zu", decoded.value->version,
                    decoded.value->hierarchy_id, decoded.value->nodes.size());
        ImGui::Text("Flags: 0x%08X | keyframe size: %u", decoded.value->flags,
                    decoded.value->keyframe_size);
        break;
    }
    case 0x11D: {
        const auto decoded = rws::decode_collision_tree(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("Collision tree version: 0x%08X | flags: 0x%08X", value.version, value.flags);
        ImGui::Text("Triangles: %u | splits: %u | triangle map: %zu", value.triangle_count,
                    value.split_count, value.triangle_map.size());
        draw_vec3("Bounds min", value.bounding_box_inf);
        draw_vec3("Bounds max", value.bounding_box_sup);
        if (!value.splits.empty()) {
            const auto& root = value.splits.front();
            ImGui::TextDisabled("Root sectors: left %u/%u/%u @ %.5g | right %u/%u/%u @ %.5g",
                                root.left.type, root.left.flags, root.left.index, root.left.value,
                                root.right.type, root.right.flags, root.right.index,
                                root.right.value);
        }
        break;
    }
    case 0x116: {
        const auto* geometry_chunk = find_owning_geometry(document.chunks(), chunk.offset);
        if (!geometry_chunk) {
            ImGui::TextDisabled("Skin is not inside a Geometry chunk");
            break;
        }
        const auto geometry = rws::decode_geometry(*geometry_chunk, bytes);
        if (!geometry) {
            ImGui::TextDisabled("Owning Geometry: %s", geometry.error.c_str());
            break;
        }
        const auto decoded = rws::decode_skin(chunk, geometry.value->vertex_count, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("Bones: %u (%u used) | vertices: %d | max weights: %u", value.bone_count,
                    value.used_bone_count, value.vertex_count, value.max_weights_per_vertex);
        ImGui::Text("Split: bone limit %u | meshes %u | RLE entries %u | trailing bytes %llu",
                    value.bone_limit, value.mesh_count, value.rle_count,
                    static_cast<unsigned long long>(value.trailing_split_bytes));
        break;
    }
    case 0x11F: {
        const auto decoded = rws::decode_user_data(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Arrays: %zu", decoded.value->arrays.size());
        for (const auto& array : decoded.value->arrays) {
            const char* format = array.format == rws::UserDataFormat::integer ? "int"
                                 : array.format == rws::UserDataFormat::real  ? "real"
                                                                              : "string";
            const auto count = array.format == rws::UserDataFormat::integer ? array.integers.size()
                               : array.format == rws::UserDataFormat::real  ? array.reals.size()
                                                                            : array.strings.size();
            ImGui::BulletText("%s: %s[%zu]", array.name.c_str(), format, count);
            if (count == 1) {
                ImGui::SameLine();
                if (array.format == rws::UserDataFormat::integer)
                    ImGui::Text("= %d (0x%X)", array.integers[0],
                                static_cast<std::uint32_t>(array.integers[0]));
                else if (array.format == rws::UserDataFormat::real)
                    ImGui::Text("= %.6g", array.reals[0]);
                else
                    ImGui::Text("= %s", array.strings[0].c_str());
            }
        }
        break;
    }
    case 0x120: {
        const auto decoded = rws::decode_material_effects(chunk, parent_type, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        if (parent_type == 0x14 || parent_type == 0x09) {
            ImGui::Text("MatFX rendering pipeline: %s",
                        value.pipeline_enabled ? "enabled" : "disabled");
        } else {
            ImGui::Text("Effect: %u | slot: %u | dual texture: %s", value.effect_type,
                        value.slot_type, value.has_dual_texture ? "present" : "absent");
            ImGui::Text("Blend source/destination: %u/%u", value.source_blend,
                        value.destination_blend);
            if (value.has_dual_texture) {
                ImGui::Text("Dual texture: %s", value.dual_texture.name.c_str());
                ImGui::Text("Filter: %u | address U/V: %u/%u", value.dual_texture.filter_mode,
                            value.dual_texture.address_u, value.dual_texture.address_v);
            }
        }
        break;
    }
    case 0x127: {
        const auto decoded = rws::decode_anisotropy(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Anisotropy coefficient: %.4f", decoded.value->coefficient);
        break;
    }
    case 0x50E: {
        const auto decoded = rws::decode_bin_mesh(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        ImGui::Text("Meshes: %zu | indices: %u | flags: 0x%08X", decoded.value->meshes.size(),
                    decoded.value->total_indices, decoded.value->flags);
        break;
    }
    case 0x907: {
        const auto decoded = rws::decode_physics_body_def(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("RwpBodyDef | root volume kind: 0x%X | version: %u", value.volume.kind,
                    value.volume.version);
        draw_physics_volume(value.volume, "Root volume");
        ImGui::Text("Mass: %.6g | scalar inertia: %.6g | body flags: 0x%08X", value.mass,
                    value.scalar_inertia, value.flags);
        ImGui::Text("Flag meanings: %s", rws::physics_body_flag_names(value.flags).c_str());
        draw_vec3("Center of mass", value.center_of_mass);
        draw_vec3("Principal inertia", value.principal_inertia);
        ImGui::Text("Inertia orientation: %.5g, %.5g, %.5g, %.5g", value.inertia_orientation[0],
                    value.inertia_orientation[1], value.inertia_orientation[2],
                    value.inertia_orientation[3]);
        ImGui::Text("Linear damping: %.6g | angular damping: %.6g", value.linear_damping,
                    value.angular_damping);
        draw_vec3("Finite-rotation axis", value.finite_rotation_axis);
        break;
    }
    case 0x909: {
        const auto decoded = rws::decode_physics_ragdoll_def(chunk, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("RwpRagdollDef | types: %u/%u", value.type_0, value.type_1);
        ImGui::Text("Bodies: %u | joints: %u | lookup table: %u x %u (%zu values)",
                    value.body_count, value.joint_count, value.table_rows, value.table_columns,
                    value.table_values.size());
        ImGui::Text("Body IDs: %zu | integer field: %u", value.body_ids.size(),
                    value.integer_field);
        if (ImGui::BeginTable("ragdoll_bodies", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Body");
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Volume");
            ImGui::TableSetupColumn("Mass");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < value.bodies.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%zu", i);
                ImGui::TableNextColumn();
                if (i < value.body_ids.size())
                    ImGui::Text("%u", value.body_ids[i]);
                else
                    ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::Text("%s", physics_volume_kind_name(value.bodies[i].volume.kind));
                ImGui::TableNextColumn();
                ImGui::Text("%.6g", value.bodies[i].mass);
            }
            ImGui::EndTable();
        }
        if (ImGui::TreeNode("Joint links and unresolved typed records")) {
            for (std::size_t i = 0; i < value.joints.size(); ++i) {
                const auto& joint = value.joints[i];
                const auto pair = i < value.joint_pairs.size() ? value.joint_pairs[i]
                                                               : std::array<std::uint16_t, 2>{};
                if (ImGui::TreeNode(&joint, "Joint %zu: bodies %u -> %u | record type %u | @0x%llX",
                                    i, pair[0], pair[1], joint.type,
                                    static_cast<unsigned long long>(joint.source_offset))) {
                    for (std::size_t field = 0; field < joint.integers.size(); ++field)
                        ImGui::Text("u32[%zu] = %u", field, joint.integers[field]);
                    for (std::size_t field = 0; field < joint.triple_records.size(); ++field)
                        ImGui::Text("0x1A[%zu] = %.6g, %.6g, %.6g", field,
                                    joint.triple_records[field][0], joint.triple_records[field][1],
                                    joint.triple_records[field][2]);
                    ImGui::TextDisabled("Numeric fields retain record order; unresolved meanings "
                                        "are intentionally not inferred.");
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
        break;
    }
    case 0xFFFFFF00U: {
        // Short collision-World leaf declarations can make a World Sector plug-in
        // appear directly below its enclosing Plane Section in the recovered tree.
        const auto owner_type = parent_type == 0x0A ? 0x09U : parent_type;
        const auto decoded = rws::decode_pyro_extension(chunk, owner_type, bytes);
        if (!decoded) {
            ImGui::TextDisabled("%s", decoded.error.c_str());
            break;
        }
        const auto& value = *decoded.value;
        ImGui::Text("Pyro plugin | owner: %s (0x%X) | version: %u",
                    rws::chunk_name(value.owner_type).data(), value.owner_type, value.version);
        ImGui::Text("Fields: %zu | strings: %zu | optional record/bounds: %s", value.words.size(),
                    value.strings.size(), value.present ? "present" : "absent");
        if (const auto flags = value.material_flags())
            ImGui::Text("Material flags/mask: 0x%08X", *flags);
        if (const auto surface = value.material_surface_type())
            ImGui::Text("Material surface type: %u", *surface);
        if (!value.object_name().empty())
            ImGui::Text("Object/surface name: %s", value.object_name().data());
        if (!value.words.empty() && ImGui::TreeNode("Raw numeric fields")) {
            for (std::size_t i = 0; i < value.words.size(); ++i)
                ImGui::Text("[%zu] %u (0x%08X)", i, value.words[i], value.words[i]);
            ImGui::TreePop();
        }
        for (std::size_t i = 0; i < value.strings.size(); ++i)
            ImGui::TextDisabled("String %zu: %s", i, value.strings[i].c_str());
        if (value.bounds) {
            ImGui::Text("Raw bounds pairs: %.4g/%.4g, %.4g/%.4g, %.4g/%.4g", (*value.bounds)[0],
                        (*value.bounds)[1], (*value.bounds)[2], (*value.bounds)[3],
                        (*value.bounds)[4], (*value.bounds)[5]);
        }
        if (!value.world_sector_vertex_bytes.empty()) {
            const auto [minimum, maximum] = std::minmax_element(
                value.world_sector_vertex_bytes.begin(), value.world_sector_vertex_bytes.end());
            ImGui::Text("World Sector per-vertex bytes: %zu (range %u..%u)",
                        value.world_sector_vertex_bytes.size(), static_cast<unsigned>(*minimum),
                        static_cast<unsigned>(*maximum));
        }
        break;
    }
    default:
        ImGui::TextDisabled("No typed decoder for this chunk yet.");
        break;
    }
}

} // namespace rwsman::ui
