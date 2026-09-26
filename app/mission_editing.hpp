#pragma once

#include "app_state.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// Mission editing on top of AppState: applies csf::MissionEditor operations,
// keeps the mission views in step with the editor, and saves and exports the
// result. UI code calls these; outcomes go to the operation log and toasts.
namespace rwsman {

[[nodiscard]] MissionRecordKey mission_record_key(const csf::MissionScene& scene, std::uint32_t entry);
[[nodiscard]] std::optional<std::uint32_t> mission_record_entry(const csf::MissionScene& scene,
                                                                const MissionRecordKey& key);
// The record under the current selection, when it is a scene record.
[[nodiscard]] MissionRecordKey selected_mission_record(const AppState& state);
void select_mission_record(AppState& state, const MissionRecordKey& key, bool frame = false);

[[nodiscard]] bool mission_editable(const AppState& state);
// Reports the outcome of an editor operation; returns whether it applied. The
// mission views keep showing the previous state until the next frame's
// refresh, so references held while drawing stay valid.
bool apply_mission_edit(AppState& state, const csf::EditResult& result);
// Selects `key` once the views include it (after the next refresh).
void select_after_refresh(AppState& state, const MissionRecordKey& key);
// Rebuilds the scene, programs, databases, overlays and actor models from the
// editor when it changed since the last call. Call once per frame, before
// anything draws.
void refresh_mission_from_editor(AppState& state);
// After the refresh, once a frame: applies the Outliner's hidden and locked
// records to the viewport and rebuilds the problem list when its inputs changed.
void update_authoring_views(AppState& state);
void mission_undo(AppState& state);
void mission_redo(AppState& state);

// Mission name for projects and archives: the package folder name (Ransom).
[[nodiscard]] std::string mission_name(const AppState& state);
[[nodiscard]] std::filesystem::path default_project_workspace(const AppState& state);
// maps/<Mission>.pak under the game folder, when it exists.
[[nodiscard]] std::filesystem::path guess_original_archive(const AppState& state);
// Saves into the open project, or creates one at `workspace` (default location
// when empty) on first save.
void save_mission_project(AppState& state, const std::filesystem::path& workspace = {});
// Loads a project's mission with its authored files.
void open_mission_project(AppState& state, const std::filesystem::path& workspace);
// Saves the project, then rebuilds the full mission archive next to nothing
// shipped: `output` must not be the original. Optionally installs it into the
// game folder (backing up the original) afterwards.
void export_mission_archive(AppState& state, const std::filesystem::path& original,
                            const std::filesystem::path& output, bool overwrite, bool install);

// Record-level actions on the selection.
void duplicate_selected_record(AppState& state);
void delete_selected_record(AppState& state, bool force = false);
// Adds an actor of `class_id` at the viewport target (ground under the view center).
// Feeds the gizmo the selected record and applies finished drags. Call once per
// frame before the viewport draws.
void update_mission_gizmo(AppState& state);
// Moves a static map prop (scene instance) of the loaded visual map.
void set_map_instance_pose(AppState& state, const rws::SceneInstance& instance, csf::Vec3 position,
                           float yaw_radians);
// Yaw of a scene instance's forward (at) axis about world Y.
[[nodiscard]] float map_instance_yaw(const rws::SceneInstance& instance);
// The editor holds the visual map that is loaded (edits to it can be shown).
[[nodiscard]] bool map_instances_editable(const AppState& state);

} // namespace rwsman
