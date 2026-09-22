#pragma once

#include "app_state.hpp"

#include <imgui.h>

#include <string>

namespace rwsman::ui {

enum class Panel {
    explorer,
    center,
    inspector,
    render_settings,
    console,
    diagnostics,
    references,
    changes,
    missions,
    count
};

[[nodiscard]] const char* workspace_key(Workspace workspace);
// Short upper-case label for the workspace tab strip.
[[nodiscard]] const char* workspace_tab_label(Workspace workspace);
[[nodiscard]] const char* panel_title(Panel panel);

// Each workspace owns its own dockspace and its own instances of every panel
// window ("Explorer###mission.explorer"), so the layouts are independent and
// ImGui persists them all in one ini file.
[[nodiscard]] std::string panel_window_name(Workspace workspace, Panel panel);
[[nodiscard]] ImGuiID dockspace_id(Workspace workspace);

// Builds the default layout for `workspace` when it has none yet, or when
// `force` is set (View > Reset layout). Explorer 18%, Inspector 22%, bottom dock
// 22%, viewport in the central node.
void ensure_layout(Workspace workspace, ImGuiID dockspace, ImVec2 size, bool force);

// True for workspaces whose center panel is the 3D viewport.
[[nodiscard]] bool has_viewport(Workspace workspace);

} // namespace rwsman::ui
