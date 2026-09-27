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

std::string start_event(const std::string& event) { return event.empty() ? default_start_event : event; }

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

std::string script_name_token(const std::string_view name) {
    std::string token;
    for (const char c : name) {
        const bool word = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        token.push_back(word ? c : '_');
    }
    if (!token.empty() && token.front() >= '0' && token.front() <= '9') token.insert(0, "S_");
    return token;
}

std::string script_text(const std::int32_t id, const std::string_view name, const std::int32_t trigger,
                        const std::vector<std::string>& events, const std::vector<std::string>& actions,
                        const std::vector<std::string>& conditions, const std::vector<ScriptVariable>& variables) {
    std::string out = "[\n  .ID " + std::to_string(id) + "\n  .NOMBRE " + script_name_token(name) +
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
    if (!variables.empty()) {
        out += "  .VARIABLES (\n";
        for (const auto& variable : variables)
            out += "    [\n      .ID " + std::to_string(variable.id) + "\n      .TYPE " + variable.type + "\n      .NOMBRE " +
                   script_name_token(variable.name) + "\n      .VALOR " + variable.value + "\n    ]\n";
        out += "  )\n";
    }
    out += "  .ACCIONES {\n";
    for (const auto& action : actions) out += "    " + action + "\n";
    return out + "  }\n]\n";
}

std::string fli_operand(const std::string_view id) {
    const bool numeric = !id.empty() && std::ranges::all_of(id, [](const char c) { return c >= '0' && c <= '9'; });
    return "(FLI " + (numeric ? "\"" + std::string(id) + "\"" : std::string(id)) + ")";
}

namespace {

// Adds scripts in order as one undo step.
EditResult add_scripts(MissionEditor& editor, std::string label, const std::vector<std::string>& texts) {
    const auto program = mission_program(editor);
    return editor.batch(std::move(label), [&] {
        Steps steps;
        std::vector<std::string> ids;
        for (const auto& text : texts) {
            std::int32_t added{};
            if (!steps.ok(editor.add_script(program, text, &added))) return steps.last;
            ids.push_back(std::to_string(added));
        }
        std::string list;
        for (const auto& id : ids) list += (list.empty() ? "" : ", ") + id;
        return steps.finish("Added script(s) " + list);
    });
}

std::string number_operand(const std::int32_t n) { return "(NUMERO " + script_number(static_cast<float>(n)) + ")"; }

} // namespace

std::pair<float, float> look_at(const Vec3 camera, const Vec3 target) {
    const auto dx = target.x - camera.x, dy = target.y - camera.y, dz = target.z - camera.z;
    return {std::atan2(dx, dz), std::atan2(-dy, std::hypot(dx, dz))};
}

