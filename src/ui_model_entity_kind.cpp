#include "rwsman/entity_kind.hpp"

#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace rwsman {
namespace {

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool contains_any(const std::string& text, std::initializer_list<std::string_view> needles) {
    return std::ranges::any_of(needles, [&](const std::string_view needle) {
        return text.find(needle) != std::string::npos;
    });
}

} // namespace

const char* entity_kind_name(const EntityKind kind) {
    switch (kind) {
    case EntityKind::player: return "Player";
    case EntityKind::enemy: return "Enemy";
    case EntityKind::animal: return "Animal";
    case EntityKind::vehicle: return "Vehicle";
    case EntityKind::pickup: return "Pickup";
    case EntityKind::usable: return "Usable object";
    case EntityKind::prop: return "Prop";
    case EntityKind::helper: return "Helper";
    case EntityKind::unresolved: return "Unknown class";
    case EntityKind::building: return "Building";
    case EntityKind::vegetation: return "Vegetation";
    case EntityKind::route: return "Route";
    case EntityKind::cover: return "Cover";
    case EntityKind::walk_grid: return "Walk grid";
    case EntityKind::camera_path: return "Camera path";
    case EntityKind::zone: return "Zone";
    case EntityKind::marker: return "Marker";
    case EntityKind::light: return "Light";
    case EntityKind::effect: return "Effect";
    case EntityKind::objective: return "Objective";
    case EntityKind::trigger: return "Trigger";
    case EntityKind::script: return "Script";
    case EntityKind::count: break;
    }
    return "";
}

const char* entity_kind_plural(const EntityKind kind) {
    switch (kind) {
    case EntityKind::player: return "Players";
    case EntityKind::enemy: return "Enemies";
    case EntityKind::animal: return "Animals";
    case EntityKind::vehicle: return "Vehicles";
    case EntityKind::pickup: return "Pickups";
    case EntityKind::usable: return "Usable objects";
    case EntityKind::prop: return "Props";
    case EntityKind::helper: return "Helpers";
    case EntityKind::unresolved: return "Unknown classes";
    case EntityKind::building: return "Buildings";
    case EntityKind::vegetation: return "Vegetation";
    case EntityKind::route: return "Routes";
    case EntityKind::cover: return "Cover";
    case EntityKind::walk_grid: return "Walk grids";
    case EntityKind::camera_path: return "Camera paths";
    case EntityKind::zone: return "Zones";
    case EntityKind::marker: return "Markers";
    case EntityKind::light: return "Lights";
    case EntityKind::effect: return "Effects";
    case EntityKind::objective: return "Objectives";
    case EntityKind::trigger: return "Triggers";
    case EntityKind::script: return "Scripts";
    case EntityKind::count: break;
    }
    return "";
}

EntityKind classify_actor(const ClassFacts& facts, const bool starts_as_player) {
    if (starts_as_player) return EntityKind::player;
    if (!facts.known) return EntityKind::unresolved;
    const auto type = lower(facts.type.value_or(""));
    const auto name = lower(facts.name), model = lower(facts.model);
    if (type == "player") return EntityKind::player;
    if (type == "aleman" || type == "ruso") return EntityKind::enemy;
    if (type == "ghost") return EntityKind::usable;
    if (type.starts_with("item_") || contains_any(type, {"cuchillo", "granada", "moneda", "bote_gas", "bote_humo"}))
        return EntityKind::pickup;
    if (contains_any(type, {"camion", "coche", "tanque", "sdk", "moto"}) || type == "bote") return EntityKind::vehicle;
    // Decorative classes cover animals and the invisible "Void" helpers too.
    if (type == "void" || name.starts_with("void")) return EntityKind::helper;
    if (contains_any(name, {"doberman", "perro", "dog", "caballo", "horse", "vaca", "cow", "gallina", "chicken"}) ||
        contains_any(model, {"doberman", "perro", "\\anim\\", "/anim/"}))
        return EntityKind::animal;
    return EntityKind::prop;
}

ClassFacts class_facts(const csf::ObjectDatabase* objects, const std::optional<std::int32_t> class_id) {
    ClassFacts facts;
    if (!objects || !class_id) return facts;
    const auto found = objects->find_class(*class_id);
    if (found.empty()) return facts;
    const auto& definition = *found.front();
    facts.known = true;
    facts.type = definition.type;
    facts.name = definition.name.value_or("");
    for (const auto& reference : definition.references)
        if (reference.kind == csf::ObjectReference::Kind::visual_model) {
            facts.model = reference.path;
            break;
        }
    return facts;
}

EntityKind classify_mission_actor(const csf::MissionScene& scene, const csf::ObjectDatabase* objects,
                                  const csf::MissionActor& actor) {
    return classify_actor(class_facts(objects, actor.class_id),
                          actor.id.has_value() && scene.player().active_player == actor.id);
}

EntityKind classify_nav_group(const std::optional<std::int32_t> type, const std::string_view name,
                              const bool helper_path) {
    if (helper_path) return EntityKind::camera_path;
    if (type == 3) return EntityKind::cover;
    if (lower(name) == "malla") return EntityKind::walk_grid;
    return EntityKind::route;
}

} // namespace rwsman
