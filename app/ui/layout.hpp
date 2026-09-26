#pragma once

#include "app_state.hpp"

#include <imgui.h>

#include <span>
#include <string>

namespace rwsman::ui {

using rwsman::Panel;

// Where a panel docks in a workspace's default layout.
enum class Dock : std::uint8_t { left, left_bottom, right, bottom, center };
struct PanelSlot {
    Panel panel;
    Dock dock;
};

// The panels of a workspace, in tab order within each dock. Mission mode
// (docs/plans/editor-ux-redesign.md, §3 B): Outliner and Assets on the left,
// Properties and the mission forms on the right, Problems, History and the
// rest at the bottom. Script: the program outline, the listing, the
// Inspector and Flow. Inspect: the workbench as it was.
[[nodiscard]] std::span<const PanelSlot> workspace_panels(Workspace workspace);
// Whether `panel` belongs to `workspace`'s layout.
[[nodiscard]] bool workspace_has_panel(Workspace workspace, Panel panel);

[[nodiscard]] const char* workspace_key(Workspace workspace);
// Short label for the mode and Inspect sub-workspace switches.
[[nodiscard]] const char* workspace_tab_label(Workspace workspace);
[[nodiscard]] const char* mode_label(Mode mode);
[[nodiscard]] const char* panel_title(Panel panel);
// Stable identifier ("outliner"), used in window names and command IDs.
[[nodiscard]] const char* panel_key(Panel panel);

// Each workspace owns its own dockspace and its own instances of every panel
// window ("Outliner###mission.outliner"), so the layouts are independent and
// ImGui persists them all in one ini file.
[[nodiscard]] std::string panel_window_name(Workspace workspace, Panel panel);
[[nodiscard]] ImGuiID dockspace_id(Workspace workspace);

// Builds the default layout for `workspace` when it has none yet, or when
// `force` is set (View > Reset layout). Side docks about 320 px (left) and
// 400 px (right, scaled), bottom dock 24%, the viewport in the central node.
void ensure_layout(Workspace workspace, ImGuiID dockspace, ImVec2 size, bool force);

// True for workspaces whose center panel is the 3D viewport.
[[nodiscard]] bool has_viewport(Workspace workspace);

} // namespace rwsman::ui
