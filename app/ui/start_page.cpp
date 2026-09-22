#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "commands.hpp"
#include "file_dialogs.hpp"
#include "rwsman/fuzzy.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <string>

namespace rwsman::ui {
namespace {

void draw_recent(AppState& state) {
    section("Recent");
    if (state.settings.recent_files.empty()) {
        dim_text("Nothing opened yet. Open a mission or drop a file on the window.");
        return;
    }
    const auto recent = state.settings.recent_files; // Opening reorders the list.
    for (const auto& entry : recent) {
        ImGui::PushID(path_utf8(entry.path).c_str());
        if (icon_row("recent", entry.mission ? icons::LC_GLOBE : icons::LC_FILE, Token::text_dim,
                     path_utf8(entry.path.filename()).c_str(), false))
            open_path(state, entry.path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nRight-click to remove from this list", path_utf8(entry.path).c_str());
        if (ImGui::BeginPopupContextItem("##recent_menu")) {
            if (ImGui::MenuItem("Remove from recent")) {
                std::erase_if(state.settings.recent_files, [&](const auto& value) { return value.path == entry.path; });
                state.settings_dirty = true;
            }
            if (ImGui::MenuItem("Copy path")) copy_to_clipboard(state, path_utf8(entry.path), "path");
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (!state.settings.recent_pairings.empty()) {
        section("Recent collision pairings");
        const auto pairings = state.settings.recent_pairings;
        for (const auto& pair : pairings) {
            const auto label = path_utf8(pair.main.filename()) + " + " + path_utf8(pair.collision.filename());
            if (icon_row(label.c_str(), icons::LC_LINK, Token::text_dim, label.c_str(), false)) {
                load_document(state, pair.main);
                if (state.document) pair_collision(state, pair.collision, false);
            }
        }
    }
}

} // namespace

// Mission list under the resource root. Shared by the start page and the
// Missions dock tab.
void draw_missions(AppState& state) {
    auto& filter = state.ui.mission_filter;
    if (state.settings.resource_root.empty()) {
        dim_text("Set a resource root to list the missions under it without a file dialog. "
                 "It can be the unpacked game folder or a single package that contains Maps/.");
        if (ImGui::Button("Choose resource root...")) request_file_dialog(state, DialogKind::resource_root);
        ImGui::SameLine();
        if (ImGui::Button("Preferences...")) state.ui.show_preferences = true;
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::PushFont(font(Font::mono));
    ImGui::TextUnformatted(path_utf8(state.settings.resource_root).c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (icon_button("rescan", icons::LC_REFRESH_CW, "Rescan the resource root")) rescan_resource_root(state);
    search_input("##mission_filter", "package, map, mission...", filter.data(), filter.size());
    const std::string query = filter.data();
    ImGui::BeginChild("##mission_list", {0.0F, 0.0F}, ImGuiChildFlags_None);
    std::string previous_package;
    bool package_open = false;
    std::size_t shown = 0;
    for (const auto& mission : state.discovered) {
        const auto label = mission.package + " / " + mission.map + " / " + mission.name;
        std::vector<std::uint32_t> positions;
        if (!query.empty()) {
            const auto match = fuzzy_match(query, label);
            if (!match) continue;
            positions = match->positions;
        }
        ++shown;
        if (mission.package != previous_package) {
            if (package_open) ImGui::TreePop();
            previous_package = mission.package;
            ImGui::PushFont(font(Font::sans_bold));
            if (!query.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
            package_open = ImGui::TreeNodeEx(mission.package.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopFont();
        }
        if (!package_open) continue;
        ImGui::PushID(path_utf8(mission.path).c_str());
        const std::string row_label = mission.map + " / " + mission.name;
        if (icon_row("mission", icons::LC_GLOBE, Token::text_dim, row_label.c_str(), false)) open_path(state, mission.path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path_utf8(mission.path).c_str());
        ImGui::PopID();
    }
    if (package_open) ImGui::TreePop();
    if (shown == 0) dim_text(state.discovered.empty() ? "No missions found under the resource root." : "No mission matches.");
    ImGui::EndChild();
}

void draw_start_page(AppState& state) {
    const float scale = ui_scale();
    const float content_width = std::min(ImGui::GetContentRegionAvail().x - 24.0F, 1100.0F * scale);
    ImGui::SetCursorPosX(std::max((ImGui::GetWindowWidth() - content_width) * 0.5F, 8.0F));
    ImGui::BeginGroup();
    ImGui::Dummy({0.0F, 24.0F * scale});
    ImGui::PushFont(font(Font::sans_bold));
    ImGui::SetWindowFontScale(1.6F);
    ImGui::TextUnformatted("CSF RWS Tools");
    ImGui::SetWindowFontScale(1.0F);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::accent));
    ImGui::TextUnformatted("// rws-man: chunk browser, mission explorer, 3D preview");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0F, 10.0F * scale});
    if (ImGui::Button((std::string(icons::LC_GLOBE) + "  Open mission...").c_str())) state.commands.run("file.open_mission");
    ImGui::SameLine();
    if (ImGui::Button((std::string(icons::LC_FOLDER_OPEN) + "  Open RWS / RPC...").c_str())) state.commands.run("file.open_rws");
    ImGui::SameLine();
    if (ImGui::Button((std::string(icons::LC_COMMAND) + "  Command palette").c_str())) state.commands.run("edit.command_palette");
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::TextUnformatted("Or drop an .scn, .rpc, .rws, or .anm file anywhere on this window.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0F, 8.0F * scale});

    if (ImGui::BeginTable("##start_columns", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings,
                          {content_width, 0.0F})) {
        ImGui::TableSetupColumn("recent", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("missions", ImGuiTableColumnFlags_WidthStretch, 1.25F);
        ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch, 0.9F);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_recent(state);
        ImGui::TableNextColumn();
        section("Missions");
        ImGui::BeginChild("##start_missions", {0.0F, 360.0F * scale}, ImGuiChildFlags_None);
        draw_missions(state);
        ImGui::EndChild();
        ImGui::TableNextColumn();
        section("Keys");
        // Generated from the command registry, so it cannot drift from the bindings.
        if (ImGui::BeginTable("##start_keys", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            for (const char* id : {"edit.go_to", "edit.command_palette", "file.open_mission", "file.open_rws",
                                   "edit.back", "edit.forward", "view.toggle_explorer", "view.toggle_inspector",
                                   "view.toggle_bottom_dock", "view.maximize_viewport", "edit.copy_identity",
                                   "view.screenshot", "help.shortcuts"}) {
                const auto* command = state.commands.find(id);
                if (!command) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(command->label.c_str());
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
                ImGui::PushFont(font(Font::mono));
                ImGui::TextUnformatted(command->shortcut.c_str());
                ImGui::PopFont();
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }
        ImGui::EndTable();
    }
    ImGui::Dummy({0.0F, 12.0F * scale});
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::PushFont(font(Font::mono));
#ifdef NDEBUG
    constexpr const char* build_type = "Release";
#else
    constexpr const char* build_type = "Debug";
#endif
#ifndef RWSMAN_VERSION
#define RWSMAN_VERSION "dev"
#endif
    ImGui::Text("rws-man %s | %s | Dear ImGui %s | built %s", RWSMAN_VERSION, build_type, IMGUI_VERSION, __DATE__);
    if (!state.config_dir.empty()) ImGui::Text("settings: %s", path_utf8(state.config_dir).c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

} // namespace rwsman::ui
