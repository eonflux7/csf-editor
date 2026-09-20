#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off
// windows.h must precede the headers below; they depend on its declarations.
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
// clang-format on
#endif

#include "csf/animation_catalog.hpp"
#include "csf/cmo.hpp"
#include "csf/document.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "geometry_preview.hpp"
#include "rws/animation.hpp"
#include "rws/decoded.hpp"
#include "rws/document.hpp"
#include "rws/obj_export.hpp"
#include "rws/physics_inspection.hpp"
#include "rws/scene_export.hpp"
#include "rws/world_recovery.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

std::optional<std::filesystem::path> dropped_file;

std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

#ifndef NDEBUG
std::filesystem::path mission_debug_log_path() {
#ifdef _WIN32
    std::array<wchar_t, 32768> executable{};
    const auto length = GetModuleFileNameW(nullptr, executable.data(),
                                           static_cast<DWORD>(executable.size()));
    if (length > 0 && length < executable.size())
        return std::filesystem::path(executable.data(), executable.data() + length).parent_path() /
               "rws-man-debug.log";
#endif
    std::error_code error;
    const auto directory = std::filesystem::current_path(error);
    return (error ? std::filesystem::path{} : directory) / "rws-man-debug.log";
}

void begin_mission_debug_log(const std::filesystem::path& path) noexcept {
    try {
        std::ofstream output(mission_debug_log_path(), std::ios::trunc);
        output << "rws-man Debug mission load\n  input (UTF-8): " << path_utf8(path)
               << '\n';
#ifdef _WIN32
        output << "  Windows ANSI code page: " << GetACP()
               << "\n  Windows OEM code page: " << GetOEMCP() << '\n';
#endif
    } catch (...) {
    }
}

void log_mission_failure(const std::filesystem::path& path, const std::string_view stage,
                         const std::string_view error) noexcept {
    try {
        std::ofstream output(mission_debug_log_path(), std::ios::app);
        output << "mission load failed\n  stage: " << stage
               << "\n  input (UTF-8): " << path_utf8(path) << "\n  error: " << error
               << '\n';
    } catch (...) {
    }
}
#endif

enum class Workspace {
    mission,
    animation,
    scene,
    geometry,
    inspector,
};

const char* workspace_name(const Workspace workspace) {
    switch (workspace) {
    case Workspace::mission:
        return "Mission";
    case Workspace::animation:
        return "Animation";
    case Workspace::scene:
        return "Scene";
    case Workspace::geometry:
        return "Geometry";
    case Workspace::inspector:
        return "Inspector";
    }
    return "Scene";
}

#ifdef _WIN32
std::optional<std::filesystem::path> choose_rws_file(const std::filesystem::path& directory) {
    std::array<wchar_t, 32768> path{};
    const auto initial = directory.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter =
        L"RenderWare models, streams, and animation (*.rpc;*.rws;*.anm)\0*.rpc;*.rws;*.anm\0RPC "
        L"Clump models (*.rpc)\0*.rpc\0Animations (*.anm)\0*.anm\0RenderWare streams "
        L"(*.rws)\0*.rws\0All files\0*.*\0\0";
    dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path(path.data());
}

std::optional<std::filesystem::path> choose_mission_file(const std::filesystem::path& directory) {
    std::array<wchar_t, 32768> path{};
    const auto initial = directory.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter = L"CSF mission scenes (*.scn)\0*.scn\0All files\0*.*\0\0";
    dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path(path.data());
}
#endif

struct RecentPair {
    std::filesystem::path main, collision;
};

std::filesystem::path pairing_settings_path() {
#ifdef _WIN32
    if (const auto* local = std::getenv("LOCALAPPDATA"))
        return std::filesystem::path(local) / "CSF RWS Tools" / "recent-pairings.txt";
#endif
    return std::filesystem::temp_directory_path() / "csf-rws-tools-recent-pairings.txt";
}

std::vector<RecentPair> load_recent_pairs() {
    std::vector<RecentPair> result;
    std::ifstream input(pairing_settings_path());
    std::string line;
    while (std::getline(input, line) && result.size() < 8) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        result.push_back({std::filesystem::path(line.substr(0, tab)),
                          std::filesystem::path(line.substr(tab + 1))});
    }
    return result;
}

void save_recent_pairs(const std::vector<RecentPair>& pairs) {
    const auto path = pairing_settings_path();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    for (const auto& pair : pairs)
        output << pair.main.string() << '\t' << pair.collision.string() << '\n';
}

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
    const auto found = std::find_if(
        instances.begin(), instances.end(),
        [offset](const rws::SceneInstance& instance) { return instance.offset == offset; });
    return found == instances.end() ? nullptr : &*found;
}