EditResult add_intro_cutscene(MissionEditor& editor, const IntroCutscene& recipe) {
    if (recipe.shots.empty()) return {false, "The cutscene needs a shot", {}};
    const auto program = mission_program(editor);
    const auto cutscene_file = editor.file_of_kind(MissionFileKind::cutscene_script);
    if (!cutscene_file) return {false, "The mission has no cutscene program (.csc)", {}};
    if (editor.objects().find_class(recipe.camera_class).empty())
        return {false, "Class " + std::to_string(recipe.camera_class) +
                           " (the invisible camera actor) is not in this mission; import it (Ambush has it)", {}};
    const auto& scene = editor.scene();
    const auto next = [](const auto& records, const std::optional<std::int32_t> wanted) {
        if (wanted) return *wanted;
        std::int32_t maximum = 0;
        for (const auto& record : records)
            if (record.id) maximum = std::max(maximum, *record.id);
        return maximum + 1;
    };
    const auto first_dummy = next(scene.dummies(), recipe.first_dummy);
    const auto first_actor = next(scene.actors(), recipe.first_actor);
    const auto first_group = next(scene.navigation(), recipe.first_group);
    // Script IDs are unique across both programs: take them all from one
    // counter, so no script is renumbered on the way in (its references would not be).
    auto next_id = editor.next_script_id(program);
    const auto take = [&](const std::optional<std::int32_t> wanted) {
        const auto id = wanted.value_or(next_id);
        next_id = std::max(next_id, id + 1);
        return id;
    };
    const auto intro_id = take(recipe.intro.id);
    const auto setup_id = recipe.zone ? take(recipe.setup.id) : 0;
    std::array<std::int32_t, 4> cutscene{};
    for (std::size_t k = 0; k < cutscene.size(); ++k) cutscene[k] = take(recipe.cutscene_ids[k]);
    const auto n = static_cast<std::int32_t>(recipe.shots.size());
    const auto fade = [](const bool in) {
        return "FX_FADE (NUMERO 1.0) (BOOL " + std::string(in ? "TRUE" : "FALSE") + ") (VECTOR 0.0 0.0 0.0)";
    };
    std::vector<std::string> intro;
    const auto zone = recipe.zone ? "(ZONA " + std::to_string(*recipe.zone) + ")" : std::string{};
    // A zone's cutscene runs once, from a fade out (Ransom's CUT_ENTRADA): DEACT
    // removes the armed entry (ACT with FALSE would arm a leave event instead, KB-scn-10).
    if (recipe.zone)
        intro.insert(intro.end(), {"DEACT_BICHO_EVENT_ZONA (PLAYER) " + zone + " (BOOL TRUE)", fade(true), "PAUSE (NUMERO 1.5)"});
    intro.insert(intro.end(), {fade(false), "CUTSCENE_NO_INTERACTIVA (BOOL TRUE)", "PLAYER_TERCERA (BOOL TRUE)"});
    // INIT is a mission event (KB-scripting-14): the actor scripts that wait
    // for it start here, as Convoy's intro sends it.
    if (recipe.send_init && !recipe.zone) intro.emplace_back("SEND_EVENT (EVENT INIT)");
    for (std::int32_t k = 0; k < n; ++k)
        intro.push_back("CREATE_VIEWPOINT (DUMMY " + std::to_string(first_dummy + k) + ") (BICHO " +
                        std::to_string(first_actor + 2 * k) + ") (BICHO " + std::to_string(first_actor + 2 * k + 1) +
                        ") (NUMERO 0.0)");
    intro.push_back("CUTSCENE_EXE (CUTSCENE " + std::to_string(cutscene[0]) + ")");
    intro.insert(intro.end(), {fade(true), "PAUSE (NUMERO 1.5)", "CUTSCENE_NO_INTERACTIVA (BOOL FALSE)",
                               "PLAYER_TERCERA (BOOL FALSE)"});
    for (std::int32_t k = 0; k < n; ++k) intro.push_back("NAVEGACION_STOP (BICHO " + std::to_string(first_actor + 2 * k) + ")");
    intro.push_back(fade(false));
    // And never again, even when another commando (another PLAYER) enters.
    if (recipe.zone) intro.push_back("TRIGGER_OFF (TRIGGER " + std::to_string(intro_id) + ")");

    std::vector<std::string> camera;
    for (std::int32_t k = 0; k < n; ++k) {
        const auto& shot = recipe.shots[static_cast<std::size_t>(k)];
        const auto travel = std::hypot(shot.camera_end.x - shot.camera.x, shot.camera_end.z - shot.camera.z);
        const auto speed = shot.speed.value_or(shot.seconds > 0 ? travel / shot.seconds : 0.0F);
        const auto cameraman = "(BICHO " + std::to_string(first_actor + 2 * k) + ")";
        camera.insert(camera.end(), {"PAUSE (NUMERO 0.01)", "CAMARA_EN_DUMMY (DUMMY " + std::to_string(first_dummy + k) + ")",
                                     "CAM_SETFILTRO (CADENA \"<NINGUNO>\")",
                                     "SET_WANTED_VEL " + cameraman + " (NUMERO " + script_number(speed) + ")",
                                     "CONTINUE (IR_A_PATHPOINT " + cameraman + " (PATHPOINT " +
                                         std::to_string(first_group + k) + " 2))",
                                     "PAUSE (NUMERO " + script_number(shot.seconds) + ")"});
    }
    const auto name = recipe.cutscene_name;
    const auto header_only = [](const std::int32_t id, const std::string& script_name) {
        return "[\n  .ID " + std::to_string(id) + "\n  .NOMBRE " + script_name_token(script_name) +
               "\n  .CARPETA \"\"\n  .FLAGS [\n    .TRIGGER 1\n    .ENABLED 1\n    .VALIDO 1\n  ]\n]\n";
    };
    const std::vector<std::string> cutscene_texts{
        script_text(cutscene[0], name, 1, {},
                    {"CUTSCENE_EXE (CUTSCENE " + std::to_string(cutscene[1]) + ")",
                     "CONTINUE (CUTSCENE_EXE (CUTSCENE " + std::to_string(cutscene[3]) + "))",
                     "CUTSCENE_EXE (CUTSCENE " + std::to_string(cutscene[2]) + ")"}),
        header_only(cutscene[1], name + "INIT"),
        script_text(cutscene[2], name + "END", 1, {},
                    {"WAIT_CONDICION (CUTSCENE_FINISHED (CUTSCENE " + std::to_string(cutscene[3]) + "))"}),
        script_text(cutscene[3], name + "_GENERAL_Camara", 1, {}, camera)};

    return editor.batch("Add intro cutscene", [&] {
        Steps steps;
        for (std::int32_t k = 0; k < n; ++k) {
            const auto& shot = recipe.shots[static_cast<std::size_t>(k)];
            const auto heading = shot.heading.value_or(
                std::atan2(shot.camera_end.x - shot.camera.x, shot.camera_end.z - shot.camera.z));
            const auto suffix = std::to_string(k + 1);
            // A zone's cutscene names its helpers after itself: names are unique in a mission.
            const auto prefix = recipe.zone ? script_name_token(recipe.cutscene_name) : std::string("INTRO");
            if (!steps.ok(editor.add_navigation_group(
                    (recipe.zone ? "Rutas_" + prefix + "_Camara_" : std::string("Rutas_Cutscene_Inicio_Camara_")) + suffix, 0,
                                                      {{shot.camera, heading, 0}, {shot.camera_end, heading, 0}},
                                                      {{1, 2}}, first_group + k)))
                return steps.last;
            ActorSpec cameraman{first_actor + 2 * k, prefix + "_CAMERA_" + suffix, recipe.camera_class, {shot.camera, 0, 0},
                                std::pair{first_group + k, 1}, {}, {}};
            ActorSpec target{first_actor + 2 * k + 1, prefix + "_TARGET_" + suffix, recipe.camera_class, {shot.target, 0, 0},
                             std::nullopt, {}, {}};
            if (!steps.ok(editor.add_actor_record(cameraman)) || !steps.ok(editor.add_actor_record(target)))
                return steps.last;
            const auto [yaw, pitch] = shot.aim.value_or(look_at(shot.camera, shot.target));
            const auto dummy = first_dummy + k;
            if (!steps.ok(editor.add_dummy("CAMARA_" + std::to_string(dummy), shot.camera, yaw, pitch, dummy)))
                return steps.last;
        }
        const auto intro_name = recipe.intro.name.empty() ? "CUTSCENE_INICIO" : recipe.intro.name;
        if (recipe.zone) {
            if (!steps.ok(editor.add_script(program, script_text(intro_id, intro_name, 1, {"BICHO_ENT_ZONA"}, intro,
                                                                 {"CMP_OP_ZONA (EVT_ZONA) (OP_BOOLEAN 0) " + zone}))) ||
                !steps.ok(editor.add_script(program,
                                            script_text(setup_id, recipe.setup.name.empty() ? intro_name + "_INI" : recipe.setup.name,
                                                        1, {recipe.arm_event.empty() ? "START_GAME" : recipe.arm_event},
                                                        {"ACT_BICHO_EVENT_ZONA (PLAYER) " + zone + " (BOOL TRUE)"}))))
                return steps.last;
        } else if (!steps.ok(editor.add_script(program, script_text(intro_id, intro_name, 1, {"START_GAME"}, intro)))) {
            return steps.last;
        }
        for (const auto& text : cutscene_texts)
            if (!steps.ok(editor.add_script(*cutscene_file, text))) return steps.last;
        return steps.finish("Added an intro of " + std::to_string(n) + " shot(s): script " + std::to_string(intro_id) +
                            ", cutscene " + std::to_string(cutscene[0]));
    });
}

