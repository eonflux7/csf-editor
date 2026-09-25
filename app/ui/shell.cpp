#include "ui/shell.hpp"

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
    const std::string window_title =
        state.mission.graph
            ? path_utf8(state.mission.graph->scene_path().filename()) +
                  (state.mission.editor && state.mission.editor->dirty() ? " *" : "") +
                  (state.mission.project ? " [" + path_utf8(state.mission.project->workspace_root.filename()) + "]"
                                         : std::string{}) +
                  " - CSF Mission Editor"
        : document ? path_utf8(document->source_path().filename()) +
                         (document->dirty() ? " *" : "") + " - CSF RWS Tools"
                   : "CSF RWS Tools - rws-man";
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

// `workspace` is the one whose dockspace this frame submitted. A panel can switch
// state.workspace mid-frame (a link in the Inspector, say); windows submitted after
// that under the new workspace's names would find no dockspace and be undocked.
void draw_panels(AppState& state, const Workspace workspace) {
    const bool has_document = state.document != nullptr;
    const bool maximized = state.ui.maximize_viewport && has_document;
    const bool viewport_workspace = has_viewport(workspace);
    auto& settings = state.settings;
    const auto mark_dirty = [&](const bool before, const bool after) {
        if (before != after) state.settings_dirty = true;
    };

    if (has_document && settings.show_explorer && !maximized) {
        const bool before = settings.show_explorer;
        panel(workspace, Panel::explorer, &settings.show_explorer, ImGuiWindowFlags_None,
              [&] { draw_explorer(state); });
        mark_dirty(before, settings.show_explorer);
    }
    if (has_document && settings.show_inspector && !maximized) {
        const bool before = settings.show_inspector;
        panel(workspace, Panel::inspector, &settings.show_inspector, ImGuiWindowFlags_None,
              [&] { draw_inspector(state); });
        mark_dirty(before, settings.show_inspector);
    }
    if (has_document && state.ui.show_render_settings && viewport_workspace &&
        workspace != Workspace::geometry && !maximized) {
        panel(workspace, Panel::render_settings, &state.ui.show_render_settings, ImGuiWindowFlags_None,
              [&] { draw_render_settings(state); }, state.ui.focus_render_settings);
        state.ui.focus_render_settings = false;
    }
    // The Console is the tab that shows first, once per workspace per session.
    // Layout restoration can finish a frame or two after the first submit, so keep
    // asking for a few frames.
    static std::array<int, 6> console_focus_frames{{4, 4, 4, 4, 4, 4}};
    const bool bottom_visible = settings.show_bottom_dock && !maximized;
    auto& pending_focus = console_focus_frames[static_cast<std::size_t>(workspace)];
    // A tab asked for by a command wins over the Console's first showing.
    if (state.ui.focus_bottom_tab >= 0) pending_focus = 0;
    if (bottom_visible && pending_focus > 0 && state.ui.focus_bottom_tab < 0) {
        state.ui.focus_bottom_tab = 0;
        --pending_focus;
    }
    if (settings.show_bottom_dock && !maximized) {
        struct Tab {
            Panel panel;
            bool* open;
            void (*draw)(AppState&);
        };
        const Tab tabs[] = {{Panel::console, &state.ui.show_console, draw_console},
                            {Panel::diagnostics, &state.ui.show_diagnostics, draw_diagnostics},
                            {Panel::references, &state.ui.show_references, draw_references},
                            {Panel::changes, &state.ui.show_changes, draw_changes},
                            {Panel::missions, &state.ui.show_missions, draw_missions}};
        int index = 0;
        for (const auto& tab : tabs) {
            if (*tab.open) {
                const bool focus = state.ui.focus_bottom_tab == index;
                panel(workspace, tab.panel, tab.open, ImGuiWindowFlags_None,
                      [&] { tab.draw(state); }, focus);
            }
            ++index;
        }
        state.ui.focus_bottom_tab = -1;
        // Closing every tab hides the dock instead of leaving an empty frame.
        if (!state.ui.show_console && !state.ui.show_diagnostics && !state.ui.show_references &&
            !state.ui.show_changes && !state.ui.show_missions) {
            settings.show_bottom_dock = false;
            state.ui.show_console = state.ui.show_diagnostics = state.ui.show_references =
                state.ui.show_changes = state.ui.show_missions = true;
            state.settings_dirty = true;
        }
    }

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
    draw_toasts(state);
}

} // namespace rwsman::ui
