#include "mission_authoring.hpp"

#include "app_util.hpp"
#include "authoring.hpp"
#include "mission_editing.hpp"
#include "rwsman/entity_kind.hpp"

#include "csf/mission_components.hpp"
#include "csf/mission_recipes.hpp"
#include "rws/world_model.hpp"

#include <imgui.h>

#include <algorithm>
#include <charconv>
#include <cmath>
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

} // namespace

csf::MissionOpsOptions component_options(AppState& state) {
    csf::MissionOpsOptions options;
    if (const auto* ground = ground_query(state))
        options.ground = [ground](const float x, const float z) -> std::optional<float> {
            if (const auto hit = ground->highest(x, z)) return hit->height;
            return std::nullopt;
        };
    return options;
}

std::optional<std::vector<csf::MissionRecordId>> add_recipe(AppState& state, const std::string& lines) {
    if (!mission_editable(state)) return std::nullopt;
    auto& editor = *state.mission.editor;
    std::int32_t id{};
    if (!apply_mission_edit(state, csf::add_component(editor, lines, component_options(state), &id))) return std::nullopt;
    for (auto& component : csf::mission_components(editor))
        if (component.id == id) return std::move(component.owns);
    return std::vector<csf::MissionRecordId>{};
}

const std::vector<csf::MissionComponent>& mission_component_list(AppState& state) {
    static const csf::MissionEditor* editor{};
    static std::uint64_t revision{~0ULL};
    static std::vector<csf::MissionComponent> components;
    const auto* current = state.mission.editor.get();
    if (current != editor || (current && current->revision() != revision)) {
        editor = current;
        revision = current ? current->revision() : ~0ULL;
        components = current ? csf::mission_components(*current) : std::vector<csf::MissionComponent>{};
    }
    return components;
}

namespace {

std::optional<csf::MissionRecordId> record_id_of(const MissionRecordKey& key) {
    using Type = csf::MissionRecordId::Type;
    switch (key.kind) {
    case MissionRecordKey::Kind::actor: return csf::MissionRecordId{Type::actor, key.id};
    case MissionRecordKey::Kind::nav_group:
    case MissionRecordKey::Kind::nav_point: return csf::MissionRecordId{Type::navigation_group, key.id};
    case MissionRecordKey::Kind::dummy: return csf::MissionRecordId{Type::dummy, key.id};
    case MissionRecordKey::Kind::area: return csf::MissionRecordId{Type::area, key.id};
    default: return std::nullopt;
    }
}

// The line and key holding a group's points: a patrol's route or a cover group.
std::optional<std::pair<std::size_t, std::string>> points_of_group(const csf::MissionComponent& component,
                                                                   const std::int32_t group) {
    for (std::size_t i = 0; i < component.lines.size(); ++i) try {
            const auto line = csf::parse_op_line(component.lines[i]);
            const bool route = (line.op == "guard-patrol" || line.op == "animal-patrol") &&
                               line.get("route") == std::to_string(group);
            const bool cover = line.op == "cover-group" && line.get("id") == std::to_string(group);
            if (route || cover) return std::pair{i, std::string("points")};
        } catch (const std::exception&) {
        }
    return std::nullopt;
}

std::string join_lines(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) text += line + '\n';
    return text;
}

} // namespace

const csf::MissionComponent* owning_component(AppState& state, const MissionRecordKey& key) {
    const auto record = record_id_of(key);
    if (!record) return nullptr;
    for (const auto& component : mission_component_list(state))
        if (std::ranges::find(component.owns, *record) != component.owns.end()) return &component;
    return nullptr;
}

bool edit_component(AppState& state, const std::int32_t id, const std::vector<std::string>& lines) {
    if (!mission_editable(state)) return false;
    return apply_mission_edit(state, csf::update_component(*state.mission.editor, id, join_lines(lines),
                                                           component_options(state)));
}

