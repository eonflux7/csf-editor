#include "ui/shell.hpp"

#include "animation_preview.hpp"
#include "authoring.hpp"
#include "app_actions.hpp"
#include "app_state.hpp"
#include "mission_editing.hpp"
#include "app_util.hpp"
#include "navigation.hpp"
#include "ui/fonts.hpp"
#include "ui/layout.hpp"
#include "ui/shortcuts.hpp"
#include "ui/theme.hpp"
#include "ui/ui.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>

namespace rwsman::ui {
namespace {

void update_window_title(AppState& state) {
    auto& document = state.document;
    static std::string previous_window_title;
    std::string window_title;
    if (state.mission.graph) {
        // "Checkpoint - Convoy slot * - CSF Mission Editor": the project, the
        // mission slot it replaces, and whether anything is unsaved.
        const auto slot = path_utf8(state.mission.graph->scene_path().stem());
        const bool dirty = edits_unsaved(state);
        const std::string project = state.authoring.project && !state.authoring.project->name.empty()
                                        ? state.authoring.project->name
                                    : state.mission.project ? path_utf8(state.mission.project->workspace_root.filename())
                                                            : std::string{};
        window_title = (project.empty() ? slot : project + " - " + slot + " slot") + (dirty ? " *" : "") +
                       " - CSF Mission Editor";
    } else if (document) {
        window_title = path_utf8(document->source_path().filename()) + (document->dirty() ? " *" : "") + " - CSF Mission Editor";
    } else {
        window_title = "CSF Mission Editor";
    }
    if (window_title != previous_window_title) {
        glfwSetWindowTitle(state.window, window_title.c_str());
        previous_window_title = window_title;
    }
}

// Submits one panel window. Returns after End(); `body` runs only when the
// window is visible (not collapsed or hidden in an inactive tab).
template <class Body>
void panel(const Workspace workspace, const Panel which, bool* open, const ImGuiWindowFlags flags,
           Body&& body, const bool focus = false) {
    const auto name = panel_window_name(workspace, which);
    if (focus) ImGui::SetNextWindowFocus();
    // Each tab has its own close button; the dock node's extra one at the right
    // end of the tab bar only duplicated it.
    ImGuiWindowClass panel_class;
    panel_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoCloseButton;
    ImGui::SetNextWindowClass(&panel_class);
    if (ImGui::Begin(name.c_str(), open, flags)) body();
    ImGui::End();
}

void draw_panel_body(AppState& state, const Panel which) {
    switch (which) {
    case Panel::explorer: return draw_explorer(state);
    case Panel::inspector: return draw_inspector(state);
    case Panel::render_settings: return draw_render_settings(state);
    case Panel::console: return draw_console(state);
    case Panel::diagnostics: return draw_diagnostics(state);
    case Panel::references: return draw_references(state);
    case Panel::changes: return draw_changes(state);
    case Panel::missions: return draw_missions(state);
    case Panel::outliner: return draw_outliner(state);
    case Panel::properties: return draw_properties(state);
    case Panel::assets: return draw_assets_panel(state);
    case Panel::problems: return draw_problems(state);
    case Panel::history: return draw_history_panel(state);
    case Panel::texts: return draw_texts_panel(state);
    case Panel::timeline: return draw_timeline(state);
    case Panel::mission_settings: return draw_mission_settings(state);
    case Panel::objectives: return draw_objectives_panel(state);
    case Panel::behaviours: return draw_behaviours(state);
    case Panel::build: return draw_build(state);
    case Panel::flow: return draw_flow_panel(state);
    case Panel::center:
    case Panel::count: break;
    }
}

bool dock_shown(const AppState& state, const Dock dock) {
    switch (dock) {
    case Dock::left:
    case Dock::left_bottom: return state.settings.show_explorer;
    case Dock::right: return state.settings.show_inspector;
    case Dock::bottom: return state.settings.show_bottom_dock;
    case Dock::center: return true;
    }
    return true;
}

// `workspace` is the one whose dockspace this frame submitted. A panel can switch
// state.workspace mid-frame (a link in the Inspector, say); windows submitted after
// that under the new workspace's names would find no dockspace and be undocked.
void draw_panels(AppState& state, const Workspace workspace) {
    const bool has_document = state.document != nullptr;
    const bool maximized = state.ui.maximize_viewport && has_document;
    const bool viewport_workspace = has_viewport(workspace);
    auto& ui = state.ui;

    if (has_document && !maximized) {
        for (const auto& slot : workspace_panels(workspace)) {
            if (slot.panel == Panel::center || !dock_shown(state, slot.dock)) continue;
            auto& open = ui.panel_open[static_cast<std::size_t>(slot.panel)];
            if (!open) continue;
            if (slot.panel == Panel::render_settings && (!viewport_workspace || workspace == Workspace::geometry))
                continue;
            const bool focus = ui.focus_panel == slot.panel && ui.focus_panel_frames > 0;
            panel(workspace, slot.panel, &open, ImGuiWindowFlags_None, [&] { draw_panel_body(state, slot.panel); },
                  focus);
            if (!open) state.settings_dirty = true;
        }
        // Closing every panel of a dock hides the dock instead of leaving an
        // empty frame; its panels come back when it is shown again.
        for (const auto dock : {Dock::left, Dock::right, Dock::bottom}) {
            bool any = false, any_open = false;
            for (const auto& slot : workspace_panels(workspace)) {
                const bool same = slot.dock == dock || (dock == Dock::left && slot.dock == Dock::left_bottom);
                if (!same || slot.panel == Panel::render_settings) continue;
                any = true;
                any_open |= ui.panel_open[static_cast<std::size_t>(slot.panel)];
            }
            if (!any || any_open || !dock_shown(state, dock)) continue;
            for (const auto& slot : workspace_panels(workspace))
                if ((slot.dock == dock || (dock == Dock::left && slot.dock == Dock::left_bottom)) &&
                    slot.panel != Panel::render_settings)
                    ui.panel_open[static_cast<std::size_t>(slot.panel)] = true;
            (dock == Dock::left ? state.settings.show_explorer
             : dock == Dock::right ? state.settings.show_inspector
                                   : state.settings.show_bottom_dock) = false;
            state.settings_dirty = true;
        }
    }
    if (ui.focus_panel_frames > 0 && --ui.focus_panel_frames == 0) ui.focus_panel.reset();

    // The center panel always exists: the viewport, listing, or start page.
    ImGuiWindowFlags center_flags = ImGuiWindowFlags_NoTitleBar;
    const bool flat = has_document && viewport_workspace;
    if (flat) center_flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (flat) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
    const auto center_name = panel_window_name(workspace, Panel::center);
    // The center node never shows a tab bar or menu button, including layouts
    // restored from an older layout.ini.
    ImGuiWindowClass center_class;
    center_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoWindowMenuButton |
                                            ImGuiDockNodeFlags_NoCloseButton;
    ImGui::SetNextWindowClass(&center_class);
    if (ImGui::Begin(center_name.c_str(), nullptr, center_flags)) {
        if (has_document)
            draw_center(state, workspace);
        else
            draw_start_page(state);
    }
    ImGui::End();
    if (flat) ImGui::PopStyleVar();
}

} // namespace

void draw_frame(AppState& state) {
    dispatch_shortcuts(state);
    refresh_mission_from_editor(state);
    update_authoring_views(state);
    tick_actor_animation(state);
    track_selection(state);
    update_mission_gizmo(state);
    if (state.preview.take_screenshot_request()) state.ui.screenshot_requested = true;
    // The HUD toggle lives in the viewport menu; mirror it into the settings.
    if (state.preview.show_hud() != state.settings.show_hud) {
        state.settings.show_hud = state.preview.show_hud();
        state.settings_dirty = true;
    }
    // Marker display options live in the viewport toolbar; mirror them too.
    if (state.preview.overlay_options() != state.settings.overlays) {
        state.settings.overlays = state.preview.overlay_options();
        state.settings_dirty = true;
    }
    update_window_title(state);

    draw_menus(state);
    draw_status_bar(state);
    if (state.workspace == Workspace::mission && state.mission.scene) draw_mission_bar(state);
    // The Inspect mode remembers its workspace for Ctrl+3.
    if (mode_of(state.workspace) == Mode::inspect) state.ui.inspect_workspace = state.workspace;

    const auto* viewport = ImGui::GetMainViewport();
    const Workspace workspace = state.workspace;
    const ImGuiID dock = dockspace_id(workspace);
    ImGui::DockSpaceOverViewport(dock, viewport, ImGuiDockNodeFlags_None);
    // Build the default layout for a workspace that has none, or on request.
    const auto* node = ImGui::DockBuilderGetNode(dock);
    const bool fresh = node == nullptr || (node->IsLeafNode() && node->Windows.Size == 0);
    if (state.ui.reset_layout || fresh) {
        ensure_layout(workspace, dock, viewport->WorkSize, true);
        state.ui.reset_layout = false;
    }

    draw_panels(state, workspace);
    draw_extra_inspectors(state);
    draw_shortcuts_window(state);
    draw_about_window(state);
    draw_palette(state);
    draw_load_overlay(state);
    draw_preferences(state);
    draw_height_report(state);
    draw_overwrite_dialog(state);
    draw_mission_dialogs(state);
    draw_project_dialogs(state);
    draw_toasts(state);
}

} // namespace rwsman::ui
