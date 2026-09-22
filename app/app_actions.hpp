#pragma once

#include "app_state.hpp"

#include <filesystem>
#include <optional>

// Operations on AppState that do not draw anything. UI code calls these; they
// report their outcome to the operation log (AppState::info/ok/warn/error).
namespace rwsman {

void restore_mission_overlays(AppState& state);
// Rebuilds AppState::discovered from settings.resource_root.
void rescan_resource_root(AppState& state);
// Pushes the viewport preferences (move speed, invert Y, default shading, HUD)
// from the settings into the preview.
void apply_viewport_settings(AppState& state);
// Path for the next screenshot: a new file in <config>/screenshots.
[[nodiscard]] std::filesystem::path next_screenshot_path(const AppState& state);
bool pair_collision(AppState& state, const std::filesystem::path& candidate_path, bool remember);
void load_document(AppState& state, const std::filesystem::path& path);
// Opens a recent (main, collision) pair: loads the main file unless it is already
// open, then pairs the collision companion with it.
void open_pairing(AppState& state, const RecentPairing& pairing);
// Starts loading a mission on a worker thread. The current mission stays until
// poll_mission_load() commits the result. With `project`, the mod project's
// edits are applied. Unsaved mission edits make it ask first, unless `discard`.
void start_mission_load(AppState& state, const std::filesystem::path& path,
                        std::optional<std::filesystem::path> project = std::nullopt, bool discard = false);
// Call once per frame: commits a finished mission load or reports its failure.
void poll_mission_load(AppState& state);
void cancel_mission_load(AppState& state);
[[nodiscard]] bool mission_load_active(const AppState& state);
// Opens a mission for .scn paths and a RenderWare document for everything else.
void open_path(AppState& state, const std::filesystem::path& path);

// How an export chooses its output path. `automatic` follows the export policy
// in the settings: never replace an existing file by default (a numbered new file
// is written), or ask first when the policy is confirm_overwrite.
enum class OutputMode { automatic, overwrite, unique };
void save_copy(AppState& state, OutputMode mode = OutputMode::automatic);
// Opens a folder in the platform file manager (detached).
void open_folder(const std::filesystem::path& folder);
// Native file dialogs. They do nothing when the platform has no dialog.
void open_document_dialog(AppState& state);
void open_mission_dialog(AppState& state);
void open_companion_dialog(AppState& state);

void export_scene_gltf(AppState& state, OutputMode mode = OutputMode::automatic);
void export_selected_clump_gltf(AppState& state, OutputMode mode = OutputMode::automatic);
void export_collision_gltf(AppState& state, OutputMode mode = OutputMode::automatic);
void export_collision_obj(AppState& state, OutputMode mode = OutputMode::automatic);
// The first path that does not exist yet: "a.gltf", "a.2.gltf", "a.3.gltf", ...
[[nodiscard]] std::filesystem::path unique_output_path(const std::filesystem::path& desired);

[[nodiscard]] const rws::Document* collision_export_document(const AppState& state);
[[nodiscard]] const rws::Chunk* selected_clump(const AppState& state);

// Imports the pre-settings recent-pairings.txt list once, when no settings file
// exists yet.
void import_legacy_pairings(AppState& state);

} // namespace rwsman
