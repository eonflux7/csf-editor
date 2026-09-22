#pragma once

#include "app_state.hpp"

#include <filesystem>

namespace rwsman {

enum class DialogKind { document, mission, companion, resource_root };

// Opens a native file or folder dialog without blocking the frame loop. Only one
// dialog is open at a time; further requests are ignored until it closes. On
// Windows the open dialogs use the Win32 common dialog synchronously instead
// (see app_actions.cpp); the folder dialog uses portable-file-dialogs everywhere.
void request_file_dialog(AppState& state, DialogKind kind, const std::filesystem::path& start_directory = {});
// Call once per frame: delivers the chosen path of a finished dialog.
void poll_file_dialogs(AppState& state);
[[nodiscard]] bool file_dialog_open(const AppState& state);

} // namespace rwsman
