#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

#include "rws/document.hpp"
#include "rws/decoded.hpp"
#include "rws/obj_export.hpp"
#include "rws/scene_export.hpp"
#include "geometry_preview.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

namespace {

std::optional<std::filesystem::path> dropped_file;

void drop_callback(GLFWwindow*, const int count, const char** paths) {
    if (count > 0) {
        dropped_file = std::filesystem::path(paths[0]);
    }
}

const rws::Chunk* find_chunk(const std::vector<rws::Chunk>& chunks, const std::uint64_t offset) {
    for (const auto& chunk : chunks) {
        if (chunk.offset == offset) return &chunk;
        if (const auto* child = find_chunk(chunk.children, offset)) return child;
    }
    return nullptr;
}

const rws::SceneInstance* find_instance(const std::span<const rws::SceneInstance> instances,
                                        const std::uint64_t offset) {
    const auto found = std::find_if(instances.begin(), instances.end(),
        [offset](const rws::SceneInstance& instance) { return instance.offset == offset; });
    return found == instances.end() ? nullptr : &*found;
}

const rws::Chunk* find_first_chunk(const std::vector<rws::Chunk>& chunks, const std::uint32_t type) {
    for (const auto& chunk : chunks) {
        if (chunk.type == type) return &chunk;
        if (const auto* child = find_first_chunk(chunk.children, type)) return child;
    }
    return nullptr;
}

const rws::Chunk* find_enclosing_clump(const std::vector<rws::Chunk>& chunks,
                                       const std::uint64_t offset,
                                       const rws::Chunk* clump = nullptr) {
    for (const auto& chunk : chunks) {
        const auto* current = chunk.type == 0x10 ? &chunk : clump;
        if (chunk.offset == offset) return current;
        if (const auto* found = find_enclosing_clump(chunk.children, offset, current)) return found;
    }
    return nullptr;
}

const rws::Chunk* find_owning_geometry(const std::vector<rws::Chunk>& chunks,
                                       std::uint64_t offset,
                                       const rws::Chunk* geometry);

const rws::Chunk* find_preview_geometry(const rws::Chunk& selected,
                                        const std::vector<rws::Chunk>& all_chunks) {
    if (selected.type == 0x0F) return &selected;
    if (selected.type == 0x10 || selected.type == 0x1A)
        return find_first_chunk(selected.children, 0x0F);
    return find_owning_geometry(all_chunks, selected.offset, nullptr);
}

const rws::Chunk* find_owning_object(const std::vector<rws::Chunk>& chunks, const std::uint64_t offset,
                                     const rws::Chunk* owner = nullptr) {
    for (const auto& chunk : chunks) {
        if (chunk.offset == offset) return owner;
        const auto* child_owner = chunk.type == 0x03 ? owner : &chunk;
        if (const auto* found = find_owning_object(chunk.children, offset, child_owner)) return found;
    }
    return nullptr;
}

const rws::Chunk* find_owning_geometry(const std::vector<rws::Chunk>& chunks,
                                       const std::uint64_t offset,
                                       const rws::Chunk* geometry = nullptr) {
    for (const auto& chunk : chunks) {
        const auto* current = chunk.type == 0x0F ? &chunk : geometry;
        if (chunk.offset == offset) return current;
        if (const auto* found = find_owning_geometry(chunk.children, offset, current)) return found;
    }
    return nullptr;
}

void draw_chunk_icon(const std::uint32_t type) {
    auto* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 item_min = ImGui::GetItemRectMin();
    const ImVec2 item_max = ImGui::GetItemRectMax();
    const float size = std::min(12.0F, item_max.y - item_min.y - 4.0F);
    const float left = item_min.x + ImGui::GetTreeNodeToLabelSpacing() + 1.0F;
    const float top = item_min.y + (item_max.y - item_min.y - size) * 0.5F;
    const float right = left + size;
    const float bottom = top + size;
    const float middle_x = (left + right) * 0.5F;
    const float middle_y = (top + bottom) * 0.5F;
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    constexpr float stroke = 1.35F;

    switch (type) {
    case 0x01: // Struct: a data table.
        draw_list->AddRect({left, top}, {right, bottom}, color, 1.0F, 0, stroke);
        draw_list->AddLine({left + 3.0F, top}, {left + 3.0F, bottom}, color, stroke);
        draw_list->AddLine({left, top + 4.0F}, {right, top + 4.0F}, color, stroke);
        draw_list->AddLine({left, top + 8.0F}, {right, top + 8.0F}, color, stroke);
        break;
    case 0x03: // Extension: plug-in/puzzle piece.
        draw_list->AddRect({left + 1.0F, top + 3.0F}, {right - 1.0F, bottom - 1.0F}, color,
                           1.0F, 0, stroke);
        draw_list->AddCircle({middle_x, top + 3.0F}, 2.2F, color, 8, stroke);
        break;
    case 0x06: // Texture: image frame.
        draw_list->AddRect({left, top + 1.0F}, {right, bottom - 1.0F}, color, 1.0F, 0, stroke);
        draw_list->AddCircleFilled({right - 3.0F, top + 4.0F}, 1.25F, color, 8);
        draw_list->AddTriangleFilled({left + 1.5F, bottom - 2.0F}, {left + 5.0F, top + 6.0F},
                                     {left + 8.0F, bottom - 2.0F}, color);
        break;
    case 0x07: // Material: shaded sphere.
        draw_list->AddCircle({middle_x, middle_y}, size * 0.46F, color, 16, stroke);
        draw_list->AddCircleFilled({middle_x - 2.0F, middle_y - 2.0F}, 2.0F, color, 10);
        break;
    case 0x08: // Material List: three swatches.
        for (int row = 0; row < 3; ++row) {
            const float y = top + 2.0F + row * 4.0F;
            draw_list->AddCircleFilled({left + 2.0F, y}, 1.3F, color, 8);
            draw_list->AddLine({left + 5.0F, y}, {right, y}, color, stroke);
        }
        break;
    case 0x09: // Atomic Sector: filled terrain facet.
        draw_list->AddTriangleFilled({middle_x, top}, {right, bottom}, {left, bottom}, color);
        draw_list->AddLine({middle_x, top}, {middle_x, bottom}, ImGui::GetColorU32(ImGuiCol_WindowBg), 1.0F);
        break;
    case 0x0B: // World: globe.
        draw_list->AddCircle({middle_x, middle_y}, size * 0.47F, color, 16, stroke);
        draw_list->AddLine({left + 1.0F, middle_y}, {right - 1.0F, middle_y}, color, stroke);
        draw_list->AddEllipse({middle_x, middle_y}, {size * 0.22F, size * 0.47F}, color, 0.0F, 12, stroke);
        break;
    case 0x0E: // Frame List: coordinate frame.
        draw_list->AddCircleFilled({left + 3.0F, bottom - 3.0F}, 1.5F, color, 8);
        draw_list->AddLine({left + 3.0F, bottom - 3.0F}, {right, bottom - 3.0F}, color, stroke);
        draw_list->AddLine({left + 3.0F, bottom - 3.0F}, {left + 3.0F, top}, color, stroke);
        draw_list->AddLine({left + 3.0F, bottom - 3.0F}, {right - 2.0F, top + 2.0F}, color, stroke);
        break;
    case 0x0F: // Geometry: wireframe triangle.
        draw_list->AddTriangle({middle_x, top}, {right, bottom}, {left, bottom}, color, stroke);
        draw_list->AddLine({middle_x, top}, {middle_x, bottom}, color, stroke);
        break;
    case 0x10: // Clump: grouped overlapping objects.
        draw_list->AddRect({left, top + 3.0F}, {right - 3.0F, bottom}, color, 1.0F, 0, stroke);
        draw_list->AddRect({left + 3.0F, top}, {right, bottom - 3.0F}, color, 1.0F, 0, stroke);
        break;
    case 0x14: // Atomic: single solid object.
        draw_list->AddQuadFilled({middle_x, top}, {right, middle_y}, {middle_x, bottom},
                                 {left, middle_y}, color);
        break;
    default:
        draw_list->AddCircleFilled({middle_x, middle_y}, 2.0F, color, 8);
        break;
    }
}

void find_clump_size_range(const std::vector<rws::Chunk>& chunks, float& minimum, float& maximum) {
    for (const auto& chunk : chunks) {
        if (chunk.type == 0x10) {
            const float size = std::log1p(static_cast<float>(chunk.declared_size));
            minimum = std::min(minimum, size);
            maximum = std::max(maximum, size);
        }
        find_clump_size_range(chunk.children, minimum, maximum);
    }
}

ImVec4 clump_size_color(const rws::Chunk& chunk, const float minimum, const float maximum) {
    const float denominator = maximum - minimum;
    const float t = denominator > 0.0001F ?
        std::clamp((std::log1p(static_cast<float>(chunk.declared_size)) - minimum) / denominator, 0.0F, 1.0F) :
        0.5F;
    constexpr ImVec4 grey{0.58F, 0.60F, 0.64F, 1.0F};
    constexpr ImVec4 green{0.31F, 0.78F, 0.43F, 1.0F};
    constexpr ImVec4 orange{1.0F, 0.56F, 0.20F, 1.0F};
    const auto blend = [](const ImVec4& from, const ImVec4& to, const float amount) {
        return ImVec4{from.x + (to.x - from.x) * amount, from.y + (to.y - from.y) * amount,
                      from.z + (to.z - from.z) * amount, 1.0F};
    };
    return t < 0.5F ? blend(grey, green, t * 2.0F) : blend(green, orange, (t - 0.5F) * 2.0F);
}

using ChunkDisplayNames = std::unordered_map<std::uint64_t, std::string>;

std::optional<rws::PyroExtensionInfo> decode_pyro_metadata(
    const rws::Chunk& owner, const std::span<const std::byte> bytes) {
    const auto* extension = rws::find_child(owner, 0x03);
    const auto* metadata = extension ? rws::find_child(*extension, 0xFFFFFF00U) : nullptr;
    if (!metadata) return std::nullopt;
    const auto decoded = rws::decode_pyro_extension(*metadata, owner.type, bytes);
    return decoded ? decoded.value : std::nullopt;
}

ChunkDisplayNames resolve_chunk_display_names(
    const std::vector<rws::Chunk>& chunks, const std::span<const std::byte> bytes,
    const std::span<const rws::SceneInstance> instances) {
    ChunkDisplayNames names;
    std::unordered_map<std::uint32_t, std::string> prototype_names;
    for (const auto& instance : instances) {
        if (!instance.prototype_name.empty())
            prototype_names.try_emplace(instance.prototype_id, instance.prototype_name);
    }

    auto visit = [&](auto&& self, const std::vector<rws::Chunk>& siblings) -> void {
        for (const auto& chunk : siblings) {
            if (chunk.type == 0x06) {
                const auto texture = rws::decode_texture(chunk, bytes);
                if (texture && !texture.value->name.empty()) names.emplace(chunk.offset, texture.value->name);
            } else if (chunk.type == 0x07 || chunk.type == 0x14) {
                const auto metadata = decode_pyro_metadata(chunk, bytes);
                if (metadata && !metadata->object_name().empty())
                    names.emplace(chunk.offset, metadata->object_name());
            } else if (chunk.type == 0x10) {
                for (const auto& child : chunk.children) {
                    if (child.type != 0x14) continue;
                    const auto metadata = decode_pyro_metadata(child, bytes);
                    if (!metadata) continue;
                    if (!metadata->object_name().empty()) {
                        names.emplace(chunk.offset, metadata->object_name());
                        break;
                    }
                    const auto object_index = metadata->atomic_object_index();
                    if (!object_index) continue;
                    const auto found = prototype_names.find(1000U + *object_index);
                    if (found != prototype_names.end()) {
                        names.emplace(chunk.offset, found->second);
                        break;
                    }
                }
            } else if (chunk.type == 0x0E) {
                const auto* metadata_chunk = find_first_chunk(chunk.children, 0xFFFFFF00U);
                if (metadata_chunk) {
                    const auto metadata = rws::decode_pyro_extension(*metadata_chunk, 0x0E, bytes);
                    if (metadata && !metadata.value->object_name().empty())
                        names.emplace(chunk.offset, metadata.value->object_name());
                }
            }
            self(self, chunk.children);
        }
    };
    visit(visit, chunks);
    return names;
}

void draw_tree(const std::vector<rws::Chunk>& chunks, std::optional<std::uint64_t>& selected,
               const ChunkDisplayNames& display_names,
               const float minimum_clump_size, const float maximum_clump_size,
               const bool reveal_selected) {
    for (const auto& chunk : chunks) {
        const bool has_children = !chunk.children.empty();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (selected && *selected == chunk.offset) flags |= ImGuiTreeNodeFlags_Selected;
        const bool colored = chunk.truncated || chunk.type == 0x10;
        if (chunk.truncated)
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 170, 64, 255));
        else if (chunk.type == 0x10)
            ImGui::PushStyleColor(ImGuiCol_Text, clump_size_color(chunk, minimum_clump_size, maximum_clump_size));
        const auto display_name = display_names.find(chunk.offset);
        const bool open = display_name == display_names.end() ?
            ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.offset + 1)),
                flags, "   %s  @ 0x%llX  (%u)", rws::chunk_name(chunk.type).data(),
                static_cast<unsigned long long>(chunk.offset), chunk.declared_size) :
            ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.offset + 1)),
                flags, "   %s  \"%s\"  @ 0x%llX  (%u)", rws::chunk_name(chunk.type).data(),
                display_name->second.c_str(), static_cast<unsigned long long>(chunk.offset),
                chunk.declared_size);
        draw_chunk_icon(chunk.type);
        if (colored) ImGui::PopStyleColor();
        if (reveal_selected && selected && *selected == chunk.offset) ImGui::SetScrollHereY(0.5F);
        if (ImGui::IsItemClicked()) selected = chunk.offset;
        if (has_children && open) {
            draw_tree(chunk.children, selected, display_names, minimum_clump_size, maximum_clump_size,
                      reveal_selected);
            ImGui::TreePop();
        }
    }
}

