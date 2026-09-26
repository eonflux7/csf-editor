#pragma once

#include "app_state.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Authoring projects in the GUI (docs/plans/editor-project-format.md): the map
// of an open project is rebuilt in the background when a Blender export
// changes, the mission reloads with it, and the height report lists the
// placements and anchored actors that no longer stand where their rules say.
namespace rwsman {

// Remembers the authoring project in `folder` (or forgets it when the folder
// has no project.csfproj) and checks it once in the background.
void set_authoring_project(AppState& state, const std::filesystem::path& folder);
// Once per frame: notices changed exports and applies finished background work.
void poll_authoring(AppState& state);
[[nodiscard]] bool authoring_busy(const AppState& state);     // a background job runs
[[nodiscard]] bool authoring_pending(const AppState& state);  // ... or one is queued
[[nodiscard]] bool authoring_open(const AppState& state);
// Rebuilds the map when stale (or always, with `force`), then reports heights.
void rebuild_authoring_map(AppState& state, bool force);
// Saves the open authoring project (without reloading it as an outside change).
void save_authoring_project(AppState& state);
// The project has edits that are not saved; with the mission's, whether
// anything is (the title bar, the status bar and closing ask about it).
[[nodiscard]] bool authoring_dirty(const AppState& state);
// Writes local.csfproj (machine settings: test install, originals, deployments) now.
void save_project_local(AppState& state);
[[nodiscard]] bool edits_unsaved(const AppState& state);
// Stores the resolved heights: placements in the project (one project undo
// step; then rebuilds the map), actors through the mission editor.
void resnap_authoring_heights(AppState& state);

// Changes the authoring project as one undo step: `change` edits a copy; when
// it returns true and the result passes check(), the copy replaces the
// project (saved with the mission) and, with `rebuild`, the map is rebuilt
// from it. Returns whether it applied.
bool edit_authoring_project(AppState& state, std::string label,
                            const std::function<bool(csf::AuthoringProject&)>& change, bool rebuild = true);
// Undo and redo over both histories: the mission editor's and the project's.
[[nodiscard]] bool can_undo_edit(const AppState& state);
[[nodiscard]] bool can_redo_edit(const AppState& state);
void undo_edit(AppState& state);
void redo_edit(AppState& state);
// A toast for a destructive edit that was just applied, with an Undo button
// (E13: undo instead of a confirmation dialog).
void notify_undoable(AppState& state, std::string message);
[[nodiscard]] std::pair<std::size_t, std::size_t> history_point(const AppState& state);
// The labels of the combined history, oldest first, and how many are applied.
[[nodiscard]] std::pair<std::vector<std::string>, std::size_t> edit_history(const AppState& state);

} // namespace rwsman
