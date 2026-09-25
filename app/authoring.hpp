#pragma once

#include "app_state.hpp"

#include <filesystem>

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
// Stores the resolved heights: placements in the project (then rebuilds the
// map), actors through the mission editor (one undo step each).
void resnap_authoring_heights(AppState& state);

} // namespace rwsman