void draw_instance_tree(const std::span<const rws::SceneInstance> instances,
                        std::optional<std::uint64_t>& selected, const bool reveal_selected) {
    if (instances.empty()) return;
    const auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!ImGui::TreeNodeEx("CSF Scene Instances", flags, "CSF Scene Instances (%zu)",
                           instances.size()))
        return;

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(instances.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& instance = instances[static_cast<std::size_t>(i)];
            const bool is_selected = selected && *selected == instance.offset;
            std::ostringstream label;
            label << (instance.prototype_name.empty() ? "Prototype " : instance.prototype_name);
            if (instance.prototype_name.empty()) label << instance.prototype_id;
            label << "  [instance " << instance.instance_id << "]  @ 0x" << std::hex
                  << std::uppercase << instance.offset;
            if (ImGui::Selectable(label.str().c_str(), is_selected)) selected = instance.offset;
            if (reveal_selected && is_selected) ImGui::SetScrollHereY(0.5F);
        }
    }
    ImGui::TreePop();
}

void draw_hex(rws::Document& document, const std::uint64_t begin, const std::uint64_t size) {
    const auto bytes = document.bytes();
    const auto end = std::min<std::uint64_t>(begin + size, bytes.size());
    const auto shown_end = std::min<std::uint64_t>(end, begin + 4096);
    ImGui::TextDisabled("Payload bytes (editable, first 4096 bytes)");
    ImGui::BeginChild("hex", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (std::uint64_t row = begin; row < shown_end; row += 16) {
        ImGui::Text("%08llX", static_cast<unsigned long long>(row));
        ImGui::SameLine(85.0F);
        for (std::uint64_t column = 0; column < 16 && row + column < shown_end; ++column) {
            const auto offset = row + column;
            auto value = static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(offset)]));
            ImGui::PushID(static_cast<int>(column));
            ImGui::SetNextItemWidth(27.0F);
            if (ImGui::InputScalar("##byte", ImGuiDataType_U32, &value, nullptr, nullptr, "%02X",
                                   ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue)) {
                document.set_byte(offset, static_cast<std::byte>(value & 0xFFU));
            }
            ImGui::PopID();
            if (column != 15) ImGui::SameLine();
        }
    }
    ImGui::EndChild();
}

