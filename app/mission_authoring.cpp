#include "mission_authoring.hpp"

#include "app_util.hpp"
#include "mission_editing.hpp"

#include "csf/mission_recipes.hpp"
#include "rws/world_model.hpp"

#include <charconv>
#include <cstring>
#include <set>

namespace rwsman {
namespace {

std::string text_of(const std::span<const char> buffer) { return {buffer.data(), strnlen(buffer.data(), buffer.size())}; }

const rws::GroundQuery* ground_query(AppState& state) {
    auto& tools = state.tools;
    const auto* collision = state.collision_document.get();
    if (tools.ground_source == collision && tools.ground) return tools.ground.get();
    tools.ground.reset();
    tools.ground_source = collision;
    if (!collision) return nullptr;
    const auto offset = rws::find_map_world(collision->bytes());
    if (!offset) return nullptr;
    const auto world = rws::parse_world_model(collision->bytes(), *offset);
    if (!world) return nullptr;
    tools.ground = std::make_shared<const rws::GroundQuery>(rws::GroundQuery::from_world(*world.value));
    return tools.ground.get();
}

// Characters stand on placement points; decoration, pickups and ghosts do not
// (hello world's prop actors, KB-scn-12).
bool gets_placement_point(const std::string& type) {
    return !(type == "DECORATIVO" || type.starts_with("ITEM") || type.find("GHOST") != std::string::npos);
}

std::optional<std::int32_t> parse_int(const std::string_view text) {
    std::int32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<float> parse_float(const std::string_view text) {
    float value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

} // namespace

std::optional<float> mission_ground(AppState& state, const float x, const float z) {
    if (const auto* ground = ground_query(state))
        if (const auto hit = ground->highest(x, z)) return hit->height;
    return std::nullopt;
}

csf::Vec3 view_ground_point(AppState& state) {
    const auto target = state.preview.view_target();
    return {target.x, target.y, target.z};
}

void build_asset_catalog(AppState& state) {
    auto& tools = state.tools;
    tools.catalog.clear();
    tools.catalog_root = state.settings.resource_root;
    std::set<std::string> packages;
    for (const auto& mission : state.discovered) {
        if (!packages.insert(mission.package).second) continue;
        const auto root = state.settings.resource_root / mission.package;
        try {
            csf::ResourceIndex index;
            index.add_root(root);
            index.build();
            const auto found = index.resolve("BDD/Objetos.bdd");
            if (found.candidate_indices.size() != 1) continue;
            const auto objects = csf::ObjectDatabase::project(
                csf::Document::load(index.resources()[found.candidate_indices.front()].path));
            for (const auto& definition : objects.definitions())
                if (definition.class_id)
                    tools.catalog.push_back({root, mission.package, *definition.class_id,
                                             definition.name.value_or(""), definition.type.value_or("")});
        } catch (const std::exception& error) {
            state.warn("Asset catalogue skipped " + mission.package + ": " + error.what());
        }
    }
    state.info("Asset catalogue: " + std::to_string(tools.catalog.size()) + " classes from " +
               std::to_string(packages.size()) + " missions");
}

void place_asset(AppState& state, const AuthoringTools::CatalogEntry& entry) {
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    const auto position = view_ground_point(state);
    const auto present = editor.objects().find_class(entry.class_id);
    if (!present.empty() && present.front()->name.value_or("") != entry.name) {
        state.notify(LogLevel::error, "This mission's class " + std::to_string(entry.class_id) + " is \"" +
                                          present.front()->name.value_or("") + "\", not \"" + entry.name +
                                          "\"; the class IDs of the two missions collide");
        return;
    }
    std::int32_t id{};
    const auto result = editor.batch("Place " + entry.name, [&] {
        if (present.empty())
            if (auto imported = editor.import_class(entry.package, entry.class_id); !imported.applied) return imported;
        if (gets_placement_point(entry.type))
            return editor.add_actor(entry.class_id, {position, 0, 0}, "", std::nullopt, &id);
        csf::ActorSpec spec;
        spec.class_id = entry.class_id;
        spec.placement = {position, 0, 0};
        return editor.add_actor_record(spec, &id);
    });
    if (apply_mission_edit(state, result)) select_after_refresh(state, {MissionRecordKey::Kind::actor, id, 0});
}

void add_preset_point(AppState& state) { state.tools.points.push_back(view_ground_point(state)); }

void apply_preset(AppState& state) {
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    auto& tools = state.tools;
    using Preset = AuthoringTools::Preset;
    const auto name = text_of(tools.name), script_name = text_of(tools.script_name), route_name = text_of(tools.route_name);
    const std::optional<std::int32_t> cover = tools.cover_group > 0 ? std::optional(tools.cover_group) : std::nullopt;
    const auto route_points = [&] {
        std::vector<csf::NavPointSpec> points;
        for (const auto& point : tools.points) points.push_back({point, 0, 0});
        return points;
    };
    const auto actor = [&](const csf::Vec3 position) {
        csf::ActorSpec spec;
        spec.name = name;
        spec.class_id = tools.class_id;
        spec.placement = {position, tools.heading, 0};
        return spec;
    };
    csf::EditResult result;
    switch (tools.preset) {
    case Preset::guard_patrol: {
        if (tools.points.empty()) return state.warn("Add the route's points first (at the view centre)");
        csf::GuardPatrol recipe{actor({}), {std::nullopt, route_name.empty() ? "RUTA_" + name : route_name, route_points(), true},
                                tools.pause, cover, {std::nullopt, script_name.empty() ? "PATRULLA_" + name : script_name}};
        result = csf::add_guard_patrol(editor, std::move(recipe));
        break;
    }
    case Preset::guard_idle: {
        csf::GuardIdle recipe{actor(view_ground_point(state)), {}, cover,
                              {std::nullopt, script_name.empty() ? "IDLE_" + name : script_name}};
        // "anim[:min-max],anim": a random number of cycles for the first form.
        std::string_view loop(tools.idle_loop.data(), strnlen(tools.idle_loop.data(), tools.idle_loop.size()));
        while (!loop.empty()) {
            const auto comma = loop.find(',');
            const auto step = loop.substr(0, comma);
            loop = comma == std::string_view::npos ? std::string_view{} : loop.substr(comma + 1);
            const auto colon = step.find(':');
            const auto animation = parse_int(step.substr(0, colon));
            if (!animation) return state.warn("Idle loop: expected animation IDs, e.g. 1881:2-4,1385");
            csf::IdleStep idle{*animation, std::nullopt};
            if (colon != std::string_view::npos) {
                const auto range = step.substr(colon + 1);
                const auto dash = range.find('-');
                const auto low = parse_float(range.substr(0, dash));
                const auto high = dash == std::string_view::npos ? std::nullopt : parse_float(range.substr(dash + 1));
                if (!low || !high) return state.warn("Idle loop: cycles are <min>-<max>, e.g. 1881:2-4");
                idle.random_cycles = std::pair{*low, *high};
            }
            recipe.loop.push_back(idle);
        }
        result = csf::add_guard_idle(editor, std::move(recipe));
        break;
    }
    case Preset::animal_patrol: {
        if (tools.points.empty()) return state.warn("Add the route's points first (at the view centre)");
        csf::AnimalPatrol recipe{actor(tools.points.front()),
                                 {std::nullopt, route_name.empty() ? "RUTA_" + name : route_name, route_points(), true},
                                 tools.walk_animation, {std::nullopt, script_name.empty() ? "RUTA_" + name : script_name}};
        result = csf::add_animal_patrol(editor, std::move(recipe));
        break;
    }
    case Preset::cover_group: {
        if (tools.points.empty()) return state.warn("Add the cover points first (at the view centre)");
        auto points = route_points();
        for (auto& point : points) point.rotation_radians = tools.cover_facing * 0.0174532925F;
        std::int32_t id{};
        result = csf::add_cover_group(editor, {std::nullopt, name.empty() ? "Parapeto" : name, points}, &id);
        if (result.applied) tools.cover_group = id;
        break;
    }
    case Preset::walk_grid: {
        const auto* ground = ground_query(state);
        const auto bounds = ground ? ground->bounds() : std::nullopt;
        if (!bounds) return state.warn("The mission has no collision map to build a walk grid on");
        csf::WalkGrid grid;
        grid.name = name.empty() ? "MALLA" : name;
        grid.spacing = tools.grid_spacing;
        // Half a step in from the edges of the ground.
        grid.min_x = (*bounds)[0] + tools.grid_spacing / 2;
        grid.min_z = (*bounds)[1] + tools.grid_spacing / 2;
        grid.max_x = (*bounds)[2] - tools.grid_spacing / 2;
        grid.max_z = (*bounds)[3] - tools.grid_spacing / 2;
        // Keep clear of the actors that are not characters (props, pickups).
        for (const auto& placed : state.mission.scene->actors())
            if (placed.position && placed.cell.value_or(-1) < 0)
                grid.avoided_circles.push_back({placed.position->x, placed.position->z, tools.grid_avoid});
        grid.ground = [ground](const float x, const float z) -> std::optional<float> {
            if (const auto hit = ground->highest(x, z)) return hit->height;
            return std::nullopt;
        };
        result = csf::add_walk_grid(editor, grid);
        break;
    }
    }
    if (apply_mission_edit(state, result)) tools.points.clear();
}

void start_new_mission(AppState& state) {
    if (!mission_editable(state)) return;
    if (apply_mission_edit(state, state.mission.editor->new_mission()))
        state.info("New mission: the scene is empty; undo (Ctrl+Z) brings the donor content back");
}

} // namespace rwsman
