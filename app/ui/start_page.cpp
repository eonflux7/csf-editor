#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "commands.hpp"
#include "file_dialogs.hpp"
#include "csf/project_pipeline.hpp"
#include "rwsman/fuzzy.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace rwsman::ui {
namespace {

// What Home shows of an authoring project: its name, slot, latest build and
// the last playtest; read again when project.csfproj changes.
struct ProjectSummary {
    std::filesystem::file_time_type time;
    std::string name, detail;
};

const ProjectSummary& project_summary(const std::filesystem::path& folder) {
    static std::map<std::filesystem::path, ProjectSummary> cache;
    std::error_code error;
    const auto time = std::filesystem::last_write_time(folder / "project.csfproj", error);
    auto& summary = cache[folder];
    if (summary.time == time && !summary.name.empty()) return summary;
    summary = {time, path_utf8(folder.filename()), {}};
    try {
        const auto project = csf::AuthoringProject::load(folder);
        if (!project.name.empty()) summary.name = project.name;
        summary.detail = project.slot.mission + " slot";
        if (const auto builds = csf::archive_builds(project); !builds.empty()) summary.detail += "  ·  built " + builds.front();
        if (!project.playtests.empty())
            summary.detail += project.playtests.back().worked ? "  ·  playtest worked" : "  ·  playtest failed";
    } catch (const std::exception&) {
        summary.detail = "project.csfproj does not read";
    }
    return summary;
}

void recent_menu(AppState& state, const RecentFile& entry) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nRight-click to remove from this list", path_utf8(entry.path).c_str());
    if (ImGui::BeginPopupContextItem("##recent_menu")) {
        if (ImGui::MenuItem("Remove from recent")) {
            std::erase_if(state.settings.recent_files, [&](const auto& value) { return value.path == entry.path; });
            state.settings_dirty = true;
        }
        if (ImGui::MenuItem("Copy path")) copy_to_clipboard(state, path_utf8(entry.path), "path");
        ImGui::EndPopup();
    }
}

void draw_recent_projects(AppState& state) {
    section("Projects");
    const auto recent = state.settings.recent_files;  // Opening reorders the list.
    std::size_t shown = 0;
    std::error_code error;
    for (const auto& entry : recent) {
        if (!std::filesystem::is_directory(entry.path, error)) continue;
        ++shown;
        ImGui::PushID(path_utf8(entry.path).c_str());
        const bool authoring = std::filesystem::is_regular_file(entry.path / "project.csfproj", error);
        const auto* summary = authoring ? &project_summary(entry.path) : nullptr;
        const auto name = summary ? summary->name : path_utf8(entry.path.filename());
        if (icon_row("project", authoring ? icons::LC_MOUNTAIN : icons::LC_FOLDER, Token::accent, name.c_str(), false))
            open_path(state, entry.path);
        recent_menu(state, entry);
        ImGui::PushFont(font(Font::caption));
        ImGui::Indent(22.0F * ui_scale());
        dim_text("%s", summary ? summary->detail.c_str() : state.ui.shown(path_utf8(entry.path.parent_path())).c_str());
        ImGui::Unindent(22.0F * ui_scale());
        ImGui::PopFont();
        ImGui::PopID();
    }
    if (shown == 0)
        dim_text("No projects yet. New project... makes one in a shipped mission's slot; opening a shipped "
                 "mission and saving it makes a project too.");
}

void draw_recent_files(AppState& state) {
    const auto recent = state.settings.recent_files;
    std::error_code error;
    bool any = false;
    for (const auto& entry : recent) {
        if (std::filesystem::is_directory(entry.path, error)) continue;
        if (!any) section("Recent files");
        any = true;
        ImGui::PushID(path_utf8(entry.path).c_str());
        if (icon_row("recent", entry.mission ? icons::LC_GLOBE : icons::LC_FILE, Token::text_dim,
                     path_utf8(entry.path.filename()).c_str(), false))
            open_path(state, entry.path);
        recent_menu(state, entry);
        ImGui::PopID();
    }
    if (!state.settings.recent_pairings.empty()) {
        section("Recent collision pairings");
        const auto pairings = state.settings.recent_pairings;
        for (const auto& pair : pairings) {
            const auto label = path_utf8(pair.main.filename()) + " + " + path_utf8(pair.collision.filename());
            if (icon_row(label.c_str(), icons::LC_LINK, Token::text_dim, label.c_str(), false))
                open_pairing(state, pair);
        }
    }
}