void draw_vec3(const char* label, const rws::Vec3& value) {
    ImGui::Text("%s: %.4f, %.4f, %.4f", label, value.x, value.y, value.z);
}

const char* physics_volume_kind_name(const std::uint32_t kind) {
    switch (kind) {
    case 0x0E: return "Sphere";
    case 0x0F: return "Capsule";
    case 0x10: return "Box";
    case 0x11: return "Cylinder";
    case 0x13: return "Trilist";
    default: return "Unknown";
    }
}

void draw_physics_volume(const rws::PhysicsVolumeInfo& volume, const char* label) {
    if (!ImGui::TreeNode(&volume, "%s: %s (0x%X)", label,
                         physics_volume_kind_name(volume.kind), volume.kind)) return;
    ImGui::Text("Version: %u | collision group: %u | flags: 0x%X",
        volume.version, volume.collision_group, volume.flags);
    ImGui::Text("Fatness: %.6g | friction: %.6g | restitution: %.6g",
        volume.fatness, volume.friction, volume.restitution);
    if (volume.capsule_half_height) {
        ImGui::Text("Radius: %.6g | half-height: %.6g",
            volume.fatness, *volume.capsule_half_height);
    } else if (volume.box_half_extents) {
        draw_vec3("Half-extents", *volume.box_half_extents);
    } else if (volume.cylinder_radius && volume.cylinder_half_height) {
        ImGui::Text("Radius: %.6g | half-height: %.6g",
            *volume.cylinder_radius, *volume.cylinder_half_height);
    }
    if (volume.trilist_mass) {
        ImGui::Text("Cached mass: %.6g", *volume.trilist_mass);
        draw_vec3("Center of mass", *volume.trilist_center_of_mass);
        draw_vec3("Principal inertia", *volume.trilist_principal_inertia);
        const auto& orientation = *volume.trilist_inertia_orientation;
        ImGui::Text("Inertia orientation: %.5g, %.5g, %.5g, %.5g",
            orientation[0], orientation[1], orientation[2], orientation[3]);
    }
    for (std::size_t index = 0; index < volume.children.size(); ++index) {
        const auto child_label = "Child " + std::to_string(index);
        draw_physics_volume(volume.children[index], child_label.c_str());
    }
    ImGui::TreePop();
}

