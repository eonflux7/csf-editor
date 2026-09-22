#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "navigation.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/layout.hpp"
#include "ui/shortcuts.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>

namespace rwsman::ui {

void draw_render_settings(AppState& state) {
    if (!state.document) {
        dim_text("Open a document to adjust how it renders.");
        return;
    }
    state.preview.draw_scene_tools(state.collision_status);
}

void draw_shortcuts_window(AppState& state) {
    if (!state.ui.show_shortcuts) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, {0.5F, 0.5F});
    ImGui::SetNextWindowSize({560.0F * ui_scale(), std::min(620.0F * ui_scale(), viewport->WorkSize.y * 0.9F)},
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard shortcuts", &state.ui.show_shortcuts)) {
        ImGui::End();
        return;
    }
    dim_text("Generated from the command registry. Ctrl+Shift+P searches every command.");
    ImGui::PushFont(font(Font::mono));
    const float key_width = ImGui::CalcTextSize("Ctrl+Shift+Space").x;
    ImGui::PopFont();
    for (const auto& [category, commands] : state.commands.by_category()) {
        section(category.c_str());
        if (ImGui::BeginTable(("##keys" + category).c_str(), 2, ImGuiTableFlags_SizingFixedFit)) {
            // One key column width for every category, so the tables line up.
            ImGui::TableSetupColumn("command", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, key_width);
            for (const auto* command : commands) {
                if (command->shortcut.empty() || !command->in_help) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(command->label.c_str());
                if (command->scope == ShortcutScope::viewport) {
                    ImGui::SameLine();
                    dim_text("(viewport)");
                }
                ImGui::TableNextColumn();
                ImGui::PushFont(font(Font::mono));
                ImGui::TextUnformatted(command->shortcut.c_str());
                ImGui::PopFont();
            }
            ImGui::EndTable();
        }
    }
    section("Camera bookmarks");
    dim_text("Shift+1...9 recalls a bookmark; Ctrl+Shift+1...9 stores one (viewport hovered). Bookmarks are kept per mission.");
    section("Viewport");
    dim_text("Left drag: look   Right drag: orbit   Middle drag: pan");
    dim_text("Wheel: zoom   Double-click: frame   WASD/QE: move (Shift = faster)");
    ImGui::End();
}

void draw_about_window(AppState& state) {
    if (!state.ui.show_about) return;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5F, 0.5F});
    ImGui::SetNextWindowSize({420.0F * ui_scale(), 0.0F}, ImGuiCond_Appearing);
    if (!ImGui::Begin("About", &state.ui.show_about, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(font(Font::sans_bold));
    ImGui::TextUnformatted("CSF RWS Tools - rws-man");
    ImGui::PopFont();
    dim_text("Chunk browser, mission explorer, and 3D preview for Commandos: Strike Force data.");
    section("Build");
    ImGui::PushFont(font(Font::mono));
    ImGui::Text("Dear ImGui %s (docking)", IMGUI_VERSION);
#ifdef NDEBUG
    ImGui::TextUnformatted("Release build");
#else
    ImGui::TextUnformatted("Debug build");
#endif
    ImGui::PopFont();
    section("Fonts");
    dim_text("Inter and IosevkaTerm (SIL OFL 1.1) and Lucide icons (ISC). See app/ui/fonts/LICENSES.md.");
    ImGui::End();
}

} // namespace rwsman::ui