EditResult add_objectives(MissionEditor& editor, const Objectives& recipe) {
    if (recipe.objectives.empty()) return {false, "No objectives to add", {}};
    auto next = editor.next_script_id(mission_program(editor));
    const auto take = [&](const ScriptSpec& spec) {
        const auto id = spec.id.value_or(next);
        next = std::max(next, id + 1);
        return id;
    };
    std::vector<std::int32_t> primaries;
    for (const auto& objective : recipe.objectives)
        if (!objective.secondary) primaries.push_back(objective.number);
    // IF (<all primaries completed>): binary ANDs, nested to the left.
    std::string all;
    for (const auto n : primaries) {
        const auto check = "(OBJETIVO_COMPLETADO " + number_operand(n) + ")";
        all = all.empty() ? check : "(AND " + all + " " + check + ")";
    }
    if (!all.empty()) all = all.substr(1, all.size() - 2);  // IF (<condition>) adds the parentheses back
    const std::vector<std::string> check_all =
        primaries.empty() ? std::vector<std::string>{}
                          : std::vector<std::string>{"IF (" + all + ")",
                                                     "  TIMED_STRING_V2 " + fli_operand(recipe.success_message) +
                                                         " (NUMERO 5.0) (NUMERO 4.0)",
                                                     "  PAUSE (NUMERO " + script_number(recipe.success_pause) + ")",
                                                     "  SET_MISSION_SUCCESS (BOOL TRUE)", "ENDIF"};
    std::vector<std::string> setup;
    for (const auto& objective : recipe.objectives)
        if (objective.kind == Objective::Kind::enter_zone)
            setup.push_back("ACT_BICHO_EVENT_ZONA (PLAYER) (ZONA " + std::to_string(objective.target) + ") (BOOL TRUE)");
    for (const auto& objective : recipe.objectives) {
        setup.push_back("SET_OBJETIVO " + number_operand(objective.number) +
                        (objective.secondary ? " (BOOL TRUE) (NUMERO 1.0)" : " (BOOL FALSE) (NUMERO 2.0)"));
        setup.push_back("SET_OBJETIVO_LABEL " + number_operand(objective.number) + " " + fli_operand(objective.label));
    }
    for (const auto& objective : recipe.objectives) {
        if (objective.kind != Objective::Kind::use_object) continue;
        const auto actor = "(BICHO " + std::to_string(objective.target) + ")";
        if (!objective.prompt.empty()) setup.push_back("BICHO_SET_CONTEXT_LABEL " + actor + " " + fli_operand(objective.prompt));
        setup.push_back("HABILITAR_GHOST " + actor + " (BOOL TRUE)");
        setup.push_back("SET_CONTEXTUAL " + actor + " (BOOL TRUE)");
        setup.push_back("ENABLE_GHOST_ILUM " + actor + " (BOOL TRUE)");
    }
    std::vector<std::string> texts{
        script_text(take(recipe.setup), recipe.setup.name.empty() ? "INIT_OBJETIVOS" : recipe.setup.name, 1,
                    {"START_GAME"}, setup)};
    for (const auto& objective : recipe.objectives) {
        const auto n = number_operand(objective.number);
        const auto message = "TIMED_STRING_V2 " + fli_operand(objective.done) + " (NUMERO 5.0) (NUMERO 4.0)";
        const auto name = objective.script.name.empty() ? "OBJETIVO_" + std::to_string(objective.number)
                                                        : objective.script.name;
        std::vector<std::string> events, conditions, actions;
        switch (objective.kind) {
        case Objective::Kind::enter_zone: {
            const auto zone = "(ZONA " + std::to_string(objective.target) + ")";
            events = {"BICHO_ENT_ZONA"};
            conditions = {"CMP_OP_ZONA (EVT_ZONA) (OP_BOOLEAN 0) " + zone};
            actions = {"DEACT_BICHO_EVENT_ZONA (PLAYER) " + zone + " (BOOL TRUE)"};
            break;
        }
        case Objective::Kind::kill_actor:
            events = {"MORIBUNDO", "MUERTO"};
            conditions = {"AND (CMP_OP_BICHO (EVT_BICHO1) (OP_BOOLEAN 0) (BICHO " + std::to_string(objective.target) +
                          ")) (NOT (OBJETIVO_COMPLETADO " + n + "))"};
            break;
        case Objective::Kind::use_object: {
            const auto actor = "(BICHO " + std::to_string(objective.target) + ")";
            events = {"EVT_GHOST_USADO"};
            conditions = {"CMP_OP_BICHO (EVT_BICHO2) (OP_BOOLEAN 0) " + actor};
            actions = {"HABILITAR_GHOST " + actor + " (BOOL FALSE)", "SET_CONTEXTUAL " + actor + " (BOOL FALSE)",
                       "ENABLE_GHOST_ILUM " + actor + " (BOOL FALSE)"};
            break;
        }
        }
        actions.push_back("SET_OBJETIVO_SUCCESS " + n + " (BOOL TRUE)");
        actions.push_back(message);
        if (!objective.secondary) actions.insert(actions.end(), check_all.begin(), check_all.end());
        texts.push_back(script_text(take(objective.script), name, 1, events, actions, conditions));
    }
    return add_scripts(editor, "Add objectives", texts);
}

