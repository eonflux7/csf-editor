#include "ui/layout.hpp"

#include "ui/fonts.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <array>

namespace rwsman::ui {
namespace {

constexpr std::array mission_panels{
    PanelSlot{Panel::outliner, Dock::left},          PanelSlot{Panel::assets, Dock::left_bottom},
    PanelSlot{Panel::properties, Dock::right},       PanelSlot{Panel::objectives, Dock::right},
    PanelSlot{Panel::behaviours, Dock::right},       PanelSlot{Panel::mission_settings, Dock::right},
    PanelSlot{Panel::render_settings, Dock::right},  PanelSlot{Panel::problems, Dock::bottom},
    PanelSlot{Panel::history, Dock::bottom},         PanelSlot{Panel::timeline, Dock::bottom},
    PanelSlot{Panel::texts, Dock::bottom},           PanelSlot{Panel::build, Dock::bottom},
    PanelSlot{Panel::console, Dock::bottom},         PanelSlot{Panel::center, Dock::center},
};
constexpr std::array script_panels{
    PanelSlot{Panel::explorer, Dock::left},  PanelSlot{Panel::inspector, Dock::right},
    PanelSlot{Panel::flow, Dock::right},     PanelSlot{Panel::problems, Dock::bottom},
    PanelSlot{Panel::references, Dock::bottom}, PanelSlot{Panel::history, Dock::bottom},
    PanelSlot{Panel::console, Dock::bottom}, PanelSlot{Panel::center, Dock::center},
};
constexpr std::array inspect_viewport_panels{
    PanelSlot{Panel::explorer, Dock::left},       PanelSlot{Panel::inspector, Dock::right},
    PanelSlot{Panel::render_settings, Dock::right}, PanelSlot{Panel::console, Dock::bottom},
    PanelSlot{Panel::diagnostics, Dock::bottom},  PanelSlot{Panel::references, Dock::bottom},
    PanelSlot{Panel::changes, Dock::bottom},      PanelSlot{Panel::missions, Dock::bottom},
    PanelSlot{Panel::center, Dock::center},
};
constexpr std::array inspect_panels{
    PanelSlot{Panel::explorer, Dock::left},      PanelSlot{Panel::inspector, Dock::right},
    PanelSlot{Panel::console, Dock::bottom},     PanelSlot{Panel::diagnostics, Dock::bottom},
    PanelSlot{Panel::references, Dock::bottom},  PanelSlot{Panel::changes, Dock::bottom},
    PanelSlot{Panel::missions, Dock::bottom},    PanelSlot{Panel::center, Dock::center},
};

} // namespace

std::span<const PanelSlot> workspace_panels(const Workspace workspace) {
    switch (workspace) {
    case Workspace::mission:
        return mission_panels;
    case Workspace::script:
        return script_panels;
    case Workspace::scene:
    case Workspace::animation:
        return inspect_viewport_panels;
    case Workspace::geometry:
    case Workspace::inspector:
        return inspect_panels;
    }
    return inspect_panels;
}

bool workspace_has_panel(const Workspace workspace, const Panel panel) {
    return std::ranges::any_of(workspace_panels(workspace), [&](const PanelSlot& slot) { return slot.panel == panel; });
}

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
        return "Mission";
    case Workspace::script:
        return "Script";
    case Workspace::animation:
        return "Animation";
    case Workspace::scene:
        return "Scene";
    case Workspace::geometry:
        return "Geometry";
    case Workspace::inspector:
        return "Hex";
    }
    return "Scene";
}

