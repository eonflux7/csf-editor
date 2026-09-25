// Mission editing UI: the Edit section of the Inspector, the Changes panel
// (project, history, mission properties, adding and importing), and the export
// and unsaved-edits dialogs. Every change goes through csf::MissionEditor via
// mission_editing.hpp; nothing here writes files directly.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "commands.hpp"
#include "file_dialogs.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/property_grid.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numbers>
#include <unordered_map>

namespace rwsman::ui {
namespace {

constexpr float degrees_per_radian = 180.0F / std::numbers::pi_v<float>;

// Edit widgets keep their own buffer while active, so a drag or a typed value
// accumulates across frames; while idle they show the current record value.
struct FieldState {
    std::array<char, 512> text{};
    std::array<float, 3> values{};
    int integer{};
    bool active{};
};

FieldState& field_state(const ImGuiID id) {
    static std::unordered_map<ImGuiID, FieldState> fields;
    return fields[id];
}

void begin_row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

bool begin_fields(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed, 110.0F * ui_scale());
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

bool text_field(const char* label, const std::string& value, std::string& edited) {
    begin_row(label);
    const auto id = std::string("##") + label;
    auto& state = field_state(ImGui::GetID(id.c_str()));
    if (!state.active) {
        const auto size = std::min(value.size(), state.text.size() - 1);
        std::memcpy(state.text.data(), value.data(), size);
        state.text[size] = '\0';
    }
    ImGui::InputText(id.c_str(), state.text.data(), state.text.size());
    state.active = ImGui::IsItemActive();
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    edited = state.text.data();
    return edited != value;
}

bool vec3_field(const char* label, const csf::Vec3& value, csf::Vec3& edited, const float speed = 1.0F) {
    begin_row(label);
    const auto id = std::string("##") + label;
    auto& state = field_state(ImGui::GetID(id.c_str()));
    if (!state.active) state.values = {value.x, value.y, value.z};
    ImGui::DragFloat3(id.c_str(), state.values.data(), speed, 0.0F, 0.0F, "%.2f");
    state.active = ImGui::IsItemActive();
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    edited = {state.values[0], state.values[1], state.values[2]};
    return true;
}

bool float_field(const char* label, const float value, float& edited, const float speed = 1.0F,
                 const char* format = "%.2f") {
    begin_row(label);
    const auto id = std::string("##") + label;
    auto& state = field_state(ImGui::GetID(id.c_str()));
    if (!state.active) state.values[0] = value;
    ImGui::DragFloat(id.c_str(), state.values.data(), speed, 0.0F, 0.0F, format);
    state.active = ImGui::IsItemActive();
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    edited = state.values[0];
    return true;
}

bool int_field(const char* label, const int value, int& edited) {
    begin_row(label);
    const auto id = std::string("##") + label;
    auto& state = field_state(ImGui::GetID(id.c_str()));
    if (!state.active) state.integer = value;
    ImGui::InputInt(id.c_str(), &state.integer, 0, 0);
    state.active = ImGui::IsItemActive();
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    edited = state.integer;
    return edited != value;
}

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool matches(const std::string& text, const char* filter) {
    return !filter || !*filter || lower(text).find(lower(filter)) != std::string::npos;
}

// A combo with a filter box. `items` are (value, label); returns the pick.
template <class T>
std::optional<T> filtered_combo(const char* id, const std::string& preview,
                                const std::vector<std::pair<T, std::string>>& items) {
    std::optional<T> picked;
    if (!ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge)) return picked;
    auto& state = field_state(ImGui::GetID("filter"));
    if (ImGui::IsWindowAppearing()) {
        state.text[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "filter", state.text.data(), state.text.size());
    std::size_t shown = 0;
    for (const auto& [value, label] : items) {
        if (!matches(label, state.text.data())) continue;
        if (++shown > 400) {
            dim_text("Refine the filter to see more.");
            break;
        }
        if (ImGui::Selectable(label.c_str(), label == preview)) picked = value;
    }
    ImGui::EndCombo();
    return picked;
}

std::vector<std::pair<std::int32_t, std::string>> class_items(const AppState& state) {
    std::vector<std::pair<std::int32_t, std::string>> items;
    if (!state.mission.objects) return items;
    for (const auto& definition : state.mission.objects->definitions())
        if (definition.class_id)
            items.emplace_back(*definition.class_id,
                               std::to_string(*definition.class_id) + "  " + definition.name.value_or("(unnamed)"));
    std::ranges::sort(items);
    return items;
}

std::string class_label(const AppState& state, const std::optional<std::int32_t> id) {
    if (!id) return "(none)";
    if (state.mission.objects)
        if (const auto found = state.mission.objects->find_class(*id); found.size() == 1)
            return std::to_string(*id) + "  " + found.front()->name.value_or("(unnamed)");
    return std::to_string(*id) + "  (not in Objetos.bdd)";
}

// The body model of a class (the character model for player classes).
std::optional<std::string> class_model(const csf::ObjectDefinition& definition) {
    for (const auto& reference : definition.references)
        if (reference.kind == csf::ObjectReference::Kind::visual_model) return reference.path;
    return std::nullopt;
}

// One entry per distinct model, naming the first class that has it.
std::vector<std::pair<std::int32_t, std::string>> model_items(const AppState& state) {
    std::map<std::string, std::pair<std::int32_t, std::string>> models;
    if (!state.mission.objects) return {};
    for (const auto& definition : state.mission.objects->definitions()) {
        const auto model = class_model(definition);
        if (!definition.class_id || !model) continue;
        models.try_emplace(lower(*model), *definition.class_id,
                           *model + "  (" + std::to_string(*definition.class_id) + " " +
                               definition.name.value_or("") + ")");
    }
    std::vector<std::pair<std::int32_t, std::string>> items;
    for (auto& [key, item] : models) items.push_back(std::move(item));
    return items;
}

std::string model_label(const AppState& state, const std::optional<std::int32_t> class_id) {
    if (class_id && state.mission.objects)
        if (const auto found = state.mission.objects->find_class(*class_id); found.size() == 1)
            if (const auto model = class_model(*found.front())) return *model;
    return "(unknown)";
}

std::vector<std::pair<std::int32_t, std::string>> animation_items(const AppState& state) {
    std::vector<std::pair<std::int32_t, std::string>> items;
    if (!state.mission.animations) return items;
    for (const auto& record : state.mission.animations->records())
        if (record.id) items.emplace_back(*record.id, std::to_string(*record.id) + "  " + record.logical_name);
    return items;
}

std::string animation_label(const AppState& state, const std::int32_t id) {
    if (state.mission.animations)
        if (const auto* record = state.mission.animations->find_id(id))
            return std::to_string(id) + "  " + record->logical_name;
    return std::to_string(id) + "  (not in Anims.bdd)";
}

csf::MissionEditor& editor(AppState& state) { return *state.mission.editor; }

void record_buttons(AppState& state, const bool duplicable) {
    if (duplicable) {
        if (ImGui::Button((std::string(icons::LC_COPY) + " Duplicate").c_str())) duplicate_selected_record(state);
        ImGui::SameLine();
    }
    if (ImGui::Button((std::string(icons::LC_TRASH) + " Delete").c_str())) delete_selected_record(state, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refuses while scripts reference the record; Shift+Delete forces");
}

void draw_actor_editor(AppState& state, const csf::MissionActor& actor) {
    const auto id = *actor.id;
    if (begin_fields("##actor_fields")) {
        std::string name;
        if (text_field("name", actor.name.value_or(""), name))
            apply_mission_edit(state, editor(state).set_actor_name(id, name));
        csf::Vec3 position = actor.position.value_or(csf::Vec3{});
        if (vec3_field("position", position, position))
            apply_mission_edit(state, editor(state).set_actor_placement(
                                          id, {position, actor.heading.value_or(0), actor.pitch.value_or(0)}));
        float heading = actor.heading.value_or(0);
        if (float_field("heading", heading, heading, 0.5F, "%.1f deg"))
            apply_mission_edit(state, editor(state).set_actor_placement(
                                          id, {position, heading, actor.pitch.value_or(0)}));
        float pitch = actor.pitch.value_or(0);
        if (float_field("pitch", pitch, pitch, 0.5F, "%.1f deg"))
            apply_mission_edit(state, editor(state).set_actor_placement(id, {position, heading, pitch}));
        begin_row("class");
        if (const auto picked = filtered_combo("##class", class_label(state, actor.class_id), class_items(state)))
            apply_mission_edit(state, editor(state).set_actor_class(id, *picked));
        begin_row("model");
        if (const auto picked = filtered_combo("##model", model_label(state, actor.class_id), model_items(state)))
            apply_mission_edit(state, editor(state).set_actor_look(id, *picked));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Another class's model; the actor keeps its behaviour (uses a copy of its class)");
        begin_row("faction");
        static const std::vector<std::pair<std::string, std::string>> factions{
            {"", "(none)"}, {"NEUTRO", "NEUTRO"}, {"ALEMAN", "ALEMAN"}, {"ALIADO", "ALIADO"}};
        if (const auto picked = filtered_combo("##faction", actor.faction.value_or("(none)"), factions))
            apply_mission_edit(state, editor(state).set_actor_faction(
                                          id, picked->empty() ? std::nullopt : std::optional{*picked}));
        int value{};
        if (int_field("collision", actor.collision.value_or(0), value))
            apply_mission_edit(state, editor(state).set_actor_integer(id, ".COLISION", value));
        if (int_field("flags", static_cast<int>(actor.flags.value_or(0)), value))
            apply_mission_edit(state, editor(state).set_actor_integer(id, ".FLAGS", value));
        if (int_field("2nd explosion", actor.secondary_explosion.value_or(0), value))
            apply_mission_edit(state, editor(state).set_actor_integer(id, ".SEGUNDA_EXPLOSION", value));
        ImGui::EndTable();
    }

    // Per-actor scripts: non-trigger scripts run with THIS = this actor.
    ImGui::Spacing();
    ImGui::TextUnformatted("Scripts");
    const auto choices = editor(state).actor_script_choices();
    auto scripts = actor.script_ids;
    for (std::size_t i = 0; i < scripts.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto choice = std::ranges::find(choices, scripts[i], &std::pair<std::int32_t, std::string>::first);
        ImGui::BulletText("%d  %s", scripts[i], choice == choices.end() ? "(unknown)" : choice->second.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(icons::LC_X)) {
            auto updated = scripts;
            updated.erase(updated.begin() + static_cast<std::ptrdiff_t>(i));
            apply_mission_edit(state, editor(state).set_actor_scripts(id, updated));
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    std::vector<std::pair<std::int32_t, std::string>> script_items;
    for (const auto& [script_id, name] : choices)
        if (std::ranges::find(scripts, script_id) == scripts.end())
            script_items.emplace_back(script_id, std::to_string(script_id) + "  " + name);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (const auto picked = filtered_combo("##add_script", "Add a script...", script_items)) {
        scripts.push_back(*picked);
        apply_mission_edit(state, editor(state).set_actor_scripts(id, scripts));
    }

    // Animation overrides: per-actor .ANIMACIONES slots.
    ImGui::Spacing();
    ImGui::TextUnformatted("Animation overrides");
    std::vector<csf::ActorAnimationOverride> overrides;
    for (const auto& binding : actor.animations)
        if (binding.id && binding.type) overrides.push_back({*binding.id, *binding.type});
    const auto slots = csf::animation_slot_names();
    std::vector<std::pair<std::string, std::string>> slot_items;
    for (const auto slot : slots) slot_items.emplace_back(std::string(slot), std::string(slot));
    const auto animations = animation_items(state);
    if (ImGui::BeginTable("##overrides", 3, ImGuiTableFlags_SizingStretchProp)) {
        for (std::size_t i = 0; i < overrides.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (const auto picked = filtered_combo("##slot", overrides[i].slot, slot_items)) {
                auto updated = overrides;
                updated[i].slot = *picked;
                apply_mission_edit(state, editor(state).set_actor_animations(id, updated));
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (const auto picked =
                    filtered_combo("##anim", animation_label(state, overrides[i].animation_id), animations)) {
                auto updated = overrides;
                updated[i].animation_id = *picked;
                apply_mission_edit(state, editor(state).set_actor_animations(id, updated));
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(icons::LC_X)) {
                auto updated = overrides;
                updated.erase(updated.begin() + static_cast<std::ptrdiff_t>(i));
                apply_mission_edit(state, editor(state).set_actor_animations(id, updated));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    static std::string new_slot = "DISTRAIDO_IDLE_ARMA1";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45F);
    if (const auto picked = filtered_combo("##new_slot", new_slot, slot_items)) new_slot = *picked;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (const auto picked = filtered_combo("##new_anim", "Add animation...", animations)) {
        auto updated = std::move(overrides);
        std::erase_if(updated, [&](const auto& value) { return value.slot == new_slot; });
        updated.push_back({*picked, new_slot});
        apply_mission_edit(state, editor(state).set_actor_animations(id, updated));
    }
    dim_text("Slots come from the game's slot table; preview a clip on the actor in the Animation "
             "workspace. Scripts can still play other animations over these.");

    ImGui::Spacing();
    record_buttons(state, true);
    ImGui::SameLine();
    const bool player = state.mission.scene->player().active_player == actor.id;
    ImGui::BeginDisabled(player);
    if (ImGui::Button(player ? "Starting player" : "Make starting player"))
        apply_mission_edit(state, editor(state).set_player_actor(id));
    ImGui::EndDisabled();
}

void draw_dummy_editor(AppState& state, const csf::MissionDummy& dummy) {
    const auto id = *dummy.id;
    if (begin_fields("##dummy_fields")) {
        csf::Vec3 position = dummy.position.value_or(csf::Vec3{});
        const float rotation = dummy.heading.value_or(0), pitch = dummy.pitch.value_or(0);
        if (vec3_field("position", position, position))
            apply_mission_edit(state, editor(state).set_dummy_placement(id, position, rotation, pitch));
        float degrees = rotation * degrees_per_radian;
        if (float_field("rotation", degrees, degrees, 0.5F, "%.1f deg"))
            apply_mission_edit(state,
                               editor(state).set_dummy_placement(id, position, degrees / degrees_per_radian, pitch));
        float pitch_degrees = pitch * degrees_per_radian;
        if (float_field("pitch", pitch_degrees, pitch_degrees, 0.5F, "%.1f deg"))
            apply_mission_edit(state, editor(state).set_dummy_placement(id, position, rotation,
                                                                        pitch_degrees / degrees_per_radian));
        ImGui::EndTable();
    }
    record_buttons(state, true);
}

void draw_light_editor(AppState& state, const csf::MissionLight& light) {
    const auto id = *light.id;
    if (begin_fields("##light_fields")) {
        csf::Vec3 position = light.position.value_or(csf::Vec3{});
        if (vec3_field("position", position, position))
            apply_mission_edit(state, editor(state).set_light(id, {position, std::nullopt, std::nullopt, std::nullopt}));
        float radius = light.radius.value_or(0);
        if (float_field("radius", radius, radius, 5.0F, "%.0f"))
            apply_mission_edit(state, editor(state).set_light(id, {std::nullopt, std::nullopt, std::nullopt, radius}));
        begin_row("color");
        auto& color_state = field_state(ImGui::GetID("##color"));
        const auto packed = static_cast<std::uint32_t>(light.color.value_or(0));
        if (!color_state.active)
            color_state.values = {static_cast<float>((packed >> 16U) & 0xFFU) / 255.0F,
                                  static_cast<float>((packed >> 8U) & 0xFFU) / 255.0F,
                                  static_cast<float>(packed & 0xFFU) / 255.0F};
        ImGui::ColorEdit3("##color", color_state.values.data());
        color_state.active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            const auto channel = [](const float value) {
                return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
            };
            const auto rgb = (packed & 0xFF000000U) | (channel(color_state.values[0]) << 16U) |
                             (channel(color_state.values[1]) << 8U) | channel(color_state.values[2]);
            apply_mission_edit(state, editor(state).set_light(id, {std::nullopt, static_cast<std::int32_t>(rgb),
                                                                   std::nullopt, std::nullopt}));
        }
        int modulate{};
        if (int_field("modulate", light.modulate.value_or(0), modulate))
            apply_mission_edit(state, editor(state).set_light(id, {std::nullopt, std::nullopt, modulate, std::nullopt}));
        ImGui::EndTable();
    }
    record_buttons(state, true);
}

void draw_nav_point_editor(AppState& state, const csf::NavPoint& point) {
    const auto group = *point.group_id, id = *point.id;
    if (begin_fields("##nav_fields")) {
        csf::Vec3 position = point.position.value_or(csf::Vec3{});
        if (vec3_field("position", position, position))
            apply_mission_edit(state, editor(state).set_navigation_point(group, id, position));
        float degrees = point.heading.value_or(0) * degrees_per_radian;
        if (float_field("rotation", degrees, degrees, 0.5F, "%.1f deg"))
            apply_mission_edit(state,
                               editor(state).set_navigation_point(group, id, position, degrees / degrees_per_radian));
        ImGui::EndTable();
    }
    // Links from this point.
    const auto& scene = *state.mission.scene;
    std::vector<std::pair<std::int32_t, std::int32_t>> linked;
    const auto collect = [&](const csf::NavConnection& link) {
        if (link.origin_group == group && link.origin_point == id && link.destination_group && link.destination_point)
            linked.emplace_back(*link.destination_group, *link.destination_point);
        else if (link.destination_group == group && link.destination_point == id && link.origin_group &&
                 link.origin_point)
            linked.emplace_back(*link.origin_group, *link.origin_point);
    };
    for (const auto& owner : scene.navigation())
        for (const auto& link : owner.connections) collect(link);
    for (const auto& link : scene.cross_group_connections()) collect(link);
    ImGui::TextUnformatted("Links");
    for (const auto& [other_group, other_point] : linked) {
        ImGui::PushID(other_group * 100000 + other_point);
        ImGui::BulletText("%d:%d", other_group, other_point);
        ImGui::SameLine();
        if (ImGui::SmallButton(icons::LC_X))
            apply_mission_edit(state, editor(state).disconnect_navigation_points(group, id, other_group, other_point));
        ImGui::PopID();
    }
    static std::array<int, 2> target{};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5F);
    ImGui::InputInt2("##link_target", target.data());
    ImGui::SameLine();
    if (ImGui::Button("Link to group:point"))
        apply_mission_edit(state, editor(state).connect_navigation_points(group, id, target[0], target[1]));
    if (ImGui::Button((std::string(icons::LC_PLUS) + " Add point here").c_str())) {
        std::int32_t added{};
        const auto base = point.position.value_or(csf::Vec3{});
        if (apply_mission_edit(state, editor(state).add_navigation_point(group, {base.x + 100.0F, base.y, base.z}, &added))) {
            apply_mission_edit(state, editor(state).connect_navigation_points(group, id, group, added));
            select_after_refresh(state, {MissionRecordKey::Kind::nav_point, group, added});
        }
    }
    ImGui::SameLine();
    record_buttons(state, false);
}

void draw_area_editor(AppState& state, const csf::MissionArea& area) {
    const auto id = *area.id;
    if (begin_fields("##area_fields")) {
        float height = area.height.value_or(0);
        if (float_field("height", height, height, 5.0F, "%.0f"))
            apply_mission_edit(state, editor(state).set_area_height(id, height));
        for (std::size_t i = 0; i < area.points.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto label = "vertex " + std::to_string(i);
            csf::Vec3 position = area.points[i];
            if (vec3_field(label.c_str(), position, position))
                apply_mission_edit(state, editor(state).set_area_point(id, i, position));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    static int vertex = 0;
    ImGui::SetNextItemWidth(80.0F * ui_scale());
    ImGui::InputInt("##vertex", &vertex);
    vertex = std::clamp(vertex, 0, std::max(0, static_cast<int>(area.points.size()) - 1));
    ImGui::SameLine();
    if (ImGui::Button("Insert after") && !area.points.empty()) {
        const auto& a = area.points[static_cast<std::size_t>(vertex)];
        const auto& b = area.points[(static_cast<std::size_t>(vertex) + 1) % area.points.size()];
        apply_mission_edit(state, editor(state).insert_area_point(
                                      id, static_cast<std::size_t>(vertex) + 1,
                                      {(a.x + b.x) * 0.5F, (a.y + b.y) * 0.5F, (a.z + b.z) * 0.5F}));
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove"))
        apply_mission_edit(state, editor(state).remove_area_point(id, static_cast<std::size_t>(vertex)));
}

// Every scalar of the record, editable by entry index (for fields without a
// typed editor). Values keep their stored kind.
void draw_raw_scalars(AppState& state, const csf::Node& node, const csf::Document& document,
                      const std::string& prefix) {
    for (const auto& child : node.children) {
        std::string name = "(item)";
        if (child.identifier_index)
            if (const auto* label = document.identifier(*child.identifier_index)) name = label->display_utf8();
        const auto path = prefix.empty() ? name : prefix + " / " + name;
        if (!child.children.empty() || std::holds_alternative<std::monostate>(child.scalar)) {
            draw_raw_scalars(state, child, document, path);
            continue;
        }
        ImGui::PushID(static_cast<int>(child.entry_index));
        const auto scene_file = editor(state).scene_file();
        if (const auto* integer = std::get_if<std::int32_t>(&child.scalar)) {
            int value{};
            if (int_field(path.c_str(), *integer, value))
                apply_mission_edit(state, editor(state).set_scalar(scene_file, child.entry_index, value));
        } else if (const auto* real = std::get_if<float>(&child.scalar)) {
            float value{};
            if (float_field(path.c_str(), *real, value, 0.1F, "%.4f") && value != *real)
                apply_mission_edit(state, editor(state).set_scalar(scene_file, child.entry_index, value));
        } else if (const auto* index = std::get_if<std::uint32_t>(&child.scalar)) {
            const auto* text = document.string(*index);
            std::string value;
            if (text && text_field(path.c_str(), text->display_utf8(), value))
                apply_mission_edit(state, editor(state).set_scalar(scene_file, child.entry_index, value));
        }
        ImGui::PopID();
    }
}

const csf::Node* find_node(const std::vector<csf::Node>& nodes, const std::uint32_t entry) {
    for (const auto& node : nodes) {
        if (node.entry_index == entry) return &node;
        if (const auto* found = find_node(node.children, entry)) return found;
    }
    return nullptr;
}

// Cached donor databases for the import picker.
struct Donor {
    std::filesystem::path root;
    csf::ObjectDatabase objects;
    csf::AnimationCatalog animations;
    std::string error;
};

const Donor& donor_for(const std::filesystem::path& root) {
    static Donor cached;
    if (cached.root == root) return cached;
    cached = Donor{root, {}, {}, {}};
    try {
        csf::ResourceIndex index;
        index.add_root(root);
        index.build();
        const auto load = [&](const char* relative) -> std::optional<csf::Document> {
            const auto found = index.resolve(relative);
            if (found.candidate_indices.size() != 1) return std::nullopt;
            return csf::Document::load(index.resources()[found.candidate_indices.front()].path);
        };
        if (const auto document = load("BDD/Objetos.bdd")) cached.objects = csf::ObjectDatabase::project(*document);
        else cached.error = "No BDD/Objetos.bdd in this folder";
        if (const auto document = load("BDD/Anims.bdd")) cached.animations = csf::AnimationCatalog::project(*document);
    } catch (const std::exception& error) {
        cached.error = error.what();
    }
    return cached;
}

void draw_project_bar(AppState& state) {
    auto& mission = state.mission;
    auto& editor = *mission.editor;
    if (mission.project)
        dim_text("Project: %s", path_utf8(mission.project->workspace_root).c_str());
    else
        dim_text("Not saved yet. Ctrl+S creates a project in %s", path_utf8(default_project_workspace(state)).c_str());
    if (editor.dirty()) {
        ImGui::SameLine();
        token_text(Token::dirty, "%s unsaved", icons::LC_DOT);
    }
    if (ImGui::Button((std::string(icons::LC_SAVE) + " Save").c_str())) save_mission_project(state);
    ImGui::SameLine();
    if (ImGui::Button("Save as...")) state.commands.run("file.save_mission_as");
    ImGui::SameLine();
    if (ImGui::Button((std::string(icons::LC_PACKAGE) + " Export .pak...").c_str())) state.ui.show_export_dialog = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(!editor.can_undo());
    if (icon_button("##undo", icons::LC_UNDO_2, "Undo (Ctrl+Z)")) mission_undo(state);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!editor.can_redo());
    if (icon_button("##redo", icons::LC_REDO_2, "Redo (Ctrl+Y)")) mission_redo(state);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    const auto tool = state.preview.edit_tool();
    if (icon_button("##select", icons::LC_MOUSE_POINTER, "Select", tool == GeometryPreview::EditTool::select))
        state.preview.set_edit_tool(GeometryPreview::EditTool::select);
    ImGui::SameLine();
    if (icon_button("##move", icons::LC_MOVE_3D, "Move tool (G): drag the center to slide on the ground, an arrow "
                                                 "for one axis. Alt toggles surface snapping.",
                    tool == GeometryPreview::EditTool::move))
        state.preview.set_edit_tool(GeometryPreview::EditTool::move);
    ImGui::SameLine();
    if (icon_button("##rotate", icons::LC_REFRESH_CW, "Rotate tool (R): drag around the ring; Ctrl snaps to 15 degrees",
                    tool == GeometryPreview::EditTool::rotate))
        state.preview.set_edit_tool(GeometryPreview::EditTool::rotate);
    ImGui::SameLine();
    bool snap = state.preview.snap_to_surface();
    if (ImGui::Checkbox("Snap to ground", &snap)) state.preview.set_snap_to_surface(snap);
}

void draw_history(AppState& state) {
    auto& editor = *state.mission.editor;
    const auto labels = editor.history_labels();
    if (labels.empty()) {
        dim_text("No edits yet. Select a record in the viewport or Explorer and change it in the "
                 "Inspector's Edit section, or use the move (G) and rotate (R) tools.");
        return;
    }
    const auto position = editor.history_position();
    if (ImGui::Selectable("(original)", position == 0))
        while (editor.can_undo()) editor.undo();
    for (std::size_t i = 0; i < labels.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool undone = i >= position;
        if (undone) ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        if (ImGui::Selectable(labels[i].c_str(), i + 1 == position)) {
            while (editor.history_position() > i + 1 && editor.undo()) {
            }
            while (editor.history_position() < i + 1 && editor.redo()) {
            }
        }
        if (undone) ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void draw_files(AppState& state) {
    const auto& editor = *state.mission.editor;
    const auto modified = editor.modified_files();
    if (modified.empty()) {
        dim_text("Every mission file matches the shipped package.");
        return;
    }
    for (const auto index : modified) {
        const auto& file = editor.files()[index];
        token_text(file.added ? Token::ok : Token::dirty, "%s %s", file.added ? icons::LC_PLUS : icons::LC_DOT,
                   path_utf8(file.relative_path).c_str());
        ImGui::SameLine();
        dim_text("%s", csf::mission_file_kind_name(file.kind));
    }
    dim_text("Only these files are replaced or added when the mission archive is exported.");
}

void draw_mission_properties(AppState& state) {
    auto& editor = *state.mission.editor;
    const auto& scene = *state.mission.scene;
    if (!begin_fields("##mission_props")) return;
    begin_row("starting player");
    std::vector<std::pair<std::int32_t, std::string>> actors;
    std::string current = "(none)";
    for (const auto& actor : scene.actors()) {
        if (!actor.id) continue;
        auto label = std::to_string(*actor.id) + "  " + actor.name.value_or("");
        if (scene.player().active_player == actor.id) current = label;
        actors.emplace_back(*actor.id, std::move(label));
    }
    if (const auto picked = filtered_combo("##player", current, actors))
        apply_mission_edit(state, editor.set_player_actor(*picked));
    begin_row("available");
    bool commando = scene.player().commando_start.value_or(0) != 0;
    bool sniper = scene.player().sniper_start.value_or(0) != 0;
    bool spy = scene.player().spy_start.value_or(0) != 0;
    bool changed = ImGui::Checkbox("Commando", &commando);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Sniper", &sniper);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Spy", &spy);
    if (changed) apply_mission_edit(state, editor.set_start_availability(commando, sniper, spy));
    int maximum{}, minimum{};
    const auto& metadata = scene.metadata();
    if (int_field("maximum score", metadata.maximum_score.value_or(0), maximum))
        apply_mission_edit(state, editor.set_scores(maximum, metadata.minimum_score.value_or(0)));
    if (int_field("minimum score", metadata.minimum_score.value_or(0), minimum))
        apply_mission_edit(state, editor.set_scores(metadata.maximum_score.value_or(0), minimum));
    ImGui::EndTable();

    ImGui::Spacing();
    if (!ImGui::TreeNode("Environment (.MUNDOVIS)")) return;
    if (begin_fields("##environment")) {
        for (const auto& field : scene.environment()) {
            ImGui::PushID(field.name.c_str());
            if (const auto* integer = std::get_if<std::int32_t>(&field.value)) {
                int value{};
                if (int_field(field.name.c_str(), *integer, value))
                    apply_mission_edit(state, editor.set_environment(field.name, value));
            } else if (const auto* real = std::get_if<float>(&field.value)) {
                float value{};
                if (float_field(field.name.c_str(), *real, value, 0.1F, "%.3f") && value != *real)
                    apply_mission_edit(state, editor.set_environment(field.name, value));
            } else if (const auto* text = std::get_if<std::string>(&field.value)) {
                std::string value;
                if (text_field(field.name.c_str(), *text, value))
                    apply_mission_edit(state, editor.set_environment(field.name, value));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TreePop();
}

void draw_assets(AppState& state) {
    auto& tools = state.tools;
    dim_text("Places a class at the ground under the viewport centre: characters on a new placement point, "
             "props and pickups without one. Classes from other missions are imported first (with their "
             "models, textures and animations); one undo step either way.");
    search_input("##asset_filter", "filter by ID, name, type or mission", tools.asset_filter.data(),
                 tools.asset_filter.size());
    const char* filter = tools.asset_filter.data();
    if (ImGui::BeginChild("##assets", {0, 200.0F * ui_scale()}, ImGuiChildFlags_Borders)) {
        section("This mission");
        if (state.mission.objects)
            for (const auto& definition : state.mission.objects->definitions()) {
                if (!definition.class_id) continue;
                const auto label = std::to_string(*definition.class_id) + "  " + definition.name.value_or("") + "  " +
                                   definition.type.value_or("");
                if (!matches(label, filter)) continue;
                ImGui::PushID(*definition.class_id);
                if (ImGui::SmallButton(icons::LC_PLUS))
                    place_asset(state, {state.mission.editor->package_root(), "this mission", *definition.class_id,
                                        definition.name.value_or(""), definition.type.value_or("")});
                ImGui::SameLine();
                ImGui::TextUnformatted(label.c_str());
                ImGui::PopID();
            }
        section("Other missions");
        if (tools.catalog_root != state.settings.resource_root || tools.catalog.empty()) {
            ImGui::BeginDisabled(state.discovered.empty());
            if (ImGui::SmallButton("List their classes")) build_asset_catalog(state);
            ImGui::EndDisabled();
            if (state.discovered.empty()) dim_text("Set the resource root in Preferences to find other missions.");
        } else {
            std::size_t shown = 0;
            for (std::size_t i = 0; i < tools.catalog.size(); ++i) {
                const auto& entry = tools.catalog[i];
                const auto label = std::to_string(entry.class_id) + "  " + entry.name + "  " + entry.type + "  (" +
                                   entry.package_name + ")";
                if (!matches(label, filter)) continue;
                if (++shown > 300) {
                    dim_text("Refine the filter to see more.");
                    break;
                }
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton(icons::LC_PLUS)) place_asset(state, entry);
                ImGui::SameLine();
                ImGui::TextUnformatted(label.c_str());
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

void draw_presets(AppState& state) {
    auto& tools = state.tools;
    using Preset = AuthoringTools::Preset;
    dim_text("Behaviour presets write the actor, its navigation groups and its script together, as one undo "
             "step. Points are taken at the ground under the viewport centre.");
    static constexpr std::array<const char*, 5> names{"Guard on patrol", "Guard idling", "Animal on patrol",
                                                      "Cover group", "Walk grid"};
    int preset = static_cast<int>(tools.preset);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##preset", &preset, names.data(), static_cast<int>(names.size())))
        tools.preset = static_cast<Preset>(preset);
    const bool actor = tools.preset == Preset::guard_patrol || tools.preset == Preset::guard_idle ||
                       tools.preset == Preset::animal_patrol;
    const bool route = tools.preset == Preset::guard_patrol || tools.preset == Preset::animal_patrol;
    if (begin_fields("##preset_fields")) {
        if (actor) {
            begin_row("Class");
            if (const auto picked = filtered_combo("##preset_class", class_label(state, tools.class_id), class_items(state)))
                tools.class_id = *picked;
        }
        begin_row(tools.preset == Preset::cover_group || tools.preset == Preset::walk_grid ? "Group name" : "Name");
        ImGui::InputText("##preset_name", tools.name.data(), tools.name.size());
        if (actor) {
            begin_row("Heading");
            ImGui::DragFloat("##preset_heading", &tools.heading, 1.0F, -180.0F, 180.0F, "%.1f deg");
            begin_row("Script name");
            ImGui::InputTextWithHint("##preset_script", "from the name", tools.script_name.data(), tools.script_name.size());
        }
        if (route) {
            begin_row("Route name");
            ImGui::InputTextWithHint("##preset_route", "from the name", tools.route_name.data(), tools.route_name.size());
        }
        if (tools.preset == Preset::guard_patrol) {
            begin_row("Pause");
            ImGui::DragFloat("##preset_pause", &tools.pause, 0.1F, 0.0F, 60.0F, "%.1f s");
        }
        if (tools.preset == Preset::guard_patrol || tools.preset == Preset::guard_idle) {
            begin_row("Cover group");
            std::vector<std::pair<int, std::string>> groups{{0, "(none)"}};
            std::string current = "(none)";
            if (state.mission.scene)
                for (const auto& group : state.mission.scene->navigation())
                    if (group.id && group.type == 3) {
                        groups.emplace_back(*group.id, std::to_string(*group.id) + "  " + group.name.value_or(""));
                        if (*group.id == tools.cover_group) current = groups.back().second;
                    }
            if (const auto picked = filtered_combo("##preset_cover", current, groups)) tools.cover_group = *picked;
        }
        if (tools.preset == Preset::guard_idle) {
            begin_row("Idle loop");
            ImGui::InputTextWithHint("##preset_idle", "animation IDs, e.g. 1881:2-4,1385", tools.idle_loop.data(),
                                     tools.idle_loop.size());
        }
        if (tools.preset == Preset::animal_patrol) {
            begin_row("Walk animation");
            ImGui::InputInt("##preset_walk", &tools.walk_animation, 0, 0);
        }
        if (tools.preset == Preset::cover_group) {
            begin_row("Facing");
            ImGui::DragFloat("##preset_facing", &tools.cover_facing, 1.0F, -180.0F, 180.0F, "%.1f deg");
        }
        if (tools.preset == Preset::walk_grid) {
            begin_row("Spacing");
            ImGui::DragFloat("##preset_spacing", &tools.grid_spacing, 10.0F, 100.0F, 5000.0F, "%.0f cm");
            begin_row("Clear of props");
            ImGui::DragFloat("##preset_avoid", &tools.grid_avoid, 10.0F, 0.0F, 2000.0F, "%.0f cm");
        }
        ImGui::EndTable();
    }
    if (route || tools.preset == Preset::cover_group) {
        if (ImGui::Button("Add point at view")) add_preset_point(state);
        ImGui::SameLine();
        ImGui::BeginDisabled(tools.points.empty());
        if (ImGui::Button("Remove last")) tools.points.pop_back();
        ImGui::SameLine();
        if (ImGui::Button("Clear")) tools.points.clear();
        ImGui::EndDisabled();
        for (std::size_t i = 0; i < tools.points.size(); ++i)
            dim_text("%zu: %.0f %.0f %.0f", i + 1, tools.points[i].x, tools.points[i].y, tools.points[i].z);
    }
    const bool ready = !actor || tools.class_id != 0;
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button(tools.preset == Preset::walk_grid ? "Generate" : "Create")) apply_preset(state);
    ImGui::EndDisabled();
    if (!ready) {
        ImGui::SameLine();
        dim_text("Pick a class (import it in Assets first).");
    }
    ImGui::Separator();
    if (ImGui::Button("Start a new mission here")) start_new_mission(state);
    ImGui::SameLine();
    dim_text("Empties this slot's actors, navigation, zones, dummies and scripts (undoable).");
}

void draw_flow(AppState& state) {
    const auto* flow = mission_flow(state);
    if (!flow) return;
    dim_text("How the scripts connect, read from the programs as they are. Findings are evidence to check, "
             "not proof of what the game does.");
    const auto script_link = [&](const std::string_view program, const std::int32_t id) {
        const auto* script = flow->script(program, id);
        const auto label = std::to_string(id) + (script ? " " + script->name : std::string{});
        ImGui::PushID(id);
        if (ImGui::SmallButton(label.c_str())) open_flow_script(state, program, id);
        ImGui::PopID();
        ImGui::SameLine();
    };
    section("Findings");
    if (flow->findings().empty()) dim_text("None.");
    for (const auto& finding : flow->findings()) {
        const auto token = finding.severity == csf::FlowFinding::Severity::error     ? Token::error
                           : finding.severity == csf::FlowFinding::Severity::warning ? Token::warn
                                                                                      : Token::text_dim;
        token_text(token, "%s", finding.message.c_str());
    }
    section("Objectives");
    if (flow->objectives().empty()) dim_text("No objectives are set up.");
    for (const auto& objective : flow->objectives()) {
        ImGui::PushID(objective.number);
        ImGui::Text("%d  %s  %s", objective.number,
                    objective.secondary ? (*objective.secondary ? "secondary" : "primary") : "",
                    text_label(state, objective.label).c_str());
        dim_text("set up by");
        ImGui::SameLine();
        for (const auto id : objective.defined_by) script_link("mission", id);
        dim_text("completed by");
        ImGui::SameLine();
        for (const auto id : objective.completed_by) script_link("mission", id);
        ImGui::NewLine();
        ImGui::PopID();
    }
    section("Events");
    if (ImGui::BeginTable("##flow_events", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Event");
        ImGui::TableSetupColumn("Starts");
        ImGui::TableSetupColumn("Raised by");
        ImGui::TableHeadersRow();
        for (const auto& event : flow->events()) {
            ImGui::PushID(event.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(event.name.c_str());
            if (event.builtin) {
                ImGui::SameLine();
                dim_text("engine");
            }
            ImGui::TableNextColumn();
            for (const auto id : event.listeners)
                script_link(flow->script("mission", id) ? "mission" : "cutscene", id);
            ImGui::TableNextColumn();
            for (const auto id : event.senders) script_link(flow->script("mission", id) ? "mission" : "cutscene", id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void draw_objectives(AppState& state) {
    auto& tools = state.tools;
    const bool project = state.authoring.project != nullptr;
    dim_text(project ? "Objectives, their completion scripts and the success check, as one undo step. Type the "
                       "text; the project stores it in the mission's text file (GlobalEK)."
                     : "Objectives, their completion scripts and the success check, as one undo step. Without an "
                       "authoring project, text fields are FLI string IDs.");
    std::vector<std::pair<int, std::string>> zones, actors;
    if (state.mission.scene) {
        for (const auto& area : state.mission.scene->areas())
            if (area.id) zones.emplace_back(*area.id, std::to_string(*area.id) + "  " + area.name.value_or(""));
        for (const auto& actor : state.mission.scene->actors())
            if (actor.id) actors.emplace_back(*actor.id, std::to_string(*actor.id) + "  " + actor.name.value_or(""));
    }
    static constexpr std::array<const char*, 3> kinds{"Reach a zone", "Kill an actor", "Use an object"};
    for (std::size_t i = 0; i < tools.objectives.size(); ++i) {
        auto& form = tools.objectives[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ImGui::Text("Objective %zu", i + 1);
        ImGui::SameLine();
        ImGui::Checkbox("secondary", &form.secondary);
        ImGui::SameLine();
        const bool remove = ImGui::SmallButton("Remove");
        if (begin_fields("##objective")) {
            begin_row("Kind");
            ImGui::Combo("##kind", &form.kind, kinds.data(), static_cast<int>(kinds.size()));
            begin_row(form.kind == 0 ? "Zone" : form.kind == 1 ? "Actor" : "Object");
            const auto& items = form.kind == 0 ? zones : actors;
            std::string current = std::to_string(form.target);
            for (const auto& [id, label] : items)
                if (id == form.target) current = label;
            if (const auto picked = filtered_combo("##target", current, items)) form.target = *picked;
            begin_row("Text");
            ImGui::InputText("##label", form.label.data(), form.label.size());
            begin_row("Done message");
            ImGui::InputText("##done", form.done.data(), form.done.size());
            if (form.kind == 2) {
                begin_row("Prompt");
                ImGui::InputText("##prompt", form.prompt.data(), form.prompt.size());
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
        if (remove) {
            tools.objectives.erase(tools.objectives.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (ImGui::Button("Add objective")) tools.objectives.emplace_back();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0F * ui_scale());
    ImGui::InputText("success message", tools.success_message.data(), tools.success_message.size());
    ImGui::BeginDisabled(tools.objectives.empty());
    if (ImGui::Button("Create objectives")) create_objectives(state);
    ImGui::EndDisabled();

    section("Starting equipment");
    for (std::size_t i = 0; i < tools.kits.size(); ++i) {
        auto& kit = tools.kits[i];
        ImGui::PushID(1000 + static_cast<int>(i));
        std::string current = std::to_string(kit.actor);
        for (const auto& [id, label] : actors)
            if (id == kit.actor) current = label;
        ImGui::SetNextItemWidth(160.0F * ui_scale());
        if (const auto picked = filtered_combo("##kit_actor", current, actors)) kit.actor = *picked;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-220.0F * ui_scale());
        ImGui::InputTextWithHint("##weapons", "weapons: 16,102@100/100,...", kit.weapons.data(), kit.weapons.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0F * ui_scale());
        ImGui::InputInt("select", &kit.selected, 0, 0);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0F * ui_scale());
        ImGui::InputInt("disguise", &kit.disguise, 0, 0);
        ImGui::PopID();
    }
    if (ImGui::Button("Add player kit")) tools.kits.emplace_back();
    ImGui::SameLine();
    ImGui::BeginDisabled(tools.kits.empty());
    if (ImGui::Button("Create equipment script")) create_equipment(state);
    ImGui::EndDisabled();

    section("Mission tips");
    ImGui::SetNextItemWidth(-150.0F * ui_scale());
    ImGui::InputTextWithHint("##tips", "FLI IDs, e.g. g200,g199", tools.tips.data(), tools.tips.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(!tools.tips[0]);
    if (ImGui::Button("Create tips script")) create_tips(state);
    ImGui::EndDisabled();
}

void draw_texts(AppState& state) {
    auto* project = state.authoring.project.get();
    if (!project || !project->texts) {
        dim_text("Mission text belongs to an authoring project with a texts record (see "
                 "docs/plans/editor-project-format.md).");
        return;
    }
    dim_text("%s in %s, IDs %d-%d. Changes rebuild the text file; package it with the project's GlobalEK "
             "workspace.",
             path_utf8(project->texts->file).c_str(), path_utf8(project->texts->archive).c_str(), project->texts->first,
             project->texts->last);
    std::optional<std::string> removed;
    std::optional<std::pair<std::string, std::string>> edited;
    if (ImGui::BeginTable("##texts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 60.0F * ui_scale());
        ImGui::TableSetupColumn("Text");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0F * ui_scale());
        for (const auto& string : project->strings) {
            ImGui::PushID(string.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(string.id.c_str());
            ImGui::TableNextColumn();
            std::string value;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (text_field("##text", string.text, value)) edited = std::pair{string.id, value};
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(icons::LC_X)) removed = string.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    auto& tools = state.tools;
    ImGui::SetNextItemWidth(-90.0F * ui_scale());
    ImGui::InputTextWithHint("##new_text", "new string", tools.new_text.data(), tools.new_text.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(!tools.new_text[0]);
    if (ImGui::Button("Add")) {
        if (const auto id = project->next_text_id()) {
            set_project_text(state, *id, std::string(tools.new_text.data()));
            tools.new_text[0] = '\0';
        } else {
            state.warn("The project's text ID range is full");
        }
    }
    ImGui::EndDisabled();
    if (edited) set_project_text(state, edited->first, edited->second);
    if (removed) set_project_text(state, *removed, std::nullopt);
}

void draw_cutscene(AppState& state) {
    auto& tools = state.tools;
    dim_text("Travelling shots for an intro cutscene: frame each shot in the viewport and capture it; the camera "
             "moves by the travel distance at constant height while keeping its target in view. Playing the shots "
             "here approximates the game (its field of view and timing differ).");
    if (ImGui::Button("Capture shot from view")) capture_shot(state);
    ImGui::SameLine();
    ImGui::BeginDisabled(tools.shots.empty());
    if (!tools.preview_started) {
        if (ImGui::Button("Play shots")) play_shots(state);
    } else if (ImGui::Button("Stop")) {
        stop_shots(state);
    }
    ImGui::EndDisabled();
    std::optional<std::size_t> removed, raised;
    for (std::size_t i = 0; i < tools.shots.size(); ++i) {
        auto& shot = tools.shots[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::Separator();
        ImGui::Text("Shot %zu", i + 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Start")) view_shot(state, i, 0.0F);
        ImGui::SameLine();
        if (ImGui::SmallButton("End")) view_shot(state, i, 1.0F);
        ImGui::SameLine();
        if (ImGui::SmallButton("Recapture")) {
            const auto eye = state.preview.eye_position(), target = state.preview.orbit_target();
            shot.camera = {eye.x, eye.y, eye.z};
            shot.target = {target.x, target.y, target.z};
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("Up")) raised = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) removed = i;
        ImGui::SetNextItemWidth(90.0F * ui_scale());
        ImGui::DragFloat("seconds", &shot.seconds, 0.1F, 0.5F, 60.0F, "%.1f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0F * ui_scale());
        float travel[2]{shot.travel_x, shot.travel_z};
        if (ImGui::DragFloat2("travel x z (cm)", travel, 5.0F, -5000.0F, 5000.0F, "%.0f")) {
            shot.travel_x = travel[0];
            shot.travel_z = travel[1];
        }
        dim_text("camera %.0f %.0f %.0f, target %.0f %.0f %.0f", shot.camera.x, shot.camera.y, shot.camera.z,
                 shot.target.x, shot.target.y, shot.target.z);
        ImGui::PopID();
    }
    if (removed) tools.shots.erase(tools.shots.begin() + static_cast<std::ptrdiff_t>(*removed));
    if (raised) std::swap(tools.shots[*raised], tools.shots[*raised - 1]);
    ImGui::Separator();
    ImGui::Checkbox("Start the actors' scripts (raise INIT)", &tools.send_init);
    ImGui::BeginDisabled(tools.shots.empty());
    if (ImGui::Button("Create intro cutscene")) create_intro(state);
    ImGui::EndDisabled();
}

void draw_import(AppState& state) {
    dim_text("Copies a class (with its models, collision, weapons, textures and animations) or an "
             "animation from another unpacked mission into this one, updating the package indexes.");
    ImGui::SetNextItemWidth(-90.0F * ui_scale());
    ImGui::InputTextWithHint("##donor", "unpacked mission folder", state.ui.import_donor.data(),
                             state.ui.import_donor.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) request_file_dialog(state, DialogKind::import_donor, state.settings.resource_root);
    if (!state.discovered.empty()) {
        std::vector<std::pair<std::string, std::string>> packages;
        for (const auto& mission : state.discovered)
            if (std::ranges::find(packages, mission.package, &std::pair<std::string, std::string>::first) ==
                packages.end())
                packages.emplace_back(mission.package, mission.package);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (const auto picked = filtered_combo("##donor_pick", "Or pick a discovered mission...", packages)) {
            const auto text = path_utf8(state.settings.resource_root / *picked);
            std::snprintf(state.ui.import_donor.data(), state.ui.import_donor.size(), "%s", text.c_str());
        }
    }
    if (!state.ui.import_donor[0]) return;
    const auto root = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(state.ui.import_donor.data()), std::strlen(state.ui.import_donor.data())));
    const auto& donor = donor_for(root);
    if (!donor.error.empty()) {
        token_text(Token::warn, "%s", donor.error.c_str());
        return;
    }
    search_input("##import_filter", "filter donor classes", state.ui.class_filter.data(), state.ui.class_filter.size());
    if (ImGui::BeginChild("##donor_classes", {0, 160.0F * ui_scale()}, ImGuiChildFlags_Borders)) {
        for (const auto& definition : donor.objects.definitions()) {
            if (!definition.class_id) continue;
            const auto label = std::to_string(*definition.class_id) + "  " + definition.name.value_or("");
            if (!matches(label, state.ui.class_filter.data())) continue;
            const bool present = state.mission.objects && !state.mission.objects->find_class(*definition.class_id).empty();
            ImGui::PushID(*definition.class_id);
            ImGui::BeginDisabled(present);
            if (ImGui::SmallButton(present ? "present" : "Import"))
                apply_mission_edit(state, state.mission.editor->import_class(root, *definition.class_id));
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    static int animation_id = 0;
    ImGui::SetNextItemWidth(120.0F * ui_scale());
    ImGui::InputInt("##import_anim", &animation_id, 0, 0);
    ImGui::SameLine();
    const auto* record = donor.animations.find_id(animation_id);
    ImGui::BeginDisabled(!record);
    if (ImGui::Button("Import animation"))
        apply_mission_edit(state, state.mission.editor->import_animation(root, animation_id));
    ImGui::EndDisabled();
    ImGui::SameLine();
    dim_text("%s", record ? record->logical_name.c_str() : "enter a donor Anims.bdd ID");
}

} // namespace

void draw_mission_edit_section(AppState& state, const std::uint32_t entry) {
    if (!mission_editable(state)) return;
    const auto& scene = *state.mission.scene;
    const auto key = mission_record_key(scene, entry);
    if (key.kind == MissionRecordKey::Kind::none) return;
    if (!begin_section(state, "Edit", true)) return;
    ImGui::PushID(static_cast<int>(key.kind) * 1000003 + key.id * 1009 + key.sub_id);
    using Kind = MissionRecordKey::Kind;
    switch (key.kind) {
    case Kind::actor:
        for (const auto& actor : scene.actors())
            if (actor.id == key.id) draw_actor_editor(state, actor);
        break;
    case Kind::dummy:
        for (const auto& dummy : scene.dummies())
            if (dummy.id == key.id) draw_dummy_editor(state, dummy);
        break;
    case Kind::light:
        for (const auto& light : scene.lights())
            if (light.id == key.id) draw_light_editor(state, light);
        break;
    case Kind::nav_point:
        if (const auto* point = scene.navigation_point(key.id, key.sub_id)) draw_nav_point_editor(state, *point);
        break;
    case Kind::area:
        for (const auto& area : scene.areas())
            if (area.id == key.id) draw_area_editor(state, area);
        break;
    case Kind::nav_group:
        if (ImGui::Button((std::string(icons::LC_PLUS) + " Add point at view center").c_str())) {
            const auto target = state.preview.view_target();
            std::int32_t added{};
            if (apply_mission_edit(state, state.mission.editor->add_navigation_point(
                                              key.id, {target.x, target.y, target.z}, &added)))
                select_after_refresh(state, {Kind::nav_point, key.id, added});
        }
        break;
    default:
        dim_text("Use the fields below to edit this record.");
        break;
    }
    if (ImGui::TreeNode("All fields")) {
        dim_text("Every stored value of the record; edits keep the stored value kind.");
        if (const auto* node = find_node(state.mission.document->roots(), entry))
            if (begin_fields("##raw_fields")) {
                draw_raw_scalars(state, *node, *state.mission.document, {});
                ImGui::EndTable();
            }
        ImGui::TreePop();
    }
    ImGui::PopID();
    end_section();
}

void draw_map_instance_edit_section(AppState& state, const rws::SceneInstance& instance) {
    if (!map_instances_editable(state)) return;
    if (!begin_section(state, "Edit", true)) return;
    ImGui::PushID(static_cast<int>(instance.offset));
    if (begin_fields("##instance_fields")) {
        csf::Vec3 position{instance.position.x, instance.position.y, instance.position.z};
        const float yaw = map_instance_yaw(instance);
        if (vec3_field("position", position, position)) set_map_instance_pose(state, instance, position, yaw);
        float degrees = yaw * degrees_per_radian;
        if (float_field("yaw", degrees, degrees, 0.5F, "%.1f deg"))
            set_map_instance_pose(state, instance, position, degrees / degrees_per_radian);
        ImGui::EndTable();
    }
    dim_text("Static map prop in the visual map stream. The move tool (G) and rotate tool (R) work "
             "on it too. Its collision is baked into the collision map and stays where it was.");
    ImGui::PopID();
    end_section();
}

void draw_changes(AppState& state) {
    if (!state.mission.editor || !state.mission.scene) {
        section("Session changes");
        if (state.document && state.document->dirty())
            token_text(Token::dirty, "%s %s has unsaved byte edits", icons::LC_DOT,
                       path_utf8(state.document->source_path().filename()).c_str());
        else
            dim_text("No pending changes. Open a mission to edit it; byte edits made in the Hex "
                     "workspace appear here.");
        return;
    }
    if (!mission_editable(state)) {
        dim_text("The mission is loading.");
        return;
    }
    draw_project_bar(state);
    if (!ImGui::BeginTabBar("##mission_editing")) return;
    if (ImGui::BeginTabItem("History")) {
        draw_history(state);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Files")) {
        draw_files(state);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Mission")) {
        draw_mission_properties(state);
        ImGui::EndTabItem();
    }
    const auto tab = [&](const char* name) {
        const bool focus = state.ui.mission_edit_tab && std::strcmp(state.ui.mission_edit_tab, name) == 0;
        if (focus) state.ui.mission_edit_tab = nullptr;
        return ImGui::BeginTabItem(name, nullptr, focus ? ImGuiTabItemFlags_SetSelected : 0);
    };
    if (tab("Assets")) {
        draw_assets(state);
        ImGui::EndTabItem();
    }
    if (tab("Presets")) {
        draw_presets(state);
        ImGui::EndTabItem();
    }
    if (tab("Objectives")) {
        draw_objectives(state);
        ImGui::EndTabItem();
    }
    if (tab("Cutscene")) {
        draw_cutscene(state);
        ImGui::EndTabItem();
    }
    if (tab("Flow")) {
        draw_flow(state);
        ImGui::EndTabItem();
    }
    if (tab("Texts")) {
        draw_texts(state);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Import")) {
        draw_import(state);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void draw_mission_dialogs(AppState& state) {
    if (state.ui.pending_discard) {
        ImGui::OpenPopup("Unsaved mission edits");
        if (ImGui::BeginPopupModal("Unsaved mission edits", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("The mission has unsaved edits. %s discards them unless they are saved first.",
                        state.ui.pending_discard_label.c_str());
            const auto run = [&state] {
                auto action = std::exchange(state.ui.pending_discard, nullptr);
                ImGui::CloseCurrentPopup();
                if (action) action();
            };
            if (ImGui::Button("Save and continue")) {
                save_mission_project(state);
                if (!state.mission.editor->dirty()) run();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard edits")) run();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                state.ui.pending_discard = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    if (!state.ui.show_export_dialog) return;
    if (!mission_editable(state)) {
        state.ui.show_export_dialog = false;
        return;
    }
    ImGui::OpenPopup("Export mission archive");
    ImGui::SetNextWindowSize({620.0F * ui_scale(), 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Export mission archive", &state.ui.show_export_dialog)) return;
    auto& ui = state.ui;
    if (ImGui::IsWindowAppearing()) {
        if (!ui.export_original[0])
            std::snprintf(ui.export_original.data(), ui.export_original.size(), "%s",
                          path_utf8(guess_original_archive(state)).c_str());
        if (!ui.export_output[0]) {
            const auto workspace = state.mission.project ? state.mission.project->workspace_root
                                                         : default_project_workspace(state);
            std::snprintf(ui.export_output.data(), ui.export_output.size(), "%s",
                          path_utf8(workspace / "export" / (mission_name(state) + ".pak")).c_str());
        }
    }
    dim_text("Rebuilds the complete mission archive from the shipped one: every entry keeps its "
             "order, name, timestamp and compressed bytes, and only the project's files are replaced "
             "or added. Drop the result into the game's maps folder in place of the original.");
    ImGui::TextUnformatted("Shipped archive");
    ImGui::SetNextItemWidth(-90.0F * ui_scale());
    ImGui::InputText("##original", ui.export_original.data(), ui.export_original.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...##original")) request_file_dialog(state, DialogKind::export_original, state.settings.game_root);
    ImGui::TextUnformatted("Write to");
    ImGui::SetNextItemWidth(-90.0F * ui_scale());
    ImGui::InputText("##output", ui.export_output.data(), ui.export_output.size());
    ImGui::SameLine();
    if (ImGui::Button("Browse...##output")) request_file_dialog(state, DialogKind::export_output);
    ImGui::Checkbox("Replace an existing output file", &ui.export_overwrite);
    ImGui::Checkbox("Install into the game (back up and replace the shipped archive)", &ui.export_install);
    if (ui.export_install)
        dim_text("The original is copied to .csf-mod-backups in the game folder; csf-mod rollback "
                 "<deployment.state> restores it.");
    if (!csf::ModProject::builtin_pak_support())
        token_text(Token::warn, "This build runs pakman-cli from PATH to write the archive.");
    const auto to_path = [](const std::array<char, 1024>& text) {
        return std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(text.data()), std::strlen(text.data())));
    };
    ImGui::BeginDisabled(!ui.export_original[0] || !ui.export_output[0]);
    if (ImGui::Button((std::string(icons::LC_PACKAGE) + " Export").c_str())) {
        export_mission_archive(state, to_path(ui.export_original), to_path(ui.export_output), ui.export_overwrite,
                               ui.export_install);
        ui.show_export_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ui.show_export_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace rwsman::ui
