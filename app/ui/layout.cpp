#include "ui/layout.hpp"

#include "ui/fonts.hpp"

#include <imgui_internal.h>

#include <algorithm>

namespace rwsman::ui {

const char* workspace_key(const Workspace workspace) {
    switch (workspace) {
    case Workspace::mission:
        return "mission";
    case Workspace::script:
        return "script";
    case Workspace::animation:
        return "anim";
    case Workspace::scene:
        return "scene";
    case Workspace::geometry:
        return "geom";
    case Workspace::inspector:
        return "hex";
    }
    return "scene";
}

const char* workspace_tab_label(const Workspace workspace) {
    switch (workspace) {
    case Workspace::mission:
        return "MISSION";
    case Workspace::script:
        return "SCRIPT";
    case Workspace::animation:
        return "ANIM";
    case Workspace::scene:
        return "SCENE";
    case Workspace::geometry:
        return "GEOM";
    case Workspace::inspector:
        return "HEX";
    }
    return "SCENE";
}

const char* panel_title(const Panel panel) {
    switch (panel) {
    case Panel::explorer:
        return "Explorer";
    case Panel::center:
        return "Viewport";
    case Panel::inspector:
        return "Inspector";
    case Panel::render_settings:
        return "Render settings";
    case Panel::console:
        return "Console";
    case Panel::diagnostics:
        return "Diagnostics";
    case Panel::references:
        return "References";
    case Panel::changes:
        return "Changes";
    case Panel::missions:
        return "Missions";
    case Panel::count:
        break;
    }
    return "";
}

namespace {
const char* panel_key(const Panel panel) {
    switch (panel) {
    case Panel::explorer:
        return "explorer";
    case Panel::center:
        return "center";
    case Panel::inspector:
        return "inspector";
    case Panel::render_settings:
        return "render";
    case Panel::console:
        return "console";
    case Panel::diagnostics:
        return "diagnostics";
    case Panel::references:
        return "references";
    case Panel::changes:
        return "changes";
    case Panel::missions:
        return "missions";
    case Panel::count:
        break;
    }
    return "";
}
} // namespace

std::string panel_window_name(const Workspace workspace, const Panel panel) {
    return std::string(panel_title(panel)) + "###" + workspace_key(workspace) + "." + panel_key(panel);
}

ImGuiID dockspace_id(const Workspace workspace) {
    return ImGui::GetID((std::string("dockspace.") + workspace_key(workspace)).c_str());
}

bool has_viewport(const Workspace workspace) {
    return workspace == Workspace::mission || workspace == Workspace::animation ||
           workspace == Workspace::scene || workspace == Workspace::geometry;
}

void ensure_layout(const Workspace workspace, const ImGuiID dockspace, const ImVec2 size,
                   const bool force) {
    if (!force && ImGui::DockBuilderGetNode(dockspace) != nullptr) return;
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, size);

    // The script listing needs width more than the viewport does.
    // Sizes are chosen in logical pixels so the panels stay readable at any
    // display scale: about 21% / 25% of a 1600 px window at 100%.
    const bool script = workspace == Workspace::script;
    const float scale = ui_scale();
    const float left_fraction =
        std::clamp((script ? 360.0F : 340.0F) * scale / std::max(size.x, 1.0F), 0.16F, 0.32F);
    const float right_fraction =
        std::clamp((script ? 440.0F : 400.0F) * scale / std::max(size.x, 1.0F), 0.20F, 0.36F);
    constexpr float bottom_fraction = 0.22F;

    ImGuiID top{}, bottom{}, left{}, remaining{}, right{}, center{};
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Down, bottom_fraction, &bottom, &top);
    ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, left_fraction, &left, &remaining);
    ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, right_fraction / (1.0F - left_fraction),
                                &right, &center);

    ImGui::DockBuilderDockWindow(panel_window_name(workspace, Panel::explorer).c_str(), left);
    ImGui::DockBuilderDockWindow(panel_window_name(workspace, Panel::inspector).c_str(), right);
    ImGui::DockBuilderDockWindow(panel_window_name(workspace, Panel::render_settings).c_str(), right);
    ImGui::DockBuilderDockWindow(panel_window_name(workspace, Panel::center).c_str(), center);
    for (const auto panel :
         {Panel::console, Panel::diagnostics, Panel::references, Panel::changes, Panel::missions})
        ImGui::DockBuilderDockWindow(panel_window_name(workspace, panel).c_str(), bottom);
    // The Console tab is the one showing when the bottom dock first appears.
    if (auto* node = ImGui::DockBuilderGetNode(bottom))
        node->SelectedTabId = ImHashStr(panel_window_name(workspace, Panel::console).c_str());
    if (auto* node = ImGui::DockBuilderGetNode(center))
        node->SetLocalFlags(ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton);
    ImGui::DockBuilderFinish(dockspace);
}

} // namespace rwsman::ui
