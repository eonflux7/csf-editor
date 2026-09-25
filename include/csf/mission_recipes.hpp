#pragma once

#include "csf/mission_edit.hpp"

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

EditResult add_guard_patrol(MissionEditor& editor, GuardPatrol recipe);
EditResult add_guard_idle(MissionEditor& editor, GuardIdle recipe);
EditResult add_animal_patrol(MissionEditor& editor, AnimalPatrol recipe);
EditResult add_cover_group(MissionEditor& editor, const CoverGroup& recipe, std::int32_t* new_id = nullptr);
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
