#pragma once

#include "app_state.hpp"

#include <filesystem>

namespace rwsman {

enum class DialogKind {
    document,
    mission,
    companion,
    resource_root,
    game_root,          // Game installation folder (holds maps/<Mission>.pak).
    projects_root,      // Folder that new mission projects are created in.
    open_project,       // Mission project folder to open.
    save_project,       // Folder for a new mission project.
    export_original,    // Shipped mission archive, for the export dialog.
    export_output,      // Where the rebuilt mission archive is written.
    import_donor,       // Another unpacked mission to import from.
    blender,            // The Blender executable.
    test_install,       // The authoring project's test install (a game copy to deploy into).
    original_archive,   // An untouched shipped archive for the authoring project's builds.
};

// Opens a native file or folder dialog without blocking the frame loop. Only one
// dialog is open at a time; further requests are ignored until it closes. On
// Windows the open dialogs use the Win32 common dialog synchronously instead
// (see app_actions.cpp); the folder dialog uses portable-file-dialogs everywhere.
void request_file_dialog(AppState& state, DialogKind kind, const std::filesystem::path& start_directory = {});
// Call once per frame: delivers the chosen path of a finished dialog.
void poll_file_dialogs(AppState& state);
[[nodiscard]] bool file_dialog_open(const AppState& state);

} // namespace rwsman
