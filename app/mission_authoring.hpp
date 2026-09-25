#pragma once

#include "app_state.hpp"

#include <optional>

// Placing assets and applying behaviour presets in the open mission (editor
// plan stage 4). The UI (app/ui/mission_editor.cpp) calls these; every change
// goes through csf::MissionEditor and is one undo step.
namespace rwsman {

// Ground height of the open mission at (x, z): its collision map.
[[nodiscard]] std::optional<float> mission_ground(AppState& state, float x, float z);
// The ground under the viewport centre, or the orbit target.
[[nodiscard]] csf::Vec3 view_ground_point(AppState& state);

// Every class of every discovered mission, built once per resource root.
void build_asset_catalog(AppState& state);
// Imports the class when the mission lacks it, then places it at the view
// centre: characters on a new placement point, props and pickups without one.
void place_asset(AppState& state, const AuthoringTools::CatalogEntry& entry);

void add_preset_point(AppState& state);
void apply_preset(AppState& state);
void start_new_mission(AppState& state);

} // namespace rwsman