EditResult add_equipment(MissionEditor& editor, const Equipment& recipe) {
    std::vector<std::string> actions;
    for (const auto& kit : recipe.kits) {
        const auto actor = "(BICHO " + std::to_string(kit.actor) + ")";
        for (const auto& weapon : kit.weapons) {
            const auto weapon_class = "(ARMA_CLASSID " + std::to_string(weapon.weapon_class) + ")";
            actions.push_back("ADD_ARMA " + actor + " " + weapon_class);
            if (weapon.ammunition)
                actions.push_back("SET_MUNICION_ARMA " + actor + " " + weapon_class + " (NUMERO " +
                                  script_number(weapon.ammunition->first) + ") (NUMERO " +
                                  script_number(weapon.ammunition->second) + ")");
        }
        if (kit.selected) actions.push_back("SELECT_ARMA " + actor + " (ARMA_CLASSID " + std::to_string(*kit.selected) + ")");
        if (kit.disguise) actions.push_back("DISFRAZAR " + actor + " (CLASSID " + std::to_string(*kit.disguise) + ")");
    }
    if (actions.empty()) return {false, "No equipment to add", {}};
    const auto id = recipe.script.id.value_or(editor.next_script_id(mission_program(editor)));
    return add_scripts(editor, "Add starting equipment",
                       {script_text(id, recipe.script.name.empty() ? "COMMANDOS_INI" : recipe.script.name, 1,
                                    {"START_GAME"}, actions)});
}