const rws::Chunk* find_first_chunk(const std::vector<rws::Chunk>& chunks,
                                   const std::uint32_t type) {
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

const rws::Chunk* find_owning_geometry(const std::vector<rws::Chunk>& chunks, std::uint64_t offset,
                                       const rws::Chunk* geometry);

const rws::Chunk* find_preview_geometry(const rws::Chunk& selected,
                                        const std::vector<rws::Chunk>& all_chunks) {
    if (selected.type == 0x0F) return &selected;
    if (selected.type == 0x10 || selected.type == 0x1A)
        return find_first_chunk(selected.children, 0x0F);
    return find_owning_geometry(all_chunks, selected.offset, nullptr);
}

const rws::Chunk* find_owning_object(const std::vector<rws::Chunk>& chunks,
                                     const std::uint64_t offset,
                                     const rws::Chunk* owner = nullptr) {
    for (const auto& chunk : chunks) {
        if (chunk.offset == offset) return owner;
        const auto* child_owner = chunk.type == 0x03 ? owner : &chunk;
        if (const auto* found = find_owning_object(chunk.children, offset, child_owner))
            return found;
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
        draw_list->AddRect({left + 1.0F, top + 3.0F}, {right - 1.0F, bottom - 1.0F}, color, 1.0F, 0,
                           stroke);
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
        draw_list->AddLine({middle_x, top}, {middle_x, bottom},
                           ImGui::GetColorU32(ImGuiCol_WindowBg), 1.0F);
        break;
    case 0x0B: // World: globe.
        draw_list->AddCircle({middle_x, middle_y}, size * 0.47F, color, 16, stroke);
        draw_list->AddLine({left + 1.0F, middle_y}, {right - 1.0F, middle_y}, color, stroke);
        draw_list->AddEllipse({middle_x, middle_y}, {size * 0.22F, size * 0.47F}, color, 0.0F, 12,
                              stroke);
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
    const float t =
        denominator > 0.0001F
            ? std::clamp((std::log1p(static_cast<float>(chunk.declared_size)) - minimum) /
                             denominator,
                         0.0F, 1.0F)
            : 0.5F;
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

std::optional<rws::PyroExtensionInfo> decode_pyro_metadata(const rws::Chunk& owner,
                                                           const std::span<const std::byte> bytes) {
    const auto* extension = rws::find_child(owner, 0x03);
    const auto* metadata = extension ? rws::find_child(*extension, 0xFFFFFF00U) : nullptr;
    if (!metadata) return std::nullopt;
    const auto decoded = rws::decode_pyro_extension(*metadata, owner.type, bytes);
    return decoded ? decoded.value : std::nullopt;
}

ChunkDisplayNames resolve_chunk_display_names(const std::vector<rws::Chunk>& chunks,
                                              const std::span<const std::byte> bytes,
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
                if (texture && !texture.value->name.empty())
                    names.emplace(chunk.offset, texture.value->name);
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
               const ChunkDisplayNames& display_names, const float minimum_clump_size,
               const float maximum_clump_size, const bool reveal_selected) {
    for (const auto& chunk : chunks) {
        const bool has_children = !chunk.children.empty();
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (selected && *selected == chunk.offset) flags |= ImGuiTreeNodeFlags_Selected;
        const bool colored = chunk.truncated || chunk.type == 0x10;
        if (chunk.truncated)
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 170, 64, 255));
        else if (chunk.type == 0x10)
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  clump_size_color(chunk, minimum_clump_size, maximum_clump_size));
        const auto display_name = display_names.find(chunk.offset);
        const bool open =
            display_name == display_names.end()
                ? ImGui::TreeNodeEx(
                      reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.offset + 1)), flags,
                      "   %s  @ 0x%llX  (%u)", rws::chunk_name(chunk.type).data(),
                      static_cast<unsigned long long>(chunk.offset), chunk.declared_size)
                : ImGui::TreeNodeEx(
                      reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.offset + 1)), flags,
                      "   %s  \"%s\"  @ 0x%llX  (%u)", rws::chunk_name(chunk.type).data(),
                      display_name->second.c_str(), static_cast<unsigned long long>(chunk.offset),
                      chunk.declared_size);
        draw_chunk_icon(chunk.type);
        if (colored) ImGui::PopStyleColor();
        if (reveal_selected && selected && *selected == chunk.offset) ImGui::SetScrollHereY(0.5F);
        if (ImGui::IsItemClicked()) selected = chunk.offset;
        if (has_children && open) {
            draw_tree(chunk.children, selected, display_names, minimum_clump_size,
                      maximum_clump_size, reveal_selected);
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
    ImGui::BeginChild("hex", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (std::uint64_t row = begin; row < shown_end; row += 16) {
        ImGui::Text("%08llX", static_cast<unsigned long long>(row));
        ImGui::SameLine(85.0F);
        for (std::uint64_t column = 0; column < 16 && row + column < shown_end; ++column) {
            const auto offset = row + column;
            auto value = static_cast<unsigned int>(
                std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(offset)]));
            ImGui::PushID(static_cast<int>(column));
            ImGui::SetNextItemWidth(27.0F);
            if (ImGui::InputScalar("##byte", ImGuiDataType_U32, &value, nullptr, nullptr, "%02X",
                                   ImGuiInputTextFlags_CharsHexadecimal |
                                       ImGuiInputTextFlags_EnterReturnsTrue)) {
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

void draw_typed_details(const rws::Chunk& chunk, rws::Document& document, std::string& status,
                        const std::uint32_t parent_type = 0) {
    const auto bytes = document.bytes();
    const auto version = rws::decode_library_id(chunk.library_id);
    ImGui::Text("RenderWare %u.%u.%u.%u, build %u", version.major, version.minor, version.revision,
                version.binary, version.build);
    ImGui::SeparatorText("Decoded structure");
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
            ImGui::TextColored(ImVec4(1, 0.35F, 0.25F, 1), "%s", decoded.error.c_str());
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
                auto output = document.source_path();
                std::ostringstream suffix;
                suffix << ".geometry_" << std::hex << chunk.offset << ".obj";
                output.replace_filename(output.stem().string() + suffix.str());
                rws::export_geometry_obj(value, bytes, output);
                status = "Exported " + output.string();
            } catch (const std::exception& error) {
                status = error.what();
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
                                   ? ImVec4(1, .3F, .25F, 1)
                                   : ImVec4(1, .72F, .25F, 1),
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
    ImGui::SeparatorText("Raw payload");
}

struct MissionOverlays {
    std::vector<rwsman::GeometryPreview::MissionOverlayPoint> points;
    std::vector<rwsman::GeometryPreview::MissionOverlayLine> lines;
};

rws::Vec3 rws_point(const csf::Vec3 value) {
    return {value.x, value.y, value.z};
}

MissionOverlays make_mission_overlays(const csf::MissionScene& scene) {
    using Kind = rwsman::GeometryPreview::MissionOverlayKind;
    MissionOverlays result;
    const auto append_orientation = [&](const Kind kind, const std::uint32_t entry,
                                        const csf::Vec3 position, const float heading,
                                        const float pitch, const ImU32 color,
                                        const float length = 120.0F) {
        const auto origin = rws_point(position);
        result.lines.push_back(
            {kind, entry, origin,
             {origin.x + std::sin(heading) * std::cos(pitch) * length,
              origin.y - std::sin(pitch) * length,
              origin.z + std::cos(heading) * std::cos(pitch) * length},
             color, true});
    };
    for (const auto& actor : scene.actors())
        if (const auto spawn = scene.actor_spawn_position(actor)) {
            const auto color = IM_COL32(255, 150, 60, 255);
            result.points.push_back({Kind::actor, actor.source.entry_index,
                                     rws_point(*spawn), actor.name.value_or("Actor"), color});
            append_orientation(
                Kind::actor, actor.source.entry_index, *spawn,
                csf::mission_actor_angle_radians(actor.heading.value_or(0)),
                csf::mission_actor_angle_radians(actor.pitch.value_or(0)), color);
        }
    std::map<std::pair<std::int32_t, std::int32_t>, csf::Vec3> nav_points;
    for (const auto& group : scene.navigation())
        for (const auto& point : group.points)
            if (point.position && point.group_id && point.id) {
                nav_points[{*point.group_id, *point.id}] = *point.position;
                const auto color = group.type.value_or(0) == 0
                                       ? IM_COL32(60, 205, 255, 255)
                                   : group.type == 1 ? IM_COL32(85, 235, 145, 255)
                                                     : IM_COL32(245, 135, 245, 255);
                result.points.push_back({Kind::navigation_point, point.source.entry_index,
                                         rws_point(*point.position),
                                         point.name.value_or("Nav point"), color});
                append_orientation(Kind::navigation_point, point.source.entry_index,
                                   *point.position, point.heading.value_or(0),
                                   point.pitch.value_or(0), color, 70.0F);
            }
    auto add_connection = [&](const csf::NavConnection& connection) {
        if (!connection.valid) return;
        const auto origin = nav_points.find({*connection.origin_group, *connection.origin_point});
        const auto destination =
            nav_points.find({*connection.destination_group, *connection.destination_point});
        if (origin != nav_points.end() && destination != nav_points.end())
            result.lines.push_back({Kind::navigation_connection, connection.source.entry_index,
                                    rws_point(origin->second), rws_point(destination->second),
                                    IM_COL32(50, 175, 225, 180), true});
    };
    for (const auto& group : scene.navigation())
        for (const auto& connection : group.connections)
            add_connection(connection);
    for (const auto& connection : scene.cross_group_connections())
        add_connection(connection);
    for (const auto& dummy : scene.dummies())
        if (dummy.position) {
            const auto color = IM_COL32(190, 105, 255, 255);
            result.points.push_back({Kind::dummy, dummy.source.entry_index,
                                     rws_point(*dummy.position), dummy.name.value_or("Dummy"),
                                     color});
            append_orientation(Kind::dummy, dummy.source.entry_index, *dummy.position,
                               dummy.heading.value_or(0), dummy.pitch.value_or(0), color);
        }
    for (const auto& area : scene.areas()) {
        for (std::size_t i = 0; i < area.points.size(); ++i) {
            const auto& a = area.points[i];
            const auto& b = area.points[(i + 1) % area.points.size()];
            result.lines.push_back({Kind::area, area.source.entry_index, rws_point(a), rws_point(b),
                                    IM_COL32(255, 215, 70, 210)});
            if (area.height && std::isfinite(*area.height) && *area.height != 0) {
                const csf::Vec3 top_a{a.x, a.y + *area.height, a.z};
                const csf::Vec3 top_b{b.x, b.y + *area.height, b.z};
                result.lines.push_back({Kind::area, area.source.entry_index, rws_point(top_a),
                                        rws_point(top_b), IM_COL32(255, 215, 70, 150)});
                result.lines.push_back({Kind::area, area.source.entry_index, rws_point(a),
                                        rws_point(top_a), IM_COL32(255, 215, 70, 110)});
            }
        }
    }
    for (const auto& light : scene.lights())
        if (light.position) {
            const auto packed = light.color.value_or(0xFFF591U);
            const auto color = IM_COL32((packed >> 16U) & 0xFFU, (packed >> 8U) & 0xFFU,
                                        packed & 0xFFU, 255);
            result.points.push_back({Kind::light, light.source.entry_index,
                                     rws_point(*light.position), light.name.value_or("Light"),
                                     color});
            if (light.radius && *light.radius > 0 && std::isfinite(*light.radius)) {
                constexpr int segments = 24;
                for (int i = 0; i < segments; ++i) {
                    const float a = static_cast<float>(i) * 6.283185307F / segments;
                    const float b = static_cast<float>(i + 1) * 6.283185307F / segments;
                    const auto center = *light.position;
                    result.lines.push_back({Kind::light,
                                            light.source.entry_index,
                                            {center.x + std::cos(a) * *light.radius, center.y,
                                             center.z + std::sin(a) * *light.radius},
                                            {center.x + std::cos(b) * *light.radius, center.y,
                                             center.z + std::sin(b) * *light.radius},
                                            (color & IM_COL32(255, 255, 255, 0)) |
                                                IM_COL32(0, 0, 0, 120)});
                }
            }
        }
    for (const auto& effect : scene.effects()) {
        if (!effect.dummy_id) continue;
        const auto dummy = std::ranges::find_if(
            scene.dummies(), [&](const auto& value) { return value.id == effect.dummy_id; });
        if (dummy == scene.dummies().end() || !dummy->position) continue;
        const auto color = IM_COL32(255, 95, 150, 255);
        result.points.push_back({Kind::effect, effect.source.entry_index,
                                 rws_point(*dummy->position), effect.name.value_or("Effect"),
                                 color});
        append_orientation(Kind::effect, effect.source.entry_index, *dummy->position,
                           dummy->heading.value_or(0), dummy->pitch.value_or(0), color, 90.0F);
    }
    return result;
}

void append_cutscene_camera_overlays(
    const csf::MissionScene& scene,
    const std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>>& cutscenes,
    MissionOverlays& output) {
    using Kind = rwsman::GeometryPreview::MissionOverlayKind;
    std::set<std::uint32_t> added;
    for (const auto& [path, timeline] : cutscenes)
        for (const auto& script : timeline.scripts())
            for (const auto& action : script.actions) {
                if (action.kind != csf::CutsceneActionKind::camera || !action.numeric_value)
                    continue;
                const auto id = static_cast<std::int32_t>(*action.numeric_value);
                const auto dummy = std::ranges::find_if(
                    scene.dummies(), [&](const auto& value) { return value.id == id; });
                if (dummy == scene.dummies().end() || !dummy->position ||
                    !added.insert(dummy->source.entry_index).second)
                    continue;
                const float heading = dummy->heading.value_or(0);
                const float pitch = dummy->pitch.value_or(0);
                const auto origin = rws_point(*dummy->position);
                const rws::Vec3 forward{std::sin(heading) * std::cos(pitch), -std::sin(pitch),
                                        std::cos(heading) * std::cos(pitch)};
                const rws::Vec3 right{std::cos(heading), 0, -std::sin(heading)};
                constexpr float length = 300.0F, width = 100.0F;
                const rws::Vec3 tip{origin.x + forward.x * length, origin.y + forward.y * length,
                                    origin.z + forward.z * length};
                const rws::Vec3 left{tip.x - right.x * width, tip.y, tip.z - right.z * width};
                const rws::Vec3 right_tip{tip.x + right.x * width, tip.y, tip.z + right.z * width};
                const auto color = IM_COL32(255, 85, 220, 255);
                output.points.push_back({Kind::cutscene_camera, dummy->source.entry_index, origin,
                                         dummy->name.value_or("Cutscene camera") + " [" +
                                             path.filename().string() + "]",
                                         color});
                output.lines.push_back(
                    {Kind::cutscene_camera, dummy->source.entry_index, origin, tip, color});
                output.lines.push_back(
                    {Kind::cutscene_camera, dummy->source.entry_index, origin, left, color});
                output.lines.push_back(
                    {Kind::cutscene_camera, dummy->source.entry_index, origin, right_tip, color});
                output.lines.push_back(
                    {Kind::cutscene_camera, dummy->source.entry_index, left, right_tip, color});
            }
}

void append_actor_collision_overlays(const csf::MissionScene& scene,
                                     const std::vector<csf::ActorAssociation>& associations,
                                     MissionOverlays& output) {
    using Kind = rwsman::GeometryPreview::MissionOverlayKind;
    std::unordered_map<std::string, std::shared_ptr<const csf::CmoDocument>> cmo_cache;
    std::unordered_map<std::string, std::shared_ptr<const rws::Document>> physics_cache;
    const auto place = [&](const csf::MissionActor& actor, const rws::Vec3 local) {
        const float h = csf::mission_actor_angle_radians(actor.heading.value_or(0));
        const float p = csf::mission_actor_angle_radians(actor.pitch.value_or(0));
        const float cy = std::cos(h), sy = std::sin(h), cp = std::cos(p), sp = std::sin(p);
        const auto origin = rws_point(*scene.actor_spawn_position(actor));
        return rws::Vec3{origin.x + cy * local.x + sy * sp * local.y + sy * cp * local.z,
                         origin.y + cp * local.y - sp * local.z,
                         origin.z - sy * local.x + cy * sp * local.y + cy * cp * local.z};
    };
    const auto add_box = [&](const csf::MissionActor& actor, const std::uint32_t entry,
                             const rws::Vec3 center, const rws::Vec3 extent, const Kind kind,
                             const ImU32 color) {
        const std::array<rws::Vec3, 8> p{
            {{center.x - extent.x, center.y - extent.y, center.z - extent.z},
             {center.x + extent.x, center.y - extent.y, center.z - extent.z},
             {center.x - extent.x, center.y + extent.y, center.z - extent.z},
             {center.x + extent.x, center.y + extent.y, center.z - extent.z},
             {center.x - extent.x, center.y - extent.y, center.z + extent.z},
             {center.x + extent.x, center.y - extent.y, center.z + extent.z},
             {center.x - extent.x, center.y + extent.y, center.z + extent.z},
             {center.x + extent.x, center.y + extent.y, center.z + extent.z}}};
        constexpr std::array<std::array<int, 2>, 12> edges{{{0, 1},
                                                            {0, 2},
                                                            {0, 4},
                                                            {1, 3},
                                                            {1, 5},
                                                            {2, 3},
                                                            {2, 6},
                                                            {3, 7},
                                                            {4, 5},
                                                            {4, 6},
                                                            {5, 7},
                                                            {6, 7}}};
        for (const auto& edge : edges)
            output.lines.push_back(
                {kind, entry, place(actor, p[edge[0]]), place(actor, p[edge[1]]), color});
    };
    for (std::size_t actor_index = 0;
         actor_index < scene.actors().size() && actor_index < associations.size(); ++actor_index) {
        const auto& actor = scene.actors()[actor_index];
        const auto& association = associations[actor_index];
        if (!scene.actor_spawn_position(actor)) continue;
        if (association.collision_models.size() == 1 &&
            association.collision_models.front().resolved_path) {
            const auto path = *association.collision_models.front().resolved_path;
            const auto key = csf::ResourceIndex::normalize_path(path);
            auto found = cmo_cache.find(key);
            if (found == cmo_cache.end()) try {
                    found = cmo_cache
                                .emplace(key, std::make_shared<csf::CmoDocument>(
                                                  csf::CmoDocument::load(path)))
                                .first;
                } catch (const std::exception&) {}
            if (found != cmo_cache.end())
                for (const auto& shape : found->second->shapes()) {
                    if (shape.bone_index) continue;
                    rws::Vec3 center{};
                    if (shape.center) center = {shape.center->x, shape.center->y, shape.center->z};
                    if (shape.offset) {
                        center.x += shape.offset->x;
                        center.y += shape.offset->y;
                        center.z += shape.offset->z;
                    }
                    if (shape.kind == csf::CmoShapeKind::box ||
                        shape.kind == csf::CmoShapeKind::ellipsoid) {
                        rws::Vec3 extent{0.25F, 0.25F, 0.25F};
                        if (shape.dimensions)
                            extent = {shape.dimensions->x * .5F, shape.dimensions->y * .5F,
                                      shape.dimensions->z * .5F};
                        add_box(actor, actor.source.entry_index, center, extent, Kind::actor_cmo,
                                IM_COL32(255, 105, 215, 225));
                    } else {
                        const float radius = shape.radius.value_or(
                            shape.dimensions ? std::max({shape.dimensions->x, shape.dimensions->y,
                                                         shape.dimensions->z}) *
                                                   .5F
                                             : 0.25F);
                        constexpr int segments = 20;
                        for (int axis = 0; axis < 3; ++axis)
                            for (int i = 0; i < segments; ++i) {
                                const float a = static_cast<float>(i) * 6.283185307F / segments,
                                            b = static_cast<float>(i + 1) * 6.283185307F / segments;
                                const auto point = [&](float angle) {
                                    if (axis == 0)
                                        return rws::Vec3{center.x + std::cos(angle) * radius,
                                                         center.y,
                                                         center.z + std::sin(angle) * radius};
                                    if (axis == 1)
                                        return rws::Vec3{center.x + std::cos(angle) * radius,
                                                         center.y + std::sin(angle) * radius,
                                                         center.z};
                                    return rws::Vec3{center.x, center.y + std::cos(angle) * radius,
                                                     center.z + std::sin(angle) * radius};
                                };
                                output.lines.push_back({Kind::actor_cmo, actor.source.entry_index,
                                                        place(actor, point(a)),
                                                        place(actor, point(b)),
                                                        IM_COL32(255, 105, 215, 225)});
                            }
                    }
                }
        }
        if (association.physics_models.size() == 1 &&
            association.physics_models.front().resolved_path) {
            const auto path = *association.physics_models.front().resolved_path;
            const auto key = csf::ResourceIndex::normalize_path(path);
            auto found = physics_cache.find(key);
            if (found == physics_cache.end()) try {
                    found = physics_cache
                                .emplace(key,
                                         std::make_shared<rws::Document>(rws::Document::load(path)))
                                .first;
                } catch (const std::exception&) {}
            if (found != physics_cache.end()) {
                const auto collect = [&](auto&& self,
                                         const std::vector<rws::Chunk>& chunks) -> void {
                    for (const auto& chunk : chunks) {
                        if (chunk.type == 0x907) {
                            const auto body =
                                rws::decode_physics_body_def(chunk, found->second->bytes());
                            if (body)
                                for (const auto& volume :
                                     rws::flatten_physics_volumes(body.value->volume)) {
                                    const auto& b = volume.world_bounds;
                                    if (b.valid)
                                        add_box(actor, actor.source.entry_index,
                                                {(b.minimum.x + b.maximum.x) * .5F,
                                                 (b.minimum.y + b.maximum.y) * .5F,
                                                 (b.minimum.z + b.maximum.z) * .5F},
                                                {(b.maximum.x - b.minimum.x) * .5F,
                                                 (b.maximum.y - b.minimum.y) * .5F,
                                                 (b.maximum.z - b.minimum.z) * .5F},
                                                Kind::actor_physics, IM_COL32(70, 225, 255, 220));
                                }
                        }
                        self(self, chunk.children);
                    }
                };
                collect(collect, found->second->chunks());
            }
        }
    }
}

const csf::CsfSourceId* find_mission_source(const csf::MissionScene& scene,
                                            const std::uint32_t entry, std::string& kind,
                                            std::string& label) {
    for (const auto& value : scene.actors())
        if (value.source.entry_index == entry) {
            kind = "Actor";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& group : scene.navigation()) {
        if (group.source.entry_index == entry) {
            kind = "Navigation group";
            label = group.name.value_or("");
            return &group.source;
        }
        for (const auto& value : group.points)
            if (value.source.entry_index == entry) {
                kind = "Navigation point";
                label = value.name.value_or("");
                return &value.source;
            }
        for (const auto& value : group.connections)
            if (value.source.entry_index == entry) {
                kind = "Navigation connection";
                return &value.source;
            }
    }
    for (const auto& value : scene.cross_group_connections())
        if (value.source.entry_index == entry) {
            kind = "Navigation connection";
            return &value.source;
        }
    for (const auto& value : scene.dummies())
        if (value.source.entry_index == entry) {
            kind = "Dummy";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.areas())
        if (value.source.entry_index == entry) {
            kind = "Area";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.lights())
        if (value.source.entry_index == entry) {
            kind = "Light";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.effects())
        if (value.source.entry_index == entry) {
            kind = "Effect";
            label = value.name.value_or("");
            return &value.source;
        }
    for (const auto& value : scene.folders())
        if (value.source.entry_index == entry) {
            kind = "Folder";
            label = value.path;
            return &value.source;
        }
    return nullptr;
}

const csf::Node* find_csf_node(const std::vector<csf::Node>& nodes, const std::uint32_t entry) {
    for (const auto& node : nodes) {
        if (node.entry_index == entry) return &node;
        if (const auto* found = find_csf_node(node.children, entry)) return found;
    }
    return nullptr;
}

void draw_csf_subtree(const csf::Document& document, const csf::Node& node) {
    std::string name = "(anonymous)";
    if (node.identifier_index)
        if (const auto* value = document.identifier(*node.identifier_index))
            name = value->display_utf8();
    std::string value;
    if (const auto* integer = std::get_if<std::int32_t>(&node.scalar))
        value = " = " + std::to_string(*integer);
    else if (const auto* real = std::get_if<float>(&node.scalar))
        value = " = " + std::to_string(*real);
    else if (const auto* index = std::get_if<std::uint32_t>(&node.scalar))
        if (const auto* text = document.string(*index))
            value = " = \"" + text->display_utf8() + "\"";
    const auto text = name + value + "  [entry " + std::to_string(node.entry_index) + "]";
    ImGui::PushID(static_cast<int>(node.entry_index));
    if (node.children.empty())
        ImGui::BulletText("%s", text.c_str());
    else if (ImGui::TreeNodeEx(text.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& child : node.children)
            draw_csf_subtree(document, child);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void draw_mission_typed_details(const csf::MissionScene& scene, const std::uint32_t entry,
                                rwsman::GeometryPreview& preview) {
    const auto position = [](const std::optional<csf::Vec3>& value) {
        if (value)
            ImGui::Text("Position: %.6g, %.6g, %.6g", value->x, value->y, value->z);
        else
            ImGui::TextDisabled("Position unavailable");
    };
    const auto unknown = [](const std::vector<csf::RawField>& fields) {
        if (fields.empty()) return;
        if (ImGui::TreeNode("Unsupported fields", "%zu unsupported field%s", fields.size(),
                            fields.size() == 1 ? "" : "s")) {
            for (const auto& field : fields)
                ImGui::BulletText("%s [entry %u]", field.name.empty() ? "(anonymous)"
                                                                     : field.name.c_str(),
                                  field.source.entry_index);
            ImGui::TreePop();
        }
    };
    const auto select_point = [&](const std::optional<std::int32_t> group_id,
                                  const std::optional<std::int32_t> point_id,
                                  const char* label) {
        if (!group_id || !point_id) return;
        for (const auto& group : scene.navigation())
            for (const auto& point : group.points)
                if (point.group_id == group_id && point.id == point_id) {
                    if (ImGui::SmallButton(label))
                        preview.select_mission_entry(point.source.entry_index);
                    return;
                }
    };
    if (const auto found = std::ranges::find_if(
            scene.actors(), [&](const auto& value) { return value.source.entry_index == entry; });
        found != scene.actors().end()) {
        ImGui::SeparatorText("Actor");
        ImGui::Text("ID %d | class %d", found->id.value_or(-1), found->class_id.value_or(-1));
        if (found->position)
            ImGui::Text("Authored .POS: %.6g, %.6g, %.6g", found->position->x,
                        found->position->y, found->position->z);
        const auto spawn = scene.actor_spawn_position(*found);
        if (spawn)
            ImGui::Text("Effective spawn: %.6g, %.6g, %.6g%s", spawn->x, spawn->y, spawn->z,
                        found->group && found->cell && *found->group >= 0 && *found->cell >= 0
                            ? " (.CELDA)"
                            : " (.POS fallback)");
        ImGui::Text("Heading %.6g deg | pitch %.6g deg", found->heading.value_or(0),
                    found->pitch.value_or(0));
        ImGui::Text("Collision %d | flags 0x%08X | secondary explosion %d",
                    found->collision.value_or(-1), found->flags.value_or(0),
                    found->secondary_explosion.value_or(-1));
        ImGui::Text("Navigation cell: %d:%d", found->group.value_or(-1),
                    found->cell.value_or(-1));
        select_point(found->group, found->cell, "Select navigation cell");
        if (found->faction) ImGui::TextWrapped("Faction: %s", found->faction->c_str());
        if (found->portrait) ImGui::TextWrapped("Portrait: %s", found->portrait->c_str());
        if (!found->script_ids.empty()) {
            std::string scripts;
            for (const auto id : found->script_ids) {
                if (!scripts.empty()) scripts += ", ";
                scripts += std::to_string(id);
            }
            ImGui::TextWrapped("Scripts: %s", scripts.c_str());
        } else if (found->script)
            ImGui::TextWrapped("Script: %s", found->script->c_str());
        for (const auto& animation : found->animations)
            ImGui::BulletText("Animation %d: %s", animation.id.value_or(-1),
                              animation.type.value_or("(unnamed)").c_str());
        if (found->door_box)
            ImGui::Text("Door box: [%.4g %.4g %.4g] to [%.4g %.4g %.4g]",
                        (*found->door_box)[0].x, (*found->door_box)[0].y,
                        (*found->door_box)[0].z, (*found->door_box)[1].x,
                        (*found->door_box)[1].y, (*found->door_box)[1].z);
        unknown(found->unknown_fields);
        return;
    }
    for (const auto& group : scene.navigation()) {
        if (group.source.entry_index == entry) {
            ImGui::SeparatorText("Navigation group");
            ImGui::Text("ID %d | type %d | %zu points | %zu local links",
                        group.id.value_or(-1), group.type.value_or(-1), group.points.size(),
                        group.connections.size());
            unknown(group.unknown_fields);
            return;
        }
        for (const auto& point : group.points)
            if (point.source.entry_index == entry) {
                ImGui::SeparatorText("Navigation point");
                ImGui::Text("Identity %d:%d", point.group_id.value_or(-1),
                            point.id.value_or(-1));
                position(point.position);
                ImGui::Text("Heading %.6g rad | pitch %.6g rad", point.heading.value_or(0),
                            point.pitch.value_or(0));
                std::size_t incoming{}, outgoing{};
                const auto count = [&](const csf::NavConnection& link) {
                    if (link.origin_group == point.group_id && link.origin_point == point.id)
                        ++outgoing;
                    if (link.destination_group == point.group_id &&
                        link.destination_point == point.id)
                        ++incoming;
                };
                for (const auto& owner : scene.navigation())
                    for (const auto& link : owner.connections) count(link);
                for (const auto& link : scene.cross_group_connections()) count(link);
                ImGui::Text("Incoming %zu | outgoing %zu", incoming, outgoing);
                unknown(point.unknown_fields);
                return;
            }
        for (const auto& link : group.connections)
            if (link.source.entry_index == entry) {
                ImGui::SeparatorText("Navigation connection");
                ImGui::Text("%d:%d -> %d:%d", link.origin_group.value_or(-1),
                            link.origin_point.value_or(-1), link.destination_group.value_or(-1),
                            link.destination_point.value_or(-1));
                ImGui::Text("Status: %s", link.valid ? "valid" : link.invalid_reason.c_str());
                select_point(link.origin_group, link.origin_point, "Select origin");
                ImGui::SameLine();
                select_point(link.destination_group, link.destination_point, "Select destination");
                return;
            }
    }
    for (const auto& link : scene.cross_group_connections())
        if (link.source.entry_index == entry) {
            ImGui::SeparatorText("Cross-group connection");
            ImGui::Text("%d:%d -> %d:%d", link.origin_group.value_or(-1),
                        link.origin_point.value_or(-1), link.destination_group.value_or(-1),
                        link.destination_point.value_or(-1));
            ImGui::Text("Status: %s", link.valid ? "valid" : link.invalid_reason.c_str());
            select_point(link.origin_group, link.origin_point, "Select origin");
            ImGui::SameLine();
            select_point(link.destination_group, link.destination_point, "Select destination");
            return;
        }
    if (const auto found = std::ranges::find_if(
            scene.dummies(), [&](const auto& value) { return value.source.entry_index == entry; });
        found != scene.dummies().end()) {
        ImGui::SeparatorText("Dummy");
        ImGui::Text("ID %d | heading %.6g rad | pitch %.6g rad", found->id.value_or(-1),
                    found->heading.value_or(0), found->pitch.value_or(0));
        position(found->position);
        for (const auto& folder : scene.folders())
            if (found->id && std::ranges::find(folder.element_ids, *found->id) !=
                                 folder.element_ids.end())
                ImGui::TextWrapped("Folder: %s", folder.path.c_str());
        for (const auto& effect : scene.effects())
            if (effect.dummy_id == found->id) {
                ImGui::PushID(static_cast<int>(effect.source.entry_index));
                if (ImGui::Selectable(("Effect: " + effect.name.value_or("(unnamed)")).c_str()))
                    preview.select_mission_entry(effect.source.entry_index);
                ImGui::PopID();
            }
        unknown(found->unknown_fields);
        return;
    }
    if (const auto found = std::ranges::find_if(
            scene.areas(), [&](const auto& value) { return value.source.entry_index == entry; });
        found != scene.areas().end()) {
        ImGui::SeparatorText("Area");
        ImGui::Text("ID %d | %zu vertices | height %.6g", found->id.value_or(-1),
                    found->points.size(), found->height.value_or(0));
        ImGui::Text("Flags 0x%08X | occlusion %d | reverb %d | limit reverb %d",
                    static_cast<unsigned>(found->flags.value_or(0)),
                    found->occlusion.value_or(-1), found->reverb.value_or(-1),
                    found->limit_reverb.value_or(-1));
        unknown(found->unknown_fields);
        return;
    }
    if (const auto found = std::ranges::find_if(
            scene.lights(), [&](const auto& value) { return value.source.entry_index == entry; });
        found != scene.lights().end()) {
        ImGui::SeparatorText("Light");
        ImGui::Text("ID %d | radius %.6g | modulation %d", found->id.value_or(-1),
                    found->radius.value_or(0), found->modulate.value_or(-1));
        position(found->position);
        const auto color = found->color.value_or(0);
        ImGui::ColorButton("SCN color",
                           ImVec4(static_cast<float>((color >> 16U) & 0xFFU) / 255.0F,
                                  static_cast<float>((color >> 8U) & 0xFFU) / 255.0F,
                                  static_cast<float>(color & 0xFFU) / 255.0F, 1));
        ImGui::SameLine();
        ImGui::Text("#%06X", color & 0xFFFFFFU);
        unknown(found->unknown_fields);
        return;
    }
    if (const auto found = std::ranges::find_if(
            scene.effects(), [&](const auto& value) { return value.source.entry_index == entry; });
        found != scene.effects().end()) {
        ImGui::SeparatorText("Effect");
        ImGui::Text("ID %d | class %d | dummy %d", found->id.value_or(-1),
                    found->class_id.value_or(-1), found->dummy_id.value_or(-1));
        ImGui::Text("Priority %d | share group %d", found->priority.value_or(-1),
                    found->share_group.value_or(-1));
        if (found->dummy_id)
            if (const auto dummy = std::ranges::find_if(
                    scene.dummies(), [&](const auto& value) { return value.id == found->dummy_id; });
                dummy != scene.dummies().end())
                if (ImGui::SmallButton("Select placement dummy"))
                    preview.select_mission_entry(dummy->source.entry_index);
        unknown(found->unknown_fields);
    }
}

} // namespace

int run_app(const std::optional<std::filesystem::path>& initial_path) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
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
    std::unique_ptr<rws::Document> collision_document;
    std::unique_ptr<csf::MissionGraph> mission_graph;
    std::unique_ptr<csf::Document> mission_document;
    std::unique_ptr<csf::MissionScene> mission_scene;
    std::unique_ptr<csf::MissionSymbolIndex> mission_symbols;
    std::unique_ptr<csf::ObjectDatabase> mission_objects;
    std::unique_ptr<csf::AnimationCatalog> mission_animations;
    csf::ScriptAnimationIndex mission_script_animations;
    std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>> mission_cutscenes;
    std::vector<csf::ActorAssociation> mission_actor_associations;
    std::shared_ptr<const rws::AnimationClip> mission_active_clip;
    std::optional<std::uint32_t> mission_animated_actor;
    std::string mission_active_animation;
    float mission_animation_time{}, mission_animation_speed{1.0F}, mission_animation_accumulator{};
    int mission_animation_fps{30};
    bool mission_animation_playing{}, mission_animation_loop{true};
    auto recent_pairs = load_recent_pairs();
    std::string collision_status = "No document loaded";
    bool main_is_collision{};
    rwsman::GeometryPreview geometry_preview;
    std::optional<std::uint64_t> selected;
    ChunkDisplayNames display_names;
    Workspace workspace = Workspace::scene;
    bool show_scene_tree = false;
    bool show_inspector = false;
    bool show_viewport_tools = false;
    bool maximize_viewport = false;
    std::optional<std::uint64_t> previous_selection;
    std::array<char, 128> mission_search{};
    std::string status =
        "Drop an .scn, .rpc, or .rws file on this window, or pass one on the command line.";
    auto restore_mission_overlays = [&] {
        if (!mission_scene) return;
        const auto overlays = make_mission_overlays(*mission_scene);
        geometry_preview.set_mission_overlays(overlays.points, overlays.lines);
    };
    auto pair_collision = [&](const std::filesystem::path& candidate_path, const bool remember) {
        if (!document) return false;
        try {
            auto candidate = std::make_unique<rws::Document>(rws::Document::load(candidate_path));
            const auto worlds = rws::recover_worlds(candidate->chunks(), candidate->bytes());
            if (worlds.empty() || std::none_of(worlds.begin(), worlds.end(), [](const auto& world) {
                    return !world.sectors.empty();
                }))
                throw std::runtime_error("Selected companion has no recoverable World sectors");
            collision_document = std::move(candidate);
            collision_status = "Loaded manual companion " + candidate_path.string();
            geometry_preview.clear();
            restore_mission_overlays();
            if (remember) {
                std::error_code error;
                const auto main_path =
                    std::filesystem::weakly_canonical(document->source_path(), error);
                const auto collision_path =
                    std::filesystem::weakly_canonical(candidate_path, error);
                recent_pairs.erase(std::remove_if(recent_pairs.begin(), recent_pairs.end(),
                                                  [&](const RecentPair& pair) {
                                                      return pair.main == main_path &&
                                                             pair.collision == collision_path;
                                                  }),
                                   recent_pairs.end());
                recent_pairs.insert(recent_pairs.begin(), {main_path, collision_path});
                if (recent_pairs.size() > 8) recent_pairs.resize(8);
                save_recent_pairs(recent_pairs);
            }
            return true;
        } catch (const std::exception& error) {
            status = "Collision companion unchanged: " + std::string(error.what());
            return false;
        }
    };
    auto load = [&](const std::filesystem::path& path) {
        try {
            auto loaded_document = std::make_unique<rws::Document>(rws::Document::load(path));
            auto source_extension = path.extension().string();
            std::ranges::transform(
                source_extension, source_extension.begin(),
                [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (source_extension == ".rpc" && (loaded_document->chunks().empty() ||
                                               loaded_document->chunks().front().type != 0x10U))
                throw std::runtime_error("RPC root is not a RenderWare Clump (0x10)");
            mission_graph.reset();
            mission_document.reset();
            mission_scene.reset();
            mission_symbols.reset();
            mission_objects.reset();
            mission_animations.reset();
            mission_script_animations = {};
            mission_cutscenes.clear();
            mission_active_clip.reset();
            mission_animated_actor.reset();
            mission_active_animation.clear();
            mission_animation_playing = false;
            mission_actor_associations.clear();
            document = std::move(loaded_document);
            collision_document.reset();
            collision_status.clear();
            main_is_collision = false;
            geometry_preview.clear();
            auto stem = path.stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            main_is_collision = stem.size() >= 4 && stem.ends_with("_col");
            if (main_is_collision) {
                const auto worlds = rws::recover_worlds(document->chunks(), document->bytes());
                collision_status = !worlds.empty() && !worlds.front().sectors.empty()
                                       ? "Opened collision World directly"
                                       : "The _col file has no recoverable World";
            } else if (source_extension != ".rpc") {
                auto companion = path;
                companion.replace_filename(path.stem().string() + "_col" +
                                           path.extension().string());
                std::error_code filesystem_error;
                if (std::filesystem::is_regular_file(companion, filesystem_error)) {
                    try {
                        auto candidate =
                            std::make_unique<rws::Document>(rws::Document::load(companion));
                        const auto worlds =
                            rws::recover_worlds(candidate->chunks(), candidate->bytes());
                        if (!worlds.empty() && !worlds.front().sectors.empty()) {
                            collision_status = "Loaded " + companion.string();
                            collision_document = std::move(candidate);
                        } else {
                            collision_status =
                                "Companion has no recoverable World: " + companion.string();
                        }
                    } catch (const std::exception& error) {
                        collision_status = "Companion failed to load: " + std::string(error.what());
                    }
                } else {
                    collision_status = "Companion not found: " + companion.string();
                }
            }
            display_names = resolve_chunk_display_names(document->chunks(), document->bytes(),
                                                        document->scene_instances());
            const auto* first_geometry = find_first_chunk(document->chunks(), 0x0F);
            if (first_geometry)
                selected = first_geometry->offset;
            else if (!document->chunks().empty())
                selected = document->chunks().front().offset;
            else
                selected.reset();
            previous_selection.reset();
            const bool has_scene = !document->scene_instances().empty() ||
                                   find_first_chunk(document->chunks(), 0x10) != nullptr ||
                                   find_first_chunk(document->chunks(), 0x0B) != nullptr ||
                                   find_first_chunk(document->chunks(), 0x907) != nullptr ||
                                   find_first_chunk(document->chunks(), 0x909) != nullptr;
            workspace = has_scene ? Workspace::scene
                                  : (first_geometry ? Workspace::geometry : Workspace::inspector);
            status = source_extension == ".rpc"
                         ? "Loaded RPC Clump model laboratory: " + path.string()
                         : "Loaded " + path.string();
            const auto title = path.filename().string() + " - CSF RWS Tools";
            glfwSetWindowTitle(window, title.c_str());
        } catch (const std::exception& error) {
            status = error.what();
        }
    };
    auto load_mission = [&](const std::filesystem::path& path) {
#ifndef NDEBUG
        begin_mission_debug_log(path);
#endif
        const char* mission_stage = "building mission resource graph";
        try {
            auto candidate_graph = std::make_unique<csf::MissionGraph>(
                csf::MissionGraph::load(csf::MissionOptions{path}));
            mission_stage = "loading and projecting the mission scene";
            auto candidate_document =
                std::make_unique<csf::Document>(csf::Document::load(candidate_graph->scene_path()));
            auto candidate_scene = std::make_unique<csf::MissionScene>(
                csf::MissionScene::project(*candidate_document));
            auto candidate_symbols = std::make_unique<csf::MissionSymbolIndex>();
            csf::ScriptAnimationIndex candidate_script_animations;
            candidate_symbols->add_scene(*candidate_scene);
            mission_stage = "loading referenced scripts and databases";
            for (const auto& node : candidate_graph->nodes()) {
                if (node.resolved_path.empty() ||
                    node.resolved_path == candidate_graph->scene_path() ||
                    (node.kind != csf::ResourceKind::mission_script &&
                     node.kind != csf::ResourceKind::cutscene_script &&
                     node.kind != csf::ResourceKind::database))
                    continue;
                const auto reference_document = csf::Document::load(node.resolved_path);
                if (reference_document.state() != csf::ParseState::non_csffbs) {
                    candidate_symbols->add_document(reference_document);
                    if (node.kind == csf::ResourceKind::mission_script)
                        candidate_script_animations.add_document(reference_document);
                }
            }
            auto candidate_objects = std::make_unique<csf::ObjectDatabase>();
            auto candidate_weapons = std::make_unique<csf::WeaponDatabase>();
            auto candidate_animations = std::make_unique<csf::AnimationCatalog>();
            std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>>
                candidate_cutscenes;
            mission_stage = "projecting mission databases and cutscenes";
            for (const auto& node : candidate_graph->nodes()) {
                auto name = path_utf8(node.resolved_path.filename());
                std::ranges::transform(name, name.begin(), [](const unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
                if (name == "objetos.bdd" && node.state == csf::LoadState::available) {
                    *candidate_objects =
                        csf::ObjectDatabase::project(csf::Document::load(node.resolved_path));
                }
                if (name == "armas.bdd" && node.state == csf::LoadState::available)
                    *candidate_weapons =
                        csf::WeaponDatabase::project(csf::Document::load(node.resolved_path));
                if (name == "anims.bdd" && node.state == csf::LoadState::available)
                    *candidate_animations = csf::AnimationCatalog::project(
                        csf::Document::load(node.resolved_path), &candidate_graph->index());
                if (node.kind == csf::ResourceKind::cutscene_script &&
                    node.state == csf::LoadState::available) {
                    const auto cutscene_document = csf::Document::load(node.resolved_path);
                    if (cutscene_document.state() != csf::ParseState::non_csffbs)
                        candidate_cutscenes.emplace_back(
                            node.resolved_path, csf::CutsceneTimeline::project(cutscene_document));
                }
            }
            auto candidate_associations = csf::associate_actors(
                *candidate_scene, *candidate_objects, candidate_graph->index());
            mission_stage = "loading mission actor models";
            std::vector<rwsman::GeometryPreview::MissionActorModel> candidate_actor_models;
            std::unordered_map<std::string, std::shared_ptr<const rws::Document>>
                actor_prototype_cache;
            constexpr std::size_t actor_budget = 512, prototype_budget = 64;
            for (std::size_t i = 0;
                 i < candidate_associations.size() && candidate_actor_models.size() < actor_budget;
                 ++i) {
                const auto& association = candidate_associations[i];
                if (association.visual_models.size() != 1 ||
                    !association.visual_models.front().resolved_path)
                    continue;
                const auto key = csf::ResourceIndex::normalize_path(
                    *association.visual_models.front().resolved_path);
                auto cached = actor_prototype_cache.find(key);
                if (cached == actor_prototype_cache.end()) {
                    if (actor_prototype_cache.size() >= prototype_budget) continue;
                    try {
                        auto loaded = std::make_shared<rws::Document>(
                            rws::Document::load(*association.visual_models.front().resolved_path));
                        if (loaded->chunks().empty() || loaded->chunks().front().type != 0x10U)
                            continue;
                        cached = actor_prototype_cache.emplace(key, std::move(loaded)).first;
                    } catch (const std::exception&) {
                        continue;
                    }
                }
                const auto& actor = candidate_scene->actors()[i];
                const auto spawn = candidate_scene->actor_spawn_position(actor);
                if (!spawn) continue;
                rwsman::GeometryPreview::MissionActorModel actor_model{
                    actor.source.entry_index, cached->second, rws_point(*spawn),
                    csf::mission_actor_angle_radians(actor.heading.value_or(0)),
                    csf::mission_actor_angle_radians(actor.pitch.value_or(0))};
                if (association.definitions.size() == 1) {
                    for (const auto weapon_id : association.definitions.front()->weapon_ids) {
                        const auto* weapon = candidate_weapons->find_id(weapon_id);
                        if (!weapon || !weapon->third_person_model) continue;
                        const auto resolution = candidate_graph->index().resolve(
                            *weapon->third_person_model);
                        if (resolution.candidate_indices.size() != 1 ||
                            resolution.status == csf::ResolutionStatus::ambiguous)
                            continue;
                        const auto& weapon_path = candidate_graph->index()
                                                      .resources()[resolution.candidate_indices.front()]
                                                      .path;
                        const auto weapon_key = csf::ResourceIndex::normalize_path(weapon_path);
                        auto weapon_cached = actor_prototype_cache.find(weapon_key);
                        if (weapon_cached == actor_prototype_cache.end()) {
                            if (actor_prototype_cache.size() >= prototype_budget) break;
                            try {
                                auto loaded =
                                    std::make_shared<rws::Document>(rws::Document::load(weapon_path));
                                if (loaded->chunks().empty() ||
                                    loaded->chunks().front().type != 0x10U)
                                    continue;
                                weapon_cached =
                                    actor_prototype_cache.emplace(weapon_key, std::move(loaded)).first;
                            } catch (const std::exception&) {
                                continue;
                            }
                        }
                        auto hand = weapon->hand.value_or("");
                        std::ranges::transform(hand, hand.begin(), [](const unsigned char value) {
                            return static_cast<char>(std::toupper(value));
                        });
                        actor_model.attachments.push_back(
                            {weapon_cached->second,
                             weapon->name.value_or("Weapon " + std::to_string(weapon_id)),
                             hand.find("IZQUIERDA") != std::string::npos ||
                                 hand.find("LEFT") != std::string::npos});
                        // Object definitions list inventory in preference order. Display the
                        // first resolved third-person model as the default equipped item.
                        break;
                    }
                }
                candidate_actor_models.push_back(std::move(actor_model));
            }
            const auto resolved = [&](const csf::DependencyKind kind) -> std::filesystem::path {
                for (const auto& edge : candidate_graph->edges()) {
                    if (edge.kind != kind || !edge.target) continue;
                    const auto found =
                        std::ranges::find_if(candidate_graph->nodes(), [&](const auto& node) {
                            return node.id == *edge.target &&
                                   node.state == csf::LoadState::available;
                        });
                    if (found != candidate_graph->nodes().end()) return found->resolved_path;
                }
                return {};
            };
            const auto visual_path = resolved(csf::DependencyKind::visual_map);
            if (visual_path.empty()) throw std::runtime_error("Mission has no resolved visual map");
            mission_stage = "loading resolved visual and collision maps";
            auto candidate_visual =
                std::make_unique<rws::Document>(rws::Document::load(visual_path));
            std::unique_ptr<rws::Document> candidate_collision;
            const auto collision_path = resolved(csf::DependencyKind::collision_map);
            if (!collision_path.empty())
                candidate_collision =
                    std::make_unique<rws::Document>(rws::Document::load(collision_path));
            auto overlays = make_mission_overlays(*candidate_scene);
            append_cutscene_camera_overlays(*candidate_scene, candidate_cutscenes, overlays);
            append_actor_collision_overlays(*candidate_scene, candidate_associations, overlays);

            mission_stage = "committing mission state and updating the UI";
            document = std::move(candidate_visual);
            collision_document = std::move(candidate_collision);
            mission_graph = std::move(candidate_graph);
            mission_document = std::move(candidate_document);
            mission_scene = std::move(candidate_scene);
            mission_symbols = std::move(candidate_symbols);
            mission_objects = std::move(candidate_objects);
            mission_animations = std::move(candidate_animations);
            mission_script_animations = std::move(candidate_script_animations);
            mission_cutscenes = std::move(candidate_cutscenes);
            mission_actor_associations = std::move(candidate_associations);
            mission_active_clip.reset();
            mission_animated_actor.reset();
            mission_active_animation.clear();
            mission_animation_playing = false;
            main_is_collision = false;
            collision_status = collision_document
                                   ? "Loaded mission collision map " + path_utf8(collision_path)
                                   : "Mission collision map is unresolved";
            geometry_preview.clear();
            geometry_preview.set_texture_catalog(mission_graph->textures());
            geometry_preview.set_mission_overlays(overlays.points, overlays.lines);
            geometry_preview.set_mission_actor_models(std::move(candidate_actor_models));
            display_names = resolve_chunk_display_names(document->chunks(), document->bytes(),
                                                        document->scene_instances());
            selected.reset();
            previous_selection.reset();
            workspace = Workspace::mission;
            show_scene_tree = true;
            show_inspector = true;
            show_viewport_tools = false;
            status = "Loaded mission " + path_utf8(mission_graph->scene_path());
            const auto title =
                path_utf8(mission_graph->scene_path().filename()) + " - CSF Mission Explorer";
            glfwSetWindowTitle(window, title.c_str());
        } catch (const std::exception& error) {
            status = "Mission unchanged: " + std::string(error.what());
#ifndef NDEBUG
            log_mission_failure(path, mission_stage, error.what());
            status += " (details: rws-man-debug.log beside rws-man.exe)";
#endif
        }
    };
    if (initial_path) {
        auto extension = initial_path->extension().string();
        std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (extension == ".scn")
            load_mission(*initial_path);
        else
            load(*initial_path);
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (dropped_file) {
            auto extension = dropped_file->extension().string();
            std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (extension == ".scn")
                load_mission(*dropped_file);
            else
                load(*dropped_file);
            dropped_file.reset();
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const auto open_document = [&] {
#ifdef _WIN32
            if (const auto path = choose_rws_file(document ? document->source_path().parent_path()
                                                           : std::filesystem::path{}))
                load(*path);
#endif
        };
        const auto open_mission = [&] {
#ifdef _WIN32
            const auto directory = mission_graph ? mission_graph->scene_path().parent_path()
                                                 : (document ? document->source_path().parent_path()
                                                             : std::filesystem::path{});
            if (const auto path = choose_mission_file(directory)) load_mission(*path);
#endif
        };
        const auto save_copy = [&] {
            if (!document) return;
            try {
                auto output = document->source_path();
                output.replace_filename(output.stem().string() + ".edited" +
                                        output.extension().string());
                document->save_as(output);
                status = "Saved " + output.string();
            } catch (const std::exception& error) {
                status = error.what();
            }
        };

        const auto& io = ImGui::GetIO();
        const bool shortcuts_enabled = !io.WantTextInput && !io.WantCaptureKeyboard;
        if (shortcuts_enabled && io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O))
            open_mission();
        else if (shortcuts_enabled && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O))
            open_document();
        if (shortcuts_enabled && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) save_copy();
        if (shortcuts_enabled && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space) &&
            (maximize_viewport || show_scene_tree || show_inspector || show_viewport_tools))
            maximize_viewport = !maximize_viewport;
        if (shortcuts_enabled && !io.KeyCtrl && !io.KeyAlt) {
            if (ImGui::IsKeyPressed(ImGuiKey_1)) workspace = Workspace::scene;
            if (ImGui::IsKeyPressed(ImGuiKey_2)) workspace = Workspace::geometry;
            if (ImGui::IsKeyPressed(ImGuiKey_3)) workspace = Workspace::inspector;
            if (ImGui::IsKeyPressed(ImGuiKey_4) && mission_scene) workspace = Workspace::animation;
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("CSF RWS Tools", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_MenuBar);

        const auto* selected_chunk =
            document && selected ? find_chunk(document->chunks(), *selected) : nullptr;
        const auto* selected_instance =
            document && selected ? find_instance(document->scene_instances(), *selected) : nullptr;
        const auto* selected_clump =
            document && selected ? find_enclosing_clump(document->chunks(), *selected) : nullptr;
        const auto* collision_export_document =
            main_is_collision ? document.get() : collision_document.get();
        static std::string previous_window_title;
        const std::string window_title =
            mission_graph
                ? path_utf8(mission_graph->scene_path().filename()) + " - CSF Mission Explorer"
            : document ? path_utf8(document->source_path().filename()) +
                             (document->dirty() ? " *" : "") + " - CSF RWS Tools"
                       : "CSF RWS Tools - rws-man";
        if (window_title != previous_window_title) {
            glfwSetWindowTitle(window, window_title.c_str());
            previous_window_title = window_title;
        }

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open mission...", "Ctrl+Shift+O")) open_mission();
                if (ImGui::MenuItem("Open RWS...", "Ctrl+O")) open_document();
                if (ImGui::BeginMenu("Collision companion", document && !main_is_collision)) {
                    if (ImGui::MenuItem("Open companion...")) {
#ifdef _WIN32
                        if (const auto path =
                                choose_rws_file(document->source_path().parent_path()))
                            pair_collision(*path, true);
#endif
                    }
                    if (ImGui::MenuItem("Clear companion", nullptr, false,
                                        collision_document != nullptr)) {
                        collision_document.reset();
                        collision_status = "Collision companion cleared";
                        geometry_preview.clear();
                        restore_mission_overlays();
                    }
                    if (ImGui::BeginMenu("Recent pairings", !recent_pairs.empty())) {
                        for (const auto& pair : recent_pairs) {
                            const auto label = pair.main.filename().string() + " + " +
                                               pair.collision.filename().string();
                            if (ImGui::MenuItem(label.c_str())) {
                                if (!document || document->source_path() != pair.main)
                                    load(pair.main);
                                if (document) pair_collision(pair.collision, false);
                            }
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save copy", "Ctrl+S", false, document != nullptr)) save_copy();
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(window, GLFW_TRUE);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Mission", nullptr, workspace == Workspace::mission,
                                    mission_scene != nullptr))
                    workspace = Workspace::mission;
                if (ImGui::MenuItem("Animation", "4", workspace == Workspace::animation,
                                    mission_scene != nullptr))
                    workspace = Workspace::animation;
                if (ImGui::MenuItem("Scene", "1", workspace == Workspace::scene,
                                    document != nullptr))
                    workspace = Workspace::scene;
                if (ImGui::MenuItem("Selected geometry", "2", workspace == Workspace::geometry,
                                    document != nullptr))
                    workspace = Workspace::geometry;
                if (ImGui::MenuItem("Inspector / Hex", "3", workspace == Workspace::inspector,
                                    document != nullptr))
                    workspace = Workspace::inspector;
                ImGui::Separator();
                ImGui::MenuItem("Scene tree", nullptr, &show_scene_tree);
                if (ImGui::MenuItem("Selection inspector", nullptr,
                                    show_inspector && !show_viewport_tools)) {
                    show_inspector = !(show_inspector && !show_viewport_tools);
                    show_viewport_tools = false;
                }
                if (ImGui::MenuItem("Maximize viewport", "Ctrl+Space", maximize_viewport,
                                    maximize_viewport || show_scene_tree || show_inspector ||
                                        show_viewport_tools))
                    maximize_viewport = !maximize_viewport;
                ImGui::Separator();
                if (ImGui::MenuItem("Maximize window")) glfwMaximizeWindow(window);
                if (ImGui::MenuItem("Restore window")) glfwRestoreWindow(window);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Tools")) {
                if (ImGui::MenuItem("Reload preview from edited bytes", nullptr, false,
                                    document != nullptr)) {
                    geometry_preview.clear();
                    restore_mission_overlays();
                    status = "Preview will reload from the current in-memory document";
                }
                if (ImGui::MenuItem("Open scene / collision tools", nullptr, false,
                                    document != nullptr)) {
                    workspace = Workspace::scene;
                    maximize_viewport = false;
                    show_inspector = true;
                    show_viewport_tools = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Export")) {
                if (ImGui::MenuItem("Whole scene (glTF)", nullptr, false, document != nullptr)) {
                    try {
                        auto output = document->source_path();
                        output.replace_filename(output.stem().string() + ".scene.gltf");
                        const auto stats =
                            rws::export_scene_gltf(document->chunks(), document->scene_instances(),
                                                   document->bytes(), output);
                        std::ostringstream message;
                        message << "Exported " << stats.atomic_instances << " atomic meshes ("
                                << stats.custom_instances << " CSF placements, "
                                << stats.unresolved_instances << " unresolved) and "
                                << stats.recovered_world_sectors << " recovered World sectors ("
                                << stats.world_sectors << " exported meshes) to "
                                << output.string();
                        status = message.str();
                    } catch (const std::exception& error) {
                        status = error.what();
                    }
                }
                if (ImGui::MenuItem("Selected Clump (glTF)", nullptr, false,
                                    selected_clump != nullptr)) {
                    try {
                        auto output = document->source_path();
                        std::ostringstream suffix;
                        suffix << document->source_path().stem().string() << ".clump_0x" << std::hex
                               << std::uppercase << selected_clump->offset << ".gltf";
                        output.replace_filename(suffix.str());
                        const auto stats =
                            rws::export_clump_gltf(*selected_clump, document->bytes(), output);
                        status = "Exported Clump with " + std::to_string(stats.atomic_instances) +
                                 " Atomics to " + output.string();
                    } catch (const std::exception& error) {
                        status = error.what();
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Collision only (glTF)", nullptr, false,
                                    collision_export_document != nullptr)) {
                    try {
                        auto output = collision_export_document->source_path();
                        output.replace_filename(output.stem().string() + ".collision.gltf");
                        const auto stats =
                            rws::export_collision_gltf(collision_export_document->chunks(),
                                                       collision_export_document->bytes(), output);
                        status = "Exported collision glTF: " + std::to_string(stats.triangles) +
                                 " triangles to " + output.string();
                    } catch (const std::exception& error) {
                        status = error.what();
                    }
                }
                if (ImGui::MenuItem("Collision only (OBJ)", nullptr, false,
                                    collision_export_document != nullptr)) {
                    try {
                        auto output = collision_export_document->source_path();
                        output.replace_filename(output.stem().string() + ".collision.obj");
                        const auto stats =
                            rws::export_collision_obj(collision_export_document->chunks(),
                                                      collision_export_document->bytes(), output);
                        status = "Exported collision OBJ: " + std::to_string(stats.triangles) +
                                 " triangles to " + output.string();
                    } catch (const std::exception& error) {
                        status = error.what();
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help")) {
                ImGui::TextDisabled("Viewport controls");
                ImGui::Separator();
                ImGui::TextUnformatted("Left drag: look   Right drag: orbit   Middle drag: pan");
                ImGui::TextUnformatted("Wheel: zoom   Double-click: frame   WASD/QE: move");
                ImGui::TextUnformatted("1/2/3/4: workspace   Ctrl+Space: maximize viewport");
                ImGui::TextUnformatted("Ctrl+Shift+O: open mission");
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        const float toolbar_height =
            ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0F;
        ImGui::BeginChild("main_toolbar", {0.0F, toolbar_height}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::Button("Open")) open_document();
        ImGui::SameLine();
        if (ImGui::Button("Mission")) open_mission();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0F);
        if (ImGui::BeginCombo("##workspace", workspace_name(workspace))) {
            const auto mission_flags =
                mission_scene ? ImGuiSelectableFlags_None : ImGuiSelectableFlags_Disabled;
            if (ImGui::Selectable("Mission", workspace == Workspace::mission, mission_flags) &&
                mission_scene)
                workspace = Workspace::mission;
            if (ImGui::Selectable("Animation", workspace == Workspace::animation, mission_flags) &&
                mission_scene)
                workspace = Workspace::animation;
            if (ImGui::Selectable("Scene", workspace == Workspace::scene))
                workspace = Workspace::scene;
            if (ImGui::Selectable("Geometry", workspace == Workspace::geometry))
                workspace = Workspace::geometry;
            if (ImGui::Selectable("Inspector", workspace == Workspace::inspector))
                workspace = Workspace::inspector;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!document);
        if (ImGui::Button(show_scene_tree && !maximize_viewport ? "Hide tree" : "Scene tree")) {
            show_scene_tree = maximize_viewport ? true : !show_scene_tree;
            maximize_viewport = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(show_inspector && !show_viewport_tools && !maximize_viewport
                              ? "Hide inspector"
                              : "Inspector")) {
            show_inspector = maximize_viewport ? true : !(show_inspector && !show_viewport_tools);
            show_viewport_tools = false;
            maximize_viewport = false;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!maximize_viewport && !show_scene_tree && !show_inspector &&
                             !show_viewport_tools);
        if (ImGui::Button(maximize_viewport ? "Restore panels" : "Maximize viewport"))
            maximize_viewport = !maximize_viewport;
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (document) {
            ImGui::SameLine();
            ImGui::TextDisabled(
                "%s%s",
                path_utf8((mission_graph ? mission_graph->scene_path() : document->source_path())
                              .filename())
                    .c_str(),
                document->dirty() ? " *" : "");
        }
        ImGui::EndChild();

        const float status_height = ImGui::GetTextLineHeightWithSpacing() + 7.0F;
        ImGui::BeginChild("workspace_area", {0.0F, -status_height}, ImGuiChildFlags_None);
        if (!document) {
            const auto available = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPosY(std::max(available.y * 0.42F, 20.0F));
            const char* prompt = "Drop an .scn/.rpc/.rws file here, or open a Mission/model";
            ImGui::SetCursorPosX(
                std::max((available.x - ImGui::CalcTextSize(prompt).x) * 0.5F, 12.0F));
            ImGui::TextDisabled("%s", prompt);
        } else {
            float minimum_clump_size = std::numeric_limits<float>::max();
            float maximum_clump_size = std::numeric_limits<float>::lowest();
            find_clump_size_range(document->chunks(), minimum_clump_size, maximum_clump_size);
            const bool reveal_selected = selected != previous_selection;
            const bool tree_visible = show_scene_tree && !maximize_viewport;
            const bool inspector_visible =
                (show_inspector || show_viewport_tools) && !maximize_viewport;
            const int visible_panel_count =
                static_cast<int>(tree_visible) + static_cast<int>(inspector_visible);
            const float workspace_width = ImGui::GetContentRegionAvail().x;
            const float panel_width =
                visible_panel_count == 0
                    ? 0.0F
                    : std::clamp((workspace_width - 320.0F -
                                  ImGui::GetStyle().ItemSpacing.x * visible_panel_count) /
                                     static_cast<float>(visible_panel_count),
                                 180.0F, 390.0F);

            if (tree_visible) {
                ImGui::BeginChild("scene_tree", {panel_width, 0.0F}, ImGuiChildFlags_Borders);
                if ((workspace == Workspace::mission || workspace == Workspace::animation) &&
                    mission_scene) {
                    ImGui::Text("%zu actors | %zu nav points | %zu effects | %zu diagnostics",
                                mission_scene->actors().size(),
                                mission_scene->navigation_stats().points,
                                mission_scene->effects().size(),
                                mission_scene->diagnostics().size());
                    ImGui::SetNextItemWidth(-1.0F);
                    ImGui::InputTextWithHint("##mission_search", "Search name, class, ID...",
                                             mission_search.data(), mission_search.size());
                } else
                    ImGui::Text("%zu chunks | %zu instances | %zu diagnostics",
                                document->chunks().size(), document->scene_instances().size(),
                                document->diagnostics().size());
                ImGui::Separator();
                if ((workspace == Workspace::mission || workspace == Workspace::animation) &&
                    mission_scene) {
                    std::string needle = mission_search.data();
                    std::ranges::transform(needle, needle.begin(), [](const unsigned char value) {
                        return static_cast<char>(std::tolower(value));
                    });
                    const auto matches = [&](const std::string& text) {
                        if (needle.empty()) return true;
                        auto lowered = text;
                        std::ranges::transform(lowered, lowered.begin(),
                                               [](const unsigned char value) {
                                                   return static_cast<char>(std::tolower(value));
                                               });
                        return lowered.find(needle) != std::string::npos;
                    };
                    const auto selected_entry = geometry_preview.selected_mission_entry();
                    if (ImGui::TreeNodeEx("Actors", ImGuiTreeNodeFlags_DefaultOpen)) {
                        for (const auto& actor : mission_scene->actors()) {
                            std::string scripts = actor.script.value_or("");
                            for (const auto id : actor.script_ids) {
                                if (!scripts.empty()) scripts += ',';
                                scripts += std::to_string(id);
                            }
                            const auto text = actor.name.value_or("(unnamed)") + "  [class " +
                                              std::to_string(actor.class_id.value_or(-1)) +
                                              ", ID " + std::to_string(actor.id.value_or(-1)) +
                                              ", group " +
                                              std::to_string(actor.group.value_or(-1)) +
                                              ", scripts " + scripts + "]";
                            ImGui::PushID(static_cast<int>(actor.source.entry_index));
                            if (matches(text) &&
                                ImGui::Selectable(text.c_str(),
                                                  selected_entry == actor.source.entry_index))
                                geometry_preview.select_mission_entry(actor.source.entry_index);
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    if (workspace == Workspace::animation &&
                        ImGui::TreeNodeEx("Cutscene camera dummies",
                                          ImGuiTreeNodeFlags_DefaultOpen)) {
                        std::set<std::uint32_t> shown;
                        for (const auto& [path, timeline] : mission_cutscenes)
                            for (const auto& script : timeline.scripts())
                                for (const auto& action : script.actions) {
                                    if (action.kind != csf::CutsceneActionKind::camera ||
                                        !action.numeric_value)
                                        continue;
                                    const auto dummy = std::ranges::find_if(
                                        mission_scene->dummies(), [&](const auto& value) {
                                            return value.id ==
                                                   static_cast<std::int32_t>(*action.numeric_value);
                                        });
                                    if (dummy == mission_scene->dummies().end() ||
                                        !shown.insert(dummy->source.entry_index).second)
                                        continue;
                                    const auto text = dummy->name.value_or("(unnamed camera)") +
                                                      " [dummy " +
                                                      std::to_string(dummy->id.value_or(-1)) +
                                                      ", " + script.name + "]";
                                    ImGui::PushID(static_cast<int>(dummy->source.entry_index));
                                    if (ImGui::Selectable(text.c_str(),
                                                          selected_entry ==
                                                              dummy->source.entry_index))
                                        geometry_preview.select_mission_entry(
                                            dummy->source.entry_index);
                                    ImGui::PopID();
                                }
                        if (shown.empty())
                            ImGui::TextDisabled("No camera actions resolve to SCN dummies");
                        ImGui::TreePop();
                    }
                    if (workspace == Workspace::mission && ImGui::TreeNode("Navigation")) {
                        for (const auto& group : mission_scene->navigation()) {
                            const auto group_text = group.name.value_or("(unnamed)") + "  (" +
                                                    std::to_string(group.points.size()) +
                                                    " points)";
                            ImGui::PushID(static_cast<int>(group.source.entry_index));
                            if (matches(group_text) &&
                                ImGui::Selectable(group_text.c_str(),
                                                  selected_entry == group.source.entry_index))
                                geometry_preview.select_mission_entry(group.source.entry_index);
                            ImGui::PopID();
                            if (!needle.empty())
                                for (const auto& point : group.points) {
                                    const auto point_text =
                                        "  " + point.name.value_or("(unnamed)") + "  [" +
                                        std::to_string(point.group_id.value_or(-1)) + ":" +
                                        std::to_string(point.id.value_or(-1)) + "]";
                                    ImGui::PushID(static_cast<int>(point.source.entry_index));
                                    if (matches(point_text) &&
                                        ImGui::Selectable(point_text.c_str(),
                                                          selected_entry ==
                                                              point.source.entry_index))
                                        geometry_preview.select_mission_entry(
                                            point.source.entry_index);
                                    ImGui::PopID();
                                }
                        }
                        ImGui::TreePop();
                    }
                    if (workspace == Workspace::mission && ImGui::TreeNode("Spatial")) {
                        for (const auto& value : mission_scene->dummies()) {
                            const auto text = "Dummy: " + value.name.value_or("(unnamed)") + " [" +
                                              std::to_string(value.id.value_or(-1)) + "]";
                            ImGui::PushID(static_cast<int>(value.source.entry_index));
                            if (matches(text) &&
                                ImGui::Selectable(text.c_str(),
                                                  selected_entry == value.source.entry_index))
                                geometry_preview.select_mission_entry(value.source.entry_index);
                            ImGui::PopID();
                        }
                        for (const auto& value : mission_scene->areas()) {
                            const auto text = "Area: " + value.name.value_or("(unnamed)") + " [" +
                                              std::to_string(value.id.value_or(-1)) + "]";
                            ImGui::PushID(static_cast<int>(value.source.entry_index));
                            if (matches(text) &&
                                ImGui::Selectable(text.c_str(),
                                                  selected_entry == value.source.entry_index))
                                geometry_preview.select_mission_entry(value.source.entry_index);
                            ImGui::PopID();
                        }
                        for (const auto& value : mission_scene->lights()) {
                            const auto text = "Light: " + value.name.value_or("(unnamed)") + " [" +
                                              std::to_string(value.id.value_or(-1)) + "]";
                            ImGui::PushID(static_cast<int>(value.source.entry_index));
                            if (matches(text) &&
                                ImGui::Selectable(text.c_str(),
                                                  selected_entry == value.source.entry_index))
                                geometry_preview.select_mission_entry(value.source.entry_index);
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    if (workspace == Workspace::mission && !mission_scene->effects().empty() &&
                        ImGui::TreeNode("Effects")) {
                        for (const auto& value : mission_scene->effects()) {
                            const auto text = value.name.value_or("(unnamed)") + " [ID " +
                                              std::to_string(value.id.value_or(-1)) + ", class " +
                                              std::to_string(value.class_id.value_or(-1)) +
                                              ", dummy " +
                                              std::to_string(value.dummy_id.value_or(-1)) + "]";
                            ImGui::PushID(static_cast<int>(value.source.entry_index));
                            if (matches(text) &&
                                ImGui::Selectable(text.c_str(),
                                                  selected_entry == value.source.entry_index))
                                geometry_preview.select_mission_entry(value.source.entry_index);
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    if (workspace == Workspace::mission && !mission_scene->folders().empty() &&
                        ImGui::TreeNode("Folders")) {
                        for (const auto& folder : mission_scene->folders()) {
                            std::vector<std::uint32_t> entries;
                            for (const auto id : folder.element_ids) {
                                const auto dummy = std::ranges::find_if(
                                    mission_scene->dummies(),
                                    [&](const auto& value) { return value.id == id; });
                                if (dummy != mission_scene->dummies().end())
                                    entries.push_back(dummy->source.entry_index);
                                const auto light = std::ranges::find_if(
                                    mission_scene->lights(),
                                    [&](const auto& value) { return value.id == id; });
                                if (light != mission_scene->lights().end())
                                    entries.push_back(light->source.entry_index);
                            }
                            bool visible = std::ranges::all_of(entries, [&](const auto entry) {
                                return geometry_preview.mission_entry_visible(entry);
                            });
                            ImGui::PushID(static_cast<int>(folder.source.entry_index));
                            if (ImGui::Checkbox("##visible", &visible))
                                geometry_preview.set_mission_entries_visible(entries, visible);
                            ImGui::SameLine();
                            const auto label = (folder.path.empty() ? "(root)" : folder.path) +
                                               " (" +
                                               std::to_string(folder.element_ids.size()) + ")";
                            if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                                for (const auto id : folder.element_ids) {
                                    const auto dummy = std::ranges::find_if(
                                        mission_scene->dummies(),
                                        [&](const auto& value) { return value.id == id; });
                                    const auto light = std::ranges::find_if(
                                        mission_scene->lights(),
                                        [&](const auto& value) { return value.id == id; });
                                    const csf::CsfSourceId* source = nullptr;
                                    std::string text = "Missing element " + std::to_string(id);
                                    if (dummy != mission_scene->dummies().end()) {
                                        source = &dummy->source;
                                        text = "Dummy: " + dummy->name.value_or("(unnamed)");
                                    } else if (light != mission_scene->lights().end()) {
                                        source = &light->source;
                                        text = "Light: " + light->name.value_or("(unnamed)");
                                    }
                                    if (source && ImGui::Selectable(
                                                      text.c_str(), selected_entry ==
                                                                        source->entry_index))
                                        geometry_preview.select_mission_entry(source->entry_index);
                                    else if (!source)
                                        ImGui::TextDisabled("%s", text.c_str());
                                }
                                ImGui::TreePop();
                            }
                            ImGui::PopID();
                        }
                        ImGui::TreePop();
                    }
                    if (workspace == Workspace::mission && mission_animations &&
                        ImGui::TreeNode("Animation catalog")) {
                        ImGui::TextDisabled("%zu logical records",
                                            mission_animations->records().size());
                        for (const auto& animation : mission_animations->records())
                            if (matches(animation.logical_name)) {
                                ImGui::PushID(static_cast<int>(animation.source.entry_index));
                                if (ImGui::TreeNode(animation.logical_name.c_str())) {
                                    ImGui::TextDisabled("source entry %u | %zu variant%s",
                                                        animation.source.entry_index,
                                                        animation.variants.size(),
                                                        animation.variants.size() == 1 ? "" : "s");
                                    if (animation.loop)
                                        ImGui::Text("Loop: %s", *animation.loop ? "yes" : "no");
                                    if (animation.blend_in)
                                        ImGui::Text("Blend in: %.6g", *animation.blend_in);
                                    if (animation.velocity_scalar)
                                        ImGui::Text("Velocity: %.6g", *animation.velocity_scalar);
                                    if (animation.translation_scalar)
                                        ImGui::Text("Translation: %.6g",
                                                    *animation.translation_scalar);
                                    if (animation.rotation_scalar)
                                        ImGui::Text("Rotation: %.6g", *animation.rotation_scalar);
                                    for (const auto& variant : animation.variants) {
                                        ImGui::BulletText("%s", variant.reference.c_str());
                                        if (variant.resolution) {
                                            ImGui::Indent();
                                            ImGui::TextDisabled("%s",
                                                                csf::resolution_status_name(
                                                                    variant.resolution->status));
                                            ImGui::Unindent();
                                        }
                                    }
                                    for (const auto& sound : animation.sounds)
                                        ImGui::BulletText(
                                            "Sound %s%s", sound.logical_id.c_str(),
                                            sound.time
                                                ? (" @ " + std::to_string(*sound.time) + " s")
                                                      .c_str()
                                                : "");
                                    ImGui::TreePop();
                                }
                                ImGui::PopID();
                            }
                        ImGui::TreePop();
                    }
                    if (!mission_cutscenes.empty() && ImGui::TreeNode("Cutscenes / control flow")) {
                        for (const auto& [path, timeline] : mission_cutscenes)
                            if (ImGui::TreeNode(path_utf8(path.filename()).c_str())) {
                                for (const auto& script : timeline.scripts())
                                    if (ImGui::TreeNode(&script, "%s [%zu blocks]",
                                                        script.name.c_str(),
                                                        script.blocks.size())) {
                                        for (const auto& block : script.blocks) {
                                            ImGui::Text("Block %u%s%s", block.index,
                                                        block.conditional ? " | branch" : "",
                                                        block.runtime_wait ? " | runtime wait"
                                                                           : "");
                                            ImGui::Indent();
                                            for (auto index : block.action_indices) {
                                                const auto& action = script.actions[index];
                                                ImGui::BulletText(
                                                    "%s: %s",
                                                    csf::cutscene_action_kind_name(action.kind),
                                                    action.opcode.c_str());
                                                if (!action.reference.empty()) {
                                                    ImGui::SameLine();
                                                    ImGui::TextDisabled("%s",
                                                                        action.reference.c_str());
                                                }
                                                ImGui::Indent();
                                                ImGui::TextDisabled("source entry %u",
                                                                    action.source.entry_index);
                                                ImGui::Unindent();
                                                if (action.kind ==
                                                        csf::CutsceneActionKind::camera &&
                                                    action.numeric_value) {
                                                    const auto dummy = std::ranges::find_if(
                                                        mission_scene->dummies(),
                                                        [&](const auto& value) {
                                                            return value.id &&
                                                                   *value.id ==
                                                                       static_cast<std::int32_t>(
                                                                           *action.numeric_value);
                                                        });
                                                    if (dummy != mission_scene->dummies().end()) {
                                                        ImGui::SameLine();
                                                        ImGui::PushID(&action);
                                                        if (ImGui::SmallButton("Preview dummy"))
                                                            geometry_preview.select_mission_entry(
                                                                dummy->source.entry_index);
                                                        ImGui::PopID();
                                                    }
                                                }
                                                if (action.kind == csf::CutsceneActionKind::fov &&
                                                    action.numeric_value) {
                                                    ImGui::SameLine();
                                                    ImGui::Text("FOV %.4g", *action.numeric_value);
                                                }
                                                if (action.kind ==
                                                        csf::CutsceneActionKind::animation &&
                                                    mission_animations) {
                                                    auto reference = action.reference;
                                                    std::ranges::transform(
                                                        reference, reference.begin(),
                                                        [](unsigned char c) {
                                                            return static_cast<char>(
                                                                std::toupper(c));
                                                        });
                                                    const auto logical = std::ranges::find_if(
                                                        mission_animations->records(),
                                                        [&](const auto& record) {
                                                            auto name = record.logical_name;
                                                            std::ranges::transform(
                                                                name, name.begin(),
                                                                [](unsigned char c) {
                                                                    return static_cast<char>(
                                                                        std::toupper(c));
                                                                });
                                                            return !name.empty() &&
                                                                   reference.find(name) !=
                                                                       std::string::npos;
                                                        });
                                                    if (logical !=
                                                        mission_animations->records().end()) {
                                                        ImGui::Indent();
                                                        ImGui::Text("Catalog: %s [entry %u]",
                                                                    logical->logical_name.c_str(),
                                                                    logical->source.entry_index);
                                                        for (const auto& variant :
                                                             logical->variants)
                                                            ImGui::TextDisabled(
                                                                "%s", variant.reference.c_str());
                                                        ImGui::Unindent();
                                                    }
                                                }
                                            }
                                            ImGui::Unindent();
                                        }
                                        ImGui::TreePop();
                                    }
                                ImGui::TreePop();
                            }
                        ImGui::TreePop();
                    }
                } else {
                    draw_tree(document->chunks(), selected, display_names, minimum_clump_size,
                              maximum_clump_size, reveal_selected);
                    draw_instance_tree(document->scene_instances(), selected, reveal_selected);
                }
                ImGui::EndChild();
                ImGui::SameLine();
            }

            const float inspector_width = inspector_visible ? panel_width : 0.0F;
            ImGui::BeginChild("primary_workspace",
                              {inspector_width > 0.0F
                                   ? -(inspector_width + ImGui::GetStyle().ItemSpacing.x)
                                   : 0.0F,
                               0.0F},
                              ImGuiChildFlags_Borders);
            if (workspace == Workspace::scene || workspace == Workspace::mission ||
                workspace == Workspace::animation) {
                if (geometry_preview.draw_scene(
                        document->chunks(), document->bytes(), document->scene_instances(),
                        document->source_path(), selected,
                        main_is_collision ? document.get() : collision_document.get(),
                        main_is_collision, collision_status)) {
                    show_inspector = true;
                    show_viewport_tools = true;
                    maximize_viewport = false;
                }
            } else if (workspace == Workspace::geometry) {
                const auto* geometry =
                    selected_chunk ? find_preview_geometry(*selected_chunk, document->chunks())
                                   : nullptr;
                if (!geometry) geometry = find_first_chunk(document->chunks(), 0x0F);
                if (geometry)
                    geometry_preview.draw(*geometry, document->bytes(), document->source_path());
                else
                    ImGui::TextDisabled("This document has no previewable Geometry.");
            } else if (selected_chunk) {
                ImGui::Text("%s (0x%08X)", rws::chunk_name(selected_chunk->type).data(),
                            selected_chunk->type);
                ImGui::Text("Header: 0x%llX   Payload: 0x%llX",
                            static_cast<unsigned long long>(selected_chunk->offset),
                            static_cast<unsigned long long>(selected_chunk->payload_offset));
                ImGui::Text("Declared: %u   Available: %llu   Library ID: 0x%08X",
                            selected_chunk->declared_size,
                            static_cast<unsigned long long>(selected_chunk->available_size),
                            selected_chunk->library_id);
                ImGui::Text(
                    "Vendor: %s (0x%06X)   Object ID: 0x%02X",
                    rws::chunk_vendor_name(rws::chunk_vendor_id(selected_chunk->type)).data(),
                    rws::chunk_vendor_id(selected_chunk->type),
                    rws::chunk_object_id(selected_chunk->type));
                const auto* owner = find_owning_object(document->chunks(), selected_chunk->offset);
                draw_typed_details(*selected_chunk, *document, status, owner ? owner->type : 0);
                draw_hex(*document, selected_chunk->payload_offset, selected_chunk->available_size);
            } else if (selected_instance) {
                ImGui::Text("CSF Scene Instance @ 0x%llX",
                            static_cast<unsigned long long>(selected_instance->offset));
                ImGui::Text("Prototype: %u   Instance ID: %u", selected_instance->prototype_id,
                            selected_instance->instance_id);
                ImGui::Text("Name: %s", selected_instance->prototype_name.empty()
                                            ? "(unnamed)"
                                            : selected_instance->prototype_name.c_str());
                ImGui::Text("Declared: %u   Physical: %llu", selected_instance->declared_size,
                            static_cast<unsigned long long>(selected_instance->physical_size));
                ImGui::Text("Position: %.6g, %.6g, %.6g", selected_instance->position.x,
                            selected_instance->position.y, selected_instance->position.z);
                ImGui::Text("Flags: 0x%08X (%s)", selected_instance->flags,
                            rws::scene_instance_flag_names(selected_instance->flags).c_str());
                ImGui::Text("Visibility distance: max %.6g, min %.6g, fade %.6g",
                            selected_instance->maximum_visibility_distance,
                            selected_instance->minimum_visibility_distance,
                            selected_instance->visibility_fade_range);
                ImGui::Text("Matrix flags: 0x%08X", selected_instance->matrix_flags);
                ImGui::Text("Rotation: [%.5g %.5g %.5g]", selected_instance->rotation[0],
                            selected_instance->rotation[1], selected_instance->rotation[2]);
                ImGui::Text("          [%.5g %.5g %.5g]", selected_instance->rotation[3],
                            selected_instance->rotation[4], selected_instance->rotation[5]);
                ImGui::Text("          [%.5g %.5g %.5g]", selected_instance->rotation[6],
                            selected_instance->rotation[7], selected_instance->rotation[8]);
                draw_hex(*document, selected_instance->offset, selected_instance->physical_size);
            } else {
                ImGui::TextDisabled("Select a chunk or scene instance to inspect it.");
            }
            ImGui::EndChild();

            if (inspector_width > 0.0F) {
                ImGui::SameLine();
                ImGui::BeginChild("selection_inspector", {0.0F, 0.0F}, ImGuiChildFlags_Borders);
                if (workspace == Workspace::scene || workspace == Workspace::mission ||
                    workspace == Workspace::animation) {
                    if (ImGui::Button("Selection", {110.0F, 0.0F})) show_viewport_tools = false;
                    ImGui::SameLine();
                    if (ImGui::Button("Viewport tools", {130.0F, 0.0F})) show_viewport_tools = true;
                    ImGui::SameLine();
                    if (ImGui::Button("Close", {-1.0F, 0.0F})) {
                        show_inspector = false;
                        show_viewport_tools = false;
                    }
                    ImGui::Separator();
                }
                if (show_viewport_tools &&
                    (workspace == Workspace::scene || workspace == Workspace::mission ||
                     workspace == Workspace::animation)) {
                    geometry_preview.draw_scene_tools(collision_status);
                } else {
                    ImGui::SeparatorText("Selection");
                    const auto mission_entry =
                        mission_scene ? geometry_preview.selected_mission_entry() : std::nullopt;
                    std::string mission_kind, mission_label;
                    const auto* mission_source =
                        mission_entry && mission_scene
                            ? find_mission_source(*mission_scene, *mission_entry, mission_kind,
                                                  mission_label)
                            : nullptr;
                    if ((workspace == Workspace::mission || workspace == Workspace::animation) &&
                        mission_source) {
                        ImGui::TextWrapped("%s", mission_kind.c_str());
                        if (!mission_label.empty()) ImGui::TextWrapped("%s", mission_label.c_str());
                        ImGui::Text("Entry %u", mission_source->entry_index);
                        ImGui::Text("Offset 0x%llX",
                                    static_cast<unsigned long long>(mission_source->range.offset));
                        ImGui::TextWrapped("%s", path_utf8(mission_source->file).c_str());
                        ImGui::TextDisabled("Stable identity is source file + entry index");
                        draw_mission_typed_details(*mission_scene, mission_source->entry_index,
                                                   geometry_preview);
                        if (const auto association = std::ranges::find_if(
                                mission_actor_associations,
                                [&](const auto& value) {
                                    return value.actor.entry_index == mission_source->entry_index;
                                });
                            association != mission_actor_associations.end()) {
                            ImGui::SeparatorText("Actor model and Physics association");
                            ImGui::Text("Class %d | definitions %zu",
                                        association->class_id.value_or(-1),
                                        association->definitions.size());
                            if (association->definitions.size() == 1 &&
                                !association->definitions.front()->weapon_ids.empty()) {
                                std::string weapon_ids;
                                for (const auto id : association->definitions.front()->weapon_ids) {
                                    if (!weapon_ids.empty()) weapon_ids += ", ";
                                    weapon_ids += std::to_string(id);
                                }
                                ImGui::TextWrapped("Default weapon IDs: %s", weapon_ids.c_str());
                                ImGui::TextDisabled(
                                    "The first resolved Armas.bdd FILE2 model is previewed.");
                            }
                            const auto draw_associations =
                                [](const char* label,
                                   const std::vector<csf::AssociationEvidence>& values) {
                                    if (values.empty()) return;
                                    if (ImGui::TreeNode(label)) {
                                        for (const auto& value : values) {
                                            ImGui::BulletText(
                                                "%s", value.resolution.original_reference.c_str());
                                            ImGui::Indent();
                                            ImGui::TextDisabled("%s via %s",
                                                                csf::resolution_status_name(
                                                                    value.resolution.status),
                                                                value.rule.c_str());
                                            if (value.resolved_path)
                                                ImGui::TextWrapped(
                                                    "%s", value.resolved_path->string().c_str());
                                            ImGui::Unindent();
                                        }
                                        ImGui::TreePop();
                                    }
                                };
                            draw_associations("Visual model", association->visual_models);
                            draw_associations("LOD variants (manual selection)",
                                              association->lod_models);
                            draw_associations("CMO source collision",
                                              association->collision_models);
                            draw_associations("Compiled Physics", association->physics_models);
                            draw_associations("Ragdoll", association->ragdolls);
                            draw_associations("Animation metadata", association->animations);
                            if (workspace == Workspace::mission && mission_animations) {
                                if (ImGui::Button("Open actor in Animation tab", {-1, 0}))
                                    workspace = Workspace::animation;
                            }
                            if (workspace == Workspace::animation && mission_animations &&
                                mission_graph) {
                                ImGui::SeparatorText("Animation playback (selected actor only)");
                                const auto actor_record = std::ranges::find_if(
                                    mission_scene->actors(), [&](const auto& value) {
                                        return value.source.entry_index ==
                                               mission_source->entry_index;
                                    });
                                std::map<const csf::AnimationRecord*, std::vector<std::string>>
                                    traced;
                                if (actor_record != mission_scene->actors().end()) {
                                    for (const auto* use : mission_script_animations.for_actor(
                                             actor_record->script_ids, actor_record->id))
                                        if (const auto* record =
                                                mission_animations->find_id(use->animation_id))
                                            traced[record].push_back(
                                                "GSC script " + std::to_string(use->script_id) +
                                                " (" + use->script_name + ") / " + use->opcode +
                                                " at entry " +
                                                std::to_string(use->source.entry_index));
                                }
                                const auto asset_key = [](const std::string& value) {
                                    auto key = std::filesystem::path(value).stem().string();
                                    std::ranges::transform(key, key.begin(), [](unsigned char c) {
                                        return static_cast<char>(std::toupper(c));
                                    });
                                    return key;
                                };
                                for (const auto& evidence : association->animations)
                                    for (const auto& record : mission_animations->records())
                                        for (const auto& variant : record.variants)
                                            if (asset_key(evidence.resolution.original_reference) ==
                                                asset_key(variant.reference))
                                                traced[&record].push_back(
                                                    "Objetos.bdd " + evidence.field + " at entry " +
                                                    std::to_string(evidence.source.entry_index));
                                if (actor_record != mission_scene->actors().end() &&
                                    !actor_record->script_ids.empty()) {
                                    std::string ids;
                                    for (const auto id : actor_record->script_ids) {
                                        if (!ids.empty()) ids += ", ";
                                        ids += std::to_string(id);
                                    }
                                    ImGui::TextWrapped("Actor scripts: %s", ids.c_str());
                                }
                                const auto assign_animation = [&](const csf::AnimationRecord&
                                                                      animation) {
                                    try {
                                        const auto variant = std::ranges::find_if(
                                            animation.variants, [](const auto& value) {
                                                return value.resolution &&
                                                       value.resolution->candidate_indices.size() ==
                                                           1 &&
                                                       value.resolution->status !=
                                                           csf::ResolutionStatus::ambiguous;
                                            });
                                        if (variant == animation.variants.end())
                                            throw std::runtime_error(
                                                "Animation has no uniquely resolved ANM variant");
                                        const auto& resource =
                                            mission_graph->index().resources()
                                                [variant->resolution->candidate_indices.front()];
                                        const auto animation_document =
                                            rws::Document::load(resource.path);
                                        if (animation_document.chunks().empty() ||
                                            animation_document.chunks().front().type != 0x1B)
                                            throw std::runtime_error(
                                                "Resolved variant is not an ANM root");
                                        auto clip = std::make_shared<rws::AnimationClip>(
                                            rws::decode_animation(
                                                animation_document.chunks().front(),
                                                animation_document.bytes()));
                                        if (!clip->valid())
                                            throw std::runtime_error(
                                                "Clip is unsupported or failed validation");
                                        if (association->visual_models.size() != 1 ||
                                            !association->visual_models.front().resolved_path)
                                            throw std::runtime_error("Selected actor has no "
                                                                     "uniquely resolved RPC model");
                                        const auto model_document = rws::Document::load(
                                            *association->visual_models.front().resolved_path);
                                        const auto* frame_chunk =
                                            find_first_chunk(model_document.chunks(), 0x0E);
                                        if (!frame_chunk)
                                            throw std::runtime_error("Selected actor model has no "
                                                                     "Frame List/HAnim hierarchy");
                                        const auto frames = rws::decode_frame_list(
                                            *frame_chunk, model_document.bytes());
                                        const auto binding = rws::decode_hanim_binding(
                                            *frame_chunk, model_document.bytes());
                                        if (!frames || !binding)
                                            throw std::runtime_error(
                                                "Selected actor model hierarchy could not be "
                                                "decoded");
                                        const auto compatibility = rws::map_animation_tracks(
                                            *clip, *binding.value, frames.value->frames.size());
                                        if (!compatibility.compatible) {
                                            std::string message = "Clip is incompatible with the "
                                                                  "selected actor hierarchy";
                                            if (!compatibility.diagnostics.empty())
                                                message += ": " + compatibility.diagnostics.front();
                                            throw std::runtime_error(message);
                                        }
                                        if (!geometry_preview.set_mission_actor_animation(
                                                mission_source->entry_index, clip, 0,
                                                mission_animation_loop))
                                            throw std::runtime_error(
                                                "Selected actor has no loaded RPC model");
                                        mission_active_clip = std::move(clip);
                                        mission_animated_actor = mission_source->entry_index;
                                        mission_active_animation = animation.logical_name;
                                        mission_animation_time = 0;
                                        mission_animation_playing = false;
                                        status = "Assigned " + animation.logical_name +
                                                 " to selected actor (AI remains disabled)";
                                    } catch (const std::exception& error) {
                                        status =
                                            std::string("Animation unchanged: ") + error.what();
                                    }
                                };
                                if (mission_active_clip &&
                                    mission_animated_actor == mission_source->entry_index) {
                                    if (mission_animation_playing) {
                                        mission_animation_accumulator += ImGui::GetIO().DeltaTime;
                                        mission_animation_time +=
                                            ImGui::GetIO().DeltaTime * mission_animation_speed;
                                        if (mission_animation_loop &&
                                            mission_active_clip->duration > 0) {
                                            mission_animation_time =
                                                std::fmod(mission_animation_time,
                                                          mission_active_clip->duration);
                                            if (mission_animation_time < 0)
                                                mission_animation_time +=
                                                    mission_active_clip->duration;
                                        } else {
                                            if (mission_animation_time >=
                                                mission_active_clip->duration) {
                                                mission_animation_time =
                                                    mission_active_clip->duration;
                                                mission_animation_playing = false;
                                            } else if (mission_animation_time <= 0) {
                                                mission_animation_time = 0;
                                                mission_animation_playing = false;
                                            }
                                        }
                                        if (mission_animation_accumulator >=
                                            1.0F / static_cast<float>(mission_animation_fps)) {
                                            static_cast<void>(
                                                geometry_preview.set_mission_actor_animation(
                                                    *mission_animated_actor, mission_active_clip,
                                                    mission_animation_time,
                                                    mission_animation_loop));
                                            mission_animation_accumulator = 0;
                                        }
                                    }
                                    ImGui::TextWrapped("Clip: %s",
                                                       mission_active_animation.c_str());
                                    ImGui::Text("%.3f / %.3f s", mission_animation_time,
                                                mission_active_clip->duration);
                                    if (ImGui::Button(mission_animation_playing
                                                          ? "Pause##actor_anim"
                                                          : "Play##actor_anim"))
                                        mission_animation_playing = !mission_animation_playing;
                                    ImGui::SameLine();
                                    if (ImGui::Button("Reset##actor_anim")) {
                                        mission_animation_time = 0;
                                        mission_animation_playing = false;
                                        static_cast<void>(
                                            geometry_preview.set_mission_actor_animation(
                                                *mission_animated_actor, mission_active_clip, 0,
                                                mission_animation_loop));
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button("Step -##actor_anim")) {
                                        mission_animation_time = std::max(
                                            0.0F, mission_animation_time -
                                                      1.0F /
                                                          static_cast<float>(mission_animation_fps));
                                        mission_animation_playing = false;
                                        static_cast<void>(
                                            geometry_preview.set_mission_actor_animation(
                                                *mission_animated_actor, mission_active_clip,
                                                mission_animation_time, mission_animation_loop));
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::Button("Step +##actor_anim")) {
                                        mission_animation_time =
                                            std::min(mission_active_clip->duration,
                                                     mission_animation_time +
                                                         1.0F / static_cast<float>(
                                                                    mission_animation_fps));
                                        mission_animation_playing = false;
                                        static_cast<void>(
                                            geometry_preview.set_mission_actor_animation(
                                                *mission_animated_actor, mission_active_clip,
                                                mission_animation_time, mission_animation_loop));
                                    }
                                    const auto seek_key = [&](const bool next) {
                                        std::optional<float> target;
                                        for (const auto& key : mission_active_clip->keyframes) {
                                            if (next && key.time > mission_animation_time + 1.0e-5F &&
                                                (!target || key.time < *target))
                                                target = key.time;
                                            if (!next &&
                                                key.time < mission_animation_time - 1.0e-5F &&
                                                (!target || key.time > *target))
                                                target = key.time;
                                        }
                                        if (!target) target = next ? mission_active_clip->duration : 0;
                                        mission_animation_time = *target;
                                        mission_animation_playing = false;
                                        static_cast<void>(
                                            geometry_preview.set_mission_actor_animation(
                                                *mission_animated_actor, mission_active_clip,
                                                mission_animation_time, mission_animation_loop));
                                    };
                                    if (ImGui::Button("Previous key##actor_anim")) seek_key(false);
                                    ImGui::SameLine();
                                    if (ImGui::Button("Next key##actor_anim")) seek_key(true);
                                    if (ImGui::Checkbox("Loop##actor_anim",
                                                        &mission_animation_loop))
                                        static_cast<void>(
                                            geometry_preview.set_mission_actor_animation(
                                                *mission_animated_actor, mission_active_clip,
                                                mission_animation_time, mission_animation_loop));
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(100);
                                    ImGui::SliderFloat("Speed##actor_anim",
                                                       &mission_animation_speed, -4, 4, "%.2fx");
                                    constexpr const char* preview_rates[] = {"15 FPS", "24 FPS",
                                                                            "25 FPS", "30 FPS",
                                                                            "60 FPS"};
                                    constexpr int preview_rate_values[] = {15, 24, 25, 30, 60};
                                    int preview_rate_index{};
                                    for (int i = 0; i < static_cast<int>(std::size(preview_rate_values));
                                         ++i)
                                        if (preview_rate_values[i] == mission_animation_fps)
                                            preview_rate_index = i;
                                    ImGui::SameLine();
                                    ImGui::SetNextItemWidth(90);
                                    if (ImGui::Combo("##actor_anim_fps", &preview_rate_index,
                                                     preview_rates,
                                                     static_cast<int>(std::size(preview_rates))))
                                        mission_animation_fps =
                                            preview_rate_values[preview_rate_index];
                                    if (ImGui::SliderFloat(
                                            "Time##actor_anim", &mission_animation_time, 0,
                                            std::max(.001F, mission_active_clip->duration),
                                            "%.3f s")) {
                                        mission_animation_playing = false;
                                        static_cast<void>(
                                            geometry_preview.set_mission_actor_animation(
                                                *mission_animated_actor, mission_active_clip,
                                                mission_animation_time, mission_animation_loop));
                                    }
                                }
                                ImGui::SeparatorText("Explicit mission animation references");
                                if (traced.empty())
                                    ImGui::TextWrapped(
                                        "None. This actor has no direct Objetos.bdd animation, "
                                        "attached SCN script animation, or GSC action explicitly "
                                        "targeting its ID. Normal AI locomotion/combat animation "
                                        "selection is runtime behavior and is not an SCN "
                                        "assignment.");
                                for (const auto& [animation, evidence] : traced) {
                                    ImGui::PushID(static_cast<int>(animation->source.entry_index));
                                    const bool resolvable =
                                        std::ranges::any_of(animation->variants, [](const auto& v) {
                                            return v.resolution &&
                                                   v.resolution->candidate_indices.size() == 1 &&
                                                   v.resolution->status !=
                                                       csf::ResolutionStatus::ambiguous;
                                        });
                                    ImGui::BeginDisabled(!resolvable);
                                    if (ImGui::Selectable(animation->logical_name.c_str(),
                                                          mission_active_animation ==
                                                              animation->logical_name))
                                        assign_animation(*animation);
                                    ImGui::EndDisabled();
                                    for (const auto& line : evidence) {
                                        ImGui::Indent();
                                        ImGui::TextDisabled("%s", line.c_str());
                                        ImGui::Unindent();
                                    }
                                    ImGui::PopID();
                                }
                                if (traced.empty())
                                    ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
                                const auto playable_label =
                                    "Playable catalog clips (validated when selected) (" +
                                    std::to_string(mission_animations->records().size() -
                                                   traced.size()) +
                                    ")";
                                if (ImGui::TreeNode(playable_label.c_str())) {
                                    const auto compatible_animations =
                                        mission_animations->compatible(
                                            association->visual_models.size() == 1 &&
                                                    association->visual_models.front().resolved_path
                                                ? path_utf8(association->visual_models.front()
                                                                .resolved_path->filename())
                                                : std::string{});
                                    for (const auto* animation : compatible_animations) {
                                        if (traced.contains(animation)) continue;
                                        const bool resolvable = std::ranges::any_of(
                                            animation->variants, [](const auto& v) {
                                                return v.resolution &&
                                                       v.resolution->candidate_indices.size() ==
                                                           1 &&
                                                       v.resolution->status !=
                                                           csf::ResolutionStatus::ambiguous;
                                            });
                                        ImGui::PushID(
                                            static_cast<int>(animation->source.entry_index));
                                        ImGui::BeginDisabled(!resolvable);
                                        if (ImGui::Selectable(animation->logical_name.c_str(),
                                                              mission_active_animation ==
                                                                  animation->logical_name))
                                            assign_animation(*animation);
                                        ImGui::EndDisabled();
                                        ImGui::PopID();
                                    }
                                    ImGui::TreePop();
                                }
                                ImGui::TextDisabled(
                                    "Evidence list: direct object metadata and PLAY_ANMBDD actions "
                                    "in scripts attached to this actor.");
                                ImGui::TextDisabled("Catalog clips are candidates, not proof of an "
                                                    "in-game assignment; exact HAnim/frame "
                                                    "compatibility is validated on selection.");
                                ImGui::TextDisabled("Only this actor is evaluated. No AI or "
                                                    "runtime-state inference.");
                            }
                            for (const auto& diagnostic : association->diagnostics)
                                ImGui::TextColored(ImVec4(1.0F, 0.72F, 0.25F, 1.0F), "%s",
                                                   diagnostic.c_str());
                        }
                        if (workspace == Workspace::animation && mission_kind == "Dummy") {
                            const auto dummy = std::ranges::find_if(
                                mission_scene->dummies(), [&](const auto& value) {
                                    return value.source.entry_index == mission_source->entry_index;
                                });
                            if (dummy != mission_scene->dummies().end()) {
                                ImGui::SeparatorText("Cutscene camera references");
                                ImGui::Text("Dummy ID %d | heading %.4g rad | pitch %.4g rad",
                                            dummy->id.value_or(-1), dummy->heading.value_or(0),
                                            dummy->pitch.value_or(0));
                                std::size_t uses{};
                                for (const auto& [path, timeline] : mission_cutscenes)
                                    for (const auto& script : timeline.scripts())
                                        for (std::size_t action_index = 0;
                                             action_index < script.actions.size(); ++action_index) {
                                            const auto& action = script.actions[action_index];
                                            if (action.kind != csf::CutsceneActionKind::camera ||
                                                !action.numeric_value ||
                                                static_cast<std::int32_t>(*action.numeric_value) !=
                                                    dummy->id)
                                                continue;
                                            ++uses;
                                            ImGui::BulletText("%s / %s",
                                                              path_utf8(path.filename()).c_str(),
                                                              script.name.c_str());
                                            ImGui::Indent();
                                            ImGui::TextDisabled("action %zu, %s, source entry %u",
                                                                action_index, action.opcode.c_str(),
                                                                action.source.entry_index);
                                            ImGui::Unindent();
                                        }
                                if (uses == 0)
                                    ImGui::TextDisabled("This dummy is not referenced by a "
                                                        "recognized camera action.");
                                else
                                    ImGui::TextDisabled("The magenta viewport marker and direction "
                                                        "wedge identify this cutscene camera.");
                            }
                        }
                        if (mission_document)
                            if (const auto* raw = find_csf_node(mission_document->roots(),
                                                                mission_source->entry_index)) {
                                ImGui::SeparatorText("Raw CSFFBS subtree");
                                draw_csf_subtree(*mission_document, *raw);
                            }
                        if (mission_symbols && mission_scene) {
                            const auto actor = std::ranges::find_if(
                                mission_scene->actors(), [&](const auto& value) {
                                    return value.source.entry_index == mission_source->entry_index;
                                });
                            std::vector<const csf::SymbolSite*> sites;
                            if (actor != mission_scene->actors().end() && actor->class_id)
                                sites = mission_symbols->exact("class:" +
                                                               std::to_string(*actor->class_id));
                            const auto effect = std::ranges::find_if(
                                mission_scene->effects(), [&](const auto& value) {
                                    return value.source.entry_index == mission_source->entry_index;
                                });
                            if (effect != mission_scene->effects().end() && effect->class_id) {
                                const auto typed = mission_symbols->exact(
                                    "class:" + std::to_string(*effect->class_id));
                                sites.insert(sites.end(), typed.begin(), typed.end());
                            }
                            const auto dummy = std::ranges::find_if(
                                mission_scene->dummies(), [&](const auto& value) {
                                    return value.source.entry_index == mission_source->entry_index;
                                });
                            if (dummy != mission_scene->dummies().end() && dummy->id) {
                                const auto uses = mission_symbols->exact(
                                    "dummy:" + std::to_string(*dummy->id));
                                sites.insert(sites.end(), uses.begin(), uses.end());
                            }
                            if (!mission_label.empty()) {
                                const auto named = mission_symbols->exact(mission_label);
                                sites.insert(sites.end(), named.begin(), named.end());
                            }
                            if (!sites.empty()) {
                                ImGui::SeparatorText("Definitions and exact uses");
                                for (const auto* site : sites) {
                                    if (site->source.file == mission_source->file &&
                                        site->source.entry_index == mission_source->entry_index)
                                        continue;
                                    ImGui::TextWrapped("%s / %s", csf::symbol_role_name(site->role),
                                                       csf::symbol_category_name(site->category));
                                    ImGui::TextDisabled(
                                        "%s : entry %u",
                                        path_utf8(site->source.file.filename()).c_str(),
                                        site->source.entry_index);
                                }
                            }
                        }
                    } else if (selected_chunk) {
                        ImGui::TextWrapped("%s", rws::chunk_name(selected_chunk->type).data());
                        ImGui::TextDisabled("Type 0x%08X", selected_chunk->type);
                        ImGui::Text("Offset  0x%llX",
                                    static_cast<unsigned long long>(selected_chunk->offset));
                        ImGui::Text("Size    %u bytes", selected_chunk->declared_size);
                        if (const auto name = display_names.find(selected_chunk->offset);
                            name != display_names.end())
                            ImGui::TextWrapped("Name    %s", name->second.c_str());
                    } else if (selected_instance) {
                        ImGui::TextWrapped("%s", selected_instance->prototype_name.empty()
                                                     ? "Unnamed scene instance"
                                                     : selected_instance->prototype_name.c_str());
                        ImGui::Text("Prototype %u", selected_instance->prototype_id);
                        ImGui::Text("Instance  %u", selected_instance->instance_id);
                        ImGui::Text("Position  %.4g, %.4g, %.4g", selected_instance->position.x,
                                    selected_instance->position.y, selected_instance->position.z);
                    } else {
                        ImGui::TextDisabled("Nothing selected");
                    }
                    ImGui::Spacing();
                    if (ImGui::Button("Open full inspector", {-1.0F, 0.0F}))
                        workspace = Workspace::inspector;
                    ImGui::SeparatorText("Document");
                    ImGui::TextWrapped("%s",
                                       path_utf8(document->source_path().filename()).c_str());
                    ImGui::Text("%zu bytes", document->bytes().size());
                    ImGui::Text("%zu diagnostics", document->diagnostics().size());
                    ImGui::TextDisabled("Collision: %s", collision_status.c_str());
                }
                ImGui::EndChild();
            }
            previous_selection = selected;
        }
        ImGui::EndChild();

        ImGui::Separator();
        if (document)
            ImGui::TextDisabled("%s%s  |  %s  |  %zu chunks  |  %zu scene instances",
                                path_utf8(document->source_path().filename()).c_str(),
                                document->dirty() ? " *" : "", status.c_str(),
                                document->chunks().size(), document->scene_instances().size());
        else
            ImGui::TextDisabled("%s", status.c_str());
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
