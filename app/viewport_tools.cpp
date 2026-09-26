// Editing in the viewport (docs/plans/editor-ux-redesign.md, Phase 2).
#include "viewport_tools.hpp"

#include "app_util.hpp"
#include "authoring.hpp"
#include "commands.hpp"
#include "csf/mission_components.hpp"
#include "csf/mission_ops.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "rwsman/entity_kind.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <random>
#include <set>
#include <unordered_set>

namespace rwsman {
namespace {

using Tool = GeometryPreview::EditTool;
using Kind = MissionRecordKey::Kind;
constexpr float degrees = 180.0F / std::numbers::pi_v<float>;

csf::Vec3 csf_point(const rws::Vec3 value) { return {value.x, value.y, value.z}; }
rws::Vec3 rws_point(const csf::Vec3 value) { return {value.x, value.y, value.z}; }

const char* tool_name(const Tool tool) {
    switch (tool) {
    case Tool::place: return "Place";
    case Tool::route: return "Route";
    case Tool::zone: return "Zone";
    case Tool::cover: return "Cover";
    default: return "";
    }
}

// The record's position (and heading, degrees for actors), for group moves.
std::optional<csf::Vec3> record_position(const AppState& state, const MissionRecordKey& key) {
    if (key.kind == Kind::placement) {
        const auto* project = state.authoring.project.get();
        if (!project || key.id < 0 || static_cast<std::size_t>(key.id) >= project->placements.size()) return std::nullopt;
        return csf_point(project->placements[static_cast<std::size_t>(key.id)].position);
    }
    if (!state.mission.scene) return std::nullopt;
    const auto& scene = *state.mission.scene;
    switch (key.kind) {
    case Kind::actor:
        for (const auto& actor : scene.actors())
            if (actor.id == key.id) return scene.actor_spawn_position(actor);
        break;
    case Kind::dummy:
        for (const auto& dummy : scene.dummies())
            if (dummy.id == key.id) return dummy.position;
        break;
    case Kind::light:
        for (const auto& light : scene.lights())
            if (light.id == key.id) return light.position;
        break;
    case Kind::nav_point:
        if (const auto* point = scene.navigation_point(key.id, key.sub_id)) return point->position;
        break;
    default:
        break;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> entry_of(const AppState& state, const MissionRecordKey& key) {
    if (key.kind == Kind::placement) return placement_entry_base + static_cast<std::uint32_t>(key.id);
    if (!state.mission.scene) return std::nullopt;
    return mission_record_entry(*state.mission.scene, key);
}

MissionRecordKey key_of(const AppState& state, const std::uint32_t entry) {
    if (const auto index = placement_index(state, entry))
        return {Kind::placement, static_cast<std::int32_t>(*index), 0};
    if (!state.mission.scene) return {};
    return mission_record_key(*state.mission.scene, entry);
}

std::string unique_name(const std::string& prefix, const std::set<std::string>& taken) {
    for (int n = 1;; ++n) {
        auto name = prefix + std::to_string(n);
        if (!taken.contains(name)) return name;
    }
}

// Mission edits and project placement moves for a set of records: mission
// records as one batch, placements as one project step.
void move_records(AppState& state, const std::vector<MissionRecordKey>& keys, const csf::Vec3 delta,
                  const std::optional<std::pair<MissionRecordKey, float>> heading, const std::string& label) {
    std::vector<MissionRecordKey> records, placements;
    for (const auto& key : keys) (key.kind == Kind::placement ? placements : records).push_back(key);
    if (!records.empty() && mission_editable(state)) {
        auto& editor = *state.mission.editor;
        const auto& scene = *state.mission.scene;
        const auto result = editor.batch(label, [&]() -> csf::EditResult {
            csf::EditResult last;
            for (const auto& key : records) {
                const auto position = record_position(state, key);
                if (!position) continue;
                const csf::Vec3 moved{position->x + delta.x, position->y + delta.y, position->z + delta.z};
                const bool turning = heading && heading->first == key;
                switch (key.kind) {
                case Kind::actor:
                    for (const auto& actor : scene.actors())
                        if (actor.id == key.id)
                            last = editor.set_actor_placement(
                                key.id, {moved, turning ? heading->second : actor.heading.value_or(0), actor.pitch.value_or(0)});
                    break;
                case Kind::dummy:
                    for (const auto& dummy : scene.dummies())
                        if (dummy.id == key.id)
                            last = editor.set_dummy_placement(key.id, moved,
                                                              turning ? heading->second / degrees : dummy.heading.value_or(0),
                                                              dummy.pitch.value_or(0));
                    break;
                case Kind::light:
                    last = editor.set_light(key.id, {moved, std::nullopt, std::nullopt, std::nullopt});
                    break;
                case Kind::nav_point:
                    last = editor.set_navigation_point(key.id, key.sub_id, moved,
                                                       turning ? std::optional{heading->second / degrees} : std::nullopt);
                    break;
                default:
                    continue;
                }
                if (!last.applied && !last.message.starts_with("No change")) return last;
            }
            return csf::EditResult{true, label};
        });
        if (result.applied)
            apply_mission_edit(state, result);
        else
            state.mission.applied_revision = ~std::uint64_t{};
    }
    if (!placements.empty())
        edit_authoring_project(state, label, [&](csf::AuthoringProject& project) {
            for (const auto& key : placements) {
                auto& placement = project.placements.at(static_cast<std::size_t>(key.id));
                placement.position = {placement.position.x + delta.x, placement.position.y + delta.y,
                                      placement.position.z + delta.z};
                if (heading && heading->first == key) placement.yaw_degrees = heading->second;
            }
            return true;
        });
}

void finish_sketch(AppState& state, const Tool tool) {
    auto& tools = state.tools;
    auto& points = tools.sketch;
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    const auto& scene = *state.mission.scene;
    std::set<std::string> taken;
    for (const auto& group : scene.navigation()) taken.insert(group.name.value_or(""));
    for (const auto& area : scene.areas()) taken.insert(area.name.value_or(""));
    std::int32_t id{};
    if (tool == Tool::route || tool == Tool::cover) {
        if (points.size() < (tool == Tool::route ? 2U : 1U))
            return state.warn(tool == Tool::route ? "A route needs two points" : "Click where the cover is first");
        std::vector<csf::NavPointSpec> specs;
        for (std::size_t i = 0; i < points.size(); ++i) {
            // Each route point faces the next; cover points face their facing.
            float rotation = tools.cover_facing / degrees;
            if (tool == Tool::route) {
                const auto& next = points[(i + 1) % points.size()];
                rotation = std::atan2(next.x - points[i].x, next.z - points[i].z);
            }
            specs.push_back({points[i], rotation, 0.0F});
        }
        csf::EditResult result;
        if (tool == Tool::route) {
            std::vector<std::pair<std::int32_t, std::int32_t>> links;
            for (std::size_t i = 1; i < specs.size(); ++i)
                links.emplace_back(static_cast<std::int32_t>(i), static_cast<std::int32_t>(i + 1));
            if (tools.route_loop && specs.size() > 2) links.emplace_back(static_cast<std::int32_t>(specs.size()), 1);
            result = editor.add_navigation_group(unique_name("RUTA_", taken), 0, specs, links, std::nullopt, &id);
        } else {
            csf::CoverGroup cover;
            cover.name = unique_name("COBERTURA_", taken);
            cover.points = specs;
            const auto owns = add_recipe(state, csf::ops_text(cover));
            if (!owns) return;
            if (!owns->empty()) select_after_refresh(state, {Kind::nav_group, owns->front().id, 0});
        }
        if (tool == Tool::route && apply_mission_edit(state, result)) select_after_refresh(state, {Kind::nav_group, id, 0});
    } else if (tool == Tool::zone) {
        if (points.size() < 3) return state.warn("A zone needs three corners");
        if (const auto problems = csf::area_polygon_problems(points); !problems.empty())
            return state.warn("The zone's shape is not valid: " + problems.front());
        if (apply_mission_edit(state, editor.add_area(unique_name("ZONA_", taken), tools.zone_height, points, std::nullopt, &id)))
            select_after_refresh(state, {Kind::area, id, 0});
    }
    points.clear();
    set_viewport_tool(state, Tool::select);
}

void place_at(AppState& state, const csf::Vec3 at) {
    auto& tools = state.tools;
    float heading = tools.place_heading;
    if (tools.place_random) {
        static std::mt19937 random(12345);
        heading = std::uniform_real_distribution<float>(-180.0F, 180.0F)(random);
    }
    if (!tools.place_building_asset.empty())
        place_building(state, tools.place_building_asset, at, heading);
    else if (tools.place_entry)
        place_asset(state, *tools.place_entry, at, heading);
}

// Viewport keys of the authoring tools (while the viewport is hovered and no
// text field has focus).
void tool_keys(AppState& state, const Tool tool) {
    auto& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    auto& tools = state.tools;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (!tools.sketch.empty())
            tools.sketch.clear();
        else
            set_viewport_tool(state, Tool::select);
    }
    if (tool == Tool::place) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) tools.place_heading -= 15.0F;
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) tools.place_heading += 15.0F;
        tools.place_heading = std::remainder(tools.place_heading, 360.0F);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && !tools.sketch.empty()) tools.sketch.pop_back();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        finish_sketch(state, tool);
}

