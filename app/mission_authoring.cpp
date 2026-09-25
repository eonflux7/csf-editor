#include "mission_authoring.hpp"

#include "app_util.hpp"
#include "authoring.hpp"
#include "mission_editing.hpp"

#include "csf/mission_recipes.hpp"
#include "rws/world_model.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
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

const csf::MissionFlow* mission_flow(AppState& state) {
    auto& tools = state.tools;
    if (!state.mission.editor) return nullptr;
    const auto revision = state.mission.applied_revision;
    if (tools.flow && tools.flow_revision == revision) return tools.flow.get();
    const csf::ProgramDocument *mission{}, *cutscene{};
    for (const auto& [path, program] : state.mission.programs) {
        const auto extension = lower_ascii(path_utf8(path.extension()));
        if (extension == ".gsc" && !mission) mission = &program;
        if (extension == ".csc" && !cutscene) cutscene = &program;
    }
    tools.flow = std::make_shared<const csf::MissionFlow>(csf::MissionFlow::build(mission, cutscene));
    tools.flow_revision = revision;
    return tools.flow.get();
}

void open_flow_script(AppState& state, const std::string_view program, const std::int32_t id) {
    const auto wanted = program == "cutscene" ? ".csc" : ".gsc";
    const auto& programs = state.mission.programs;
    for (std::size_t document = 0; document < programs.size(); ++document) {
        if (lower_ascii(path_utf8(programs[document].first.extension())) != wanted) continue;
        const auto& scripts = programs[document].second.scripts();
        for (std::size_t i = 0; i < scripts.size(); ++i)
            if (scripts[i].id == id) {
                state.selected_program_document = document;
                state.selected_program_script = i;
                state.workspace = Workspace::script;
                return;
            }
    }
    state.warn("Script " + std::to_string(id) + " is not in the " + std::string(program) + " program");
}

std::string text_label(const AppState& state, const std::string_view id) {
    if (const auto* project = state.authoring.project.get())
        for (const auto& string : project->strings)
            if (string.id == id) return std::string(id) + "  \"" + string.text + "\"";
    return std::string(id);
}

void set_project_text(AppState& state, const std::string& id, const std::optional<std::string>& text) {
    auto* project = state.authoring.project.get();
    if (!project) return state.warn("Open an authoring project to edit mission text");
    auto found = std::ranges::find(project->strings, id, &csf::ProjectText::id);
    if (!text) {
        if (found != project->strings.end()) project->strings.erase(found);
    } else if (found != project->strings.end()) {
        found->text = *text;
    } else {
        project->strings.push_back({id, *text});
    }
    if (const auto problems = project->check(); !problems.empty()) return state.warn(problems.front());
    save_authoring_project(state);
    rebuild_authoring_map(state, false);
}

namespace {

std::string buffer_text(const std::span<const char> buffer) { return {buffer.data(), strnlen(buffer.data(), buffer.size())}; }

// The FLI ID for a text field: with an authoring project the field holds the
// string, which gets the next free ID; otherwise the field is the ID.
std::optional<std::string> text_id(AppState& state, const std::span<const char> field, std::vector<csf::ProjectText>& added) {
    const auto value = buffer_text(field);
    if (value.empty()) return std::string{};
    auto* project = state.authoring.project.get();
    if (!project) return value;
    for (const auto& string : project->strings)
        if (string.text == value) return string.id;
    for (const auto& string : added)
        if (string.text == value) return string.id;
    auto copy = *project;
    copy.strings.insert(copy.strings.end(), added.begin(), added.end());
    const auto id = copy.next_text_id();
    if (!id) return std::nullopt;
    added.push_back({*id, value});
    return id;
}

bool store_texts(AppState& state, const std::vector<csf::ProjectText>& added) {
    auto* project = state.authoring.project.get();
    if (!project || added.empty()) return true;
    project->strings.insert(project->strings.end(), added.begin(), added.end());
    if (const auto problems = project->check(); !problems.empty()) {
        project->strings.resize(project->strings.size() - added.size());
        state.warn(problems.front());
        return false;
    }
    save_authoring_project(state);
    rebuild_authoring_map(state, false);
    return true;
}

} // namespace

void create_objectives(AppState& state) {
    if (!mission_editable(state)) return;
    auto& tools = state.tools;
    if (tools.objectives.empty()) return state.warn("Add an objective first");
    std::vector<csf::ProjectText> added;
    csf::Objectives recipe;
    recipe.success_message = buffer_text(tools.success_message);
    std::int32_t number = 1;
    for (const auto& form : tools.objectives) {
        csf::Objective objective;
        objective.number = number++;
        objective.secondary = form.secondary;
        objective.kind = static_cast<csf::Objective::Kind>(form.kind);
        objective.target = form.target;
        const auto label = text_id(state, form.label, added), done = text_id(state, form.done, added),
                   prompt = text_id(state, form.prompt, added);
        if (!label || !done || !prompt) return state.warn("The project's text ID range is full");
        if (label->empty() || done->empty()) return state.warn("Every objective needs its text and its done message");
        objective.label = *label;
        objective.done = *done;
        objective.prompt = *prompt;
        recipe.objectives.push_back(std::move(objective));
    }
    if (!store_texts(state, added)) return;
    if (apply_mission_edit(state, csf::add_objectives(*state.mission.editor, recipe))) tools.objectives.clear();
}

