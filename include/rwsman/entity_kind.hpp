#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace csf {
class MissionScene;
class ObjectDatabase;
struct MissionActor;
} // namespace csf

// What a mission record or project placement is to a mission author: the
// Outliner groups by it, and the UI gives each kind one colour and one icon
// (docs/plans/editor-ux-redesign.md, B2 and V1).
namespace rwsman {

enum class EntityKind : std::uint8_t {
    // Actors, by their class.
    player,
    enemy,
    animal,
    vehicle,
    pickup,
    usable,      // ghost objects the player uses (the radio)
    prop,
    helper,      // invisible actors: cutscene cameras and their targets
    unresolved,  // a class that is not in Objetos.bdd
    // Project placements.
    building,
    vegetation,
    // Navigation groups.
    route,
    cover,
    walk_grid,
    camera_path,
    // Other scene records.
    zone,
    marker,  // dummies
    light,
    effect,
    // Mission logic.
    objective,
    trigger,
    script,
    count
};

[[nodiscard]] const char* entity_kind_name(EntityKind kind);    // "Enemy"
[[nodiscard]] const char* entity_kind_plural(EntityKind kind);  // "Enemies"

// What the class database says about an actor's class.
struct ClassFacts {
    bool known{};                     // the class is in Objetos.bdd
    std::optional<std::string> type;  // .TIPO: ALEMAN, DECORATIVO, ITEM_ARMA, ...
    std::string name, model;          // .NOMBRE and the body model path
};

// `starts_as_player`: the scene makes this actor a starting commando.
[[nodiscard]] EntityKind classify_actor(const ClassFacts& facts, bool starts_as_player);
// The class facts of `class_id` in `objects` (unknown when either is missing).
[[nodiscard]] ClassFacts class_facts(const csf::ObjectDatabase* objects, std::optional<std::int32_t> class_id);
// An actor of `scene` by its class, the scene's starting player being a player.
[[nodiscard]] EntityKind classify_mission_actor(const csf::MissionScene& scene, const csf::ObjectDatabase* objects,
                                                const csf::MissionActor& actor);
// A navigation group by its .TIPO and name; `helper_path` when only helper
// actors (cutscene cameras) stand on it.
[[nodiscard]] EntityKind classify_nav_group(std::optional<std::int32_t> type, std::string_view name,
                                            bool helper_path);

} // namespace rwsman
