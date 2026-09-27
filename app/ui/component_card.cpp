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
#include "rwsman/entity_kind.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/pickers.hpp"
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

// A guard's idle loop, "<anim>[:<min>-<max>],...": the animations it plays
// in turn, each once or a random number of times, picked by name and
// previewed on the guard.
void idle_loop_rows(AppState& state, Lines& lines) {
    struct Step {
        std::int32_t animation{};
        std::string times;  // "" once, else "<min>-<max>"
    };
    std::vector<Step> steps;
    const auto loop = lines.lines.front().get("loop");
    for (std::size_t at = 0; at < loop.size();) {
        const auto comma = std::min(loop.find(',', at), loop.size());
        const auto item = loop.substr(at, comma - at);
        at = comma + 1;
        const auto colon = item.find(':');
        try {
            steps.push_back({std::stoi(item.substr(0, colon)), colon == std::string::npos ? "" : item.substr(colon + 1)});
        } catch (const std::exception&) {
        }
    }
    const auto write = [&](const std::vector<Step>& values) {
        std::string text;
        for (const auto& step : values)
            text += (text.empty() ? "" : ",") + std::to_string(step.animation) + (step.times.empty() ? "" : ":" + step.times);
        if (!text.empty()) lines.set(0, "loop", text);
    };
    const auto actor = integer_of(lines.lines.front(), "id", 0);
    // Clips for another weapon stance are left out (a rifle's SF* for an MP40 guard).
    const std::optional<std::int32_t> soldier = integer_of(lines.lines.front(), "class", 0);
    for (std::size_t k = 0; k < steps.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        property_row(k == 0 ? "Idle animations" : "",
                     k == 0 ? "What the guard plays at its post, in turn, over and over. Times: 1 plays it once; "
                              "2-4 plays it a random number of times in that range. The play button previews it "
                              "on the guard."
                            : nullptr);
        const float buttons = ImGui::GetFrameHeightWithSpacing() * 2.0F + 64.0F * ui_scale();
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - buttons, 80.0F));
        const auto id = "##idle_anim_" + std::to_string(k + 1);
        if (const auto picked = animation_combo(state, id.c_str(), steps[k].animation, true, soldier);
            picked && *picked != steps[k].animation) {
            auto changed = steps;
            changed[k].animation = *picked;
            write(changed);
        }
        ImGui::SameLine(0.0F, 2.0F);
        animation_preview_button(state, ("##preview_idle_" + std::to_string(k + 1)).c_str(), actor, steps[k].animation);
        ImGui::SameLine(0.0F, 2.0F);
        ImGui::SetNextItemWidth(56.0F * ui_scale());
        std::string times;
        if (edit_text_value(("##idle_times_" + std::to_string(k + 1)).c_str(), steps[k].times.empty() ? "1" : steps[k].times,
                            times, "1")) {
            auto changed = steps;
            changed[k].times = times == "1" || times.empty() ? "" : times;
            if (!changed[k].times.empty() && changed[k].times.find('-') == std::string::npos)
                changed[k].times += "-" + changed[k].times;  // "3" plays it three times
            write(changed);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Times: 1, or a range such as 2-4");
        ImGui::SameLine(0.0F, 2.0F);
        ImGui::BeginDisabled(steps.size() <= 1);
        if (ImGui::SmallButton(icons::LC_X)) {
            auto changed = steps;
            changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(k));
            write(changed);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    property_row(steps.empty() ? "Idle animations" : "");
    if (const auto picked = animation_combo(state, "##idle_add", 0, true, soldier)) {
        auto changed = steps;
        changed.push_back({*picked, ""});
        write(changed);
    }
}