std::optional<csf::EditResult> move_component_record(AppState& state, const MissionRecordKey& key, const csf::Vec3 position,
                                                     const float heading_radians) {
    const auto* owner = owning_component(state, key);
    if (!owner || owner->lines.empty() || !mission_editable(state)) return std::nullopt;
    // Edited by hand: the move goes to the record, never over those edits.
    if (csf::component_state(*state.mission.editor, *owner) != csf::ComponentState::clean) return std::nullopt;
    auto lines = owner->lines;
    // The intro's cameras, targets and paths are its shots' parts.
    if (owner->op() == "shot") {
        const auto record = record_id_of(key);
        if (!record) return std::nullopt;
        const auto role = shot_record_role(lines, record->type, record->id,
                                           key.kind == MissionRecordKey::Kind::nav_point ? key.sub_id : 0);
        if (!role) return std::nullopt;
        try {
            auto shots = timeline_shots(lines);
            set_shot_part(shots[role->shot], role->part, position);
            lines = lines_with_shots(lines, shots);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        return csf::update_component(*state.mission.editor, owner->id, join_lines(lines), component_options(state));
    }
    try {
        if (key.kind == MissionRecordKey::Kind::actor) {
            auto line = csf::parse_op_line(lines.front());
            const auto heading = csf::op_number(std::round(heading_radians / 0.0174532925F * 100.0F) / 100.0F);
            if (line.find("pos") && line.op != "guard-patrol") {
                line.set("pos", csf::op_vec3(position));
            } else if (line.op == "guard-patrol") {
                // A patrolling guard stands on its route's first point.
                auto points = csf::parse_op_points(line.get("points"));
                if (points.empty()) return std::nullopt;
                points.front().position = position;
                line.set("points", csf::op_points(points));
            } else {
                return std::nullopt;
            }
            if (line.get("heading", "0") != heading) line.set("heading", heading);
            lines.front() = csf::format_op_line(line);
        } else if (key.kind == MissionRecordKey::Kind::nav_point) {
            const auto where = points_of_group(*owner, key.id);
            if (!where) return std::nullopt;
            auto line = csf::parse_op_line(lines[where->first]);
            auto points = csf::parse_op_points(line.get(where->second));
            const auto index = static_cast<std::size_t>(key.sub_id - 1);
            if (key.sub_id < 1 || index >= points.size()) return std::nullopt;
            points[index].position = position;
            points[index].rotation_radians = heading_radians;
            line.set(where->second, csf::op_points(points));
            lines[where->first] = csf::format_op_line(line);
        } else {
            return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return csf::update_component(*state.mission.editor, owner->id, join_lines(lines), component_options(state));
}

std::optional<csf::EditResult> delete_component_record(AppState& state, const MissionRecordKey& key) {
    const auto* owner = owning_component(state, key);
    if (!owner || !mission_editable(state)) return std::nullopt;
    auto& editor = *state.mission.editor;
    if (key.kind == MissionRecordKey::Kind::nav_point)
        if (const auto where = points_of_group(*owner, key.id)) try {
                auto lines = owner->lines;
                auto line = csf::parse_op_line(lines[where->first]);
                auto points = csf::parse_op_points(line.get(where->second));
                const auto index = static_cast<std::size_t>(key.sub_id - 1);
                if (key.sub_id >= 1 && index < points.size() && points.size() > 2) {
                    points.erase(points.begin() + static_cast<std::ptrdiff_t>(index));
                    line.set(where->second, csf::op_points(points));
                    lines[where->first] = csf::format_op_line(line);
                    return csf::update_component(editor, owner->id, join_lines(lines), component_options(state));
                }
            } catch (const std::exception&) {
            }
    return csf::delete_component(editor, owner->id);
}

namespace {

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

void place_asset(AppState& state, const AuthoringTools::CatalogEntry& entry, const std::optional<csf::Vec3> at,
                 const float heading) {
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    const auto position = at ? *at : view_ground_point(state);
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
            return editor.add_actor(entry.class_id, {position, heading, 0}, "", std::nullopt, &id);
        csf::ActorSpec spec;
        spec.class_id = entry.class_id;
        spec.placement = {position, heading, 0};
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
            if (string.id == id) return string.text;
    return std::string(id);
}

void set_project_text(AppState& state, const std::string& id, const std::optional<std::string>& text) {
    if (!state.authoring.project) return state.warn("Open an authoring project to edit mission text");
    const auto label = text ? "Text " + id : "Remove text " + id;
    edit_authoring_project(state, label, [&](csf::AuthoringProject& project) {
        auto found = std::ranges::find(project.strings, id, &csf::ProjectText::id);
        if (!text) {
            if (found == project.strings.end()) return false;
            project.strings.erase(found);
        } else if (found != project.strings.end()) {
            found->text = *text;
        } else {
            project.strings.push_back({id, *text});
        }
        return true;
    });
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
    if (!state.authoring.project || added.empty()) return true;
    return edit_authoring_project(state, "Add " + std::to_string(added.size()) + " texts",
                                  [&](csf::AuthoringProject& project) {
                                      project.strings.insert(project.strings.end(), added.begin(), added.end());
                                      return true;
                                  });
}

} // namespace

std::optional<std::string> game_text_id(AppState& state, const std::string& text) {
    if (text.empty() || !state.authoring.project) return text;
    std::vector<csf::ProjectText> added;
    std::array<char, 512> buffer{};
    text.copy(buffer.data(), std::min(text.size(), buffer.size() - 1));
    auto id = text_id(state, buffer, added);
    if (!id || !store_texts(state, added)) return std::nullopt;
    return id;
}

void create_trigger(AppState& state) {
    if (!mission_editable(state)) return;
    auto trigger = state.tools.trigger_draft;
    for (auto& action : trigger.actions)
        if (action.kind == csf::TriggerAction::Kind::message) {
            const auto id = game_text_id(state, action.text);
            if (!id) return state.warn("The project's text ID range is full");
            action.text = *id;
        }
    if (trigger.script.name.empty()) trigger.script.name = "TRIGGER";
    if (add_recipe(state, csf::ops_text(trigger))) {
        state.tools.trigger_draft.actions.clear();
        state.tools.trigger_draft.script.name.clear();
    }
}

namespace {

// The first component whose first line is one of `ops`.
const csf::MissionComponent* component_with(AppState& state, std::initializer_list<std::string_view> ops) {
    for (const auto& component : mission_component_list(state))
        if (std::ranges::find(ops, component.op()) != ops.end()) return &component;
    return nullptr;
}

// Inserts `line` before the component's line `op` (its closing line) and
// regenerates it; one undo step. Refused while its records carry hand edits.
bool insert_component_line(AppState& state, const csf::MissionComponent& component, const std::string_view op,
                           const std::string& line) {
    if (csf::component_state(*state.mission.editor, component) != csf::ComponentState::clean) {
        state.warn(csf::component_title(component) + " was edited by hand: keep those edits or regenerate it first");
        return false;
    }
    auto lines = component.lines;
    auto at = std::ranges::find_if(lines, [&](const std::string& value) {
        try {
            return csf::parse_op_line(value).op == op;
        } catch (const std::exception&) {
            return false;
        }
    });
    lines.insert(at, line);
    return edit_component(state, component.id, lines);
}

std::optional<std::int32_t> first_actor_of(AppState& state, std::initializer_list<EntityKind> kinds) {
    const auto& scene = *state.mission.scene;
    for (const auto& actor : scene.actors())
        if (actor.id && std::ranges::find(kinds, classify_mission_actor(scene, state.mission.objects.get(), actor)) !=
                            kinds.end())
            return actor.id;
    return std::nullopt;
}

} // namespace

const csf::MissionComponent* objectives_component(AppState& state) {
    return component_with(state, {"objective", "objectives"});
}

std::optional<std::int32_t> default_objective_target(AppState& state, const std::string_view kind) {
    if (!state.mission.scene) return std::nullopt;
    if (kind == "zone") {
        for (const auto& area : state.mission.scene->areas())
            if (area.id) return area.id;
        return std::nullopt;
    }
    if (kind == "use") return first_actor_of(state, {EntityKind::usable, EntityKind::prop});
    return first_actor_of(state, {EntityKind::enemy, EntityKind::animal});
}

void add_objective(AppState& state) {
    if (!mission_editable(state)) return;
    const auto* existing = objectives_component(state);
    std::int32_t number = 1;
    if (existing)
        for (const auto& value : existing->lines) try {
                if (const auto line = csf::parse_op_line(value); line.op == "objective")
                    number = std::max(number, std::stoi(line.get("n", "0")) + 1);
            } catch (const std::exception&) {
            }
    // Aimed at the first zone, or the first enemy when there is no zone.
    std::string kind = "zone";
    auto target = default_objective_target(state, kind);
    if (!target) target = default_objective_target(state, kind = "kill");
    if (!target) return state.warn("Draw a zone or place an enemy first: an objective needs a target");
    const auto label = game_text_id(state, state.authoring.project ? "Objective " + std::to_string(number) : "g014");
    const auto done = game_text_id(state, state.authoring.project ? "Objective " + std::to_string(number) + " complete"
                                                                  : "g014");
    if (!label || !done) return state.warn("The project's text ID range is full");
    csf::OpLine line{"objective", {}};
    line.set("n", std::to_string(number));
    line.set("kind", kind);
    line.set("target", std::to_string(*target));
    line.set("label", *label);
    line.set("done", *done);
    if (existing) {
        (void)insert_component_line(state, *existing, "objectives", csf::format_op_line(line));
        return;
    }
    (void)add_recipe(state, csf::format_op_line(line) + "\nobjectives success=g014\n");
}

const csf::MissionComponent* equipment_component(AppState& state) { return component_with(state, {"kit", "equipment"}); }

void add_kit(AppState& state, const std::int32_t actor) {
    if (!mission_editable(state)) return;
    // A pistol to start with: the Luger when the mission has it.
    std::int32_t weapon = 0;
    if (state.mission.weapons) {
        for (const auto& definition : state.mission.weapons->definitions())
            if (definition.id && (weapon == 0 || *definition.id == 102)) weapon = *definition.id;
    }
    if (weapon == 0) return state.warn("The mission's Armas.bdd has no weapons");
    const auto line = "kit actor=" + std::to_string(actor) + " weapons=" + std::to_string(weapon);
    if (const auto* existing = equipment_component(state)) {
        (void)insert_component_line(state, *existing, "equipment", line);
        return;
    }
    (void)add_recipe(state, line + "\nequipment\n");
}

const csf::MissionComponent* tips_component(AppState& state) { return component_with(state, {"tips"}); }

void add_tip(AppState& state) {
    if (!mission_editable(state)) return;
    const auto id = game_text_id(state, state.authoring.project ? "A tip for the player" : "g200");
    if (!id) return state.warn("The project's text ID range is full");
    const auto* existing = tips_component(state);
    if (!existing) {
        (void)add_recipe(state, "tips tips=" + *id + "\n");
        return;
    }
    if (csf::component_state(*state.mission.editor, *existing) != csf::ComponentState::clean)
        return state.warn("The tips were edited by hand: keep those edits or regenerate them first");
    try {
        auto line = csf::parse_op_line(existing->lines.front());
        const auto tips = line.get("tips");
        line.set("tips", tips.empty() ? *id : tips + "," + *id);
        (void)edit_component(state, existing->id, {csf::format_op_line(line)});
    } catch (const std::exception& error) {
        state.warn(std::string("The tips do not parse: ") + error.what());
    }
}

std::int32_t default_idle_animation(AppState& state, const std::optional<std::int32_t> class_id) {
    // A looping idle of the actor's model, else hello world's standing idle.
    const auto* animations = state.mission.animations.get();
    if (animations) {
        const auto facts = class_facts(state.mission.objects.get(), class_id);
        const auto model = std::filesystem::path(facts.model).filename().string();
        for (const auto* record : animations->compatible(model))
            if (record->id && record->loop.value_or(false) && !record->model_context.empty() &&
                lower_ascii(record->logical_name).find("idle") != std::string::npos)
                return *record->id;
        if (animations->find_id(1385)) return 1385;
        for (const auto& record : animations->records())
            if (record.id && record.loop.value_or(false)) return *record.id;
        for (const auto& record : animations->records())
            if (record.id) return *record.id;
    }
    return 1385;
}

void give_behaviour(AppState& state, const std::int32_t actor_id, const BehaviourKind kind) {
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    const auto& actors = state.mission.scene->actors();
    const auto actor = std::ranges::find(actors, std::optional(actor_id), &csf::MissionActor::id);
    if (actor == actors.end() || !actor->class_id) return state.warn("Actor " + std::to_string(actor_id) + " is not in the mission");
    if (owning_component(state, {MissionRecordKey::Kind::actor, actor_id, 0}))
        return state.warn("The actor already has a behaviour: change it on its card");
    const auto name = actor->name.value_or("GUARD_" + std::to_string(actor_id));
    const auto position = actor->position.value_or(csf::Vec3{});
    const float heading = actor->heading.value_or(0);
    csf::OpLine line{kind == BehaviourKind::idle     ? "guard-idle"
                     : kind == BehaviourKind::patrol ? "guard-patrol"
                                                     : "animal-patrol",
                     {}};
    line.set("id", std::to_string(actor_id));
    line.set("name", name);
    line.set("class", std::to_string(*actor->class_id));
    line.set("heading", csf::op_number(std::round(heading * 100.0F) / 100.0F));
    if (actor->pitch && *actor->pitch != 0) line.set("pitch", csf::op_number(*actor->pitch));
    if (actor->portrait) line.set("portrait", *actor->portrait);
    if (kind == BehaviourKind::idle) {
        line.set("pos", csf::op_vec3(position));
        line.set("loop", std::to_string(default_idle_animation(state, actor->class_id)));
        line.set("script-name", "IDLE_" + name);
    } else {
        // A two-point route from where it stands, 5 m ahead of it.
        const float radians = heading * 0.0174532925F;
        csf::Vec3 end{position.x + 500.0F * std::sin(radians), position.y, position.z + 500.0F * std::cos(radians)};
        if (const auto ground = mission_ground(state, end.x, end.z)) end.y = *ground;
        if (kind == BehaviourKind::animal) line.set("pos", csf::op_vec3(position));
        line.set("route-name", "RUTA_" + name);
        line.set("points", csf::op_points({{position, 0, 0}, {end, 0, 0}}));
        if (kind == BehaviourKind::patrol) {
            line.set("pause", "3");
            line.set("script-name", "PATRULLA_" + name);
        } else {
            // A looping walk of the animal's model.
            std::int32_t walk = 0;
            if (const auto* animations = state.mission.animations.get()) {
                const auto facts = class_facts(state.mission.objects.get(), actor->class_id);
                for (const auto* record : animations->compatible(std::filesystem::path(facts.model).filename().string()))
                    if (record->id && !record->model_context.empty() &&
                        lower_ascii(record->logical_name).find("andar") != std::string::npos) {
                        walk = *record->id;
                        break;
                    }
            }
            if (walk == 0) return state.warn("No walk animation fits this animal's model: pick one on its card later");
            line.set("walk", std::to_string(walk));
            line.set("script-name", "RUTA_" + name);
        }
    }
    const auto options = component_options(state);
    const auto text = csf::format_op_line(line) + "\n";
    // Made again by the recipe, in its place, keeping its ID: scripts that
    // name it (an objective's target) still do.
    const auto result = editor.replace_in_place("Give " + name + " a behaviour", [&] {
        if (auto deleted = editor.delete_record({csf::MissionRecordId::Type::actor, actor_id}, true); !deleted.applied)
            return deleted;
        return csf::add_component(editor, text, options);
    });
    if (apply_mission_edit(state, result)) select_after_refresh(state, {MissionRecordKey::Kind::actor, actor_id, 0});
}

namespace {

// ImGui's clock: UI scripts run it on a fixed step, so playback is repeatable.
double now_seconds() { return ImGui::GetTime(); }

rws::Vec3 rws_vec(const csf::Vec3 v) { return {v.x, v.y, v.z}; }

} // namespace

CutsceneWhen cutscene_when(const csf::MissionComponent& component) {
    CutsceneWhen when;
    if (component.lines.empty()) return when;
    try {
        const auto line = csf::parse_op_line(component.lines.back());
        if (const auto* zone = line.find("zone")) when.zone = std::stoi(*zone);
        when.arm = line.get("arm");
        when.name = line.get("cutscene-name", "CUT_INICIO");
    } catch (const std::exception&) {
    }
    return when;
}

std::vector<const csf::MissionComponent*> cutscene_components(AppState& state) {
    std::vector<const csf::MissionComponent*> list;
    for (const auto& component : mission_component_list(state))
        if (component.op() == "shot") list.push_back(&component);
    std::ranges::stable_partition(list, [](const auto* component) { return !cutscene_when(*component).zone; });
    return list;
}

const csf::MissionComponent* intro_component(AppState& state) {
    const auto list = cutscene_components(state);
    for (const auto* component : list)
        if (component->id == state.tools.timeline_component) return component;
    return list.empty() ? nullptr : list.front();
}

std::string cutscene_title(AppState& state, const csf::MissionComponent& component) {
    const auto when = cutscene_when(component);
    if (!when.zone) return "Intro (at the start)";
    std::string zone = "zone " + std::to_string(*when.zone);
    if (state.mission.scene)
        for (const auto& area : state.mission.scene->areas())
            if (area.id == when.zone && area.name) zone = *area.name + " (" + zone + ")";
    return when.name + ": entering " + zone + (when.arm.empty() ? "" : ", after " + when.arm);
}

void set_cutscene_when(AppState& state, const CutsceneWhen& when) {
    const auto* cutscene = intro_component(state);
    if (!cutscene || cutscene->lines.empty() || !mission_editable(state)) return;
    auto lines = cutscene->lines;
    try {
        auto line = csf::parse_op_line(lines.back());
        if (when.zone) {
            line.set("zone", std::to_string(*when.zone));
            // The zone's setup script takes a free ID now: regenerating would
            // otherwise give it one the cutscene program's scripts keep.
            if (!line.find("setup")) line.set("setup", std::to_string(state.mission.editor->next_script_id(0)));
            if (when.arm.empty()) line.erase("arm");
            else line.set("arm", when.arm);
        } else {
            for (const auto* key : {"zone", "arm", "setup", "setup-name"}) line.erase(key);
        }
        lines.back() = csf::format_op_line(line);
    } catch (const std::exception& error) {
        return state.warn(std::string("The cutscene's lines do not parse: ") + error.what());
    }
    edit_component(state, cutscene->id, lines);
}

std::vector<TimelineShot> intro_shots(AppState& state) {
    const auto* intro = intro_component(state);
    if (!intro) return {};
    try {
        return timeline_shots(intro->lines);
    } catch (const std::exception&) {
        return {};
    }
}

bool edit_intro_shots(AppState& state, const std::vector<TimelineShot>& shots) {
    const auto* intro = intro_component(state);
    if (!intro || !mission_editable(state)) return false;
    if (shots.empty()) return apply_mission_edit(state, csf::delete_component(*state.mission.editor, intro->id));
    std::vector<std::string> lines;
    try {
        lines = lines_with_shots(intro->lines, shots);
    } catch (const std::exception& error) {
        state.warn(std::string("The intro's lines do not parse: ") + error.what());
        return false;
    }
    // Each shot takes the next dummy, group and two actors from the intro's
    // first IDs: with more shots, a record outside the intro may hold the next
    // ones, and then the intro takes free IDs instead.
    if (shots.size() > timeline_shots(intro->lines).size()) try {
            auto& back = lines.back();
            auto line = csf::parse_op_line(back);
            const auto records = state.mission.editor->record_ids();
            const auto taken = [&](const csf::MissionRecordId::Type type, const std::string_view key, const int per_shot) {
                const auto* value = line.find(key);
                if (!value) return false;
                const auto first = std::stoi(*value);
                for (int id = first; id < first + per_shot * static_cast<int>(shots.size()); ++id) {
                    const csf::MissionRecordId record{type, id};
                    if (std::ranges::find(records, record) != records.end() &&
                        std::ranges::find(intro->owns, record) == intro->owns.end())
                        return true;
                }
                return false;
            };
            using Type = csf::MissionRecordId::Type;
            if (taken(Type::dummy, "dummy", 1) || taken(Type::actor, "actor", 2) ||
                taken(Type::navigation_group, "group", 1)) {
                for (const auto* key : {"dummy", "actor", "group"}) line.erase(key);
                back = csf::format_op_line(line);
            }
        } catch (const std::exception&) {
        }
    return edit_component(state, intro->id, lines);
}

bool intro_sends_init(AppState& state) {
    const auto* intro = intro_component(state);
    if (!intro || intro->lines.empty()) return true;
    try {
        return csf::parse_op_line(intro->lines.back()).get("send-init", "1") != "0";
    } catch (const std::exception&) {
        return true;
    }
}

void set_intro_sends_init(AppState& state, const bool send) {
    const auto* intro = intro_component(state);
    if (!intro || intro->lines.empty()) return;
    auto lines = intro->lines;
    try {
        auto line = csf::parse_op_line(lines.back());
        if (send) line.erase("send-init");
        else line.set("send-init", "0");
        lines.back() = csf::format_op_line(line);
    } catch (const std::exception&) {
        return;
    }
    edit_component(state, intro->id, lines);
}

namespace {

TimelineShot shot_from_view(AppState& state) {
    const auto eye = state.preview.eye_position(), target = state.preview.orbit_target();
    TimelineShot shot;
    shot.camera = {eye.x, eye.y, eye.z};
    // Two metres sideways at constant height, as hello world's shots travel;
    // drag the end in the viewport or capture it.
    const float yaw = std::atan2(target.x - eye.x, target.z - eye.z);
    shot.end = {eye.x + 200.0F * std::cos(yaw), eye.y, eye.z - 200.0F * std::sin(yaw)};
    shot.target = {target.x, target.y, target.z};
    return shot;
}

} // namespace

void capture_shot(AppState& state) {
    if (!mission_editable(state)) return;
    auto shots = intro_shots(state);
    shots.push_back(shot_from_view(state));
    if (intro_component(state)) {
        if (edit_intro_shots(state, shots)) state.tools.timeline_shot = shots.size() - 1;
        return;
    }
    // The first shot makes the intro.
    auto& editor = *state.mission.editor;
    csf::IntroCutscene recipe;
    csf::CameraShot shot;
    shot.camera = shots.back().camera;
    shot.camera_end = shots.back().end;
    shot.target = shots.back().target;
    shot.seconds = shots.back().seconds;
    recipe.shots.push_back(shot);
    // The invisible camera actor: Ambush's class 197, imported when missing.
    const auto ambush = state.settings.resource_root / "Ambush";
    const bool have_class = !editor.objects().find_class(recipe.camera_class).empty();
    if (!have_class && !std::filesystem::is_directory(ambush))
        return state.warn("The cutscene needs Ambush's invisible camera actor (class 197); set the resource root "
                          "to the unpacked game so it can be imported");
    const auto result = editor.batch("Add intro cutscene", [&] {
        if (!have_class)
            if (auto imported = editor.import_class(ambush, recipe.camera_class); !imported.applied) return imported;
        return csf::add_component(editor, csf::ops_text(recipe), component_options(state));
    });
    if (apply_mission_edit(state, result)) state.tools.timeline_shot = 0;
}

void add_zone_cutscene(AppState& state) {
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    const auto& areas = state.mission.scene->areas();
    const auto zone = std::ranges::find_if(areas, [](const auto& area) { return area.id.has_value(); });
    if (zone == areas.end()) return state.warn("A zone cutscene needs a zone: draw one with the Zone tool first");
    if (editor.objects().find_class(197).empty())
        return state.warn("Add an intro first: it brings the invisible camera actor (class 197) cutscenes need");
    const auto shot = shot_from_view(state);
    csf::IntroCutscene recipe;
    recipe.shots.push_back({shot.camera, shot.end, shot.target, shot.seconds, {}, {}, {}});
    recipe.zone = *zone->id;
    // Names are unique per mission, and the cutscene names its helpers after itself.
    std::set<std::string> taken;
    for (const auto* cutscene : cutscene_components(state)) taken.insert(cutscene_when(*cutscene).name);
    for (int n = 1;; ++n) {
        recipe.cutscene_name = "CUT_ZONA_" + std::to_string(n);
        if (!taken.contains(recipe.cutscene_name)) break;
    }
    recipe.intro.name = "CUTSCENE_ZONA_" + recipe.cutscene_name.substr(9);
    std::int32_t id{};
    if (apply_mission_edit(state, csf::add_component(editor, csf::ops_text(recipe), component_options(state), &id))) {
        state.tools.timeline_component = id;
        state.tools.timeline_shot = 0;
    }
}

void recapture_shot(AppState& state, const std::size_t index, const ShotPart part) {
    auto shots = intro_shots(state);
    if (index >= shots.size()) return;
    const auto eye = state.preview.eye_position(), target = state.preview.orbit_target();
    if (part == ShotPart::target) {
        shots[index].target = {target.x, target.y, target.z};
    } else {
        set_shot_part(shots[index], part, {eye.x, eye.y, eye.z});
        // The view looks at the target it captures along with the camera.
        shots[index].target = {target.x, target.y, target.z};
    }
    edit_intro_shots(state, shots);
}

void view_shot(AppState& state, const std::size_t index, const float t) {
    const auto shots = intro_shots(state);
    if (index >= shots.size()) return;
    auto& tools = state.tools;
    tools.timeline_shot = index;
    tools.timeline_time = shot_start(shots, index) + std::clamp(t, 0.0F, 1.0F) * shots[index].seconds;
    const auto view = shot_view(shots[index], t);
    state.preview.look_from(rws_vec(view.eye), rws_vec(view.target));
}

void scrub_timeline(AppState& state, const float seconds) {
    const auto shots = intro_shots(state);
    if (const auto at = timeline_position(shots, seconds)) {
        view_shot(state, at->shot, at->fraction);
        state.tools.timeline_time = std::clamp(seconds, 0.0F, timeline_length(shots));
    }
}

void play_shots(AppState& state) {
    const auto shots = intro_shots(state);
    if (shots.empty()) return;
    auto& tools = state.tools;
    // From the playhead, or from the start when it is at the end.
    if (tools.timeline_time >= timeline_length(shots) - 0.01F) tools.timeline_time = 0.0F;
    tools.preview_started = now_seconds() - tools.timeline_time;
    update_shot_preview(state);
}

void stop_shots(AppState& state) { state.tools.preview_started.reset(); }

void update_shot_preview(AppState& state) {
    auto& tools = state.tools;
    if (!tools.preview_started) return;
    const auto shots = intro_shots(state);
    const auto elapsed = static_cast<float>(now_seconds() - *tools.preview_started);
    const auto length = timeline_length(shots);
    scrub_timeline(state, std::min(elapsed, length));
    if (elapsed >= length) tools.preview_started.reset();
    else state.ui.animating = true;
}

void place_building(AppState& state, const std::string& asset, const std::optional<csf::Vec3> where,
                    const float heading) {
    auto* project = state.authoring.project.get();
    if (!project) return;
    const auto at = where ? *where : view_ground_point(state);
    csf::ProjectPlacement placement;
    placement.kind = csf::ProjectPlacement::Kind::building;
    placement.asset = asset;
    for (int n = 1;; ++n) {
        placement.id = asset + "-" + std::to_string(n);
        if (std::ranges::none_of(project->placements, [&](const auto& p) { return p.id == placement.id; })) break;
    }
    placement.position = {at.x, mission_ground(state, at.x, at.z).value_or(at.y), at.z};
    placement.yaw_degrees = heading;
    placement.height = {csf::HeightRule::Mode::ground, 0.0F, {}, {}};
    if (edit_authoring_project(state, "Place " + placement.id, [&](csf::AuthoringProject& edited) {
            edited.placements.push_back(placement);
            return true;
        }))
        state.ok("Placed " + placement.id + "; the map rebuilds");
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
        result = csf::add_component(editor, csf::ops_text(recipe), component_options(state));
        break;
    }
    case Preset::guard_idle: {
        csf::GuardIdle recipe{actor(tools.points.empty() ? view_ground_point(state) : tools.points.front()), {}, cover,
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
        result = csf::add_component(editor, csf::ops_text(recipe), component_options(state));
        break;
    }
    case Preset::animal_patrol: {
        if (tools.points.empty()) return state.warn("Add the route's points first (at the view centre)");
        csf::AnimalPatrol recipe{actor(tools.points.front()),
                                 {std::nullopt, route_name.empty() ? "RUTA_" + name : route_name, route_points(), true},
                                 tools.walk_animation, {std::nullopt, script_name.empty() ? "RUTA_" + name : script_name}};
        result = csf::add_component(editor, csf::ops_text(recipe), component_options(state));
        break;
    }
    case Preset::cover_group: {
        if (tools.points.empty()) return state.warn("Add the cover points first (at the view centre)");
        auto points = route_points();
        for (auto& point : points) point.rotation_radians = tools.cover_facing * 0.0174532925F;
        const auto owns = add_recipe(state, csf::ops_text(csf::CoverGroup{std::nullopt, name.empty() ? "Parapeto" : name, points}));
        if (!owns) return;
        if (!owns->empty()) tools.cover_group = owns->front().id;
        tools.points.clear();
        return;
    }
    case Preset::walk_grid: {
        const auto grid = preset_walk_grid(state);
        if (!grid) return state.warn("The mission has no collision map to build a walk grid on");
        result = csf::add_component(editor, csf::ops_text(*grid), component_options(state));
        tools.preview_grid = false;
        break;
    }
    }
    if (apply_mission_edit(state, result)) tools.points.clear();
}

std::optional<csf::WalkGrid> preset_walk_grid(AppState& state) {
    auto& tools = state.tools;
    const auto* ground = ground_query(state);
    const auto bounds = ground ? ground->bounds() : std::nullopt;
    if (!bounds || !state.mission.scene) return std::nullopt;
    csf::WalkGrid grid;
    const auto name = text_of(tools.name);
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
    return grid;
}

void update_walk_grid_preview(AppState& state) {
    auto& tools = state.tools;
    if (!tools.preview_grid || !state.mission.editor) return;
    const std::tuple key{tools.grid_spacing, tools.grid_avoid, state.mission.editor->revision()};
    if (key == tools.grid_key) return;
    tools.grid_key = key;
    tools.grid_points.clear();
    tools.grid_links.clear();
    if (const auto grid = preset_walk_grid(state)) {
        auto layout = csf::walk_grid_layout(*grid);
        for (const auto& point : layout.points) tools.grid_points.push_back(point.position);
        tools.grid_links = std::move(layout.links);
    }
}

void start_new_mission(AppState& state) {
    if (!mission_editable(state)) return;
    if (apply_mission_edit(state, state.mission.editor->new_mission()))
        state.info("New mission: the scene is empty; undo (Ctrl+Z) brings the donor content back");
}

} // namespace rwsman