// The sketch and the pointer's ground point, drawn over the viewport.
void draw_tool_preview(AppState& state, const Tool tool, ImDrawList* draw_list) {
    auto& preview = state.preview;
    const auto& tools = state.tools;
    const auto screen = [&](const csf::Vec3 point) { return preview.screen_position(rws_point(point)); };
    const float scale = ui::ui_scale();
    const EntityKind kind = tool == Tool::route   ? EntityKind::route
                            : tool == Tool::zone  ? EntityKind::zone
                            : tool == Tool::cover ? EntityKind::cover
                                                  : EntityKind::prop;
    const auto color = ui::kind_color_u32(kind);
    auto points = tools.sketch;
    const auto hover = preview.hover_ground();
    if (hover && tool != Tool::place) points.push_back(csf_point(*hover));
    const bool closed = tool == Tool::zone || (tool == Tool::route && tools.route_loop);
    if (tool == Tool::zone && points.size() >= 3) {
        std::vector<ImVec2> polygon;
        for (const auto& point : points)
            if (const auto at = screen(point)) polygon.push_back(*at);
        if (polygon.size() == points.size())
            draw_list->AddConvexPolyFilled(polygon.data(), static_cast<int>(polygon.size()), ui::kind_color_u32(kind, 0.12F));
    }
    for (std::size_t i = 0; i + 1 < points.size() + (closed && points.size() > 2 ? 1 : 0); ++i) {
        const auto a = screen(points[i]), b = screen(points[(i + 1) % points.size()]);
        if (a && b) draw_list->AddLine(*a, *b, color, 2.0F * scale);
    }
    for (std::size_t i = 0; i < tools.sketch.size(); ++i)
        if (const auto at = screen(tools.sketch[i])) {
            draw_list->AddCircleFilled(*at, 5.0F * scale, color);
            char number[8];
            std::snprintf(number, sizeof(number), "%zu", i + 1);
            draw_list->AddText({at->x + 7.0F * scale, at->y - 16.0F * scale}, color, number);
        }
    if (!hover) return;
    const auto at = screen(csf_point(*hover));
    if (!at) return;
    if (tool == Tool::place) {
        // The asset's footprint and facing where it would stand.
        const auto label = !tools.place_building_asset.empty() ? tools.place_building_asset
                           : tools.place_entry                ? tools.place_entry->name
                                                              : std::string("Pick an asset");
        const float heading = tools.place_heading / degrees;
        const auto tip = screen({hover->x + std::sin(heading) * 150.0F, hover->y, hover->z + std::cos(heading) * 150.0F});
        draw_list->AddCircle(*at, 14.0F * scale, ui::color_u32(ui::Token::accent), 24, 2.0F * scale);
        draw_list->AddCircleFilled(*at, 4.0F * scale, ui::color_u32(ui::Token::accent));
        if (tip && !tools.place_random) draw_list->AddLine(*at, *tip, ui::color_u32(ui::Token::accent), 2.0F * scale);
        draw_list->AddText({at->x + 18.0F * scale, at->y - 8.0F * scale}, ui::color_u32(ui::Token::accent), label.c_str());
    } else {
        draw_list->AddCircle(*at, 7.0F * scale, color, 16, 2.0F * scale);
    }
}