// What starts a behaviour: INIT (the mission start, or the intro), or a
// mission event, so a vehicle's passengers take their posts when it arrives.
void start_row(AppState& state, Lines& lines) {
    property_row("Starts", "When the behaviour begins: when the mission starts (the intro raises INIT for it), or "
                           "when a mission event is raised, such as an arrival another script announces with "
                           "SEND_EVENT. Until then the actor stands where it was placed.");
    const auto current = lines.lines.front().get("start");
    if (const auto picked = event_combo(state, "##start", current, "When the mission starts");
        picked && *picked != current) {
        if (picked->empty()) lines.lines.front().erase("start");
        else lines.lines.front().set("start", *picked);
        lines.apply();
    }
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
    for (const auto& key : {"id", "name", "class", "heading", "pitch", "portrait", "cover", "script", "script-name", "start"})
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
        idle_loop_rows(state, lines);
    }
    cover_row(state, lines, 0);
    start_row(state, lines);
}

void animal_fields(AppState& state, Lines& lines) {
    auto& line = lines.lines.front();
    property_row("Walk animation", "The animation it walks its route with.");
    const auto walk = integer_of(line, "walk", 0);
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeightWithSpacing());
    if (const auto picked = animation_combo(state, "##walk", walk, true); picked && *picked != walk)
        lines.set(0, "walk", std::to_string(*picked));
    ImGui::SameLine(0.0F, 2.0F);
    animation_preview_button(state, "##preview_walk", integer_of(line, "id", 0), walk);
    points_row(lines, 0, 1);
    start_row(state, lines);
}

// A game text of a line: the words with an authoring project (a new string
// gets an ID the line keeps), the FLI string ID without one.
void text_row(AppState& state, Lines& lines, const std::size_t index, const char* label, const char* key,
              const char* help, const std::string& suffix = {}) {
    property_row(label, help);
    const auto field = std::string("##") + key + suffix;  // "Objectives::label_1" in UI scripts
    if (auto id = game_text_field(state, field.c_str(), lines.lines[index].get(key))) lines.set(index, key, std::move(*id));
}

// The records an objective of `kind` can target, for its combo.
std::vector<std::pair<std::int32_t, std::string>> objective_targets(AppState& state, const std::string& kind) {
    std::vector<std::pair<std::int32_t, std::string>> items;
    const auto& scene = *state.mission.scene;
    if (kind == "zone") {
        for (const auto& area : scene.areas())
            if (area.id) items.emplace_back(*area.id, area.name.value_or("zone") + "  (zone " + std::to_string(*area.id) + ")");
        return items;
    }
    for (const auto& actor : scene.actors()) {
        if (!actor.id) continue;
        const auto entity = classify_mission_actor(scene, state.mission.objects.get(), actor);
        const bool fits = kind == "use" ? entity == EntityKind::usable || entity == EntityKind::prop
                                        : entity == EntityKind::enemy || entity == EntityKind::animal ||
                                              entity == EntityKind::vehicle;
        if (fits)
            items.emplace_back(*actor.id, actor.name.value_or("actor") + "  (" + entity_kind_name(entity) + " " +
                                              std::to_string(*actor.id) + ")");
    }
    return items;
}

