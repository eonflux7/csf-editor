#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"

#include <imgui.h>

namespace rwsman::ui {

void draw_center(AppState& state, const Workspace workspace) {
    auto& document = state.document;
    if (workspace == Workspace::script) {
        draw_script_view(state);
    } else if (workspace == Workspace::scene || workspace == Workspace::mission ||
               workspace == Workspace::animation) {
        if (state.preview.draw_scene(
                document->chunks(), document->bytes(), document->scene_instances(),
                document->source_path(), state.selected, collision_export_document(state),
                state.main_is_collision, state.collision_status)) {
            state.ui.show_render_settings = true;
            state.ui.focus_render_settings = true;
        }
    } else if (workspace == Workspace::geometry) {
        const auto* selected_chunk =
            state.selected ? find_chunk(document->chunks(), *state.selected) : nullptr;
        const auto* geometry =
            selected_chunk ? find_preview_geometry(*selected_chunk, document->chunks()) : nullptr;
        if (!geometry) geometry = find_first_chunk(document->chunks(), 0x0F);
        if (geometry)
            state.preview.draw(*geometry, document->bytes(), document->source_path());
        else
            ImGui::TextDisabled("This document has no previewable Geometry.");
    } else {
        draw_hex_view(state);
    }
}

} // namespace rwsman::ui