bool trigger_when_proven(const Trigger::When when) noexcept {
    // A mission-level script on a custom event: shipped programs listen to
    // custom events from actor scripts only. A body found is Country's
    // watcher: VEO_BICHO on a dead actor is not yet seen working in game.
    return when != Trigger::When::event && when != Trigger::When::body_found;
}

bool trigger_action_proven(const TriggerAction::Kind kind) noexcept {
    // The alarm is Convoy's own pattern, not yet played in a mission of ours.
    return kind != TriggerAction::Kind::alarm;
}

bool trigger_needs_setup(const Trigger& trigger) noexcept {
    return trigger.when == Trigger::When::enter_zone || trigger.when == Trigger::When::object_used;
}

std::vector<std::string> trigger_script_texts(const Trigger& trigger, const std::int32_t script_id,
                                              const std::int32_t setup_id) {
    using When = Trigger::When;
    using Kind = TriggerAction::Kind;
    const auto name = trigger.script.name.empty() ? "TRIGGER_" + std::to_string(script_id) : trigger.script.name;
    const auto target_actor = "(BICHO " + std::to_string(trigger.target) + ")";
    const auto zone = "(ZONA " + std::to_string(trigger.target) + ")";
    const auto ghost = [&](const std::string& actor, const bool on) {
        const std::string value = on ? " (BOOL TRUE)" : " (BOOL FALSE)";
        return std::vector<std::string>{"HABILITAR_GHOST " + actor + value, "SET_CONTEXTUAL " + actor + value,
                                        "ENABLE_GHOST_ILUM " + actor + value};
    };
    std::vector<std::string> texts;
    if (trigger_needs_setup(trigger)) {
        std::vector<std::string> setup;
        if (trigger.when == When::enter_zone) setup.push_back("ACT_BICHO_EVENT_ZONA (PLAYER) " + zone + " (BOOL TRUE)");
        else setup = ghost(target_actor, true);
        texts.push_back(script_text(setup_id, trigger.setup.name.empty() ? name + "_INI" : trigger.setup.name, 1,
                                    {"START_GAME"}, setup));
    }
    std::vector<std::string> events, actions, extra_conditions;
    std::vector<ScriptVariable> variables;
    std::string condition;
    switch (trigger.when) {
    case When::mission_start:
    case When::timer: events = {"START_GAME"}; break;
    case When::alerted:
        // Convoy's SET_ALARMA, as the shipped program writes it.
        events = {"IA_CHANGE_STATE"};
        extra_conditions = {"CMP_OP_BANDO (GET_BANDO (EVT_BICHO1)) (OP_BOOLEAN 0) (BANDO ALEMAN)",
                            "OR (CMP_OP_ACTITUD (EVT_ACTITUD) (OP_BOOLEAN 0) (ACTITUD ALERTA)) (CMP_OP_ACTITUD "
                            "(EVT_ACTITUD) (OP_BOOLEAN 0) (ACTITUD COMBATIENDO))"};
        break;
    case When::body_found: {
        // There is no body-found event: once a second, for each watched
        // soldier who is dead, whether a living one sees him; the finder is
        // kept for the alarm's position. Only the dead cost anything.
        events = {"START_GAME"};
        variables = {{1, "BOOL", "Encontrado", "FALSE"}, {2, "BICHO", "Descubridor", "0"}};
        actions = {"WHILE (NOT (VAR 1))", "  PAUSE (NUMERO 1.0)"};
        for (const auto dead : trigger.watch) {
            actions.push_back("  IF (NOT (ESTA_VIVO (BICHO " + std::to_string(dead) + ")))");
            for (const auto finder : trigger.watch) {
                if (finder == dead) continue;
                const auto who = "(BICHO " + std::to_string(finder) + ")";
                actions.insert(actions.end(), {"    IF (AND (ESTA_VIVO " + who + ") (VEO_BICHO " + who + " (BICHO " +
                                                   std::to_string(dead) + ")))",
                                               "      SET (VAR 1) (BOOL TRUE)", "      SET (VAR 2) " + who, "    ENDIF"});
            }
            actions.emplace_back("  ENDIF");
        }
        actions.emplace_back("WEND");
        break;
    }
    case When::enter_zone:
        events = {"BICHO_ENT_ZONA"};
        condition = "CMP_OP_ZONA (EVT_ZONA) (OP_BOOLEAN 0) " + zone;
        break;
    case When::actor_killed:
        events = {"MORIBUNDO", "MUERTO"};
        condition = "CMP_OP_BICHO (EVT_BICHO1) (OP_BOOLEAN 0) " + target_actor;
        break;
    case When::object_used:
        events = {"EVT_GHOST_USADO"};
        condition = "CMP_OP_BICHO (EVT_BICHO2) (OP_BOOLEAN 0) " + target_actor;
        break;
    case When::event: events = {trigger.event}; break;
    }
    // The watcher decides when it happens, so its objective test comes then.
    std::string late_test;
    if (trigger.if_objective) {
        const auto done = "OBJETIVO_COMPLETADO " + number_operand(trigger.if_objective->first);
        const auto test = trigger.if_objective->second ? done : "NOT (" + done + ")";
        if (trigger.when == When::body_found) late_test = test;
        else if (trigger.when == When::alerted) extra_conditions.push_back(test);
        else condition = condition.empty() ? test : "AND (" + condition + ") (" + test + ")";
    }
    const auto first_action = actions.size();
    if (trigger.when == When::timer) actions.push_back("PAUSE (NUMERO " + script_number(trigger.seconds) + ")");
    // Once: the zone stops reporting the player, and every kind turns the trigger off at the end.
    if (trigger.when == When::enter_zone) actions.push_back("DEACT_BICHO_EVENT_ZONA (PLAYER) " + zone + " (BOOL TRUE)");
    for (const auto& action : trigger.actions) {
        const auto actor = "(BICHO " + std::to_string(action.number) + ")";
        switch (action.kind) {
        case Kind::complete_objective:
            actions.push_back("SET_OBJETIVO_SUCCESS " + number_operand(action.number) + " (BOOL TRUE)");
            break;
        case Kind::message: actions.push_back("TIMED_STRING_V2 " + fli_operand(action.text) + " (NUMERO 5.0) (NUMERO 4.0)"); break;
        case Kind::raise_event: actions.push_back("SEND_EVENT (EVENT " + action.text + ")"); break;
        case Kind::alarm: {
            // As Convoy's camp: a threat heard far around where it happened, then the alarm.
            const std::string where = trigger.when == When::actor_killed || trigger.when == When::alerted
                                          ? "(EVT_BICHO1)"
                                      : trigger.when == When::object_used ? target_actor
                                      : trigger.when == When::enter_zone  ? "(PLAYER)"
                                      : trigger.when == When::body_found  ? "(VAR 2)"
                                                                          : "";
            if (!where.empty())
                actions.push_back("CREA_ESTIMULO_ACUSTICO (REACTIVIDAD AMENAZA_DIRECTA) (GET_PATHPOINT " + where +
                                  ") (NUMERO 20000.0) (NUMERO 5.0)");
            actions.push_back("ACTIVAR_ALARMA (NUMERO " + script_number(static_cast<float>(action.number)) + ")");
            break;
        }
        case Kind::ai_alert: actions.push_back("SET_IA_ALERTA " + actor + " (IA_ALERTA " + action.text + ")"); break;
        case Kind::ai_combat: actions.push_back("SET_IA_COMBATE " + actor + " (IA_COMBATE " + action.text + ")"); break;
        case Kind::enable_ghost:
        case Kind::disable_ghost:
            for (auto& line : ghost(actor, action.kind == Kind::enable_ghost)) actions.push_back(std::move(line));
            break;
        case Kind::mission_success: actions.push_back("SET_MISSION_SUCCESS (BOOL TRUE)"); break;
        }
    }
    if (!late_test.empty()) {
        for (auto k = first_action; k < actions.size(); ++k) actions[k].insert(0, "  ");
        actions.insert(actions.begin() + static_cast<std::ptrdiff_t>(first_action), "IF (" + late_test + ")");
        actions.emplace_back("ENDIF");
    }
    if (trigger.when == When::actor_killed || trigger.when == When::object_used || trigger.when == When::event ||
        trigger.when == When::enter_zone || trigger.when == When::alerted)
        actions.push_back("TRIGGER_OFF (TRIGGER " + std::to_string(script_id) + ")");
    auto conditions = condition.empty() ? std::vector<std::string>{} : std::vector{condition};
    conditions.insert(conditions.end(), extra_conditions.begin(), extra_conditions.end());
    texts.push_back(script_text(script_id, name, 1, events, actions, conditions, variables));
    return texts;
}