// The objectives, each with its text, what completes it and its message,
// and the success rule (E5, Phase 7): typed text, targets picked by name.
void objective_fields(AppState& state, Lines& lines) {
    std::vector<std::size_t> objectives;
    for (std::size_t i = 0; i < lines.lines.size(); ++i)
        if (lines.lines[i].op == "objective") objectives.push_back(i);
    for (std::size_t k = 0; k < objectives.size(); ++k) {
        const auto i = objectives[k];
        auto& line = lines.lines[i];
        ImGui::PushID(static_cast<int>(i));
        const auto suffix = "_" + std::to_string(k + 1);  // "Objectives::kind_2" in UI scripts
        const auto kind = line.get("kind", "zone");
        property_row(("Objective " + std::to_string(k + 1)).c_str());
        int importance = line.get("secondary", "0") != "0" ? 1 : 0;
        static constexpr const char* importances[]{"Primary: needed to win", "Secondary: optional"};
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeightWithSpacing(), 80.0F));
        if (ImGui::Combo(("##secondary" + suffix).c_str(), &importance, importances, IM_ARRAYSIZE(importances))) {
            if (importance == 1) line.set("secondary", "1");
            else line.erase("secondary");
            lines.apply();
        }
        name_last_item(("##secondary" + suffix).c_str());
        ImGui::SameLine(0.0F, 2.0F);
        if (ImGui::SmallButton((std::string(icons::LC_X) + "##remove" + suffix).c_str())) {
            // The objectives after it move up a number (triggers that name
            // objective numbers are not renumbered).
            lines.lines.erase(lines.lines.begin() + static_cast<std::ptrdiff_t>(i));
            int n = 0;
            for (auto& value : lines.lines)
                if (value.op == "objective") value.set("n", std::to_string(++n));
            if (n > 0) lines.apply();
            ImGui::PopID();
            return;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Remove this objective (Ctrl+Z brings it back)");
        text_row(state, lines, i, "Shown as", "label", "The objective as the objectives screen lists it.", suffix);
        property_row("Done when", "What completes it. It completes itself: no trigger is needed for that.");
        static constexpr const char* kinds[]{"The player reaches a zone", "An actor is killed", "The player uses an object"};
        int choice = kind == "zone" ? 0 : kind == "kill" ? 1 : 2;
        const bool kind_changed = ImGui::Combo(("##kind" + suffix).c_str(), &choice, kinds, IM_ARRAYSIZE(kinds));
        name_last_item(("##kind" + suffix).c_str());
        if (kind_changed) {
            const std::string wanted = choice == 0 ? "zone" : choice == 1 ? "kill" : "use";
            line.set("kind", wanted);
            // A zone's ID means nothing as an actor's: aim at the first fitting record.
            if (const auto target = default_objective_target(state, wanted)) line.set("target", std::to_string(*target));
            lines.apply();
            ImGui::PopID();
            return;
        }
        property_row(kind == "zone" ? "Zone" : kind == "kill" ? "Actor" : "Object",
                     "Pick it from the list, or with the eyedropper in the viewport or the Outliner.");
        const auto target = integer_of(line, "target", 0);
        const auto items = objective_targets(state, kind);
        std::string current = "(missing: pick one)";
        for (const auto& [value, label] : items)
            if (value == target) current = label;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeightWithSpacing(), 80.0F));
        if (const auto picked = filtered_combo(("##target" + suffix).c_str(), current, items); picked && *picked != target)
            lines.set(i, "target", std::to_string(*picked));
        ImGui::SameLine(0.0F, 2.0F);
        const std::int32_t component = lines.id;
        auto copy = lines.lines;
        pick_button(state, ("##pick_target" + suffix).c_str(),
                    {kind == "zone" ? MissionRecordKey::Kind::area : MissionRecordKey::Kind::actor},
                    "Pick the objective's target",
                    [&state, component, i, copy](const MissionRecordKey& key) mutable {
                        copy[i].set("target", std::to_string(key.id));
                        std::vector<std::string> text;
                        for (const auto& value : copy) text.push_back(csf::format_op_line(value));
                        edit_component(state, component, text);
                    });
        if (kind == "use")
            text_row(state, lines, i, "Prompt", "prompt", "The label the object shows the player (\"Sabotage\").", suffix);
        text_row(state, lines, i, "Message", "done", "Shown on screen when it is completed.", suffix);
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (const auto end = lines.find("objectives")) {
        text_row(state, lines, *end, "Success", "success",
                 "When every primary objective is complete: this message, then the mission is won.");
        property_row("Win after", "Seconds between the success message and the end of the mission.");
        float pause{};
        if (edit_float_value("##success_pause", number_of(lines.lines[*end], "pause", 4.0F), pause, 0.1F, "%.1f s") &&
            pause >= 0.0F)
            lines.set(*end, "pause", csf::op_number(std::round(pause * 10.0F) / 10.0F));
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

// The players' starting kits: who, which weapons (with their ammunition),
// which one is in hand and a disguise; weapons picked by name (Phase 7).
void equipment_fields(AppState& state, Lines& lines) {
    struct Weapon {
        std::int32_t id{};
        std::string ammo;  // "<loaded>/<carried>" or ""
    };
    const auto players = player_items(state);
    const auto weapons = weapon_items(state);
    int number = 0;
    for (std::size_t i = 0; i < lines.lines.size(); ++i) {
        auto& line = lines.lines[i];
        if (line.op != "kit") continue;
        ImGui::PushID(static_cast<int>(i));
        const auto suffix = "_" + std::to_string(++number);
        property_row("Player", "The commando who starts with this kit.");
        const auto actor = integer_of(line, "actor", 0);
        std::string current = "actor " + std::to_string(actor);
        for (const auto& [value, label] : players)
            if (value == actor) current = label;
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeightWithSpacing(), 80.0F));
        if (const auto picked = filtered_combo(("##kit_actor" + suffix).c_str(), current, players); picked && *picked != actor)
            lines.set(i, "actor", std::to_string(*picked));
        ImGui::SameLine(0.0F, 2.0F);
        if (ImGui::SmallButton((std::string(icons::LC_X) + "##remove_kit" + suffix).c_str())) {
            lines.lines.erase(lines.lines.begin() + static_cast<std::ptrdiff_t>(i));
            if (lines.find("kit")) lines.apply();
            else state.warn("A kit list needs one kit: delete the component to remove the last one");
            ImGui::PopID();
            return;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Remove this kit");
        std::vector<Weapon> kit;
        const auto list = line.get("weapons");
        for (std::size_t at = 0; at < list.size();) {
            const auto comma = std::min(list.find(',', at), list.size());
            const auto item = list.substr(at, comma - at);
            at = comma + 1;
            const auto sign = item.find('@');
            try {
                kit.push_back({std::stoi(item.substr(0, sign)), sign == std::string::npos ? "" : item.substr(sign + 1)});
            } catch (const std::exception&) {
            }
        }
        const auto write = [&](const std::vector<Weapon>& values) {
            std::string text;
            for (const auto& weapon : values)
                text += (text.empty() ? "" : ",") + std::to_string(weapon.id) + (weapon.ammo.empty() ? "" : "@" + weapon.ammo);
            if (!text.empty()) lines.set(i, "weapons", text);
        };
        for (std::size_t w = 0; w < kit.size(); ++w) {
            ImGui::PushID(static_cast<int>(w));
            property_row(w == 0 ? "Weapons" : "",
                         w == 0 ? "What the commando carries. Ammo: <in the weapon>/<carried>, empty for the "
                                  "game's default."
                                : nullptr);
            ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 80.0F * ui_scale(), 80.0F));
            if (const auto picked = filtered_combo(("##weapon" + suffix + "_" + std::to_string(w + 1)).c_str(),
                                                   weapon_label(state, kit[w].id), weapons);
                picked && *picked != kit[w].id) {
                auto changed = kit;
                changed[w].id = *picked;
                write(changed);
            }
            ImGui::SameLine(0.0F, 2.0F);
            ImGui::SetNextItemWidth(54.0F * ui_scale());
            std::string ammo;
            if (edit_text_value(("##ammo" + suffix + "_" + std::to_string(w + 1)).c_str(), kit[w].ammo, ammo, "ammo")) {
                auto changed = kit;
                changed[w].ammo = ammo;
                write(changed);
            }
            ImGui::SameLine(0.0F, 2.0F);
            ImGui::BeginDisabled(kit.size() <= 1);
            if (ImGui::SmallButton(icons::LC_X)) {
                auto changed = kit;
                changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(w));
                write(changed);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        property_row(kit.empty() ? "Weapons" : "");
        if (const auto picked = filtered_combo(("##add_weapon" + suffix).c_str(), std::string("Add a weapon..."), weapons)) {
            auto changed = kit;
            changed.push_back({*picked, ""});
            write(changed);
        }
        property_row("In hand", "The weapon selected when the mission starts.");
        std::vector<std::pair<std::int32_t, std::string>> held{{0, "(nothing)"}};
        for (const auto& weapon : kit) held.emplace_back(weapon.id, weapon_label(state, weapon.id));
        const auto selected = integer_of(line, "select", 0);
        if (const auto picked = filtered_combo(("##select" + suffix).c_str(),
                                               selected ? weapon_label(state, selected) : std::string("(nothing)"), held);
            picked && *picked != selected) {
            if (*picked == 0) line.erase("select");
            else line.set("select", std::to_string(*picked));
            lines.apply();
        }
        property_row("Disguise", "A uniform the commando starts wearing (the spy): the class whose look it takes.");
        const auto disguise = integer_of(line, "disguise", 0);
        auto uniforms = character_class_items(state);
        uniforms.insert(uniforms.begin(), {0, "(none)"});
        if (const auto picked = filtered_combo(("##disguise" + suffix).c_str(),
                                               disguise ? class_label(state, disguise) : std::string("(none)"), uniforms);
            picked && *picked != disguise) {
            if (*picked == 0) line.erase("disguise");
            else line.set("disguise", std::to_string(*picked));
            lines.apply();
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
}

// The mission tips, typed (or FLI IDs without a project), in order.
void tips_fields(AppState& state, Lines& lines) {
    auto& line = lines.lines.front();
    std::vector<std::string> tips;
    const auto list = line.get("tips");
    for (std::size_t at = 0; at < list.size();) {
        const auto comma = std::min(list.find(',', at), list.size());
        if (comma > at) tips.push_back(list.substr(at, comma - at));
        at = comma + 1;
    }
    const auto write = [&](const std::vector<std::string>& values) {
        std::string text;
        for (const auto& tip : values) text += (text.empty() ? "" : ",") + tip;
        if (!text.empty()) lines.set(0, "tips", text);
    };
    for (std::size_t k = 0; k < tips.size(); ++k) {
        ImGui::PushID(static_cast<int>(k));
        property_row(("Tip " + std::to_string(k + 1)).c_str());
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeightWithSpacing(), 80.0F));
        if (auto id = game_text_field(state, ("##tip_" + std::to_string(k + 1)).c_str(), tips[k])) {
            auto changed = tips;
            changed[k] = std::move(*id);
            write(changed);
        }
        ImGui::SameLine(0.0F, 2.0F);
        ImGui::BeginDisabled(tips.size() <= 1);
        if (ImGui::SmallButton(icons::LC_X)) {
            auto changed = tips;
            changed.erase(changed.begin() + static_cast<std::ptrdiff_t>(k));
            write(changed);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
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
    case When::alerted: return "A guard is alerted";
    case When::body_found: return "A guard finds a body";
    }
    return "?";
}

