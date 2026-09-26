#pragma once

#include "csf/mission_edit.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Behaviour presets: the patterns hello world proved in-game, each applied to
// a MissionEditor as one undo step (docs/plans/editor-blender-authoring.md,
// stage 4). Positions are game units; heights are the caller's (the editor
// snaps to the ground before calling). Presets never guess at arbitrary
// script control flow: they write whole scripts of known shape.
namespace csf {

struct ScriptSpec {
    std::optional<std::int32_t> id;  // the next free script ID when empty
    std::string name;                // UTF-8
};

struct RouteSpec {
    std::optional<std::int32_t> id;
    std::string name;
    std::vector<NavPointSpec> points;
    bool loop{true};  // links the last point back to the first
};

// A soldier walking a route, pausing at each point. With a cover group it
// takes cover there when alerted and in combat (SELECT_GRUPO_PARAPETO and
// MOVIL_A_PARAPETO for both modes, as Escape's INIT scripts do). The actor
// stands on the route's first point.
struct GuardPatrol {
    ActorSpec actor;
    RouteSpec route;
    float pause_seconds{3.0F};
    std::optional<std::int32_t> cover_group;
    ScriptSpec script;
};

// One animation of an idle loop; with `random_cycles` it plays a random number
// of times in that range (PLAY_ANMBDD_CICLOS ... RANDOM).
struct IdleStep {
    std::int32_t animation{};
    std::optional<std::pair<float, float>> random_cycles;
};

// A soldier standing still, playing animations in a loop (smoking, the radio).
struct GuardIdle {
    ActorSpec actor;
    std::vector<IdleStep> loop;
    std::optional<std::int32_t> cover_group;
    ScriptSpec script;
};

// An animal walking a route with its walk animation (IR_A_PATHPOINT_ANIM).
struct AnimalPatrol {
    ActorSpec actor;
    RouteSpec route;
    std::int32_t walk_animation{};
    ScriptSpec script;
};

// Cover points (.TIPO 3), each facing where the cover faces.
struct CoverGroup {
    std::optional<std::int32_t> id;
    std::string name;
    std::vector<NavPointSpec> points;
};

// A walking grid over the map (.TIPO 0): points `spacing` apart, with every
// other row shifted half a step when `stagger`, left out inside the excluded
// boxes (x0 z0 x1 z1, inclusive) and closer than each circle's radius to its
// centre (x z r); linked to the right and upper neighbours. `ground` gives the
// height at (x, z), or nothing where there is no ground (the point is left out).
struct WalkGrid {
    std::optional<std::int32_t> id;
    std::string name{"MALLA"};
    float spacing{1000.0F};
    float min_x{}, max_x{}, min_z{}, max_z{};
    bool stagger{true};
    std::vector<std::array<float, 4>> excluded_boxes;
    std::vector<std::array<float, 3>> avoided_circles;
    std::function<std::optional<float>(float, float)> ground;
};

// Objectives (hello world's proven pattern): one START_GAME script sets up
// every objective (SET_OBJETIVO, its label, zone events, usable ghosts), and
// one script per objective completes it and shows its message. Completing a
// primary objective checks whether all primaries are done and ends the
// mission in success. Text is FLI string IDs ("0900" or g014).
struct Objective {
    enum class Kind : std::uint8_t { enter_zone, kill_actor, use_object };
    std::int32_t number{};
    bool secondary{};
    Kind kind{Kind::enter_zone};
    std::int32_t target{};  // zone (area) ID, or actor ID
    std::string label;      // the objective's text
    std::string done;       // shown when completed
    std::string prompt;     // use_object: the context label on the object
    ScriptSpec script;
};

struct Objectives {
    std::vector<Objective> objectives;
    ScriptSpec setup;
    std::string success_message{"g014"};
    float success_pause{4.0F};
};

// Starting weapons (and disguise) of the player characters, on START_GAME.
struct Kit {
    std::int32_t actor{};
    struct Weapon {
        std::int32_t weapon_class{};
        std::optional<std::pair<float, float>> ammunition;  // SET_MUNICION_ARMA
    };
    std::vector<Weapon> weapons;
    std::optional<std::int32_t> selected;  // SELECT_ARMA
    std::optional<std::int32_t> disguise;  // DISFRAZAR as this class
};

struct Equipment {
    std::vector<Kit> kits;
    ScriptSpec script;
};

// Mission tips (the in-game help list), on START_GAME.
struct Tips {
    std::vector<std::string> tips;  // FLI string IDs
    std::pair<float, float> position{0.115F, 0.25F};  // TIMED_STRING_INITPOS
    ScriptSpec script;
};

// An intro cutscene of travelling camera shots (KB-scripting-44, the Ambush
// pattern proven by hello world v13): per shot an invisible camera actor that
// travels a two-point path at constant speed, an invisible target actor it
// keeps aimed at, and a camera dummy registered as their viewpoint. The mission
// program's START_GAME script fades, disables control, raises INIT (actor
// scripts start on it) and runs the cutscene program's camera script, which
// cuts between the shots. Positions are absolute game coordinates.
struct CameraShot {
    Vec3 camera, camera_end, target;
    float seconds{4.0F};
    // Exact values when given (hello world's recipe rounds them to 6 decimals);
    // otherwise computed from the positions.
    std::optional<std::pair<float, float>> aim;  // the dummy's .ROT and .ROT_X, radians
    std::optional<float> heading;                // the path points' .ROT, radians
    std::optional<float> speed;                  // SET_WANTED_VEL, cm/s
};

struct IntroCutscene {
    std::vector<CameraShot> shots;
    std::int32_t camera_class{197};  // invisible, physics-free (Ambush); import it when missing
    bool send_init{true};
    // First IDs; each shot takes the next dummy and group and two actors.
    std::optional<std::int32_t> first_dummy, first_actor, first_group;
    ScriptSpec intro{std::nullopt, "CUTSCENE_INICIO"};
    // Cutscene program scripts: the cutscene, its INIT and END, the camera.
    std::array<std::optional<std::int32_t>, 4> cutscene_ids{};
    std::string cutscene_name{"CUT_INICIO"};
};

// A trigger (docs/plans/editor-ux-redesign.md, E10): When something happens,
// If an objective is or is not complete, Do some actions; one trigger script
// of known shape (with a START_GAME setup script when the event needs one:
// zone events for the player, a usable ghost). Only patterns seen working are
// "proven": hello world's (in game) or the shipped programs' and the KB's;
// the others carry trigger_*_proven() == false and the GUI shows them only
// on request, marked unverified.
struct TriggerAction {
    enum class Kind : std::uint8_t {
        complete_objective,  // SET_OBJETIVO_SUCCESS (NUMERO n) (BOOL TRUE)
        message,             // TIMED_STRING_V2 (FLI text) 5 s
        raise_event,         // SEND_EVENT (EVENT name)
        alarm,               // Convoy's camp alarm: an acoustic stimulus and ACTIVAR_ALARMA (NUMERO seconds)
        ai_alert,            // SET_IA_ALERTA (BICHO actor) (IA_ALERTA mode)
        ai_combat,           // SET_IA_COMBATE (BICHO actor) (IA_COMBATE mode)
        enable_ghost,        // HABILITAR_GHOST, SET_CONTEXTUAL, ENABLE_GHOST_ILUM (BOOL TRUE)
        disable_ghost,       // ... (BOOL FALSE)
        mission_success,     // SET_MISSION_SUCCESS (BOOL TRUE)
    };
    Kind kind{Kind::message};
    std::int32_t number{};  // objective number, actor ID or seconds
    std::string text;       // FLI string ID, event name or AI mode
};

struct Trigger {
    enum class When : std::uint8_t {
        mission_start,  // START_GAME
        enter_zone,     // the player enters `target` (a zone)
        actor_killed,   // actor `target` dies (MORIBUNDO or MUERTO)
        object_used,    // the player uses actor `target`'s ghost
        event,          // `event` is raised (SEND_EVENT)
        timer,          // `seconds` after the mission starts
    };
    When when{When::mission_start};
    std::int32_t target{};
    std::string event;
    float seconds{};
    // Only while objective `first` is complete (`second` true) or not.
    std::optional<std::pair<std::int32_t, bool>> if_objective;
    std::vector<TriggerAction> actions;
    ScriptSpec script;  // the trigger
    ScriptSpec setup;   // its START_GAME setup, when the event needs one
};

[[nodiscard]] bool trigger_when_proven(Trigger::When when) noexcept;
[[nodiscard]] bool trigger_action_proven(TriggerAction::Kind kind) noexcept;
[[nodiscard]] bool trigger_needs_setup(const Trigger& trigger) noexcept;
// The trigger's script texts (the setup first, when it has one) with the IDs given.
[[nodiscard]] std::vector<std::string> trigger_script_texts(const Trigger& trigger, std::int32_t script_id,
                                                            std::int32_t setup_id);
EditResult add_trigger(MissionEditor& editor, const Trigger& recipe);

// The dummy aim (.ROT, .ROT_X) looking from `camera` at `target`.
[[nodiscard]] std::pair<float, float> look_at(Vec3 camera, Vec3 target);

EditResult add_intro_cutscene(MissionEditor& editor, const IntroCutscene& recipe);
EditResult add_objectives(MissionEditor& editor, const Objectives& recipe);
EditResult add_equipment(MissionEditor& editor, const Equipment& recipe);
EditResult add_tips(MissionEditor& editor, const Tips& recipe);
// An FLI operand as script source writes it: numeric IDs are quoted.
[[nodiscard]] std::string fli_operand(std::string_view id);

EditResult add_guard_patrol(MissionEditor& editor, GuardPatrol recipe);
EditResult add_guard_idle(MissionEditor& editor, GuardIdle recipe);
EditResult add_animal_patrol(MissionEditor& editor, AnimalPatrol recipe);
EditResult add_cover_group(MissionEditor& editor, const CoverGroup& recipe, std::int32_t* new_id = nullptr);
// The walk grid's points (IDs 1..n in order) and links, as add_walk_grid makes them (and a preview shows them).
struct WalkGridLayout {
    std::vector<NavPointSpec> points;
    std::vector<std::pair<std::int32_t, std::int32_t>> links;
};
[[nodiscard]] WalkGridLayout walk_grid_layout(const WalkGrid& recipe);
EditResult add_walk_grid(MissionEditor& editor, const WalkGrid& recipe, std::int32_t* new_id = nullptr);
// Links every point of `group` to the nearest point of `target` (in XZ),
// as routes join the walking grid.
EditResult link_to_nearest(MissionEditor& editor, std::int32_t group, std::int32_t target);

// Script source text (.gsc record) in the layout hello world's scene.py writes.
[[nodiscard]] std::string script_text(std::int32_t id, std::string_view name, std::int32_t trigger,
                                      const std::vector<std::string>& events, const std::vector<std::string>& actions,
                                      const std::vector<std::string>& conditions = {});
// A real as script source writes it (always with a decimal point).
[[nodiscard]] std::string script_number(float value);

} // namespace csf