EditResult add_trigger(MissionEditor& editor, const Trigger& recipe) {
    if (recipe.actions.empty()) return {false, "The trigger does nothing: add an action", {}};
    if (recipe.when == Trigger::When::event && recipe.event.empty()) return {false, "Name the event it waits for", {}};
    if (recipe.when == Trigger::When::body_found && recipe.watch.size() < 2)
        return {false, "A body is found by another watched soldier: watch at least two", {}};
    auto next = editor.next_script_id(mission_program(editor));
    const auto setup_id = trigger_needs_setup(recipe) ? recipe.setup.id.value_or(next) : 0;
    if (trigger_needs_setup(recipe)) next = std::max(next, setup_id + 1);
    const auto script_id = recipe.script.id.value_or(next);
    auto texts = trigger_script_texts(recipe, script_id, setup_id);
    return add_scripts(editor, "Add trigger " + (recipe.script.name.empty() ? std::to_string(script_id) : recipe.script.name),
                       texts);
}

EditResult add_tips(MissionEditor& editor, const Tips& recipe) {
    if (recipe.tips.empty()) return {false, "No tips to add", {}};
    std::vector<std::string> actions{"TIMED_STRING_INITPOS (NUMERO " + script_number(recipe.position.first) + ") (NUMERO " +
                                     script_number(recipe.position.second) + ")"};
    for (const auto& tip : recipe.tips) actions.push_back("ADD_TIP_MISSION " + fli_operand(tip));
    const auto id = recipe.script.id.value_or(editor.next_script_id(mission_program(editor)));
    return add_scripts(editor, "Add mission tips",
                       {script_text(id, recipe.script.name.empty() ? "INIT_MISSION" : recipe.script.name, 1,
                                    {"START_GAME"}, actions)});
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
        if (!steps.ok(editor.add_script(program, script_text(id, recipe.script.name, 0, {start_event(recipe.start_event)}, actions),
                                          &added)))
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
                             script_text(id, recipe.script.name, 0, {start_event(recipe.start_event)}, actions), {});
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
        if (!steps.ok(editor.add_script(
                program, script_text(id, recipe.script.name, 0, {start_event(recipe.start_event)}, actions), &added)))
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