const char* when_help(const When when) {
    switch (when) {
    case When::alerted:
        return "Any German soldier turning alert or starting to fight, as Convoy's camp alarm does. A guard who is "
               "shot at counts too, so a quiet kill in his sight can still set it off.";
    case When::body_found:
        return "A watched soldier sees another watched soldier dead. There is no such event in the game: the "
               "trigger checks every second whether a living one sees a dead one, so the script grows with the "
               "square of the number watched (35 soldiers make about 1200 checks); only the dead cost anything "
               "while playing.";
    default: return nullptr;
    }
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
    // Room after it for the "unverified" mark and the help icon.
    if (!csf::trigger_when_proven(trigger.when) || when_help(trigger.when))
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 110.0F * ui_scale(), 80.0F));
    if (ImGui::BeginCombo("##trigger_when", when_label(trigger.when))) {
        for (const auto when : {When::mission_start, When::enter_zone, When::actor_killed, When::object_used, When::timer,
                                When::alerted, When::body_found, When::event})
            if (unverified || csf::trigger_when_proven(when) || when == trigger.when) {
                if (ImGui::Selectable(when_label(when), when == trigger.when))
                    change([when, enemies = enemy_actors(state)](csf::Trigger& value) {
                        value.when = when;
                        // Everyone can find everyone, until the author narrows it.
                        if (when == When::body_found && value.watch.empty()) value.watch = enemies;
                    });
                if (const auto* help = when_help(when); help && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                    ImGui::SetTooltip("%s", help);
                unverified_mark(csf::trigger_when_proven(when));
            }
        ImGui::EndCombo();
    } else {
        name_last_item("##trigger_when");
    }
    unverified_mark(csf::trigger_when_proven(trigger.when));
    if (const auto* help = when_help(trigger.when)) help_marker(help);
    if (trigger.when == When::body_found) {
        // Who can find whom: every enemy by default; soldiers added later are
        // not watched until the author says so.
        const auto enemies = enemy_actors(state);
        std::size_t missing = 0;
        for (const auto enemy : enemies)
            if (std::ranges::find(trigger.watch, enemy) == trigger.watch.end()) ++missing;
        property_row("Watched", "The soldiers who can find a body, and whose bodies can be found.");
        ImGui::Text("%zu soldiers", trigger.watch.size());
        if (missing > 0) {
            ImGui::SameLine();
            token_text(Token::warn, "%zu not watched", missing);
            property_row("");
            if (ImGui::SmallButton("Watch every enemy"))
                change([enemies](csf::Trigger& value) { value.watch = enemies; });
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Watch all %zu enemy soldiers of the mission.", enemies.size());
        }
        if (trigger.watch.size() < 2) {
            property_row("");
            token_text(Token::warn, "Watch at least two soldiers.");
        }
    }
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
    const auto objectives = objective_items(state);
    const auto objective_label = [&](const std::int32_t number) {
        for (const auto& [value, label] : objectives)
            if (value == number) return label;
        return "objective " + std::to_string(number);
    };
    if (trigger.if_objective) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (const auto picked = filtered_combo("##trigger_if_number", objective_label(trigger.if_objective->first), objectives))
            change([number = *picked](csf::Trigger& value) { value.if_objective->first = number; });
    }
    std::optional<std::int32_t> completes_itself;
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
            if (const auto picked = filtered_combo("##objective", objective_label(action.number), objectives))
                update([number = *picked](csf::TriggerAction& value) { value.number = number; });
            if (std::ranges::find(objectives, action.number, &std::pair<std::int32_t, std::string>::first) !=
                objectives.end())
                completes_itself = action.number;
            break;
        case Kind::alarm:
            if (edit_int_value("##number", action.number, number) && number >= 0)
                update([number](csf::TriggerAction& value) { value.number = number; });
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Seconds the alarm sounds");
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
    if (completes_itself) {
        property_row("");
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::warn));
        ImGui::TextWrapped("%s Objective %d already completes itself (see Objectives). Completing it here too can "
                           "stop the mission from being won; use this only for an objective nothing else completes.",
                           icons::LC_TRIANGLE_ALERT, *completes_itself);
        ImGui::PopStyleColor();
    }
    property_row(trigger.actions.empty() ? "Do" : "");
    if (ImGui::BeginCombo("##trigger_add_action", "Add an action...", ImGuiComboFlags_HeightLargest)) {
        for (const auto kind : {Kind::message, Kind::alarm, Kind::ai_alert, Kind::ai_combat, Kind::enable_ghost,
                                Kind::disable_ghost, Kind::raise_event, Kind::mission_success, Kind::complete_objective})
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
    if (modified) options.subtitle = "edited by hand";
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
        else if (op == "animal-patrol") animal_fields(state, lines);
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
        else if (op == "tips") tips_fields(state, lines);
        else if (op == "trigger") trigger_card_fields(state, lines);
        end_properties();
    }
    ImGui::EndDisabled();
    // What most edits never need: the recipe's own lines, and turning it
    // into ordinary records (Delete of a whole list is here too, away from
    // its Add button).
    if (!ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::PopID();
        end_card();
        return;
    }
    ImGui::BeginDisabled(modified);
    lines_editor(state, component);
    ImGui::EndDisabled();
    if (secondary_button(component.op() == "trigger" ? "Convert to script" : "Detach"))
        apply_mission_edit(state, csf::detach_component(editor, component.id));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Keep the records and forget the recipe: they become ordinary records.");
    ImGui::SameLine();
    if (danger_button(("Delete " + csf::component_kind_title(component.op())).c_str())) {
        if (const auto result = csf::delete_component(editor, component.id); apply_mission_edit(state, result)) {
            notify_undoable(state, result.message);
            state.preview.clear_mission_selection();
            state.selection = {};
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Delete the component and every record it made (Ctrl+Z brings them back).");
    ImGui::TreePop();
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
