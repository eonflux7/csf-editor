#include "csf/mission_recipes.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace csf {
namespace {

std::size_t mission_program(const MissionEditor& editor) {
    const auto file = editor.file_of_kind(MissionFileKind::mission_script);
    if (!file) throw std::invalid_argument("The mission has no mission program (.gsc)");
    return *file;
}

// Stops a batch at the first rejected operation; its result is the batch's.
struct Steps {
    EditResult last{true, {}, {}};
    std::vector<std::string> warnings;
    bool ok(EditResult result) {
        warnings.insert(warnings.end(), result.warnings.begin(), result.warnings.end());
        last = std::move(result);
        return last.applied;
    }
    EditResult finish(std::string message) {
        return {true, std::move(message), std::move(warnings)};
    }
};

std::vector<std::pair<std::int32_t, std::int32_t>> route_links(const RouteSpec& route) {
    std::vector<std::pair<std::int32_t, std::int32_t>> links;
    const auto n = static_cast<std::int32_t>(route.points.size());
    for (std::int32_t k = 1; k < n; ++k) links.emplace_back(k, k + 1);
    if (route.loop && n > 2) links.emplace_back(n, 1);
    return links;
}

std::vector<std::string> cover_lines(const std::optional<std::int32_t> group) {
    if (!group) return {};
    return {"SELECT_GRUPO_PARAPETO (THIS) (GRUPO_PATHPOINT " + std::to_string(*group) + ")",
            "SET_IA_ALERTA (THIS) (IA_ALERTA MOVIL_A_PARAPETO)",
            "SET_IA_COMBATE (THIS) (IA_COMBATE MOVIL_A_PARAPETO)"};
}

std::int32_t script_id(const MissionEditor& editor, const ScriptSpec& spec) {
    return spec.id ? *spec.id : editor.next_script_id(mission_program(editor));
}

// Adds the script, then the actor that runs it (and whatever `before` adds
// first), as one undo step.
EditResult actor_with_script(MissionEditor& editor, std::string label, ActorSpec actor, const std::string& text,
                             const std::function<bool(Steps&)>& before) {
    const auto program = mission_program(editor);
    return editor.batch(std::move(label), [&] {
        Steps steps;
        if (before && !before(steps)) return steps.last;
        std::int32_t added{};
        if (!steps.ok(editor.add_script(program, text, &added))) return steps.last;
        actor.scripts.push_back(added);
        std::int32_t actor_id{};
        if (!steps.ok(editor.add_actor_record(actor, &actor_id))) return steps.last;
        return steps.finish("Added " + actor.name + " (" + std::to_string(actor_id) + ") with script " +
                            std::to_string(added));
    });
}

} // namespace

