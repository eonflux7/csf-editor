// The Timeline panel (docs/archive/editor/editor-ux-redesign.md, E11): a cutscene (the
// intro, or one a zone plays) as one strip per shot along a time ruler. Drag a strip's right edge
// to change how long the shot lasts, drag on the ruler (or click a strip) to
// move the playhead, which moves the viewport camera through the shots. The
// selected shot's camera, end and target are records in the viewport: select
// one to move it with the gizmo, or capture it from the view. Every edit
// regenerates the intro component as one undo step.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "commands.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "viewport_tools.hpp"
#include "ui/pickers.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace rwsman::ui {
namespace {

// A duration being dragged: the shot, and its seconds as the drag makes them.
struct EdgeDrag {
    std::size_t shot{};
    float original{}, seconds{}, pixels_per_second{};
};
std::optional<EdgeDrag> edge_drag;

MissionRecordKey key_of(const ShotRecord& record) {
    using Type = csf::MissionRecordId::Type;
    if (record.type == Type::navigation_group)
        return {MissionRecordKey::Kind::nav_point, record.id, record.point};
    return {MissionRecordKey::Kind::actor, record.id, 0};
}

// The time ruler, the shot strips and the playhead.
void draw_strips(AppState& state, std::vector<TimelineShot>& shots) {
    auto& tools = state.tools;
    const float scale = ui_scale();
    const float ruler_height = 18.0F * scale, strip_height = 34.0F * scale, edge_width = 8.0F * scale;
    const float width = std::max(ImGui::GetContentRegionAvail().x, 100.0F * scale);
    // While an edge is dragged the scale stays as it was when the drag began,
    // so the edge follows the pointer.
    const float total = timeline_length(shots);
    const float per_second = edge_drag ? edge_drag->pixels_per_second : (width - edge_width) / std::max(total, 1.0F);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();

    // Ruler: ticks every second (every five when crowded), labels on whole
    // multiples, and scrubbing.
    draw->AddRectFilled(origin, {origin.x + width, origin.y + ruler_height}, color_u32(Token::bg2));
    const int step = per_second < 18.0F * scale ? 5 : 1;
    for (int second = 0; second <= static_cast<int>(total) + 1; second += step) {
        const float x = origin.x + static_cast<float>(second) * per_second;
        if (x > origin.x + width) break;
        draw->AddLine({x, origin.y + ruler_height * 0.55F}, {x, origin.y + ruler_height}, color_u32(Token::line));
        char label[16];
        std::snprintf(label, sizeof(label), "%ds", second);
        draw->AddText({x + 3.0F * scale, origin.y + 1.0F * scale}, color_u32(Token::text_dim), label);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##ruler", {width, ruler_height});
    name_last_item("ruler");
    if (ImGui::IsItemActive()) {
        stop_shots(state);
        scrub_timeline(state, (ImGui::GetIO().MousePos.x - origin.x) / per_second);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to move the playhead: the view follows the camera.");

    // Strips.
    const float top = origin.y + ruler_height + 2.0F * scale;
    float x = origin.x;
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const float seconds = edge_drag && edge_drag->shot == i ? edge_drag->seconds : shots[i].seconds;
        const float right = x + seconds * per_second;
        const bool selected = tools.timeline_shot == i;
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorScreenPos({x, top});
        ImGui::InvisibleButton("##shot", {std::max(right - x - edge_width, 1.0F), strip_height});
        char name[32];
        std::snprintf(name, sizeof(name), "shot-%zu", i + 1);
        name_last_item(name);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) {
            stop_shots(state);
            scrub_timeline(state, shot_start(shots, i) + (ImGui::GetIO().MousePos.x - x) / per_second);
            tools.timeline_shot = i;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            if (const auto* intro = intro_component(state))
                if (const auto record = shot_record(intro->lines, i, ShotPart::camera))
                    select_mission_record(state, key_of(*record));
        const auto fill = selected ? color_u32(Token::accent, 0.35F) : color_u32(Token::bg2, hovered ? 1.0F : 0.8F);
        draw->AddRectFilled({x, top}, {right, top + strip_height}, fill, 3.0F * scale);
        draw->AddRect({x, top}, {right, top + strip_height}, color_u32(selected ? Token::accent : Token::line), 3.0F * scale,
                      0, selected ? 2.0F * scale : 1.0F);
        char label[48];
        std::snprintf(label, sizeof(label), "Shot %zu  %.1f s", i + 1, seconds);
        draw->PushClipRect({x, top}, {right - edge_width, top + strip_height}, true);
        draw->AddText({x + 6.0F * scale, top + (strip_height - ImGui::GetTextLineHeight()) * 0.5F},
                      color_u32(Token::text), label);
        draw->PopClipRect();

        // The right edge: drag to change the duration; the cut marker.
        ImGui::SetCursorScreenPos({right - edge_width, top});
        ImGui::InvisibleButton("##edge", {edge_width, strip_height});
        std::snprintf(name, sizeof(name), "edge-%zu", i + 1);
        name_last_item(name);
        const bool edge_hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (edge_hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActivated()) edge_drag = EdgeDrag{i, shots[i].seconds, shots[i].seconds, per_second};
        if (ImGui::IsItemActive() && edge_drag && edge_drag->shot == i)
            edge_drag->seconds = snap_shot_seconds(edge_drag->original +
                                                   ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0F).x / per_second);
        if (ImGui::IsItemDeactivated() && edge_drag && edge_drag->shot == i) {
            const auto seconds_now = edge_drag->seconds;
            edge_drag.reset();
            if (seconds_now != shots[i].seconds) {
                shots[i].seconds = seconds_now;
                edit_intro_shots(state, shots);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to change how long shot %zu lasts.", i + 1);
        draw->AddLine({right, top - 2.0F * scale}, {right, top + strip_height + 2.0F * scale},
                      color_u32(edge_hot ? Token::accent : Token::text_dim), edge_hot ? 3.0F * scale : 1.5F * scale);
        ImGui::PopID();
        x = right;
    }

    // Playhead.
    const float head = origin.x + std::min(tools.timeline_time, timeline_length(shots)) * per_second;
    draw->AddLine({head, origin.y}, {head, top + strip_height + 2.0F * scale}, color_u32(Token::error), 2.0F * scale);
    draw->AddTriangleFilled({head - 5.0F * scale, origin.y}, {head + 5.0F * scale, origin.y},
                            {head, origin.y + 7.0F * scale}, color_u32(Token::error));
    ImGui::SetCursorScreenPos({origin.x, top + strip_height + 6.0F * scale});
    ImGui::Dummy({width, 0.0F});
}

// A shot's camera, end or target: where it is, select its record (the gizmo
// moves it), or take it from the view.
void part_row(AppState& state, const std::vector<std::string>& lines, const std::size_t shot, const ShotPart part,
              const csf::Vec3 position, const char* label, const char* help) {
    const char* name = part == ShotPart::camera ? "camera" : part == ShotPart::end ? "end" : "target";
    ImGui::PushID(static_cast<int>(part));
    property_row(label, help);
    ImGui::Text("%.0f  %.0f  %.0f", position.x, position.y, position.z);
    ImGui::SameLine();
    const auto record = shot_record(lines, shot, part);
    ImGui::BeginDisabled(!record);
    if (ImGui::SmallButton(("Select##select_" + std::string(name)).c_str()) && record)
        select_mission_record(state, key_of(*record), true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select it in the viewport, to move it with the gizmo.");
    ImGui::SameLine();
    if (ImGui::SmallButton(("From view##view_" + std::string(name)).c_str())) recapture_shot(state, shot, part);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(part == ShotPart::target ? "The point the view looks at."
                                                   : "The view's camera, looking at the view's target.");
    ImGui::PopID();
}

void shot_card(AppState& state, std::vector<TimelineShot>& shots) {
    auto& tools = state.tools;
    const auto* intro = intro_component(state);
    if (!intro || shots.empty()) return;
    tools.timeline_shot = std::min(tools.timeline_shot, shots.size() - 1);
    const auto index = tools.timeline_shot;
    auto& shot = shots[index];
    const auto title = "Shot " + std::to_string(index + 1);
    if (!begin_card("##shot", title.c_str(), {icons::LC_CAMERA})) return;
    const auto lines = intro->lines;  // the edits below replace the component
    if (begin_properties("##shot_properties")) {
        property_row("Duration", "Seconds the shot lasts; the camera's speed follows from its path.");
        float seconds{};
        if (edit_float_value("##seconds", shot.seconds, seconds, 0.1F, "%.1f s")) {
            shot.seconds = snap_shot_seconds(seconds);
            edit_intro_shots(state, shots);
        }
        part_row(state, lines, index, ShotPart::camera, shot.camera, "Camera", "Where the camera starts.");
        part_row(state, lines, index, ShotPart::end, shot.end, "Camera end", "Where it is when the shot cuts.");
        part_row(state, lines, index, ShotPart::target, shot.target, "Target", "What it keeps in view.");
        end_properties();
    }
    if (secondary_button("Look through")) view_shot(state, index, 0.0F);
    ImGui::SameLine();
    ImGui::BeginDisabled(index == 0);
    if (secondary_button("Earlier")) {
        std::swap(shots[index], shots[index - 1]);
        if (edit_intro_shots(state, shots)) tools.timeline_shot = index - 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(index + 1 >= shots.size());
    if (secondary_button("Later")) {
        std::swap(shots[index], shots[index + 1]);
        if (edit_intro_shots(state, shots)) tools.timeline_shot = index + 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (danger_button(shots.size() == 1 ? "Delete cutscene" : "Remove shot")) {
        shots.erase(shots.begin() + static_cast<std::ptrdiff_t>(index));
        if (edit_intro_shots(state, shots) && index > 0) tools.timeline_shot = index - 1;
    }
    end_card();
}

// Which cutscene the timeline edits, and when it plays: at the start, or
// once when the player enters a zone (armed at the start or by an event).
void cutscene_header(AppState& state) {
    auto& tools = state.tools;
    const auto cutscenes = cutscene_components(state);
    const auto* current = intro_component(state);
    if (!current) return;
    // Selecting a record of another cutscene (in the viewport or the Outliner)
    // switches to it, once per selection: the list above can switch away again.
    static std::optional<MissionRecordKey> seen;
    const auto selected = selected_records(state);
    const auto key = selected.size() == 1 ? std::optional(selected.front()) : std::nullopt;
    if (key != seen) {
        seen = key;
        if (const auto* owner = key ? owning_component(state, *key) : nullptr;
            owner && owner->op() == "shot" && owner->id != current->id) {
            tools.timeline_component = owner->id;
            tools.timeline_shot = 0;
            current = intro_component(state);
        }
    }
    std::vector<std::pair<std::int32_t, std::string>> items;
    for (const auto* cutscene : cutscenes) items.emplace_back(cutscene->id, cutscene_title(state, *cutscene));
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55F, 420.0F * ui_scale()));
    if (const auto picked = filtered_combo("##cutscene", cutscene_title(state, *current), items);
        picked && *picked != current->id) {
        tools.timeline_component = *picked;
        tools.timeline_shot = 0;
        tools.timeline_time = 0.0F;
        stop_shots(state);
        return;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!mission_editable(state));
    if (secondary_button((std::string(icons::LC_PLUS) + " Zone cutscene").c_str())) add_zone_cutscene(state);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("A cutscene that plays once when the player enters a zone (Ransom's farm entrance), its "
                          "first shot from the view.");

    const auto when = cutscene_when(*current);
    const bool other_start = std::ranges::any_of(cutscenes, [&](const auto* cutscene) {
        return cutscene->id != current->id && !cutscene_when(*cutscene).zone;
    });
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Plays");
    ImGui::SameLine();
    static constexpr const char* modes[]{"At the start", "When the player enters a zone"};
    int mode = when.zone ? 1 : 0;
    ImGui::SetNextItemWidth(220.0F * ui_scale());
    ImGui::BeginDisabled(!mission_editable(state));
    if (ImGui::Combo("##plays", &mode, modes, IM_ARRAYSIZE(modes)) && mode != (when.zone ? 1 : 0)) {
        if (mode == 0 && other_start) {
            state.warn("The mission already has an intro at the start");
        } else {
            auto changed = when;
            changed.zone.reset();
            if (mode == 1)
                for (const auto& area : state.mission.scene->areas())
                    if (area.id && !changed.zone) changed.zone = area.id;
            if (mode == 1 && !changed.zone) state.warn("Draw a zone first (the Zone tool)");
            else set_cutscene_when(state, changed);
        }
    }
    name_last_item("##plays");
    if (when.zone) {
        ImGui::SameLine();
        std::vector<std::pair<std::int32_t, std::string>> zones;
        std::string zone_label = "zone " + std::to_string(*when.zone);
        for (const auto& area : state.mission.scene->areas())
            if (area.id) {
                auto label = area.name.value_or("zone") + "  (zone " + std::to_string(*area.id) + ")";
                if (area.id == when.zone) zone_label = label;
                zones.emplace_back(*area.id, std::move(label));
            }
        ImGui::SetNextItemWidth(200.0F * ui_scale());
        if (const auto picked = filtered_combo("##cutscene_zone", zone_label, zones); picked && *picked != *when.zone) {
            auto changed = when;
            changed.zone = *picked;
            set_cutscene_when(state, changed);
        }
        ImGui::SameLine();
        pick_button(state, "##pick_cutscene_zone", {MissionRecordKey::Kind::area}, "Pick the zone",
                    [&state](const MissionRecordKey& key) {
                        const auto* cutscene = intro_component(state);
                        if (!cutscene) return;
                        auto changed = cutscene_when(*cutscene);
                        changed.zone = key.id;
                        set_cutscene_when(state, changed);
                    });
        ImGui::SameLine();
        ImGui::TextUnformatted("armed");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0F * ui_scale());
        if (const auto picked = event_combo(state, "##cutscene_arm", when.arm, "from the start");
            picked && *picked != when.arm) {
            auto changed = when;
            changed.arm = *picked;
            set_cutscene_when(state, changed);
        }
        help_marker("Once armed, the cutscene plays the first time the player enters the zone, never again. Arm "
                    "it with an event when it should wait for something, such as an officer reaching his post "
                    "(his script raises the event).");
    }
    ImGui::EndDisabled();
}

} // namespace

void draw_timeline(AppState& state) {
    if (!state.mission.editor) {
        empty_state(icons::LC_VIDEO, "Open a mission to edit its intro cutscene.");
        return;
    }
    auto& tools = state.tools;
    auto shots = intro_shots(state);
    const bool editable = mission_editable(state);
    if (shots.empty()) {
        ImGui::BeginDisabled(!editable);
        if (empty_state(icons::LC_VIDEO,
                        "No intro cutscene yet. Frame the first shot in the viewport, then capture it; each "
                        "shot moves the camera sideways while it keeps its target in view.",
                        "Capture shot from view"))
            capture_shot(state);
        ImGui::EndDisabled();
        return;
    }
    cutscene_header(state);
    shots = intro_shots(state);  // the header may have switched or changed the cutscene
    if (shots.empty()) return;
    ImGui::BeginDisabled(!editable);
    if (primary_button((std::string(icons::LC_CAMERA) + " Capture shot").c_str())) capture_shot(state);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a shot from the viewport's camera and target.");
    ImGui::SameLine();
    if (!tools.preview_started) {
        if (secondary_button((std::string(icons::LC_PLAY) + " Play").c_str())) play_shots(state);
    } else if (secondary_button((std::string(icons::LC_SQUARE) + " Stop").c_str())) {
        stop_shots(state);
    }
    ImGui::SameLine();
    dim_text("%.1f / %.1f s", tools.timeline_time, timeline_length(shots));
    ImGui::SameLine();
    help_marker("Playing approximates the game: its field of view differs, and the game fades in after a "
                "short pause. Double-click a shot to select its camera in the viewport.");
    if (const auto* cutscene = intro_component(state); cutscene && !cutscene_when(*cutscene).zone) {
        ImGui::SameLine();
        bool send_init = intro_sends_init(state);
        ImGui::BeginDisabled(!editable);
        if (ImGui::Checkbox("Start the actors' scripts", &send_init)) set_intro_sends_init(state, send_init);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The intro raises INIT, the event actor scripts start on.");
    }

    shots = intro_shots(state);  // the toolbar may have changed them
    if (shots.empty()) return;
    draw_strips(state, shots);  // scrubbing works read-only too
    ImGui::BeginDisabled(!editable);
    shot_card(state, shots);
    ImGui::EndDisabled();
}

} // namespace rwsman::ui