std::string tool_hint(const AppState& state, const Tool tool) {
    const auto& tools = state.tools;
    switch (tool) {
    case Tool::place:
        if (!tools.place_entry && tools.place_building_asset.empty())
            return "Place: pick an asset in the Assets panel (Esc: back to Select)";
        return "Place: click on the ground  ·  Shift keeps placing  ·  [ ] turn  ·  Esc done";
    case Tool::route:
        return "Route: click points  ·  Enter creates it  ·  Backspace removes the last  ·  Esc cancels  (" +
               std::to_string(tools.sketch.size()) + " points)";
    case Tool::zone: {
        std::string hint = "Zone: click the corners  ·  Enter creates it  ·  Backspace, Esc  (" +
                           std::to_string(tools.sketch.size()) + " corners)";
        if (tools.sketch.size() >= 3)
            if (const auto problems = csf::area_polygon_problems(tools.sketch); !problems.empty())
                hint += "  ·  " + problems.front();
        return hint;
    }
    case Tool::cover:
        return "Cover: click where soldiers take cover  ·  Enter creates the group  ·  Esc cancels";
    default:
        break;
    }
    if (state.ui.pick) return state.ui.pick->hint + "  (Esc cancels)";
    if (!state.ui.selection_extra.empty())
        return std::to_string(state.ui.selection_extra.size() + 1) +
               " selected  ·  dragging one moves them all  ·  Shift+click adds, Ctrl+click toggles";
    return {};
}