std::string script_number(const float value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    std::string text(buffer.data(), end);
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

std::string script_text(const std::int32_t id, const std::string_view name, const std::int32_t trigger,
                        const std::vector<std::string>& events, const std::vector<std::string>& actions,
                        const std::vector<std::string>& conditions) {
    std::string out = "[\n  .ID " + std::to_string(id) + "\n  .NOMBRE " + std::string(name) +
                      "\n  .CARPETA \"\"\n  .FLAGS [\n    .TRIGGER " + std::to_string(trigger) +
                      "\n    .ENABLED 1\n    .VALIDO 1\n  ]\n";
    if (!events.empty()) {
        out += "  .EVENTOS (\n";
        for (const auto& event : events) out += "    (" + event + ")\n";
        out += "  )\n";
    }
    if (!conditions.empty()) {
        out += "  .CONDICIONES {\n";
        for (const auto& condition : conditions) out += "    " + condition + "\n";
        out += "  }\n";
    }
    out += "  .ACCIONES {\n";
    for (const auto& action : actions) out += "    " + action + "\n";
    return out + "  }\n]\n";
}

EditResult add_guard_patrol(MissionEditor& editor, GuardPatrol recipe) {
    if (recipe.route.points.empty()) return {false, "A patrol route needs points", {}};
    const auto id = script_id(editor, recipe.script);
    auto actions = cover_lines(recipe.cover_group);
    actions.emplace_back("WHILE (BOOL TRUE)");
    std::int32_t route = recipe.route.id.value_or(0);
    const auto run = [&](Steps& steps) {
        if (!steps.ok(editor.add_navigation_group(recipe.route.name, 0, recipe.route.points, route_links(recipe.route),
                                                  recipe.route.id, &route)))
            return false;
        return true;
    };
    // The route ID is known only once the group exists, so write the script
    // after it: build the text lazily inside the batch.
    const auto program = mission_program(editor);
    return editor.batch("Add patrol " + recipe.actor.name, [&] {
        Steps steps;
        if (!run(steps)) return steps.last;
        for (std::size_t k = 0; k < recipe.route.points.size(); ++k) {
            actions.push_back("  IR_A_PATHPOINT (THIS) (PATHPOINT " + std::to_string(route) + " " + std::to_string(k + 1) + ")");
            actions.push_back("  PAUSE (NUMERO " + script_number(recipe.pause_seconds) + ")");
        }
        actions.emplace_back("WEND");
        std::int32_t added{};
        if (!steps.ok(editor.add_script(program, script_text(id, recipe.script.name, 0, {"INIT"}, actions), &added)))
            return steps.last;
        auto actor = recipe.actor;
        actor.placement.position = recipe.route.points.front().position;
        actor.cell = std::pair{route, 1};
        actor.scripts.push_back(added);
        std::int32_t actor_id{};
        if (!steps.ok(editor.add_actor_record(actor, &actor_id))) return steps.last;
        return steps.finish("Added patrol " + actor.name + " (" + std::to_string(actor_id) + ") on route " +
                            std::to_string(route));
    });
}

EditResult add_guard_idle(MissionEditor& editor, GuardIdle recipe) {
    if (recipe.loop.empty()) return {false, "An idle loop needs an animation", {}};
    const auto id = script_id(editor, recipe.script);
    auto actions = cover_lines(recipe.cover_group);
    actions.emplace_back("WHILE (BOOL TRUE)");
    for (const auto& step : recipe.loop)
        actions.push_back(step.random_cycles
                              ? "  PLAY_ANMBDD_CICLOS (THIS) (ANM_BDD " + std::to_string(step.animation) +
                                    ") (RANDOM (NUMERO " + script_number(step.random_cycles->first) + ") (NUMERO " +
                                    script_number(step.random_cycles->second) + "))"
                              : "  PLAY_ANMBDD (THIS) (ANM_BDD " + std::to_string(step.animation) + ")");
    actions.emplace_back("WEND");
    return actor_with_script(editor, "Add " + recipe.actor.name, recipe.actor,
                             script_text(id, recipe.script.name, 0, {"INIT"}, actions), {});
}

EditResult add_animal_patrol(MissionEditor& editor, AnimalPatrol recipe) {
    if (recipe.route.points.empty()) return {false, "A route needs points", {}};
    const auto id = script_id(editor, recipe.script);
    const auto program = mission_program(editor);
    return editor.batch("Add " + recipe.actor.name, [&] {
        Steps steps;
        std::int32_t route{};
        if (!steps.ok(editor.add_navigation_group(recipe.route.name, 0, recipe.route.points, route_links(recipe.route),
                                                  recipe.route.id, &route)))
            return steps.last;
        std::vector<std::string> actions{"WHILE (BOOL TRUE)"};
        for (std::size_t k = 0; k < recipe.route.points.size(); ++k)
            actions.push_back("  IR_A_PATHPOINT_ANIM (THIS) (PATHPOINT " + std::to_string(route) + " " +
                              std::to_string(k + 1) + ") (BOOL TRUE) (ANM_BDD " +
                              std::to_string(recipe.walk_animation) + ")");
        actions.emplace_back("WEND");
        std::int32_t added{};
        if (!steps.ok(editor.add_script(program, script_text(id, recipe.script.name, 0, {"INIT"}, actions), &added)))
            return steps.last;
        auto actor = recipe.actor;
        actor.scripts.push_back(added);
        std::int32_t actor_id{};
        if (!steps.ok(editor.add_actor_record(actor, &actor_id))) return steps.last;
        return steps.finish("Added " + actor.name + " (" + std::to_string(actor_id) + ") walking route " +
                            std::to_string(route));
    });
}

EditResult add_cover_group(MissionEditor& editor, const CoverGroup& recipe, std::int32_t* new_id) {
    if (recipe.points.empty()) return {false, "A cover group needs points", {}};
    return editor.add_navigation_group(recipe.name, 3, recipe.points, {}, recipe.id, new_id);
}

EditResult add_walk_grid(MissionEditor& editor, const WalkGrid& recipe, std::int32_t* new_id) {
    if (!(recipe.spacing > 0) || recipe.max_x < recipe.min_x || recipe.max_z < recipe.min_z || !recipe.ground)
        return {false, "The walk grid needs a positive spacing, a range and ground", {}};
    const auto columns = static_cast<std::int32_t>(std::floor((recipe.max_x - recipe.min_x) / recipe.spacing)) + 1;
    const auto rows = static_cast<std::int32_t>(std::floor((recipe.max_z - recipe.min_z) / recipe.spacing)) + 1;
    std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> index;
    std::vector<NavPointSpec> points;
    for (std::int32_t row = 0; row < rows; ++row)
        for (std::int32_t column = 0; column < columns; ++column) {
            const float x = recipe.min_x + static_cast<float>(column) * recipe.spacing +
                            (recipe.stagger && row % 2 == 0 ? recipe.spacing / 2 : 0.0F);
            const float z = recipe.min_z + static_cast<float>(row) * recipe.spacing;
            const bool boxed = std::ranges::any_of(recipe.excluded_boxes, [&](const auto& b) {
                return x >= b[0] && x <= b[2] && z >= b[1] && z <= b[3];
            });
            const bool near = std::ranges::any_of(recipe.avoided_circles, [&](const auto& c) {
                return std::hypot(x - c[0], z - c[1]) < c[2];
            });
            if (boxed || near) continue;
            const auto height = recipe.ground(x, z);
            if (!height) continue;
            index[{column, row}] = static_cast<std::int32_t>(points.size() + 1);
            points.push_back({{x, *height, z}, 0, 0});
        }
    std::vector<std::pair<std::int32_t, std::int32_t>> links;
    for (std::int32_t row = 0; row < rows; ++row)
        for (std::int32_t column = 0; column < columns; ++column) {
            const auto a = index.find({column, row});
            if (a == index.end()) continue;
            for (const auto& [dc, dr] : {std::pair{1, 0}, std::pair{0, 1}})
                if (const auto b = index.find({column + dc, row + dr}); b != index.end())
                    links.emplace_back(a->second, b->second);
        }
    if (points.empty()) return {false, "No walk grid point has ground under it", {}};
    return editor.add_navigation_group(recipe.name, 0, points, links, recipe.id, new_id);
}

EditResult link_to_nearest(MissionEditor& editor, const std::int32_t group, const std::int32_t target) {
    const auto& scene = editor.scene();
    const NavGroup* from{};
    const NavGroup* to{};
    for (const auto& value : scene.navigation()) {
        if (value.id == group) from = &value;
        if (value.id == target) to = &value;
    }
    if (!from || !to) return {false, "Both navigation groups must exist", {}};
    std::vector<std::array<std::int32_t, 2>> pairs;
    for (const auto& point : from->points) {
        if (!point.id || !point.position) continue;
        const NavPoint* best{};
        float best_distance = std::numeric_limits<float>::max();
        for (const auto& candidate : to->points) {
            if (!candidate.id || !candidate.position) continue;
            const auto d = std::hypot(candidate.position->x - point.position->x, candidate.position->z - point.position->z);
            if (d < best_distance) {
                best_distance = d;
                best = &candidate;
            }
        }
        if (best) pairs.push_back({*point.id, *best->id});
    }
    return editor.batch("Link group " + std::to_string(group) + " to " + std::to_string(target), [&] {
        Steps steps;
        for (const auto& [a, b] : pairs)
            if (!steps.ok(editor.connect_navigation_points(group, a, target, b))) return steps.last;
        return steps.finish("Linked " + std::to_string(pairs.size()) + " points of group " + std::to_string(group) +
                            " to group " + std::to_string(target));
    });
}

} // namespace csf