const char* mode_label(const Mode mode) {
    switch (mode) {
    case Mode::mission:
        return "Mission";
    case Mode::script:
        return "Script";
    case Mode::inspect:
        return "Inspect";
    }
    return "";
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
        return "Output";
    case Panel::diagnostics:
        return "Diagnostics";
    case Panel::references:
        return "References";
    case Panel::changes:
        return "Changes";
    case Panel::missions:
        return "Missions";
    case Panel::outliner:
        return "Outliner";
    case Panel::properties:
        return "Properties";
    case Panel::assets:
        return "Assets";
    case Panel::problems:
        return "Problems";
    case Panel::history:
        return "History";
    case Panel::texts:
        return "Texts";
    case Panel::timeline:
        return "Intro cutscene";
    case Panel::mission_settings:
        return "Mission";
    case Panel::objectives:
        return "Objectives";
    case Panel::behaviours:
        return "Behaviours";
    case Panel::build:
        return "Build";
    case Panel::flow:
        return "Flow";
    case Panel::count:
        break;
    }
    return "";
}

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
    case Panel::outliner:
        return "outliner";
    case Panel::properties:
        return "properties";
    case Panel::assets:
        return "assets";
    case Panel::problems:
        return "problems";
    case Panel::history:
        return "history";
    case Panel::texts:
        return "texts";
    case Panel::timeline:
        return "timeline";
    case Panel::mission_settings:
        return "mission_settings";
    case Panel::objectives:
        return "objectives";
    case Panel::behaviours:
        return "behaviours";
    case Panel::build:
        return "build";
    case Panel::flow:
        return "flow";
    case Panel::count:
        break;
    }
    return "";
}

std::string panel_window_name(const Workspace workspace, const Panel panel) {
    return std::string(panel_title(panel)) + "###" + workspace_key(workspace) + "." + panel_key(panel);
}

ImGuiID dockspace_id(const Workspace workspace) {
    // The suffix is the layout version: a layout.ini written for another set
    // of panels starts over with the default layout instead of floating the
    // new panels.
    return ImGui::GetID((std::string("dockspace.") + workspace_key(workspace) + ".v2").c_str());
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

    // Sizes are chosen in logical pixels so the panels stay readable at any
    // display scale; the script listing needs width more than the viewport does.
    const bool script = workspace == Workspace::script;
    const bool mission = workspace == Workspace::mission;
    const float scale = ui_scale();
    const float left_fraction =
        std::clamp((script ? 360.0F : 320.0F) * scale / std::max(size.x, 1.0F), 0.16F, 0.30F);
    const float right_fraction =
        std::clamp((script ? 440.0F : 400.0F) * scale / std::max(size.x, 1.0F), 0.20F, 0.36F);
    const float bottom_fraction = mission ? 0.24F : 0.22F;

    ImGuiID top{}, bottom{}, left{}, remaining{}, right{}, center{}, left_top{}, left_bottom{};
    ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Down, bottom_fraction, &bottom, &top);
    ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, left_fraction, &left, &remaining);
    ImGui::DockBuilderSplitNode(remaining, ImGuiDir_Right, right_fraction / (1.0F - left_fraction),
                                &right, &center);
    const auto panels = workspace_panels(workspace);
    const bool split_left =
        std::ranges::any_of(panels, [](const PanelSlot& slot) { return slot.dock == Dock::left_bottom; });
    left_top = left;
    if (split_left) ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42F, &left_bottom, &left_top);

    ImGuiID first_bottom{};
    for (const auto& slot : panels) {
        const ImGuiID node = slot.dock == Dock::left          ? left_top
                             : slot.dock == Dock::left_bottom ? left_bottom
                             : slot.dock == Dock::right       ? right
                             : slot.dock == Dock::bottom      ? bottom
                                                              : center;
        ImGui::DockBuilderDockWindow(panel_window_name(workspace, slot.panel).c_str(), node);
        if (slot.dock == Dock::bottom && first_bottom == 0) first_bottom = ImHashStr(panel_window_name(workspace, slot.panel).c_str());
    }
    // The first tab of each dock is the one showing when the layout appears.
    if (auto* node = ImGui::DockBuilderGetNode(bottom); node && first_bottom) node->SelectedTabId = first_bottom;
    if (auto* node = ImGui::DockBuilderGetNode(right))
        for (const auto& slot : panels)
            if (slot.dock == Dock::right) {
                node->SelectedTabId = ImHashStr(panel_window_name(workspace, slot.panel).c_str());
                break;
            }
    if (auto* node = ImGui::DockBuilderGetNode(center))
        node->SetLocalFlags(ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton);
    ImGui::DockBuilderFinish(dockspace);
}

} // namespace rwsman::ui
