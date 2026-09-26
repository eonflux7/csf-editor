// The Outliner (docs/plans/editor-ux-redesign.md, B2): the mission's content
// grouped by what it is to an author (players, enemies, props, zones, routes,
// objectives...), not by where it is stored. Rows select their record, double
// clicks frame it, and the eye and lock toggles hide a record in the viewport
// or keep it from being picked there.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "commands.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "viewport_tools.hpp"
#include "rwsman/entity_kind.hpp"
#include "csf/mission_components.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <functional>
#include <cctype>
#include <map>
#include <set>

namespace rwsman::ui {
namespace {

struct Row {
    EntityKind kind{};
    MissionRecordKey key;
    std::string label, detail;
    std::vector<Row> children;  // a route's points
    bool problem{};
    std::string tooltip;
    // Rows that are not scene records: an objective, a project placement.
    std::function<void()> activate;
};

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Rows of the open mission, grouped by kind in the order an author works.
std::vector<std::pair<EntityKind, std::vector<Row>>> build_rows(AppState& state) {
    std::map<EntityKind, std::vector<Row>> groups;
    const auto& scene = *state.mission.scene;
    const auto* objects = state.mission.objects.get();

    std::set<std::pair<int, std::int32_t>> problem_subjects;
    for (const auto& problem : state.problems)
        if (problem.severity != Problem::Severity::note)
            problem_subjects.emplace(static_cast<int>(problem.subject.kind), problem.subject.id);
    const auto has_problem = [&](const ProblemSubject::Kind kind, const std::int32_t id) {
        return problem_subjects.contains({static_cast<int>(kind), id});
    };

    std::map<std::int32_t, EntityKind> actor_kinds;
    for (const auto& actor : scene.actors()) {
        if (!actor.id) continue;
        const auto facts = class_facts(objects, actor.class_id);
        const auto kind = classify_mission_actor(scene, objects, actor);
        actor_kinds[*actor.id] = kind;
        Row row{kind, {MissionRecordKey::Kind::actor, *actor.id, 0}, actor.name.value_or("(unnamed)")};
        row.detail = facts.known ? facts.name : "class " + std::to_string(actor.class_id.value_or(0));
        row.problem = has_problem(ProblemSubject::Kind::actor, *actor.id);
        row.tooltip = "Actor " + std::to_string(*actor.id) + ", class " + std::to_string(actor.class_id.value_or(0)) +
                      (facts.type ? " (" + *facts.type + ")" : std::string{});
        groups[kind].push_back(std::move(row));
    }

    // Navigation groups; a group only helper actors stand on is a camera path.
    std::map<std::int32_t, std::pair<int, int>> standing;  // group -> (helpers, others)
    for (const auto& actor : scene.actors())
        if (actor.group && actor.id) {
            auto& [helpers, others] = standing[*actor.group];
            (actor_kinds[*actor.id] == EntityKind::helper ? helpers : others) += 1;
        }
    for (const auto& group : scene.navigation()) {
        if (!group.id) continue;
        const auto found = standing.find(*group.id);
        const bool helper_path = found != standing.end() && found->second.first > 0 && found->second.second == 0;
        const auto kind = classify_nav_group(group.type, group.name.value_or(""), helper_path);
        Row row{kind, {MissionRecordKey::Kind::nav_group, *group.id, 0}, group.name.value_or("(unnamed)")};
        row.detail = std::to_string(group.points.size()) + " points";
        row.problem = has_problem(ProblemSubject::Kind::nav_group, *group.id);
        for (const auto& point : group.points)
            if (point.id)
                row.children.push_back({kind,
                                        {MissionRecordKey::Kind::nav_point, *group.id, *point.id},
                                        "Point " + std::to_string(*point.id)});
        groups[kind].push_back(std::move(row));
    }
    for (const auto& area : scene.areas()) {
        if (!area.id) continue;
        Row row{EntityKind::zone, {MissionRecordKey::Kind::area, *area.id, 0}, area.name.value_or("(unnamed)")};
        row.detail = std::to_string(area.points.size()) + " corners";
        row.problem = has_problem(ProblemSubject::Kind::area, *area.id);
        groups[EntityKind::zone].push_back(std::move(row));
    }
    for (const auto& dummy : scene.dummies())
        if (dummy.id)
            groups[EntityKind::marker].push_back(
                {EntityKind::marker, {MissionRecordKey::Kind::dummy, *dummy.id, 0},
                 dummy.name && !dummy.name->empty() ? *dummy.name : "Marker " + std::to_string(*dummy.id)});
    for (const auto& light : scene.lights())
        if (light.id)
            groups[EntityKind::light].push_back(
                {EntityKind::light, {MissionRecordKey::Kind::light, *light.id, 0},
                 light.name && !light.name->empty() ? *light.name : "Light " + std::to_string(*light.id)});
    for (const auto& effect : scene.effects())
        if (effect.id)
            groups[EntityKind::effect].push_back({EntityKind::effect,
                                                  {MissionRecordKey::Kind::effect, *effect.id, 0},
                                                  effect.name.value_or("Effect " + std::to_string(*effect.id))});

    // Project placements (the map's buildings and props).
    if (const auto* project = state.authoring.project.get())
        for (std::size_t index = 0; index < project->placements.size(); ++index) {
            const auto& placement = project->placements[index];
            const auto kind =
                placement.kind == csf::ProjectPlacement::Kind::prop ? EntityKind::vegetation : EntityKind::building;
            Row row{kind, {MissionRecordKey::Kind::placement, static_cast<std::int32_t>(index), 0}, placement.id,
                    placement.kind == csf::ProjectPlacement::Kind::building ? placement.asset
                    : placement.kind == csf::ProjectPlacement::Kind::piece  ? "donor piece"
                                                                            : "donor prop"};
            row.problem = std::ranges::any_of(state.problems, [&](const Problem& problem) {
                return problem.subject.kind == ProblemSubject::Kind::placement && problem.subject.text == placement.id;
            });
            row.tooltip = "Placed by the project and built into the map: moving or deleting it rebuilds the map.";
            groups[kind].push_back(std::move(row));
        }

    // Objectives, from the mission flow.
    if (const auto* flow = mission_flow(state))
        for (const auto& objective : flow->objectives()) {
            Row row{EntityKind::objective, {}, std::to_string(objective.number) + "  " + text_label(state, objective.label)};
            row.detail = objective.secondary ? (*objective.secondary ? "secondary" : "primary") : "";
            row.activate = [&state] { show_panel(state, Panel::objectives); };
            groups[EntityKind::objective].push_back(std::move(row));
        }

    // What a component made nests under it: a patrol's route under its guard,
    // the intro's cameras, targets and paths under one row (B2).
    const auto take = [&](const MissionRecordKey& key) -> std::optional<Row> {
        for (auto& [kind, rows] : groups)
            for (auto it = rows.begin(); it != rows.end(); ++it)
                if (it->key == key) {
                    auto row = std::move(*it);
                    rows.erase(it);
                    return row;
                }
        return std::nullopt;
    };
    const auto find = [&](const MissionRecordKey& key) -> Row* {
        for (auto& [kind, rows] : groups)
            for (auto& row : rows)
                if (row.key == key) return &row;
        return nullptr;
    };
    const auto key_of = [](const csf::MissionRecordId& record) {
        using Type = csf::MissionRecordId::Type;
        switch (record.type) {
        case Type::actor: return MissionRecordKey{MissionRecordKey::Kind::actor, record.id, 0};
        case Type::navigation_group: return MissionRecordKey{MissionRecordKey::Kind::nav_group, record.id, 0};
        case Type::dummy: return MissionRecordKey{MissionRecordKey::Kind::dummy, record.id, 0};
        case Type::area: return MissionRecordKey{MissionRecordKey::Kind::area, record.id, 0};
        default: return MissionRecordKey{};
        }
    };
    for (const auto& component : mission_component_list(state)) {
        const auto op = component.op();
        const auto title = csf::component_title(component);
        if (op == "shot" || op == "intro") {
            Row intro{EntityKind::camera_path, {}, "Intro cutscene"};
            for (const auto& record : component.owns)
                if (const auto key = key_of(record); key.kind != MissionRecordKey::Kind::none)
                    if (auto row = take(key)) intro.children.push_back(std::move(*row));
            intro.detail = std::to_string(std::ranges::count(component.lines, std::string("shot"), [](const std::string& line) {
                               return line.substr(0, line.find(' '));
                           })) + " shots";
            intro.tooltip = "The intro's cameras, targets and paths; edit the shots in the Intro cutscene tab.";
            intro.activate = [&state] { show_panel(state, Panel::timeline); };
            groups[EntityKind::camera_path].push_back(std::move(intro));
            continue;
        }
        const auto main = std::ranges::find(component.owns, csf::MissionRecordId::Type::actor, &csf::MissionRecordId::type);
        Row* owner = main != component.owns.end() ? find(key_of(*main)) : nullptr;
        for (const auto& record : component.owns) {
            const auto key = key_of(record);
            if (key.kind == MissionRecordKey::Kind::none) continue;
            if (auto* row = find(key); row && !owner) {
                row->tooltip = "Made by " + title + ": edit it in Properties";
                continue;
            }
            if (owner && key != owner->key)
                if (auto row = take(key)) {
                    row->children.clear();
                    owner->children.push_back(std::move(*row));
                }
        }
        if (owner) {
            owner->detail += op == "guard-patrol"    ? "  ·  patrol"
                             : op == "guard-idle"    ? "  ·  at a post"
                             : op == "animal-patrol" ? "  ·  patrol"
                                                     : "";
            owner->tooltip = "Made by " + title + ": edit it in Properties";
        }
    }

    constexpr std::array order{
        EntityKind::objective, EntityKind::player,     EntityKind::enemy,   EntityKind::animal,
        EntityKind::vehicle,   EntityKind::usable,     EntityKind::pickup,  EntityKind::prop,
        EntityKind::building,  EntityKind::vegetation, EntityKind::zone,    EntityKind::route,
        EntityKind::cover,     EntityKind::walk_grid,  EntityKind::marker,  EntityKind::camera_path,
        EntityKind::helper,    EntityKind::light,      EntityKind::effect,  EntityKind::unresolved,
    };
    std::vector<std::pair<EntityKind, std::vector<Row>>> result;
    for (const auto kind : order)
        if (auto found = groups.find(kind); found != groups.end() && !found->second.empty())
            result.emplace_back(kind, std::move(found->second));
    return result;
}

bool contains(const std::vector<MissionRecordKey>& keys, const MissionRecordKey& key) {
    return std::ranges::find(keys, key) != keys.end();
}

void toggle(std::vector<MissionRecordKey>& keys, const MissionRecordKey& key) {
    if (const auto found = std::ranges::find(keys, key); found != keys.end())
        keys.erase(found);
    else
        keys.push_back(key);
}

// A small icon toggle drawn at the right edge of a row. Returns true when clicked.
bool row_toggle(const char* id, const char* icon, const bool active, const char* tooltip, const float x,
                const float y) {
    ImGui::SetCursorScreenPos({x, y});
    ImGui::PushStyleColor(ImGuiCol_Button, transparent());
    ImGui::PushStyleColor(ImGuiCol_Text, color(active ? Token::accent : Token::text_dim));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2.0F, 0.0F});
    const bool clicked = ImGui::SmallButton((std::string(icon) + "##" + id).c_str());
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

void draw_row(AppState& state, const Row& row, const float indent) {
    const bool record = row.key.kind != MissionRecordKey::Kind::none;
    const auto selected_key = selected_mission_record(state);
    const bool selected = record && (selected_key == row.key || contains(state.ui.selection_extra, row.key));
    const bool hidden = record && contains(state.ui.hidden_records, row.key);
    const bool locked = record && contains(state.ui.locked_records, row.key);
    const float scale = ui_scale();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float right = start.x + ImGui::GetContentRegionAvail().x;

    ImGui::PushID(row.label.c_str());
    ImGui::PushID(static_cast<int>(row.key.kind) * 1000003 + row.key.id * 1009 + row.key.sub_id);
    // The row's name is its ID, so UI scripts can address it ("Outliner::OFICIAL").
    const bool clicked = ImGui::Selectable(("##" + row.label).c_str(), selected,
                                           ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap);
    // The whole row, including the toggles drawn over the selectable.
    const bool hovered = ImGui::IsMouseHoveringRect(start, {right, start.y + ImGui::GetTextLineHeightWithSpacing()}) &&
                         ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    if (clicked) {
        const auto& io = ImGui::GetIO();
        if (record && (io.KeyShift || io.KeyCtrl))
            select_with_modifiers(state, row.key, io.KeyShift, io.KeyCtrl);
        else if (record)
            select_mission_record(state, row.key, ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
        else if (row.activate)
            row.activate();
        if (!record && !row.children.empty() && !state.ui.outliner_open_rows.erase(row.label))
            state.ui.outliner_open_rows.insert(row.label);
    }
    if (!row.tooltip.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", row.tooltip.c_str());
    if (record && ImGui::BeginPopupContextItem("##row_menu")) {
        if (!selected) select_mission_record(state, row.key, false);
        draw_command_menu_item(state, "view.frame_selection");
        draw_command_menu_item(state, "mission.duplicate");
        draw_command_menu_item(state, "mission.delete");
        ImGui::Separator();
        if (ImGui::MenuItem(hidden ? "Show in viewport" : "Hide in viewport")) {
            toggle(state.ui.hidden_records, row.key);
            state.ui.record_states_dirty = true;
        }
        if (ImGui::MenuItem(locked ? "Unlock" : "Lock (not pickable)")) {
            toggle(state.ui.locked_records, row.key);
            state.ui.record_states_dirty = true;
        }
        ImGui::Separator();
        draw_command_menu_item(state, "mission.references");
        draw_command_menu_item(state, "view.workspace.scene");
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos({start.x + indent, start.y});
    ImGui::PushStyleColor(ImGuiCol_Text, kind_color(row.kind, hidden ? 0.4F : 1.0F));
    ImGui::TextUnformatted(kind_icon(row.kind));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, color(hidden ? Token::text_dim : selected ? Token::accent : Token::text));
    ImGui::TextUnformatted(row.label.c_str());
    ImGui::PopStyleColor();
    if (!row.detail.empty()) {
        ImGui::SameLine();
        ImGui::PushFont(font(Font::caption));
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::TextUnformatted(row.detail.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    // Right edge: problem marker, then (for records) lock and eye.
    float x = right - 18.0F * scale;
    if (record) {
        const bool show_toggles = hovered || hidden || locked;
        if (show_toggles) {
            if (row_toggle("eye", hidden ? icons::LC_EYE_OFF : icons::LC_EYE, hidden,
                           hidden ? "Show in the viewport" : "Hide in the viewport", x, start.y)) {
                toggle(state.ui.hidden_records, row.key);
                state.ui.record_states_dirty = true;
            }
            x -= 20.0F * scale;
            if (row_toggle("lock", locked ? icons::LC_LOCK : icons::LC_LOCK_OPEN, locked,
                           locked ? "Unlock: pickable in the viewport again" : "Lock: not pickable in the viewport", x,
                           start.y)) {
                toggle(state.ui.locked_records, row.key);
                state.ui.record_states_dirty = true;
            }
            x -= 20.0F * scale;
        }
    }
    if (row.problem) {
        ImGui::SetCursorScreenPos({x, start.y});
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::warn));
        ImGui::TextUnformatted(icons::LC_TRIANGLE_ALERT);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Has problems (see the Problems panel)");
    }
    ImGui::PopID();
    ImGui::PopID();
    ImGui::SetCursorScreenPos({start.x, start.y + ImGui::GetTextLineHeightWithSpacing()});
    if (selected && state.ui.focus_panel != Panel::outliner && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.5F);
}

bool matches(const Row& row, const std::string& query) {
    if (query.empty()) return true;
    return lower(row.label).find(query) != std::string::npos || lower(row.detail).find(query) != std::string::npos;
}

} // namespace

void draw_outliner(AppState& state) {
    if (!state.mission.scene) {
        empty_state(icons::LC_LIST_TREE, "Open a mission or a project; its content is listed here by what it is.");
        return;
    }
    search_input("##outliner_search", "filter by name or class", state.ui.outliner_query.data(),
                 state.ui.outliner_query.size());
    const auto query = lower(state.ui.outliner_query.data());
    const auto groups = build_rows(state);
    ImGui::BeginChild("##outliner_rows", {0, 0}, ImGuiChildFlags_None);
    for (const auto& [kind, rows] : groups) {
        std::vector<const Row*> shown;
        for (const auto& row : rows)
            if (matches(row, query) || std::ranges::any_of(row.children, [&](const Row& child) { return matches(child, query); }))
                shown.push_back(&row);
        if (shown.empty()) continue;
        ImGui::PushID(static_cast<int>(kind));
        // Helpers (cutscene cameras and targets) start folded: they belong to
        // the intro, not to the mission's cast.
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
        if (kind != EntityKind::helper && kind != EntityKind::camera_path && kind != EntityKind::vegetation)
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        // A filter opens every group; a selection made elsewhere (the viewport) opens its group once.
        const auto selected_key = selected_mission_record(state);
        const bool holds_selection = selected_key.kind != MissionRecordKey::Kind::none &&
                                     std::ranges::any_of(shown, [&](const Row* row) { return row->key == selected_key; });
        if (!query.empty() || (holds_selection && !(state.ui.outliner_opened_for == selected_key))) {
            ImGui::SetNextItemOpen(true);
            if (holds_selection) state.ui.outliner_opened_for = selected_key;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text));
        ImGui::PushFont(font(Font::sans_bold));
        const bool open = ImGui::TreeNodeEx(entity_kind_plural(kind), flags);
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushFont(font(Font::caption));
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::Text("%zu", shown.size());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (open) {
            for (const auto* row : shown) {
                draw_row(state, *row, 4.0F * ui_scale());
                // A group's points, or what a component made, when the row
                // or one of them is selected (the intro's row opens on a click).
                const auto selected = selected_mission_record(state);
                const bool child_selected = std::ranges::any_of(row->children, [&](const Row& child) {
                    return child.key == selected || (child.key.kind == MissionRecordKey::Kind::nav_group &&
                                                     selected.kind == MissionRecordKey::Kind::nav_point &&
                                                     selected.id == child.key.id);
                });
                const bool expanded = (row->key.kind != MissionRecordKey::Kind::none && selected == row->key) ||
                                      child_selected ||
                                      (row->key.kind == MissionRecordKey::Kind::none && !row->children.empty() &&
                                       state.ui.outliner_open_rows.contains(row->label));
                if (expanded)
                    for (const auto& child : row->children) draw_row(state, child, 22.0F * ui_scale());
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace rwsman::ui
