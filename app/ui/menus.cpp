#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "navigation.hpp"
#include "ui/fonts.hpp"
#include "ui/layout.hpp"
#include "ui/theme.hpp"

#include <imgui.h>

#include <algorithm>
#include <set>
#include <string>

namespace rwsman::ui {
namespace {

void draw_command_item(AppState& state, const Command& command) {
    if (command.separator_before) ImGui::Separator();
    const bool enabled = !command.enabled || command.enabled();
    const bool checked = command.checked && command.checked();
    if (ImGui::MenuItem(command.label.c_str(), command.shortcut.c_str(), checked, enabled))
        state.commands.run(command.id);
}

void draw_recent_files(AppState& state) {
    if (!ImGui::BeginMenu("Open recent", !state.settings.recent_files.empty())) return;
    // Copy: opening a file reorders the settings list while we iterate.
    const auto recent = state.settings.recent_files;
    for (const auto& entry : recent) {
        std::error_code error;
        const auto label = std::string(std::filesystem::is_directory(entry.path, error) ? "[project] "
                                       : entry.mission                                  ? "[mission] "
                                                                                        : "[file] ") +
                           path_utf8(entry.path.filename());
        if (ImGui::MenuItem(label.c_str())) open_path(state, entry.path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path_utf8(entry.path).c_str());
    }
    ImGui::EndMenu();
}

void draw_recent_pairings(AppState& state) {
    if (!ImGui::BeginMenu("Recent pairings", !state.settings.recent_pairings.empty())) return;
    const auto pairings = state.settings.recent_pairings;
    for (const auto& pair : pairings) {
        const auto label = path_utf8(pair.main.filename()) + " + " + path_utf8(pair.collision.filename());
        if (ImGui::MenuItem(label.c_str())) open_pairing(state, pair);
    }
    ImGui::EndMenu();
}

void draw_category(AppState& state, const std::string& category) {
    std::set<std::string> emitted_submenus;
    for (const auto& command : state.commands.commands()) {
        if (command.category != category || !command.in_menu) continue;
        if (!command.submenu.empty()) {
            if (!emitted_submenus.insert(command.submenu).second) continue;
            if (command.separator_before) ImGui::Separator();
            if (ImGui::BeginMenu(command.submenu.c_str())) {
                for (const auto& member : state.commands.commands())
                    if (member.category == category && member.submenu == command.submenu &&
                        member.in_menu)
                        draw_command_item(state, member);
                ImGui::EndMenu();
            }
            continue;
        }
        draw_command_item(state, command);
        if (command.id == "file.open_rws") draw_recent_files(state);
        if (command.id == "file.clear_companion") draw_recent_pairings(state);
    }
}

} // namespace

void draw_command_menu_item(AppState& state, const char* command_id) {
    if (const auto* command = state.commands.find(command_id)) draw_command_item(state, *command);
}

void draw_selection_context_menu(AppState& state) {
    for (const char* id : {"view.frame_selection", "view.isolate_selection"}) draw_command_menu_item(state, id);
    ImGui::Separator();
    for (const char* id : {"edit.copy_id", "edit.copy_identity", "edit.copy_path"}) draw_command_menu_item(state, id);
    ImGui::Separator();
    for (const char* id : {"nav.show_in_hex", "mission.references", "export.clump_gltf"}) draw_command_menu_item(state, id);
}

namespace {

// One tab of the mode switch: text, an accent underline when active. Returns
// whether it was clicked (and is available).
bool mode_tab(const char* label, const bool active, const bool available, const char* tooltip, const float pad) {
    const ImVec2 size{ImGui::CalcTextSize(label).x + pad * 2.0F, ImGui::GetFrameHeight()};
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool clicked = ImGui::InvisibleButton("##tab", size) && available;
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered();
    auto* draw_list = ImGui::GetWindowDrawList();
    const Token token = active ? Token::accent : (available ? (hovered ? Token::text : Token::text_dim) : Token::line);
    draw_list->AddText({position.x + pad, position.y + (size.y - ImGui::GetTextLineHeight()) * 0.5F}, color_u32(token),
                       label);
    if (active)
        draw_list->AddLine({position.x + pad * 0.5F, position.y + size.y - 1.0F},
                           {position.x + size.x - pad * 0.5F, position.y + size.y - 1.0F}, color_u32(Token::accent),
                           2.0F);
    if (hovered && tooltip && *tooltip) ImGui::SetTooltip("%s", tooltip);
    ImGui::SameLine(0.0F, 0.0F);
    return clicked;
}

} // namespace

void draw_workspace_tabs(AppState& state) {
    // Mission, Script and Inspect on the right; in Inspect, its four
    // workspaces as a smaller switch before them.
    constexpr Workspace inspect_order[] = {Workspace::scene, Workspace::geometry, Workspace::animation,
                                           Workspace::inspector};
    constexpr Mode modes[] = {Mode::mission, Mode::script, Mode::inspect};
    const float pad = 9.0F * ui_scale(), small_pad = 6.0F * ui_scale();
    // Without a document there is no mode to show as active (Home).
    const bool inspecting = state.document && mode_of(state.workspace) == Mode::inspect;
    float total = 0.0F;
    ImGui::PushFont(font(Font::sans_bold));
    for (const auto mode : modes) total += ImGui::CalcTextSize(mode_label(mode)).x + pad * 2.0F;
    ImGui::PopFont();
    if (inspecting) {
        ImGui::PushFont(font(Font::caption));
        for (const auto workspace : inspect_order)
            total += ImGui::CalcTextSize(workspace_tab_label(workspace)).x + small_pad * 2.0F;
        ImGui::PopFont();
        total += 16.0F * ui_scale();
    }
    const float start = ImGui::GetWindowWidth() - total - ImGui::GetStyle().WindowPadding.x;
    if (start > ImGui::GetCursorPosX() + 24.0F) ImGui::SetCursorPosX(start);
    const auto shortcut = [&](const char* id) {
        const auto* command = state.commands.find(id);
        return command ? command->label + "  " + command->shortcut : std::string{};
    };
    if (inspecting) {
        ImGui::PushFont(font(Font::caption));
        for (const auto workspace : inspect_order) {
            const std::string id = std::string("view.workspace.") + workspace_key(workspace);
            if (mode_tab(workspace_tab_label(workspace), state.workspace == workspace,
                         workspace_available(state, workspace), shortcut(id.c_str()).c_str(), small_pad))
                state.commands.run(id);
        }
        ImGui::PopFont();
        ImGui::Dummy({16.0F * ui_scale(), 1.0F});
        ImGui::SameLine(0.0F, 0.0F);
    }
    ImGui::PushFont(font(Font::sans_bold));
    for (const auto mode : modes) {
        const char* id = mode == Mode::mission  ? "view.workspace.mission"
                         : mode == Mode::script ? "view.workspace.script"
                                                : "view.mode.inspect";
        const auto* command = state.commands.find(id);
        const bool available = command && (!command->enabled || command->enabled());
        if (mode_tab(mode_label(mode), state.document && mode_of(state.workspace) == mode,
                     available, shortcut(id).c_str(), pad))
            state.commands.run(id);
    }
    ImGui::PopFont();
}

void draw_menus(AppState& state) {
    if (!ImGui::BeginMainMenuBar()) return;
    for (const char* category : {"File", "Edit", "View", "Place", "Mission", "Build", "Inspect", "Help"}) {
        if (!ImGui::BeginMenu(category)) continue;
        draw_category(state, category);
        ImGui::EndMenu();
    }
    draw_workspace_tabs(state);
    ImGui::EndMainMenuBar();
}

} // namespace rwsman::ui
