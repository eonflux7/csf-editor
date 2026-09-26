#pragma once

#include "app_state.hpp"

#include "csf/mission_components.hpp"

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
// Imports the class when the mission lacks it, then places it at `at` (the
// ground under the view centre by default), facing `heading` degrees:
// characters on a new placement point, props and pickups without one.
void place_asset(AppState& state, const AuthoringTools::CatalogEntry& entry,
                 std::optional<csf::Vec3> at = std::nullopt, float heading = 0.0F);

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

// Places a building asset of the authoring project on the ground at `at`
// (under the view centre by default), turned `heading` degrees (a project
// placement; the map rebuilds).
void place_building(AppState& state, const std::string& asset, std::optional<csf::Vec3> at = std::nullopt,
                    float heading = 0.0F);

// Components (csf/mission_components.hpp): the options their lines run
// with (the collision map's ground), and a recipe's lines added as one; the
// records it made, or nothing when it was refused (the reason is reported).
[[nodiscard]] csf::MissionOpsOptions component_options(AppState& state);
std::optional<std::vector<csf::MissionRecordId>> add_recipe(AppState& state, const std::string& lines);

// The mission's components (parsed once per editor revision) and the one
// that made a record: an actor, a navigation group or point, a dummy, an area.
[[nodiscard]] const std::vector<csf::MissionComponent>& mission_component_list(AppState& state);
[[nodiscard]] const csf::MissionComponent* owning_component(AppState& state, const MissionRecordKey& key);
// Replaces a component's lines and regenerates its records; one undo step.
bool edit_component(AppState& state, std::int32_t id, const std::vector<std::string>& lines);
// A move (heading in radians) or deletion of a record a component made, done
// as an edit of its lines (an actor's pos=, a route or cover point in
// points=) or, for a deletion, of the whole component; nothing when no
// component made it.
std::optional<csf::EditResult> move_component_record(AppState& state, const MissionRecordKey& key, csf::Vec3 position,
                                                     float heading_radians);
std::optional<csf::EditResult> delete_component_record(AppState& state, const MissionRecordKey& key);

// The walk grid the preset makes (over the collision map, clear of props),
// and its points and links for the viewport preview (kept current).
[[nodiscard]] std::optional<csf::WalkGrid> preset_walk_grid(AppState& state);
void update_walk_grid_preview(AppState& state);

void add_preset_point(AppState& state);
void apply_preset(AppState& state);
void start_new_mission(AppState& state);

} // namespace rwsman
