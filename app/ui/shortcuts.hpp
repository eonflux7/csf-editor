#pragma once

#include "app_state.hpp"
#include "rwsman/commands.hpp"

#include <imgui.h>

#include <optional>
#include <string_view>

namespace rwsman::ui {

// Maps a shortcut key name ("P", "F12", "Left", "Comma") to an ImGui key.
[[nodiscard]] std::optional<ImGuiKey> key_from_name(std::string_view name);

// Runs the command whose shortcut was pressed this frame. Global shortcuts are
// ignored while a text field has focus; viewport shortcuts also require the
// pointer to be over the 3D viewport. Mouse buttons 4 and 5 map to back/forward.
void dispatch_shortcuts(AppState& state);

} // namespace rwsman::ui