void create_equipment(AppState& state) {
    if (!mission_editable(state)) return;
    csf::Equipment recipe;
    for (const auto& form : state.tools.kits) {
        csf::Kit kit;
        kit.actor = form.actor;
        std::string_view list(form.weapons.data(), strnlen(form.weapons.data(), form.weapons.size()));
        while (!list.empty()) {
            const auto comma = list.find(',');
            const auto item = list.substr(0, comma);
            list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            const auto at = item.find('@');
            const auto weapon = parse_int(item.substr(0, at));
            if (!weapon) return state.warn("Weapons: <class>[@<ammo>/<ammo>], e.g. 102@100/100");
            csf::Kit::Weapon entry{*weapon, std::nullopt};
            if (at != std::string_view::npos) {
                const auto ammo = item.substr(at + 1);
                const auto slash = ammo.find('/');
                const auto first = parse_float(ammo.substr(0, slash));
                const auto second = slash == std::string_view::npos ? std::nullopt : parse_float(ammo.substr(slash + 1));
                if (!first || !second) return state.warn("Ammunition is <loaded>/<carried>, e.g. 102@100/100");
                entry.ammunition = std::pair{*first, *second};
            }
            kit.weapons.push_back(entry);
        }
        if (form.selected) kit.selected = form.selected;
        if (form.disguise) kit.disguise = form.disguise;
        recipe.kits.push_back(std::move(kit));
    }
    if (apply_mission_edit(state, csf::add_equipment(*state.mission.editor, recipe))) state.tools.kits.clear();
}

void create_tips(AppState& state) {
    if (!mission_editable(state)) return;
    csf::Tips recipe;
    std::string_view list(state.tools.tips.data(), strnlen(state.tools.tips.data(), state.tools.tips.size()));
    while (!list.empty()) {
        const auto comma = list.find(',');
        if (const auto tip = list.substr(0, comma); !tip.empty()) recipe.tips.emplace_back(tip);
        list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    if (apply_mission_edit(state, csf::add_tips(*state.mission.editor, recipe))) state.tools.tips[0] = '\0';
}

namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

rws::Vec3 rws_vec(const csf::Vec3 v) { return {v.x, v.y, v.z}; }

} // namespace

void capture_shot(AppState& state) {
    const auto eye = state.preview.eye_position(), target = state.preview.orbit_target();
    AuthoringTools::ShotForm shot;
    shot.camera = {eye.x, eye.y, eye.z};
    shot.target = {target.x, target.y, target.z};
    state.tools.shots.push_back(shot);
}

void view_shot(AppState& state, const std::size_t index, const float t) {
    if (index >= state.tools.shots.size()) return;
    const auto& shot = state.tools.shots[index];
    state.preview.look_from({shot.camera.x + t * shot.travel_x, shot.camera.y, shot.camera.z + t * shot.travel_z},
                            rws_vec(shot.target));
}

void play_shots(AppState& state) {
    if (state.tools.shots.empty()) return;
    state.tools.preview_started = now_seconds();
    update_shot_preview(state);
}

void stop_shots(AppState& state) { state.tools.preview_started.reset(); }

void update_shot_preview(AppState& state) {
    auto& tools = state.tools;
    if (!tools.preview_started) return;
    auto elapsed = static_cast<float>(now_seconds() - *tools.preview_started);
    for (std::size_t i = 0; i < tools.shots.size(); ++i) {
        const auto seconds = std::max(tools.shots[i].seconds, 0.01F);
        if (elapsed <= seconds) {
            view_shot(state, i, elapsed / seconds);
            state.ui.animating = true;
            return;
        }
        elapsed -= seconds;
    }
    view_shot(state, tools.shots.size() - 1, 1.0F);
    tools.preview_started.reset();
}

void create_intro(AppState& state) {
    if (!mission_editable(state)) return;
    auto& tools = state.tools;
    if (tools.shots.empty()) return state.warn("Capture a shot first");
    auto& editor = *state.mission.editor;
    csf::IntroCutscene recipe;
    recipe.send_init = tools.send_init;
    for (const auto& form : tools.shots) {
        csf::CameraShot shot;
        shot.camera = form.camera;
        // Constant height, as hello world's shots: the path keeps the camera's height.
        shot.camera_end = {form.camera.x + form.travel_x, form.camera.y, form.camera.z + form.travel_z};
        shot.target = form.target;
        shot.seconds = form.seconds;
        recipe.shots.push_back(shot);
    }
    // The invisible camera actor: Ambush's class 197, imported when missing.
    const auto ambush = state.settings.resource_root / "Ambush";
    const bool have_class = !editor.objects().find_class(recipe.camera_class).empty();
    if (!have_class && !std::filesystem::is_directory(ambush))
        return state.warn("The cutscene needs Ambush's invisible camera actor (class 197); set the resource root "
                          "to the unpacked game so it can be imported");
    const auto result = editor.batch("Add intro cutscene", [&] {
        if (!have_class)
            if (auto imported = editor.import_class(ambush, recipe.camera_class); !imported.applied) return imported;
        return csf::add_intro_cutscene(editor, recipe);
    });
    if (apply_mission_edit(state, result)) tools.shots.clear();
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
