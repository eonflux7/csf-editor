// The component card in Properties (docs/plans/editor-ux-redesign.md, E1 and
// E12): the recipe that made the selected record, with its parameters. Every
// edit rewrites the component's lines and regenerates its records in place
// (csf/mission_components.hpp), one undo step; a component whose records were
// edited by hand offers to keep those edits (detach) or regenerate.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "authoring.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "viewport_tools.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include "csf/mission_components.hpp"

#include <imgui.h>

#include <cmath>
#include <functional>

namespace rwsman::ui {
namespace {

// A component's lines as parsed lines, written back after an edit.
struct Lines {
    AppState& state;
    std::int32_t id;
    std::vector<csf::OpLine> lines;

    void apply() {
        std::vector<std::string> text;
        for (const auto& line : lines) text.push_back(csf::format_op_line(line));
        edit_component(state, id, text);
    }
    void set(const std::size_t index, const std::string_view key, std::string value) {
        lines[index].set(key, std::move(value));
        apply();
    }
    std::optional<std::size_t> find(const std::string_view op) const {
        for (std::size_t i = 0; i < lines.size(); ++i)
            if (lines[i].op == op) return i;
        return std::nullopt;
    }
};

float number_of(const csf::OpLine& line, const std::string_view key, const float fallback) {
    try {
        const auto* value = line.find(key);
        return value ? std::stof(*value) : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

int integer_of(const csf::OpLine& line, const std::string_view key, const int fallback) {
    try {
        const auto* value = line.find(key);
        return value ? std::stoi(*value) : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

// The cover group the guard takes cover in, from the mission's cover groups.
void cover_row(AppState& state, Lines& lines, const std::size_t index) {
    property_row("Cover", "Where the guard takes cover when alerted and in combat.");
    const auto current = integer_of(lines.lines[index], "cover", 0);
    std::string preview = "none";
    for (const auto& group : state.mission.scene->navigation())
        if (group.id == current) preview = group.name.value_or("group " + std::to_string(current));
    const bool open = ImGui::BeginCombo("##cover", preview.c_str());
    if (!open) name_last_item("##cover");
    if (open) {
        if (ImGui::Selectable("none", current == 0)) {
            lines.lines[index].erase("cover");
            lines.apply();
        }
        for (const auto& group : state.mission.scene->navigation())
            if (group.id && group.type == 3 &&
                ImGui::Selectable((group.name.value_or("group") + "##" + std::to_string(*group.id)).c_str(),
                                  *group.id == current))
                lines.set(index, "cover", std::to_string(*group.id));
        ImGui::EndCombo();
    }
}

// A route or cover group's points: moved in the viewport, added and removed here.
void points_row(Lines& lines, const std::size_t index, const std::size_t minimum) {
    std::vector<csf::NavPointSpec> points;
    try {
        points = csf::parse_op_points(lines.lines[index].get("points"));
    } catch (const std::exception&) {
        return;
    }
    property_row("Points", "Move them in the viewport: the component keeps them. Add one after the last, or "
                           "remove the last.");
    ImGui::Text("%zu", points.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Add point") && !points.empty()) {
        auto next = points.back();
        // Continue the last leg, or step aside for a single point.
        const auto previous = points.size() > 1 ? points[points.size() - 2].position : csf::Vec3{};
        const csf::Vec3 step = points.size() > 1 ? csf::Vec3{next.position.x - previous.x, 0, next.position.z - previous.z}
                                                 : csf::Vec3{300.0F, 0.0F, 0.0F};
        const float length = std::hypot(step.x, step.z);
        const float scale = length > 1.0F ? 300.0F / length : 1.0F;
        next.position = {next.position.x + step.x * scale, next.position.y, next.position.z + step.z * scale};
        if (const auto ground = mission_ground(lines.state, next.position.x, next.position.z)) next.position.y = *ground;
        points.push_back(next);
        lines.set(index, "points", csf::op_points(points));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(points.size() <= minimum);
    if (ImGui::SmallButton("Remove last")) {
        points.pop_back();
        lines.set(index, "points", csf::op_points(points));
    }
    ImGui::EndDisabled();
}

// Idle and patrol are the same guard doing different things (E12): switching
// keeps its ID, name, class, look, cover and script, and makes or drops its
// route.
void behaviour_row(AppState& state, Lines& lines) {
    auto& line = lines.lines.front();
    const bool patrol = line.op == "guard-patrol";
    property_row("Behaviour", "Switching keeps the guard, its script's ID and its cover group.");
    int choice = patrol ? 1 : 0;
    static constexpr const char* behaviours[]{"At a post (idle loop)", "Patrol a route"};
    const bool changed_choice = ImGui::Combo("##behaviour", &choice, behaviours, IM_ARRAYSIZE(behaviours));
    name_last_item("##behaviour");
    if (!changed_choice || choice == (patrol ? 1 : 0)) return;
    csf::OpLine changed{patrol ? "guard-idle" : "guard-patrol", {}};
    for (const auto& key : {"id", "name", "class", "heading", "pitch", "portrait", "cover", "script", "script-name"})
        if (const auto* value = line.find(key)) changed.set(key, *value);
    std::vector<csf::OpLine> extra;
    if (patrol) {
        // The post is where the route starts.
        try {
            const auto points = csf::parse_op_points(line.get("points"));
            changed.set("pos", csf::op_vec3(points.empty() ? csf::Vec3{} : points.front().position));
        } catch (const std::exception&) {
            return;
        }
        changed.set("loop", "1385");  // hello world's standing idle
    } else {
        // A two-point route from the post, 5 m ahead of it.
        csf::Vec3 start{};
        try {
            const auto v = csf::parse_op_points(line.get("pos") + ",0");
            start = v.front().position;
        } catch (const std::exception&) {
        }
        const float heading = number_of(line, "heading", 0.0F) * 0.0174532925F;
        csf::Vec3 end{start.x + 500.0F * std::sin(heading), start.y, start.z + 500.0F * std::cos(heading)};
        if (const auto ground = mission_ground(state, end.x, end.z)) end.y = *ground;
        std::int32_t route = 1;
        for (const auto& group : state.mission.scene->navigation()) route = std::max(route, group.id.value_or(0) + 1);
        changed.set("route", std::to_string(route));
        changed.set("route-name", "RUTA_" + line.get("name"));
        changed.set("points", csf::op_points({{start, 0, 0}, {end, 0, 0}}));
        changed.set("pause", "3");
    }
    lines.lines.front() = std::move(changed);
    lines.apply();
}

void guard_fields(AppState& state, Lines& lines) {
    auto& line = lines.lines.front();
    behaviour_row(state, lines);
    if (line.op == "guard-patrol") {
        property_row("Pause", "Seconds the guard waits at each point.");
        float pause{};
        if (edit_float_value("##pause", number_of(line, "pause", 3.0F), pause, 0.1F, "%.1f s") && pause >= 0.0F)
            lines.set(0, "pause", csf::op_number(std::round(pause * 10.0F) / 10.0F));
        property_row("Loop", "Walk back to the first point after the last one; otherwise turn back.");
        bool loop = line.get("loop", "1") != "0";
        if (ImGui::Checkbox("##loop", &loop)) {
            if (loop) line.erase("loop");
            else line.set("loop", "0");
            lines.apply();
        }
        points_row(lines, 0, 1);
    } else {
        property_row("Idle loop", "Animation IDs played in turn; <id>:<min>-<max> plays one a random number of times.");
        std::string loop;
        if (edit_text_value("##idle_loop", line.get("loop"), loop) && !loop.empty()) lines.set(0, "loop", loop);
    }
    cover_row(state, lines, 0);
}

void animal_fields(Lines& lines) {
    auto& line = lines.lines.front();
    property_row("Walk animation", "The animation it walks with (IR_A_PATHPOINT_ANIM).");
    int walk{};
    if (edit_int_value("##walk", integer_of(line, "walk", 0), walk) && walk > 0) lines.set(0, "walk", std::to_string(walk));
    points_row(lines, 0, 1);
}

// A game text: the project's string when it has one (typed here, kept under
// its ID), otherwise the FLI string ID itself.
void text_row(AppState& state, Lines& lines, const std::size_t index, const char* label, const char* key,
              const char* help) {
    property_row(label, help);
    const auto id = lines.lines[index].get(key);
    const auto* project = state.authoring.project.get();
    const csf::ProjectText* string = nullptr;
    if (project)
        for (const auto& value : project->strings)
            if (value.id == id) string = &value;
    const auto field = std::string("##") + key;  // "Objectives::label" in UI scripts
    std::string edited;
    if (string) {
        if (edit_text_value(field.c_str(), string->text, edited) && !edited.empty()) set_project_text(state, id, edited);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Text %s of the project", id.c_str());
    } else if (edit_text_value(field.c_str(), id, edited, "FLI string ID") && !edited.empty()) {
        lines.set(index, key, edited);
    }
}

void objective_fields(AppState& state, Lines& lines) {
    int number = 0;
    for (std::size_t i = 0; i < lines.lines.size(); ++i) {
        auto& line = lines.lines[i];
        if (line.op != "objective") continue;
        ImGui::PushID(static_cast<int>(i));
        const auto kind = line.get("kind");
        const bool zone = kind == "zone";
        property_row(("Objective " + std::to_string(++number)).c_str());
        static constexpr const char* kinds[]{"Reach a zone", "Kill an actor", "Use an object"};
        int choice = zone ? 0 : kind == "kill" ? 1 : 2;
        const bool kind_changed = ImGui::Combo("##kind", &choice, kinds, IM_ARRAYSIZE(kinds));
        name_last_item("##kind");
        if (kind_changed) {
            line.set("kind", choice == 0 ? "zone" : choice == 1 ? "kill" : "use");
            lines.apply();
        }
        property_row(zone ? "Zone" : "Actor", "Pick it in the viewport or the Outliner.");
        const auto target = integer_of(line, "target", 0);
        std::string name = "(missing)";
        if (zone) {
            for (const auto& area : state.mission.scene->areas())
                if (area.id == target) name = area.name.value_or("");
        } else {
            for (const auto& actor : state.mission.scene->actors())
                if (actor.id == target) name = actor.name.value_or("");
        }
        ImGui::Text("%s  %s %d", name.c_str(), zone ? "zone" : "actor", target);
        ImGui::SameLine();
        const std::int32_t component = lines.id;
        const auto line_index = i;
        auto copy = lines.lines;
        pick_button(state, "##pick_objective_target",
                    {zone ? MissionRecordKey::Kind::area : MissionRecordKey::Kind::actor}, "Pick the objective's target",
                    [&state, component, line_index, copy](const MissionRecordKey& key) mutable {
                        copy[line_index].set("target", std::to_string(key.id));
                        std::vector<std::string> text;
                        for (const auto& value : copy) text.push_back(csf::format_op_line(value));
                        edit_component(state, component, text);
                    });
        text_row(state, lines, i, "Text", "label", "The objective as the objectives screen shows it.");
        text_row(state, lines, i, "Done", "done", "The message shown when it is completed.");
        if (choice == 2) text_row(state, lines, i, "Prompt", "prompt", "The label on the object to use.");
        property_row("Secondary", "Secondary objectives are not needed to win.");
        bool secondary = line.get("secondary", "0") != "0";
        if (ImGui::Checkbox("##secondary", &secondary)) {
            if (secondary) line.set("secondary", "1");
            else line.erase("secondary");
            lines.apply();
        }
        ImGui::PopID();
    }
}

void intro_fields(Lines& lines) {
    int number = 0;
    for (std::size_t i = 0; i < lines.lines.size(); ++i) {
        if (lines.lines[i].op != "shot") continue;
        ImGui::PushID(static_cast<int>(i));
        property_row(("Shot " + std::to_string(++number)).c_str(), "Seconds the shot lasts.");
        float seconds{};
        if (edit_float_value("##seconds", number_of(lines.lines[i], "seconds", 4.0F), seconds, 0.1F, "%.1f s") &&
            seconds > 0.1F) {
            // The camera speed follows from the length of its path and the time.
            lines.lines[i].erase("speed");
            lines.set(i, "seconds", csf::op_number(std::round(seconds * 10.0F) / 10.0F));
        }
        ImGui::PopID();
    }
}

void equipment_fields(AppState& state, Lines& lines) {
    int number = 0;
    for (std::size_t i = 0; i < lines.lines.size(); ++i) {
        auto& line = lines.lines[i];
        if (line.op != "kit") continue;
        ImGui::PushID(static_cast<int>(i));
        property_row(("Kit " + std::to_string(++number)).c_str(), "The player character who starts with it.");
        ImGui::Text("actor %d", integer_of(line, "actor", 0));
        ImGui::SameLine();
        const std::int32_t component = lines.id;
        auto copy = lines.lines;
        pick_button(state, "##pick_kit_owner", {MissionRecordKey::Kind::actor}, "Pick the player character",
                    [&state, component, i, copy](const MissionRecordKey& key) mutable {
                        copy[i].set("actor", std::to_string(key.id));
                        std::vector<std::string> text;
                        for (const auto& value : copy) text.push_back(csf::format_op_line(value));
                        edit_component(state, component, text);
                    });
        property_row("Weapons", "Class IDs, each with its ammunition: <class>[@<loaded>/<carried>],...");
        std::string edited;
        if (edit_text_value("##weapons", line.get("weapons"), edited) && !edited.empty())
            lines.set(i, "weapons", edited);
        property_row("In hand", "The weapon class selected at the start (empty: none).");
        if (edit_text_value("##select", line.get("select"), edited)) {
            if (edited.empty()) line.erase("select");
            else line.set("select", edited);
            lines.apply();
        }
        ImGui::PopID();
    }
}

void tips_fields(Lines& lines) {
    property_row("Tips", "FLI string IDs of the tips, in order.");
    std::string edited;
    if (edit_text_value("##tips", lines.lines.front().get("tips"), edited)) lines.set(0, "tips", edited);
}

// ---- Triggers (E10) ----------------------------------------------------------------

using Kind = csf::TriggerAction::Kind;
using When = csf::Trigger::When;

const char* when_label(const When when) {
    switch (when) {
    case When::mission_start: return "The mission starts";
    case When::enter_zone: return "The player enters a zone";
    case When::actor_killed: return "An actor dies";
    case When::object_used: return "The player uses an object";
    case When::event: return "An event is raised";
    case When::timer: return "Some seconds after the start";
    }
    return "?";
}

const char* action_label(const Kind kind) {
    switch (kind) {
    case Kind::complete_objective: return "Complete an objective";
    case Kind::message: return "Show a message";
    case Kind::raise_event: return "Raise an event";
    case Kind::alarm: return "Sound the alarm";
    case Kind::ai_alert: return "Set an actor's alert behaviour";
    case Kind::ai_combat: return "Set an actor's combat behaviour";
    case Kind::enable_ghost: return "Make an object usable";
    case Kind::disable_ghost: return "Make an object unusable";
    case Kind::mission_success: return "Win the mission";
    }
    return "?";
}

std::string record_name(AppState& state, const bool zone, const std::int32_t id) {
    if (zone) {
        for (const auto& area : state.mission.scene->areas())
            if (area.id == id) return area.name.value_or("zone") + "  (zone " + std::to_string(id) + ")";
        return "(pick a zone)";
    }
    for (const auto& actor : state.mission.scene->actors())
        if (actor.id == id) return actor.name.value_or("actor") + "  (actor " + std::to_string(id) + ")";
    return "(pick an actor)";
}

void unverified_mark(const bool proven) {
    if (proven) return;
    ImGui::SameLine();
    token_text(Token::warn, "unverified");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Seen in the shipped missions, not yet played in a mission made here.");
}

} // namespace

void trigger_fields(AppState& state, const csf::Trigger& trigger, const TriggerChange& change, const bool typed_texts) {
    const bool unverified = state.tools.show_unverified;
    property_row("When");
    if (ImGui::BeginCombo("##trigger_when", when_label(trigger.when))) {
        for (const auto when : {When::mission_start, When::enter_zone, When::actor_killed, When::object_used, When::timer,
                                When::event})
            if (unverified || csf::trigger_when_proven(when) || when == trigger.when) {
                if (ImGui::Selectable(when_label(when), when == trigger.when))
                    change([when](csf::Trigger& value) { value.when = when; });
                unverified_mark(csf::trigger_when_proven(when));
            }
        ImGui::EndCombo();
    } else {
        name_last_item("##trigger_when");
    }
    unverified_mark(csf::trigger_when_proven(trigger.when));
    if (trigger.when == When::enter_zone || trigger.when == When::actor_killed || trigger.when == When::object_used) {
        const bool zone = trigger.when == When::enter_zone;
        property_row(zone ? "Zone" : trigger.when == When::actor_killed ? "Actor" : "Object",
                     "Pick it in the viewport or the Outliner.");
        ImGui::TextUnformatted(record_name(state, zone, trigger.target).c_str());
        ImGui::SameLine();
        pick_button(state, "##pick_trigger_target", {zone ? MissionRecordKey::Kind::area : MissionRecordKey::Kind::actor},
                    zone ? "Pick the zone" : "Pick the actor",
                    [change](const MissionRecordKey& key) { change([id = key.id](csf::Trigger& value) { value.target = id; }); });
    }
    if (trigger.when == When::event) {
        property_row("Event", "The name another script raises with SEND_EVENT.");
        std::string edited;
        if (edit_text_value("##trigger_event", trigger.event, edited, "EVENT_NAME"))
            change([edited](csf::Trigger& value) { value.event = edited; });
    }
    if (trigger.when == When::timer) {
        property_row("Seconds");
        float seconds{};
        if (edit_float_value("##trigger_seconds", trigger.seconds, seconds, 0.5F, "%.1f s") && seconds >= 0)
            change([seconds](csf::Trigger& value) { value.seconds = seconds; });
    }
    property_row("If", "Only while an objective is (or is not yet) complete.");
    static constexpr const char* conditions[]{"Always", "Objective ... is complete", "Objective ... is not complete"};
    int condition = !trigger.if_objective ? 0 : trigger.if_objective->second ? 1 : 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.65F);
    if (ImGui::Combo("##trigger_if", &condition, conditions, IM_ARRAYSIZE(conditions)))
        change([condition](csf::Trigger& value) {
            if (condition == 0) value.if_objective.reset();
            else value.if_objective = std::pair{value.if_objective ? value.if_objective->first : 1, condition == 1};
        });
    name_last_item("##trigger_if");
    if (trigger.if_objective) {
        ImGui::SameLine();
        int number{};
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (edit_int_value("##trigger_if_number", trigger.if_objective->first, number) && number > 0)
            change([number](csf::Trigger& value) { value.if_objective->first = number; });
    }
    for (std::size_t i = 0; i < trigger.actions.size(); ++i) {
        const auto& action = trigger.actions[i];
        ImGui::PushID(static_cast<int>(i));
        property_row(i == 0 ? "Do" : "");
        ImGui::TextUnformatted(action_label(action.kind));
        unverified_mark(csf::trigger_action_proven(action.kind));
        ImGui::SameLine();
        const auto update = [&](const std::function<void(csf::TriggerAction&)>& edit) {
            change([i, edit](csf::Trigger& value) {
                if (i < value.actions.size()) edit(value.actions[i]);
            });
        };
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 30.0F * ui_scale(), 60.0F));
        int number{};
        std::string edited;
        switch (action.kind) {
        case Kind::complete_objective:
        case Kind::alarm:
            if (edit_int_value("##number", action.number, number) && number >= 0)
                update([number](csf::TriggerAction& value) { value.number = number; });
            break;
        case Kind::message:
            if (edit_text_value("##text", action.text, edited, typed_texts && state.authoring.project ? "the message" : "FLI ID"))
                update([edited](csf::TriggerAction& value) { value.text = edited; });
            break;
        case Kind::raise_event:
            if (edit_text_value("##text", action.text, edited, "EVENT_NAME"))
                update([edited](csf::TriggerAction& value) { value.text = edited; });
            break;
        case Kind::ai_alert:
        case Kind::ai_combat:
        case Kind::enable_ghost:
        case Kind::disable_ghost:
            ImGui::TextUnformatted(record_name(state, false, action.number).c_str());
            ImGui::SameLine();
            pick_button(state, "##pick_action_actor", {MissionRecordKey::Kind::actor}, "Pick the actor",
                        [change, i](const MissionRecordKey& key) {
                            change([i, id = key.id](csf::Trigger& value) {
                                if (i < value.actions.size()) value.actions[i].number = id;
                            });
                        });
            if (action.kind == Kind::ai_alert || action.kind == Kind::ai_combat) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(160.0F * ui_scale());
                if (edit_text_value("##mode", action.text, edited, "MOVIL_A_PARAPETO"))
                    update([edited](csf::TriggerAction& value) { value.text = edited; });
            }
            break;
        case Kind::mission_success: break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(icons::LC_X)) change([i](csf::Trigger& value) {
                if (i < value.actions.size()) value.actions.erase(value.actions.begin() + static_cast<std::ptrdiff_t>(i));
            });
        ImGui::PopID();
    }
    property_row(trigger.actions.empty() ? "Do" : "");
    if (ImGui::BeginCombo("##trigger_add_action", "Add an action...", ImGuiComboFlags_HeightLargest)) {
        for (const auto kind : {Kind::complete_objective, Kind::message, Kind::alarm, Kind::raise_event, Kind::ai_alert,
                                Kind::ai_combat, Kind::enable_ghost, Kind::disable_ghost, Kind::mission_success})
            if (unverified || csf::trigger_action_proven(kind)) {
                if (ImGui::Selectable(action_label(kind)))
                    change([kind](csf::Trigger& value) {
                        csf::TriggerAction action{kind, 0, {}};
                        if (kind == Kind::complete_objective) action.number = 1;
                        if (kind == Kind::alarm) action.number = 60;
                        if (kind == Kind::ai_alert || kind == Kind::ai_combat) action.text = "MOVIL_A_PARAPETO";
                        value.actions.push_back(action);
                    });
                unverified_mark(csf::trigger_action_proven(kind));
            }
        ImGui::EndCombo();
    } else {
        name_last_item("##trigger_add_action");
    }
}

namespace {

void trigger_card_fields(AppState& state, Lines& lines) {
    csf::Trigger trigger;
    try {
        trigger = csf::parse_trigger(lines.lines.front());
    } catch (const std::exception& error) {
        dim_text("%s", error.what());
        return;
    }
    const auto id = lines.id;
    auto others = lines.lines;
    trigger_fields(state, trigger, [&state, id, others](const std::function<void(csf::Trigger&)>& edit) {
        auto copy = others;
        auto value = csf::parse_trigger(copy.front());
        edit(value);
        copy.front() = csf::parse_op_line(csf::ops_text(value));
        std::vector<std::string> text;
        for (const auto& line : copy) text.push_back(csf::format_op_line(line));
        edit_component(state, id, text);
    }, false);
}

// The lines themselves, for anything the fields above do not cover.
void lines_editor(AppState& state, const csf::MissionComponent& component) {
    if (!ImGui::TreeNodeEx("Operation lines", ImGuiTreeNodeFlags_SpanAvailWidth)) return;
    static std::int32_t shown_for = -1;
    static std::uint64_t shown_revision = ~0ULL;
    static std::array<char, 16384> buffer{};
    if (shown_for != component.id || shown_revision != state.mission.editor->revision()) {
        std::string text;
        for (const auto& line : component.lines) text += line + '\n';
        buffer.fill('\0');
        text.copy(buffer.data(), std::min(text.size(), buffer.size() - 1));
        shown_for = component.id;
        shown_revision = state.mission.editor->revision();
    }
    ImGui::InputTextMultiline("##lines", buffer.data(), buffer.size(),
                              {-1.0F, ImGui::GetTextLineHeight() * static_cast<float>(component.lines.size() + 2)});
    if (secondary_button("Apply lines")) {
        std::vector<std::string> lines;
        std::string_view text(buffer.data());
        while (!text.empty()) {
            const auto end = text.find('\n');
            if (const auto line = text.substr(0, end); !line.empty()) lines.emplace_back(line);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        }
        edit_component(state, component.id, lines);
        shown_revision = ~0ULL;
    }
    ImGui::TreePop();
}

} // namespace

void draw_component_card(AppState& state, const MissionRecordKey& key) {
    if (const auto* owner = owning_component(state, key)) draw_component_card(state, owner->id);
}

void draw_component_card(AppState& state, const std::int32_t id) {
    if (!mission_editable(state)) return;
    const auto& list = mission_component_list(state);
    const auto found = std::ranges::find(list, id, &csf::MissionComponent::id);
    if (found == list.end()) return;
    const auto component = *found;  // edits below replace the list
    auto& editor = *state.mission.editor;
    const bool modified = csf::component_state(editor, component) == csf::ComponentState::modified;
    CardOptions options{icons::LC_COMPONENT};
    const auto title = csf::component_title(component);
    options.subtitle = modified ? "edited by hand" : "made by a recipe";
    options.help = "The recipe that made this record and the ones that go with it. Changing a value here makes "
                   "them again, in place, in one undo step; moving its points or its actor in the viewport does too.";
    if (!begin_card("##component", title.c_str(), options)) return;
    ImGui::PushID(component.id);
    if (modified) {
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::warn));
        ImGui::TextWrapped("%s Its records were edited by hand since the recipe made them.", icons::LC_TRIANGLE_ALERT);
        ImGui::PopStyleColor();
        if (secondary_button("Keep my edits"))
            apply_mission_edit(state, csf::detach_component(editor, component.id));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Detach: the records stay as they are and are edited by hand from now on.");
        ImGui::SameLine();
        if (danger_button("Regenerate"))
            if (const auto result = csf::regenerate_component(editor, component.id, component_options(state));
                apply_mission_edit(state, result))
                notify_undoable(state, "Regenerated " + title + "; the hand edits are gone");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Make the records again from the recipe, dropping the hand edits (undo brings them back).");
        ImGui::Spacing();
    }
    Lines lines{state, component.id, {}};
    try {
        for (const auto& line : component.lines) lines.lines.push_back(csf::parse_op_line(line));
    } catch (const std::exception& error) {
        dim_text("The component's lines do not parse: %s", error.what());
    }
    // Hand edits are never overwritten by a parameter change: keep or regenerate first.
    ImGui::BeginDisabled(modified);
    if (!lines.lines.empty() && begin_properties("##component_rows")) {
        const auto op = component.op();
        if (op == "guard-patrol" || op == "guard-idle") guard_fields(state, lines);
        else if (op == "animal-patrol") animal_fields(lines);
        else if (op == "cover-group") points_row(lines, 0, 1);
        else if (op == "walk-grid") {
            property_row("Spacing", "Centimetres between neighbouring points.");
            float spacing{};
            if (edit_float_value("##spacing", number_of(lines.lines[0], "spacing", 1000.0F), spacing, 10.0F, "%.0f cm") &&
                spacing >= 100.0F)
                lines.set(0, "spacing", csf::op_number(std::round(spacing)));
        } else if (op == "objective" || op == "objectives") objective_fields(state, lines);
        else if (op == "shot" || op == "intro") intro_fields(lines);
        else if (op == "kit" || op == "equipment") equipment_fields(state, lines);
        else if (op == "tips") tips_fields(lines);
        else if (op == "trigger") trigger_card_fields(state, lines);
        end_properties();
    }
    lines_editor(state, component);
    ImGui::EndDisabled();
    if (secondary_button(component.op() == "trigger" ? "Convert to script" : "Detach"))
        apply_mission_edit(state, csf::detach_component(editor, component.id));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Keep the records and forget the recipe: they become ordinary records.");
    ImGui::SameLine();
    if (danger_button("Delete")) {
        if (const auto result = csf::delete_component(editor, component.id); apply_mission_edit(state, result)) {
            notify_undoable(state, result.message);
            state.preview.clear_mission_selection();
            state.selection = {};
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Delete the component and every record it made (Ctrl+Z brings them back).");
    ImGui::PopID();
    end_card();
}

void draw_component_cards(AppState& state, const std::initializer_list<std::string_view> ops) {
    if (!mission_editable(state)) return;
    std::vector<std::int32_t> ids;
    for (const auto& component : mission_component_list(state))
        if (std::ranges::find(ops, component.op()) != ops.end()) ids.push_back(component.id);
    for (const auto id : ids) draw_component_card(state, id);
}

} // namespace rwsman::ui