// The active tool's options, a small panel at the top of the viewport.
void draw_tool_options(AppState& state, const Tool tool, const ImVec4 canvas) {
    auto& tools = state.tools;
    const float scale = ui::ui_scale();
    ImGui::SetCursorScreenPos({canvas.x + 52.0F * scale, canvas.y + 50.0F * scale});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::color(ui::Token::bg0, 0.9F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8.0F * scale, 6.0F * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0F * scale);
    if (ImGui::BeginChild("##tool_options", {0, 0},
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(ui::font(ui::Font::sans_bold));
        ImGui::TextUnformatted(tool_name(tool));
        ImGui::PopFont();
        ImGui::SameLine();
        switch (tool) {
        case Tool::place: {
            const auto label = !tools.place_building_asset.empty() ? "building " + tools.place_building_asset
                               : tools.place_entry                ? tools.place_entry->name
                                                                  : std::string("(no asset)");
            ui::dim_text("%s", label.c_str());
            ImGui::SetNextItemWidth(110.0F * scale);
            ImGui::DragFloat("heading", &tools.place_heading, 1.0F, -180.0F, 180.0F, "%.0f deg");
            ImGui::SameLine();
            ImGui::Checkbox("random", &tools.place_random);
            ImGui::SameLine();
            ImGui::Checkbox("keep placing", &tools.place_keep);
            break;
        }
        case Tool::route:
            ImGui::Checkbox("closed loop", &tools.route_loop);
            break;
        case Tool::zone:
            ImGui::SetNextItemWidth(100.0F * scale);
            ImGui::DragFloat("height", &tools.zone_height, 5.0F, 10.0F, 5000.0F, "%.0f cm");
            break;
        case Tool::cover:
            ImGui::SetNextItemWidth(100.0F * scale);
            ImGui::DragFloat("facing", &tools.cover_facing, 1.0F, -180.0F, 180.0F, "%.0f deg");
            break;
        default:
            break;
        }
        if (tool != Tool::place) {
            ImGui::SameLine();
            ImGui::BeginDisabled(tools.sketch.empty());
            if (ui::primary_button("Create")) finish_sketch(state, tool);
            ImGui::SameLine();
            if (ui::secondary_button("Clear")) tools.sketch.clear();
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ui::icon_button("##close_tool", ui::icons::LC_X, "Back to Select (Esc)")) set_viewport_tool(state, Tool::select);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Applies a viewport selection event (Shift/Ctrl clicks, boxes) to the
// multi-selection. Runs after track_selection made the clicked record primary.
void apply_pick_event(AppState& state, const GeometryPreview::PickEvent& event) {
    auto& extra = state.ui.selection_extra;
    const auto previous = state.ui.last_primary;
    if (event.is_box) {
        std::vector<MissionRecordKey> keys;
        for (const auto entry : event.box)
            if (const auto key = key_of(state, entry); key.kind != Kind::none) keys.push_back(key);
        if (keys.empty()) return;
        if (!event.ctrl) extra.clear();
        const auto primary = previous.kind != Kind::none && event.ctrl ? previous : keys.front();
        for (const auto& key : keys)
            if (!(key == primary) && std::ranges::find(extra, key) == extra.end()) extra.push_back(key);
        if (primary.kind == Kind::placement) {
            if (const auto entry = entry_of(state, primary)) state.preview.select_mission_entry(*entry);
        } else {
            select_mission_record(state, primary, false);
        }
        return;
    }
    const auto clicked = event.entry ? key_of(state, *event.entry) : MissionRecordKey{};
    if (!event.shift && !event.ctrl) {
        extra.clear();
        return;
    }
    if (clicked.kind == Kind::none) return;
    if (event.ctrl && (std::ranges::find(extra, clicked) != extra.end() || clicked == previous)) {
        // Ctrl on a selected record takes it out, keeping the rest (the click
        // made it primary; the previous primary, or the next one, returns).
        std::erase(extra, clicked);
        auto next = previous;
        if (clicked == previous) {
            if (extra.empty()) {
                state.preview.clear_mission_selection();
                state.selection = {};
                return;
            }
            next = extra.front();
            extra.erase(extra.begin());
        }
        if (const auto entry = entry_of(state, next)) state.preview.select_mission_entry(*entry);
        return;
    }
    if (previous.kind != Kind::none && !(previous == clicked) && std::ranges::find(extra, previous) == extra.end())
        extra.push_back(previous);
    std::erase(extra, clicked);
}

MissionRecordKey primary_key(const AppState& state) {
    if (state.selection.kind == SelectionRef::Kind::mission_entry)
        return key_of(state, static_cast<std::uint32_t>(state.selection.a));
    return {};
}

} // namespace

std::optional<std::size_t> placement_index(const AppState& state, const std::uint32_t entry) {
    if (entry < placement_entry_base || !state.authoring.project) return std::nullopt;
    const std::size_t index = entry - placement_entry_base;
    if (index >= state.authoring.project->placements.size()) return std::nullopt;
    return index;
}

const csf::ProjectPlacement* selected_placement(const AppState& state) {
    if (state.selection.kind != SelectionRef::Kind::mission_entry) return nullptr;
    const auto index = placement_index(state, static_cast<std::uint32_t>(state.selection.a));
    return index ? &state.authoring.project->placements[*index] : nullptr;
}

void append_placement_overlays(const AppState& state, GeometryPreview::MissionOverlaySet& overlays) {
    const auto* project = state.authoring.project.get();
    if (!project) return;
    for (std::size_t i = 0; i < project->placements.size(); ++i) {
        const auto& placement = project->placements[i];
        const bool building = placement.kind != csf::ProjectPlacement::Kind::prop;
        GeometryPreview::MissionOverlayPoint point;
        point.kind = GeometryPreview::MissionOverlayKind::actor;
        point.source_entry = placement_entry_base + static_cast<std::uint32_t>(i);
        point.position = placement.position;
        point.label = placement.id;
        point.color = ui::kind_color_u32(building ? EntityKind::building : EntityKind::vegetation);
        point.sublayer = building ? "Project buildings" : "Project map props";
        const float yaw = placement.yaw_degrees / degrees;
        point.heading = rws::Vec3{std::sin(yaw), 0.0F, std::cos(yaw)};
        overlays.points.push_back(std::move(point));
    }
}

void set_viewport_tool(AppState& state, const Tool tool) {
    if (state.preview.edit_tool() != tool) state.tools.sketch.clear();
    state.preview.set_edit_tool(tool);
}

void arm_place_tool(AppState& state, const AuthoringTools::CatalogEntry& entry) {
    state.tools.place_entry = entry;
    state.tools.place_building_asset.clear();
    set_viewport_tool(state, Tool::place);
    if (state.workspace != Workspace::mission) state.workspace = Workspace::mission;
}

std::vector<MissionRecordKey> selected_records(const AppState& state) {
    std::vector<MissionRecordKey> keys;
    if (const auto primary = primary_key(state); primary.kind != Kind::none) keys.push_back(primary);
    for (const auto& key : state.ui.selection_extra)
        if (std::ranges::find(keys, key) == keys.end()) keys.push_back(key);
    return keys;
}

void select_with_modifiers(AppState& state, const MissionRecordKey& key, const bool shift, const bool ctrl) {
    GeometryPreview::PickEvent event;
    event.entry = entry_of(state, key);
    event.shift = shift;
    event.ctrl = ctrl;
    state.ui.last_primary = primary_key(state);
    if (!(ctrl && (key == state.ui.last_primary || std::ranges::find(state.ui.selection_extra, key) !=
                                                        state.ui.selection_extra.end()))) {
        if (key.kind == Kind::placement) {
            if (event.entry) state.preview.select_mission_entry(*event.entry);
        } else {
            select_mission_record(state, key, false);
        }
    }
    apply_pick_event(state, event);
}

void request_pick(AppState& state, std::vector<MissionRecordKey::Kind> kinds, std::string hint,
                  std::function<void(const MissionRecordKey&)> done, std::string field) {
    state.ui.pick = UiState::PickRequest{std::move(kinds), std::move(hint), std::move(done), primary_key(state),
                                         std::move(field)};
    set_viewport_tool(state, Tool::select);
}

void pick_button(AppState& state, const char* id, std::vector<MissionRecordKey::Kind> kinds, const std::string& hint,
                 std::function<void(const MissionRecordKey&)> done) {
    const std::string field = std::string(id) + "/" + std::to_string(ImGui::GetID(id));
    const bool active = state.ui.pick && state.ui.pick->field == field;
    if (ui::icon_button(id, ui::icons::LC_PIPETTE, active ? "Picking: click one in the viewport or the Outliner (Esc cancels)"
                                                          : "Pick in the viewport or the Outliner",
                        active)) {
        if (active)
            state.ui.pick.reset();
        else
            request_pick(state, std::move(kinds), hint, std::move(done), field);
    }
}

// The walk grid the Behaviours panel would generate, over the viewport.
void draw_walk_grid_preview(AppState& state) {
    if (!state.tools.preview_grid || state.tools.preset != AuthoringTools::Preset::walk_grid) return;
    auto& preview = state.preview;
    const auto canvas = preview.canvas_rect();
    auto* draw_list = ImGui::GetWindowDrawList();
    const float scale = ui::ui_scale();
    const auto& points = state.tools.grid_points;
    draw_list->PushClipRect({canvas.x, canvas.y}, {canvas.z, canvas.w}, true);
    for (const auto& [a, b] : state.tools.grid_links) {
        const auto from = preview.screen_position(rws_point(points[static_cast<std::size_t>(a - 1)]));
        const auto to = preview.screen_position(rws_point(points[static_cast<std::size_t>(b - 1)]));
        if (from && to) draw_list->AddLine(*from, *to, ui::kind_color_u32(EntityKind::walk_grid, 0.35F), 1.0F * scale);
    }
    for (const auto& point : points)
        if (const auto at = preview.screen_position(rws_point(point)))
            draw_list->AddCircleFilled(*at, 2.5F * scale, ui::kind_color_u32(EntityKind::walk_grid, 0.8F));
    draw_list->PopClipRect();
}

void update_viewport_tools(AppState& state) {
    auto& preview = state.preview;
    // Selection events of the last frame (Shift/Ctrl clicks, boxes).
    if (const auto event = preview.take_pick_event(); event && !state.ui.pick) apply_pick_event(state, *event);
    // A pending pick takes the next record selected, in the viewport or the
    // Outliner, and puts the selection back.
    if (state.ui.pick) {
        const auto key = primary_key(state);
        if (key.kind != Kind::none && !(key == state.ui.pick->restore)) {
            auto pick = std::move(*state.ui.pick);
            state.ui.pick.reset();
            if (std::ranges::find(pick.kinds, key.kind) != pick.kinds.end())
                pick.done(key);
            else
                state.warn("That is not one of the records this field takes");
            if (pick.restore.kind != Kind::none)
                select_mission_record(state, pick.restore, false);
            else {
                state.preview.clear_mission_selection();
                state.selection = {};
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.ui.pick.reset();
        }
    }
    // Records that no longer exist leave the multi-selection.
    std::erase_if(state.ui.selection_extra, [&](const MissionRecordKey& key) { return !entry_of(state, key); });
    std::unordered_set<std::uint32_t> extra_entries;
    for (const auto& key : state.ui.selection_extra)
        if (const auto entry = entry_of(state, key)) extra_entries.insert(*entry);
    preview.set_extra_selection(std::move(extra_entries));
    state.ui.last_primary = primary_key(state);

    const auto tool = preview.edit_tool();
    preview.set_tool_hint(tool_hint(state, tool));
    auto clicks = preview.take_viewport_clicks();
    // Assets dropped from the Assets panel, whatever the tool.
    std::erase_if(clicks, [&](const GeometryPreview::ViewportClick& click) {
        if (!click.drop) return false;
        auto& tools = state.tools;
        if (!click.ground)
            state.warn("No ground under the pointer");
        else if (!tools.drag_building.empty())
            place_building(state, tools.drag_building, csf_point(*click.ground), tools.place_heading);
        else if (tools.drag_entry && mission_editable(state))
            place_asset(state, *tools.drag_entry, csf_point(*click.ground), tools.place_heading);
        tools.drag_entry.reset();
        tools.drag_building.clear();
        return true;
    });
    draw_walk_grid_preview(state);
    if (!GeometryPreview::authoring_tool(tool) || !mission_editable(state)) return;
    // Clicks of the authoring tools.
    for (const auto& click : clicks) {
        if (!click.ground) {
            state.warn("No ground under the pointer");
            continue;
        }
        const auto at = csf_point(*click.ground);
        if (tool == Tool::place) {
            if (!state.tools.place_entry && state.tools.place_building_asset.empty()) {
                show_panel(state, Panel::assets);
                continue;
            }
            place_at(state, at);
            if (!click.shift && !state.tools.place_keep) set_viewport_tool(state, Tool::select);
        } else {
            state.tools.sketch.push_back(at);
            if (click.double_click) finish_sketch(state, tool);
        }
    }
    if (preview.canvas_hovered()) tool_keys(state, tool);
    const auto canvas = preview.canvas_rect();
    auto* draw_list = ImGui::GetWindowDrawList();
    if (!GeometryPreview::authoring_tool(preview.edit_tool())) return;
    draw_list->PushClipRect({canvas.x, canvas.y}, {canvas.z, canvas.w}, true);
    draw_tool_preview(state, tool, draw_list);
    draw_list->PopClipRect();
    draw_tool_options(state, tool, canvas);
}

void align_selection_to_ground(AppState& state) {
    const auto keys = selected_records(state);
    if (keys.empty()) return;
    // One record at a time: each has its own ground height.
    std::vector<MissionRecordKey> records;
    for (const auto& key : keys)
        if (key.kind != Kind::placement) records.push_back(key);
    if (!records.empty() && mission_editable(state)) {
        auto& editor = *state.mission.editor;
        const auto& scene = *state.mission.scene;
        const auto result = editor.batch("Align " + std::to_string(records.size()) + " to the ground", [&] {
            csf::EditResult last{true, "Align"};
            for (const auto& key : records) {
                const auto position = record_position(state, key);
                if (!position) continue;
                const auto ground = mission_ground(state, position->x, position->z);
                if (!ground || std::abs(*ground - position->y) < 0.01F) continue;
                const csf::Vec3 moved{position->x, *ground, position->z};
                if (key.kind == Kind::actor) {
                    for (const auto& actor : scene.actors())
                        if (actor.id == key.id)
                            last = editor.set_actor_placement(key.id, {moved, actor.heading.value_or(0), actor.pitch.value_or(0)});
                } else if (key.kind == Kind::dummy) {
                    for (const auto& dummy : scene.dummies())
                        if (dummy.id == key.id)
                            last = editor.set_dummy_placement(key.id, moved, dummy.heading.value_or(0), dummy.pitch.value_or(0));
                } else if (key.kind == Kind::nav_point) {
                    last = editor.set_navigation_point(key.id, key.sub_id, moved);
                }
                if (!last.applied && !last.message.starts_with("No change")) return last;
            }
            return csf::EditResult{true, "Align"};
        });
        apply_mission_edit(state, result);
    }
}

void delete_selection(AppState& state, const bool force) {
    const auto keys = selected_records(state);
    if (keys.size() <= 1 && (keys.empty() || keys.front().kind != Kind::placement)) {
        delete_selected_record(state, force);
        return;
    }
    std::vector<MissionRecordKey> records;
    std::vector<std::size_t> placements;
    for (const auto& key : keys)
        (key.kind == Kind::placement ? (void)placements.push_back(static_cast<std::size_t>(key.id)) : records.push_back(key));
    if (!records.empty() && mission_editable(state)) {
        auto& editor = *state.mission.editor;
        // Records a component made go with their component.
        std::set<std::int32_t> components;
        std::erase_if(records, [&](const MissionRecordKey& key) {
            const auto* owner = owning_component(state, key);
            if (owner) components.insert(owner->id);
            return owner != nullptr;
        });
        const auto result = editor.batch("Delete " + std::to_string(records.size() + components.size()) + " records", [&] {
            csf::EditResult last{true, "Delete"};
            for (const auto id : components)
                if (last = csf::delete_component(editor, id); !last.applied) return last;
            for (const auto& key : records) {
                switch (key.kind) {
                case Kind::actor: last = editor.delete_actor(key.id, force); break;
                case Kind::dummy: last = editor.delete_dummy(key.id, force); break;
                case Kind::light: last = editor.delete_light(key.id); break;
                case Kind::area: last = editor.delete_area(key.id, force); break;
                case Kind::nav_group: last = editor.delete_navigation_group(key.id, force); break;
                case Kind::nav_point: last = editor.delete_navigation_point(key.id, key.sub_id, force); break;
                default: continue;
                }
                if (!last.applied) return last;
            }
            return csf::EditResult{true, "Delete"};
        });
        if (!apply_mission_edit(state, result)) return;
    }
    if (!placements.empty())
        edit_authoring_project(state, "Delete " + std::to_string(placements.size()) + " placements",
                               [&](csf::AuthoringProject& project) {
                                   std::ranges::sort(placements, std::greater{});
                                   for (const auto index : placements)
                                       project.placements.erase(project.placements.begin() + static_cast<std::ptrdiff_t>(index));
                                   return true;
                               });
    notify_undoable(state, "Deleted " + std::to_string(keys.size()) + " records");
    state.ui.selection_extra.clear();
    state.preview.clear_mission_selection();
    state.selection = {};
}

void duplicate_selection(AppState& state) {
    const auto keys = selected_records(state);
    if (keys.size() <= 1 && (keys.empty() || keys.front().kind != Kind::placement)) {
        duplicate_selected_record(state);
        return;
    }
    constexpr csf::Vec3 offset{150.0F, 0.0F, 150.0F};
    std::vector<MissionRecordKey> records;
    for (const auto& key : keys)
        if (key.kind != Kind::placement) records.push_back(key);
    if (!records.empty() && mission_editable(state)) {
        auto& editor = *state.mission.editor;
        const auto result = editor.batch("Duplicate " + std::to_string(records.size()) + " records", [&] {
            csf::EditResult last{true, "Duplicate"};
            for (const auto& key : records) {
                switch (key.kind) {
                case Kind::actor: last = editor.duplicate_actor(key.id, offset); break;
                case Kind::dummy: last = editor.duplicate_dummy(key.id, offset); break;
                case Kind::light: last = editor.duplicate_light(key.id, offset); break;
                default: continue;
                }
                if (!last.applied) return last;
            }
            return csf::EditResult{true, "Duplicate"};
        });
        apply_mission_edit(state, result);
    }
    std::vector<csf::ProjectPlacement> copies;
    if (const auto* project = state.authoring.project.get())
        for (const auto& key : keys)
            if (key.kind == Kind::placement) copies.push_back(project->placements.at(static_cast<std::size_t>(key.id)));
    if (!copies.empty())
        edit_authoring_project(state, "Duplicate " + std::to_string(copies.size()) + " placements",
                               [&](csf::AuthoringProject& project) {
                                   std::set<std::string> ids;
                                   for (const auto& placement : project.placements) ids.insert(placement.id);
                                   for (auto copy : copies) {
                                       const auto stem = copy.id.substr(0, copy.id.find_last_of('-'));
                                       for (int n = 2;; ++n)
                                           if (ids.insert(stem + "-" + std::to_string(n)).second) {
                                               copy.id = stem + "-" + std::to_string(n);
                                               break;
                                           }
                                       copy.position = {copy.position.x + offset.x, copy.position.y, copy.position.z + offset.z};
                                       project.placements.push_back(std::move(copy));
                                   }
                                   return true;
                               });
}

bool apply_group_drag(AppState& state, const GeometryPreview::EditDrag& drag) {
    const auto primary = key_of(state, drag.source_entry);
    if (primary.kind == Kind::none) return false;
    const bool group = !state.ui.selection_extra.empty();
    if (!group && primary.kind != Kind::placement) return false;
    const auto start = record_position(state, primary);
    if (!start) return false;
    const csf::Vec3 delta{drag.position.x - start->x, drag.position.y - start->y, drag.position.z - start->z};
    const float heading_degrees = drag.heading_radians * degrees;
    auto keys = selected_records(state);
    if (std::ranges::find(keys, primary) == keys.end()) keys.insert(keys.begin(), primary);
    const std::string label = keys.size() == 1 ? "Move placement" : "Move " + std::to_string(keys.size()) + " records";
    move_records(state, keys, delta, std::pair{primary, heading_degrees}, label);
    return true;
}

} // namespace rwsman
