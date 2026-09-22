#pragma once

namespace rwsman {
struct AppState;
}

namespace rwsman::ui {

// Handles global shortcuts, draws the menu bar, panels, and status bar for one
// ImGui frame. Called between ImGui::NewFrame() and ImGui::Render().
void draw_frame(AppState& state);

} // namespace rwsman::ui