void draw_typed_details(const rws::Chunk& chunk, rws::Document& document, std::string& status,
                        const std::uint32_t parent_type = 0) {
    const auto bytes = document.bytes();
    const auto version = rws::decode_library_id(chunk.library_id);
    ImGui::Text("RenderWare %u.%u.%u.%u, build %u", version.major, version.minor,
                version.revision, version.binary, version.build);
    ImGui::SeparatorText("Decoded structure");
    switch (chunk.type) {
    case 0x06: {
        const auto decoded = rws::decode_texture(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Name: %s", decoded.value->name.c_str());
        ImGui::Text("Mask: %s", decoded.value->mask_name.c_str());
        ImGui::Text("Filter: %u | address U/V: %u/%u | packed: 0x%08X",
            decoded.value->filter_mode, decoded.value->address_u, decoded.value->address_v,
            decoded.value->filter_addressing);
        break;
    }
    case 0x08: {
        const auto decoded = rws::decode_material_list(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Materials: %d | remap entries: %zu", decoded.value->material_count,
            decoded.value->remap.size());
        break;
    }
    case 0x07: {
        const auto decoded = rws::decode_material(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        const auto& value = *decoded.value;
        ImGui::Text("RGBA: %u, %u, %u, %u | textured: %s", value.color[0], value.color[1],
            value.color[2], value.color[3], value.textured ? "yes" : "no");
        ImGui::Text("Surface: ambient %.3f, specular %.3f, diffuse %.3f",
            value.ambient, value.specular, value.diffuse);
        break;
    }
    case 0x09: {
        const auto decoded = rws::decode_world_sector(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        const auto& value = *decoded.value;
        ImGui::Text("Vertices: %d | triangles: %d | material base: %d", value.vertex_count,
            value.triangle_count, value.material_window_base);
        draw_vec3("Bounds min", value.bounding_box_inf);
        draw_vec3("Bounds max", value.bounding_box_sup);
        break;
    }
    case 0x0A: {
        const auto decoded = rws::decode_plane_sector(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        const auto& value = *decoded.value;
        ImGui::Text("Axis: %d | split: %.4f | left %.4f (%s) | right %.4f (%s)", value.axis,
            value.split, value.left_value, value.left_is_world_sector ? "leaf" : "branch",
            value.right_value, value.right_is_world_sector ? "leaf" : "branch");
        break;
    }
    case 0x0B: {
        const auto decoded = rws::decode_world(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
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
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Frames: %zu", decoded.value->frames.size());
        if (ImGui::BeginTable("frames", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Index"); ImGui::TableSetupColumn("Parent"); ImGui::TableSetupColumn("Position");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < decoded.value->frames.size(); ++i) {
                const auto& frame = decoded.value->frames[i];
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%zu", i);
                ImGui::TableNextColumn(); ImGui::Text("%d", frame.parent);
                ImGui::TableNextColumn(); ImGui::Text("%.3f, %.3f, %.3f", frame.position.x, frame.position.y, frame.position.z);
            }
            ImGui::EndTable();
        }
        break;
    }
    case 0x0F: {
        const auto decoded = rws::decode_geometry(chunk, bytes);
        if (!decoded) { ImGui::TextColored(ImVec4(1, 0.35F, 0.25F, 1), "%s", decoded.error.c_str()); break; }
        const auto& value = *decoded.value;
        ImGui::Text("Vertices: %d | triangles: %d | morph targets: %d | UV sets: %u",
            value.vertex_count, value.triangle_count, value.morph_target_count, value.texcoord_sets);
        ImGui::Text("Format: 0x%08X | Struct bytes: %llu (validated)", value.format,
            static_cast<unsigned long long>(value.computed_size));
        for (std::size_t i = 0; i < value.morph_targets.size(); ++i) {
            const auto& morph = value.morph_targets[i];
            ImGui::Text("Morph %zu: radius %.3f | vertices %s | normals %s", i, morph.sphere.radius,
                morph.has_vertices ? "yes" : "no", morph.has_normals ? "yes" : "no");
        }
        const char* layout = value.triangle_layout == rws::TriangleLayout::stream_order ? "RenderWare stream" :
            value.triangle_layout == rws::TriangleLayout::memory_order ? "memory order" : "ambiguous";
        ImGui::Text("Triangle layout: %s | materials: %d", layout, value.material_count);
        if (value.triangle_layout != rws::TriangleLayout::unknown && ImGui::Button("Export this geometry to OBJ")) {
            try {
                auto output = document.source_path();
                std::ostringstream suffix;
                suffix << ".geometry_" << std::hex << chunk.offset << ".obj";
                output.replace_filename(output.stem().string() + suffix.str());
                rws::export_geometry_obj(value, bytes, output);
                status = "Exported " + output.string();
            } catch (const std::exception& error) { status = error.what(); }
        }
        break;
    }
    case 0x10: {
        const auto decoded = rws::decode_clump(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Atomics: %d | lights: %d | cameras: %d", decoded.value->atomics,
            decoded.value->lights, decoded.value->cameras);
        break;
    }
    case 0x14: {
        const auto decoded = rws::decode_atomic(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Frame index: %d | geometry index: %d | flags: 0x%08X",
            decoded.value->frame_index, decoded.value->geometry_index, decoded.value->flags);
        break;
    }
    case 0x1F: {
        const auto decoded = rws::decode_right_to_render(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Pipeline plugin: 0x%08X (%s) | extra data: 0x%08X",
            decoded.value->plugin_id,
            rws::chunk_name(decoded.value->plugin_id).data(), decoded.value->extra_data);
        break;
    }
    case 0x24: {
        const auto decoded = rws::decode_table_of_contents(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Entries: %zu", decoded.value->entries.size());
        for (const auto& entry : decoded.value->entries) {
            ImGui::BulletText("%s (0x%X) at 0x%08X | object ID 0x%08X",
                rws::chunk_name(entry.chunk_type).data(), entry.chunk_type, entry.offset, entry.object_id);
        }
        break;
    }
    case 0x11E: {
        const auto decoded = rws::decode_hanim(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("HAnim version: 0x%08X | hierarchy ID: %d | nodes: %zu",
            decoded.value->version, decoded.value->hierarchy_id, decoded.value->nodes.size());
        ImGui::Text("Flags: 0x%08X | keyframe size: %u", decoded.value->flags, decoded.value->keyframe_size);
        break;
    }
    case 0x11D: {
        const auto decoded = rws::decode_collision_tree(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
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
                root.right.type, root.right.flags, root.right.index, root.right.value);
        }
        break;
    }
    case 0x116: {
        const auto* geometry_chunk = find_owning_geometry(document.chunks(), chunk.offset);
        if (!geometry_chunk) { ImGui::TextDisabled("Skin is not inside a Geometry chunk"); break; }
        const auto geometry = rws::decode_geometry(*geometry_chunk, bytes);
        if (!geometry) { ImGui::TextDisabled("Owning Geometry: %s", geometry.error.c_str()); break; }
        const auto decoded = rws::decode_skin(chunk, geometry.value->vertex_count, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
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
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Arrays: %zu", decoded.value->arrays.size());
        for (const auto& array : decoded.value->arrays) {
            const char* format = array.format == rws::UserDataFormat::integer ? "int" :
                array.format == rws::UserDataFormat::real ? "real" : "string";
            const auto count = array.format == rws::UserDataFormat::integer ? array.integers.size() :
                array.format == rws::UserDataFormat::real ? array.reals.size() : array.strings.size();
            ImGui::BulletText("%s: %s[%zu]", array.name.c_str(), format, count);
            if (count == 1) {
                ImGui::SameLine();
                if (array.format == rws::UserDataFormat::integer) ImGui::Text("= %d (0x%X)", array.integers[0],
                    static_cast<std::uint32_t>(array.integers[0]));
                else if (array.format == rws::UserDataFormat::real) ImGui::Text("= %.6g", array.reals[0]);
                else ImGui::Text("= %s", array.strings[0].c_str());
            }
        }
        break;
    }
    case 0x120: {
        const auto decoded = rws::decode_material_effects(chunk, parent_type, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        const auto& value = *decoded.value;
        if (parent_type == 0x14 || parent_type == 0x09) {
            ImGui::Text("MatFX rendering pipeline: %s", value.pipeline_enabled ? "enabled" : "disabled");
        } else {
            ImGui::Text("Effect: %u | slot: %u | dual texture: %s", value.effect_type,
                value.slot_type, value.has_dual_texture ? "present" : "absent");
            ImGui::Text("Blend source/destination: %u/%u", value.source_blend, value.destination_blend);
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
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Anisotropy coefficient: %.4f", decoded.value->coefficient);
        break;
    }
    case 0x50E: {
        const auto decoded = rws::decode_bin_mesh(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        ImGui::Text("Meshes: %zu | indices: %u | flags: 0x%08X",
            decoded.value->meshes.size(), decoded.value->total_indices, decoded.value->flags);
        break;
    }
    case 0x907: {
        const auto decoded = rws::decode_physics_body_def(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        const auto& value = *decoded.value;
        ImGui::Text("RwpBodyDef | root volume kind: 0x%X | version: %u", value.volume.kind,
            value.volume.version);
        draw_physics_volume(value.volume, "Root volume");
        ImGui::Text("Mass: %.6g | scalar inertia: %.6g | body flags: 0x%08X",
            value.mass, value.scalar_inertia, value.flags);
        ImGui::Text("Flag meanings: %s", rws::physics_body_flag_names(value.flags).c_str());
        draw_vec3("Center of mass", value.center_of_mass);
        draw_vec3("Principal inertia", value.principal_inertia);
        ImGui::Text("Inertia orientation: %.5g, %.5g, %.5g, %.5g",
            value.inertia_orientation[0], value.inertia_orientation[1],
            value.inertia_orientation[2], value.inertia_orientation[3]);
        ImGui::Text("Linear damping: %.6g | angular damping: %.6g",
            value.linear_damping, value.angular_damping);
        draw_vec3("Finite-rotation axis", value.finite_rotation_axis);
        break;
    }
    case 0x909: {
        const auto decoded = rws::decode_physics_ragdoll_def(chunk, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        const auto& value = *decoded.value;
        ImGui::Text("RwpRagdollDef | types: %u/%u", value.type_0, value.type_1);
        ImGui::Text("Bodies: %u | joints: %u | lookup table: %u x %u (%zu values)",
            value.body_count, value.joint_count, value.table_rows, value.table_columns,
            value.table_values.size());
        ImGui::Text("Body IDs: %zu | integer field: %u", value.body_ids.size(), value.integer_field);
        break;
    }
    case 0xFFFFFF00U: {
        // Short collision-World leaf declarations can make a World Sector plug-in
        // appear directly below its enclosing Plane Section in the recovered tree.
        const auto owner_type = parent_type == 0x0A ? 0x09U : parent_type;
        const auto decoded = rws::decode_pyro_extension(chunk, owner_type, bytes);
        if (!decoded) { ImGui::TextDisabled("%s", decoded.error.c_str()); break; }
        const auto& value = *decoded.value;
        ImGui::Text("Pyro plugin | owner: %s (0x%X) | version: %u",
            rws::chunk_name(value.owner_type).data(), value.owner_type, value.version);
        ImGui::Text("Fields: %zu | strings: %zu | optional record/bounds: %s",
            value.words.size(), value.strings.size(), value.present ? "present" : "absent");
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
            ImGui::Text("Raw bounds pairs: %.4g/%.4g, %.4g/%.4g, %.4g/%.4g",
                (*value.bounds)[0], (*value.bounds)[1], (*value.bounds)[2],
                (*value.bounds)[3], (*value.bounds)[4], (*value.bounds)[5]);
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
    ImGui::SeparatorText("Raw payload");
}

} // namespace

int run_app(const std::optional<std::filesystem::path>& initial_path) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    auto* window = glfwCreateWindow(1400, 850, "CSF RWS Tools - rws-man", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, drop_callback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::unique_ptr<rws::Document> document;
    rwsman::GeometryPreview geometry_preview;
    std::optional<std::uint64_t> selected;
    ChunkDisplayNames display_names;
    std::string status = "Drop an .rws file on this window or pass one on the command line.";
    auto load = [&](const std::filesystem::path& path) {
        try {
            document = std::make_unique<rws::Document>(rws::Document::load(path));
            display_names = resolve_chunk_display_names(
                document->chunks(), document->bytes(), document->scene_instances());
            geometry_preview.clear();
            if (const auto* geometry = find_first_chunk(document->chunks(), 0x0F)) selected = geometry->offset;
            else if (!document->chunks().empty()) selected = document->chunks().front().offset;
            else selected.reset();
            status = "Loaded " + path.string();
        } catch (const std::exception& error) {
            status = error.what();
        }
    };
    if (initial_path) load(*initial_path);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (dropped_file) {
            load(*dropped_file);
            dropped_file.reset();
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(width), static_cast<float>(height)));
        ImGui::Begin("CSF RWS Tools", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_MenuBar);
        if (ImGui::BeginMenuBar()) {
            if (document && ImGui::MenuItem("Save copy", nullptr, false, true)) {
                try {
                    auto output = document->source_path();
                    output.replace_filename(output.stem().string() + ".edited" + output.extension().string());
                    document->save_as(output);
                    status = "Saved " + output.string();
                } catch (const std::exception& error) { status = error.what(); }
            }
            if (document && ImGui::MenuItem("Export whole scene (glTF)", nullptr, false, true)) {
                try {
                    auto output = document->source_path();
                    output.replace_filename(output.stem().string() + ".scene.gltf");
                    const auto stats = rws::export_scene_gltf(document->chunks(),
                        document->scene_instances(), document->bytes(), output);
                    std::ostringstream message;
                    message << "Exported " << stats.atomic_instances << " atomic meshes ("
                            << stats.custom_instances << " CSF placements, "
                            << stats.unresolved_instances << " unresolved) and "
                            << stats.world_sectors << " World sectors to " << output.string();
                    status = message.str();
                } catch (const std::exception& error) { status = error.what(); }
            }
            const auto* selected_clump = document && selected ?
                find_enclosing_clump(document->chunks(), *selected) : nullptr;
            if (ImGui::MenuItem("Export selected Clump (glTF)", nullptr, false,
                                selected_clump != nullptr)) {
                try {
                    auto output = document->source_path();
                    std::ostringstream suffix;
                    suffix << document->source_path().stem().string() << ".clump_0x" << std::hex
                           << std::uppercase << selected_clump->offset << ".gltf";
                    output.replace_filename(suffix.str());
                    const auto stats = rws::export_clump_gltf(
                        *selected_clump, document->bytes(), output);
                    status = "Exported Clump with " + std::to_string(stats.atomic_instances) +
                             " Atomics to " + output.string();
                } catch (const std::exception& error) { status = error.what(); }
            }
            ImGui::EndMenuBar();
        }
        ImGui::TextUnformatted(status.c_str());
        ImGui::Separator();
        if (document) {
            static std::optional<std::uint64_t> previous_selection;
            float minimum_clump_size = std::numeric_limits<float>::max();
            float maximum_clump_size = std::numeric_limits<float>::lowest();
            find_clump_size_range(document->chunks(), minimum_clump_size, maximum_clump_size);
            const bool reveal_selected = selected != previous_selection;
            ImGui::BeginChild("tree", ImVec2(490, 0), ImGuiChildFlags_Borders);
            ImGui::Text("%zu bytes | %zu chunks | %zu CSF instances | %zu diagnostics%s",
                document->bytes().size(), document->chunks().size(), document->scene_instances().size(),
                document->diagnostics().size(),
                document->dirty() ? " | modified" : "");
            draw_tree(document->chunks(), selected, display_names, minimum_clump_size,
                      maximum_clump_size, reveal_selected);
            draw_instance_tree(document->scene_instances(), selected, reveal_selected);
            ImGui::EndChild();
            previous_selection = selected;
            ImGui::SameLine();
            ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_Borders);
            const auto* chunk = selected ? find_chunk(document->chunks(), *selected) : nullptr;
            const auto* instance = selected ? find_instance(document->scene_instances(), *selected) : nullptr;
            if (chunk) {
                ImGui::Text("%s (0x%08X)", rws::chunk_name(chunk->type).data(), chunk->type);
                ImGui::Text("Header: 0x%llX   Payload: 0x%llX", static_cast<unsigned long long>(chunk->offset),
                    static_cast<unsigned long long>(chunk->payload_offset));
                ImGui::Text("Declared: %u   Available: %llu   Library ID: 0x%08X", chunk->declared_size,
                    static_cast<unsigned long long>(chunk->available_size), chunk->library_id);
                ImGui::Text("Vendor: %s (0x%06X)   Object ID: 0x%02X",
                    rws::chunk_vendor_name(rws::chunk_vendor_id(chunk->type)).data(),
                    rws::chunk_vendor_id(chunk->type), rws::chunk_object_id(chunk->type));
                const auto payload_offset = chunk->payload_offset;
                const auto available_size = chunk->available_size;
                const auto* owner = find_owning_object(document->chunks(), chunk->offset);
                const auto* geometry = find_preview_geometry(*chunk, document->chunks());
                if (ImGui::BeginTabBar("chunk_views")) {
                    if (geometry && ImGui::BeginTabItem("3D Preview")) {
                        if (chunk->type == 0x10 || chunk->type == 0x1A)
                            ImGui::TextDisabled("Previewing the first Geometry contained by this %s.",
                                rws::chunk_name(chunk->type).data());
                        geometry_preview.draw(*geometry, document->bytes(), document->source_path());
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Whole RWS Scene")) {
                        geometry_preview.draw_scene(document->chunks(), document->bytes(),
                            document->scene_instances(), document->source_path(), selected);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Inspector / Hex")) {
                        draw_typed_details(*chunk, *document, status, owner ? owner->type : 0);
                        draw_hex(*document, payload_offset, available_size);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            } else if (instance) {
                ImGui::Text("CSF Scene Instance @ 0x%llX",
                            static_cast<unsigned long long>(instance->offset));
                if (instance->prototype_id >= 1000U)
                    ImGui::Text("Prototype: %u (Pyro object index %u)   Instance ID: %u",
                                instance->prototype_id, instance->prototype_id - 1000U,
                                instance->instance_id);
                else
                    ImGui::Text("Prototype: %u   Instance ID: %u", instance->prototype_id,
                                instance->instance_id);
                ImGui::Text("Name: %s", instance->prototype_name.empty() ? "(unnamed)" :
                                                              instance->prototype_name.c_str());
                ImGui::Text("Flags: 0x%08X   Declared: %u   Physical: %llu", instance->flags,
                            instance->declared_size,
                            static_cast<unsigned long long>(instance->physical_size));
                ImGui::Text("Flag meanings: %s",
                            rws::scene_instance_flag_names(instance->flags).c_str());
                ImGui::Text("Visibility distance: max %.6g, min %.6g, fade %.6g",
                            instance->maximum_visibility_distance,
                            instance->minimum_visibility_distance,
                            instance->visibility_fade_range);
                ImGui::Text("Position: %.6g, %.6g, %.6g", instance->position.x,
                            instance->position.y, instance->position.z);
                ImGui::Text("Matrix flags: 0x%08X", instance->matrix_flags);
                ImGui::Text("Rotation: [%.5g %.5g %.5g]", instance->rotation[0],
                            instance->rotation[1], instance->rotation[2]);
                ImGui::Text("          [%.5g %.5g %.5g]", instance->rotation[3],
                            instance->rotation[4], instance->rotation[5]);
                ImGui::Text("          [%.5g %.5g %.5g]", instance->rotation[6],
                            instance->rotation[7], instance->rotation[8]);
                if (ImGui::BeginTabBar("instance_views")) {
                    if (ImGui::BeginTabItem("Whole RWS Scene")) {
                        geometry_preview.draw_scene(document->chunks(), document->bytes(),
                            document->scene_instances(), document->source_path(), selected);
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Raw record")) {
                        draw_hex(*document, instance->offset, instance->physical_size);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
            } else {
                ImGui::TextDisabled("Select a chunk or CSF scene instance to inspect it.");
            }
            ImGui::EndChild();
        }
        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, width, height);
        glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    geometry_preview.clear();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::optional<std::filesystem::path> initial_path;
    if (argv != nullptr && argc > 1) initial_path = argv[1];
    if (argv != nullptr) LocalFree(argv);
    return run_app(initial_path);
}
#else
int main(int argc, char** argv) {
    const std::optional<std::filesystem::path> initial_path =
        argc > 1 ? std::optional<std::filesystem::path>(argv[1]) : std::nullopt;
    return run_app(initial_path);
}
#endif