WalkGridLayout walk_grid_layout(const WalkGrid& recipe) {
    WalkGridLayout layout;
    if (!(recipe.spacing > 0) || recipe.max_x < recipe.min_x || recipe.max_z < recipe.min_z || !recipe.ground) return layout;
    const auto columns = static_cast<std::int32_t>(std::floor((recipe.max_x - recipe.min_x) / recipe.spacing)) + 1;
    const auto rows = static_cast<std::int32_t>(std::floor((recipe.max_z - recipe.min_z) / recipe.spacing)) + 1;
    std::map<std::pair<std::int32_t, std::int32_t>, std::int32_t> index;
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
            index[{column, row}] = static_cast<std::int32_t>(layout.points.size() + 1);
            layout.points.push_back({{x, *height, z}, 0, 0});
        }
    for (std::int32_t row = 0; row < rows; ++row)
        for (std::int32_t column = 0; column < columns; ++column) {
            const auto a = index.find({column, row});
            if (a == index.end()) continue;
            for (const auto& [dc, dr] : {std::pair{1, 0}, std::pair{0, 1}})
                if (const auto b = index.find({column + dc, row + dr}); b != index.end())
                    layout.links.emplace_back(a->second, b->second);
        }
    return layout;
}

EditResult add_walk_grid(MissionEditor& editor, const WalkGrid& recipe, std::int32_t* new_id) {
    if (!(recipe.spacing > 0) || recipe.max_x < recipe.min_x || recipe.max_z < recipe.min_z || !recipe.ground)
        return {false, "The walk grid needs a positive spacing, a range and ground", {}};
    const auto layout = walk_grid_layout(recipe);
    if (layout.points.empty()) return {false, "No walk grid point has ground under it", {}};
    return editor.add_navigation_group(recipe.name, 0, layout.points, layout.links, recipe.id, new_id);
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
