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

namespace rwsman::ui {

void draw_changes(AppState& state) {
    section("Session changes");
    if (state.document && state.document->dirty())
        token_text(Token::dirty, "%s %s has unsaved byte edits", icons::LC_DOT,
                   path_utf8(state.document->source_path().filename()).c_str());
    else
        dim_text("No pending changes. Byte edits made in the Hex workspace appear here.");
    dim_text("Guarded authoring (Phase 06) will list validated edits and staged mod files here.");
}

void draw_render_settings(AppState& state) {
    if (!state.document) {
        dim_text("Open a document to adjust how it renders.");
        return;
    }
    state.preview.draw_scene_tools(state.collision_status);
}

void draw_shortcuts_window(AppState& state) {
    if (!state.ui.show_shortcuts) return;
    ImGui::SetNextWindowSize({520.0F * ui_scale(), 520.0F * ui_scale()}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard shortcuts", &state.ui.show_shortcuts)) {
        ImGui::End();
        return;
    }
    dim_text("Generated from the command registry. Ctrl+Shift+P searches every command.");
    for (const auto& [category, commands] : state.commands.by_category()) {
        section(category.c_str());
        if (ImGui::BeginTable(("##keys" + category).c_str(), 2, ImGuiTableFlags_SizingStretchProp)) {
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
    dim_text("IBM Plex Sans / Plex Mono (SIL OFL 1.1) and Lucide icons (ISC). See app/ui/fonts/LICENSES.md.");
    ImGui::End();
}

} // namespace rwsman::ui