// One line of the setup checklist: done (with its value) or a button to do it.
void setup_item(AppState& state, const char* label, const std::filesystem::path& value, const DialogKind kind,
                const char* why) {
    const bool done = !value.empty();
    ImGui::PushStyleColor(ImGuiCol_Text, color(done ? Token::ok : Token::warn));
    ImGui::TextUnformatted(done ? icons::LC_CIRCLE_CHECK : icons::LC_CIRCLE_ALERT);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    help_marker(why);
    ImGui::PushID(label);
    if (done) {
        ImGui::PushFont(font(Font::caption));
        dim_text("%s", state.ui.shown(path_utf8(value)).c_str());
        ImGui::PopFont();
    } else if (ImGui::SmallButton("Choose...")) {
        request_file_dialog(state, kind);
    }
    ImGui::PopID();
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
    // Home (docs/archive/editor/editor-ux-redesign.md, A2): projects first, then a
    // shipped mission to start from, then setup and keys.
    const float scale = ui_scale();
    const float content_width = std::min(ImGui::GetContentRegionAvail().x - 24.0F, 1100.0F * scale);
    ImGui::SetCursorPosX(std::max((ImGui::GetWindowWidth() - content_width) * 0.5F, 8.0F));
    ImGui::BeginGroup();
    ImGui::Dummy({0.0F, 24.0F * scale});
    ImGui::PushFont(font(Font::title));
    ImGui::TextUnformatted("CSF Mission Editor");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::TextUnformatted("Missions for Commandos Strike Force: a Blender-built map, placed actors and props, "
                           "patrols, zones, objectives and an intro.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0F, 10.0F * scale});
    if (primary_button((std::string(icons::LC_PLUS) + "  New project...").c_str())) state.commands.run("file.new_project");
    ImGui::SameLine();
    if (secondary_button((std::string(icons::LC_FOLDER_OPEN) + "  Open project...").c_str()))
        state.commands.run("file.open_project");
    ImGui::SameLine();
    if (secondary_button((std::string(icons::LC_GLOBE) + "  Open mission...").c_str()))
        state.commands.run("file.open_mission");
    ImGui::SameLine();
    if (secondary_button((std::string(icons::LC_BINARY) + "  Explore game files...").c_str()))
        state.commands.run("file.open_rws");
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::TextUnformatted("Or drop a project folder, an .scn, .rpc, .rws or .anm file anywhere on this window.");
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0F, 8.0F * scale});

    if (ImGui::BeginTable("##start_columns", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings,
                          {content_width, 0.0F})) {
        ImGui::TableSetupColumn("recent", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("missions", ImGuiTableColumnFlags_WidthStretch, 1.2F);
        ImGui::TableSetupColumn("setup", ImGuiTableColumnFlags_WidthStretch, 0.9F);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        draw_recent_projects(state);
        draw_recent_files(state);
        ImGui::TableNextColumn();
        section("Start from a shipped mission");
        // Only a mission list needs its own scrolling region; the "set a resource
        // root" prompt sizes to its text.
        if (state.settings.resource_root.empty()) {
            draw_missions(state);
        } else {
            ImGui::BeginChild("##start_missions", {0.0F, 360.0F * scale}, ImGuiChildFlags_None);
            draw_missions(state);
            ImGui::EndChild();
        }
        ImGui::TableNextColumn();
        section("Setup");
        setup_item(state, "Game files", state.settings.resource_root, DialogKind::resource_root,
                   "The unpacked game (or one unpacked package with Maps/): shipped missions to start from, and "
                   "the classes and models to place.");
        setup_item(state, "Game install", state.settings.game_root, DialogKind::game_root,
                   "The installed game's folder (with maps/<Mission>.pak): where built missions are deployed.");
        setup_item(state, "Projects folder", state.settings.projects_root, DialogKind::projects_root,
                   "Where new projects are created. Unset: the config directory's projects/ folder.");
        setup_item(state, "Blender", state.settings.blender, DialogKind::blender,
                   "The Blender executable, for Edit in Blender. Unset: blender from PATH.");
        section("Keys");
        // Generated from the command registry, so it cannot drift from the bindings.
        if (ImGui::BeginTable("##start_keys", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            for (const char* id : {"edit.go_to", "edit.command_palette", "view.workspace.mission",
                                   "view.workspace.script", "view.mode.inspect", "mission.tool_move",
                                   "mission.tool_rotate", "edit.undo", "file.save", "help.shortcuts"}) {
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
    if (state.ui.deterministic) {
        ImGui::Text("csf-editor %s | Dear ImGui %s", RWSMAN_VERSION, IMGUI_VERSION);
    } else {
        ImGui::Text("csf-editor %s | %s | Dear ImGui %s | built %s", RWSMAN_VERSION, build_type, IMGUI_VERSION,
                    __DATE__);
        if (!state.config_dir.empty()) ImGui::Text("settings: %s", path_utf8(state.config_dir).c_str());
    }
    ImGui::PopFont();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

} // namespace rwsman::ui
