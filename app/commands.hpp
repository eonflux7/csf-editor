#pragma once

#include "app_state.hpp"

namespace rwsman {

// Registers every user-facing command. Menus, keyboard shortcuts, the command
// palette, and the Help cheat sheet all read this one registry, so a binding can
// only be defined (and changed) here.
void register_commands(AppState& state);

// Copies the current selection's identity string to the clipboard.
void copy_selection_identity(AppState& state);
void copy_to_clipboard(AppState& state, const std::string& text, const char* what = "text");
void toggle_bottom_panel(AppState& state, bool UiState::*panel);
// Steps the user UI scale by 10% (`step` > 0 larger, < 0 smaller, 0 back to 100%).
void change_ui_scale(AppState& state, float step);

} // namespace rwsman
