#pragma once

#include "app_state.hpp"

#include <optional>
#include <string>
#include <string_view>

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

// The mission's flow (events, objectives, findings), current with the editor.
[[nodiscard]] const csf::MissionFlow* mission_flow(AppState& state);
// Opens a mission ("mission") or cutscene ("cutscene") script in the Script workspace.
void open_flow_script(AppState& state, std::string_view program, std::int32_t id);
// An FLI string ID with its text when the authoring project has it.
[[nodiscard]] std::string text_label(const AppState& state, std::string_view id);
void create_objectives(AppState& state);
void create_equipment(AppState& state);
void create_tips(AppState& state);
// Adds, edits or removes a string of the authoring project (then rebuilds its text file).
void set_project_text(AppState& state, const std::string& id, const std::optional<std::string>& text);

// Cutscene shots: capture the viewport camera, look through a shot (t from 0
// at its start to 1 at its end), play all shots in the viewport (an
// approximation: the game's field of view and timing differ), and create the
// intro cutscene from them.
void capture_shot(AppState& state);
void view_shot(AppState& state, std::size_t index, float t);
void play_shots(AppState& state);
void stop_shots(AppState& state);
void update_shot_preview(AppState& state);  // once per frame
void create_intro(AppState& state);

// Places a building asset of the authoring project at the ground under the
// view centre (a project placement; the map rebuilds).
void place_building(AppState& state, const std::string& asset);

void add_preset_point(AppState& state);
void apply_preset(AppState& state);
void start_new_mission(AppState& state);

} // namespace rwsman
