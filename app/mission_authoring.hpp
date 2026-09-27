#pragma once

#include "app_state.hpp"

#include "csf/mission_components.hpp"
#include "rwsman/cutscene_timeline.hpp"

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
// The words of an FLI string ID when the authoring project has it, else the ID.
[[nodiscard]] std::string text_label(const AppState& state, std::string_view id);
// Objectives, starting kits and tips (Phase 7): one component each per
// mission, grown in place. Adding an objective appends it to the mission's
// objectives (made when there are none) with placeholder texts, aimed at the
// first zone or else the first enemy; a kit gives a player a pistol; a tip
// gets placeholder text. Each is one undo step (plus the project's texts).
[[nodiscard]] const csf::MissionComponent* objectives_component(AppState& state);
[[nodiscard]] const csf::MissionComponent* equipment_component(AppState& state);
[[nodiscard]] const csf::MissionComponent* tips_component(AppState& state);
void add_objective(AppState& state);
void add_kit(AppState& state, std::int32_t actor);
void add_tip(AppState& state);
// The first sensible target for an objective of `kind` ("zone", "kill", "use").
[[nodiscard]] std::optional<std::int32_t> default_objective_target(AppState& state, std::string_view kind);
// A looping idle animation for a guard of `class_id`.
[[nodiscard]] std::int32_t default_idle_animation(AppState& state, std::optional<std::int32_t> class_id);
// Gives an actor no component made a behaviour: the recipe makes it again in
// its place, keeping its ID, name, class, position and heading, and owns it
// from then on (its other scripts and animation overrides are dropped). One
// undo step.
enum class BehaviourKind : std::uint8_t { idle, patrol, animal };
void give_behaviour(AppState& state, std::int32_t actor_id, BehaviourKind kind);
// The FLI ID of a game text: with an authoring project `text` is the string
// (found, or given the next free ID in the project's range, one project undo
// step); without one it is the ID itself. Nothing when the range is full.
[[nodiscard]] std::optional<std::string> game_text_id(AppState& state, const std::string& text);
// Makes the New trigger form's trigger a component (its message texts become IDs).
void create_trigger(AppState& state);
// Adds, edits or removes a string of the authoring project (then rebuilds its text file).
void set_project_text(AppState& state, const std::string& id, const std::optional<std::string>& text);

// The intro cutscene's timeline (E11): its shots are the intro component's
// shot lines, and every edit regenerates the component as one undo step.
// Capturing the view appends a shot (the first one creates the intro, importing
// Ambush's invisible camera actor when the mission lacks it). The playhead
// moves the viewport camera through the shots, an approximation: the game's
// field of view differs.
[[nodiscard]] const csf::MissionComponent* intro_component(AppState& state);
// Every cutscene component: the ones at the start first, then zone cutscenes.
[[nodiscard]] std::vector<const csf::MissionComponent*> cutscene_components(AppState& state);
// When a cutscene plays: at the start, or once when the player enters `zone`,
// armed at the start or when `arm` is raised.
struct CutsceneWhen {
    std::optional<std::int32_t> zone;
    std::string arm;
    std::string name;  // the cutscene's name (CUT_INICIO)
};
[[nodiscard]] CutsceneWhen cutscene_when(const csf::MissionComponent& component);
// "Intro (at the start)", "CUT_OFICIAL: entering Village (zone 6)".
[[nodiscard]] std::string cutscene_title(AppState& state, const csf::MissionComponent& component);
// Changes the timeline's cutscene's When (one undo step).
void set_cutscene_when(AppState& state, const CutsceneWhen& when);
// A new cutscene played when the player enters a zone, its first shot from
// the view; the timeline then edits it.
void add_zone_cutscene(AppState& state);
[[nodiscard]] std::vector<TimelineShot> intro_shots(AppState& state);
bool edit_intro_shots(AppState& state, const std::vector<TimelineShot>& shots);
// Whether the intro raises INIT (the actors' scripts start on it).
bool intro_sends_init(AppState& state);
void set_intro_sends_init(AppState& state, bool send);
void capture_shot(AppState& state);
// A shot's camera or target from the viewport camera.
void recapture_shot(AppState& state, std::size_t index, ShotPart part);
void scrub_timeline(AppState& state, float seconds);  // moves the playhead and the view
void view_shot(AppState& state, std::size_t index, float t);
void play_shots(AppState& state);
void stop_shots(AppState& state);
void update_shot_preview(AppState& state);  // once per frame

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
