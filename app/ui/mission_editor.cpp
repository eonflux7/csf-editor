// Mission editing UI: the record edit cards (Properties and the Inspector's
// Edit section), the Mission mode panels (History, Mission settings, Assets,
// Behaviours, Objectives, Intro cutscene, Flow, Texts, Build), and the export
// and unsaved-edits dialogs. Every change goes through csf::MissionEditor via
// mission_editing.hpp; nothing here writes files directly.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "authoring.hpp"
#include "commands.hpp"
#include "file_dialogs.hpp"
#include "mission_authoring.hpp"
#include "references.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "rwsman/entity_kind.hpp"
#include "viewport_tools.hpp"
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
        if (secondary_button((std::string(icons::LC_COPY) + " Duplicate").c_str())) duplicate_selected_record(state);
        ImGui::SameLine();
    }
    if (danger_button((std::string(icons::LC_TRASH) + " Delete").c_str())) delete_selected_record(state, false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Refuses while scripts reference the record; Shift+Delete forces");
}

void draw_actor_editor(AppState& state, const csf::MissionActor& actor) {
    const auto id = *actor.id;
    csf::Vec3 position = actor.position.value_or(csf::Vec3{});
    float heading = actor.heading.value_or(0);
    if (begin_card("##transform", "Transform", {icons::LC_MOVE_3D})) {
        if (begin_fields("##actor_transform")) {
            if (vec3_field("position", position, position))
                apply_mission_edit(state, editor(state).set_actor_placement(
                                              id, {position, actor.heading.value_or(0), actor.pitch.value_or(0)}));
            if (float_field("heading", heading, heading, 0.5F, "%.1f deg"))
                apply_mission_edit(state, editor(state).set_actor_placement(
                                              id, {position, heading, actor.pitch.value_or(0)}));
            float pitch = actor.pitch.value_or(0);
            if (float_field("pitch", pitch, pitch, 0.5F, "%.1f deg"))
                apply_mission_edit(state, editor(state).set_actor_placement(id, {position, heading, pitch}));
            ImGui::EndTable();
        }
        end_card();
    }

    if (begin_card("##identity", "Identity", {icons::LC_USER})) {
        if (begin_fields("##actor_identity")) {
            std::string name;
            if (text_field("name", actor.name.value_or(""), name))
                apply_mission_edit(state, editor(state).set_actor_name(id, name));
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
            ImGui::EndTable();
        }
        const bool player = state.mission.scene->player().active_player == actor.id;
        ImGui::BeginDisabled(player);
        if (secondary_button(player ? "Starting player" : "Make starting player"))
            apply_mission_edit(state, editor(state).set_player_actor(id));
        ImGui::EndDisabled();
        end_card();
    }

    // Per-actor scripts: non-trigger scripts run with THIS = this actor.
    if (begin_card("##scripts", "Scripts", {icons::LC_SCROLL_TEXT, {}, nullptr,
                                            "Scripts that run with THIS = this actor. Behaviours in the "
                                            "Behaviours panel write the actor, its route and its script "
                                            "together."})) {
        const auto choices = editor(state).actor_script_choices();
        auto scripts = actor.script_ids;
        if (scripts.empty()) dim_text("No scripts: the actor stands still.");
        for (std::size_t i = 0; i < scripts.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto choice =
                std::ranges::find(choices, scripts[i], &std::pair<std::int32_t, std::string>::first);
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
        if (secondary_button("Set up a behaviour...")) {
            state.tools.class_id = actor.class_id.value_or(0);
            show_panel(state, Panel::behaviours);
        }
        end_card();
    }

    // Animation overrides: per-actor .ANIMACIONES slots.
    CardOptions animation_options{icons::LC_FILM};
    animation_options.default_open = !actor.animations.empty();
    animation_options.help = "Slots come from the game's slot table; preview a clip on the actor in the Inspect "
                             "mode's Animation workspace. Scripts can still play other animations over these.";
    if (begin_card("##animations", "Animation overrides", animation_options)) {
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
        end_card();
    }

    CardOptions advanced{icons::LC_SLIDERS_HORIZONTAL};
    advanced.default_open = false;
    advanced.help = "Raw actor fields the editor does not explain yet; the values are written as they are.";
    if (begin_card("##advanced", "Advanced", advanced)) {
        if (begin_fields("##actor_advanced")) {
            int value{};
            if (int_field("collision", actor.collision.value_or(0), value))
                apply_mission_edit(state, editor(state).set_actor_integer(id, ".COLISION", value));
            if (int_field("flags", static_cast<int>(actor.flags.value_or(0)), value))
                apply_mission_edit(state, editor(state).set_actor_integer(id, ".FLAGS", value));
            if (int_field("2nd explosion", actor.secondary_explosion.value_or(0), value))
                apply_mission_edit(state, editor(state).set_actor_integer(id, ".SEGUNDA_EXPLOSION", value));
            ImGui::EndTable();
        }
        end_card();
    }
    record_buttons(state, true);
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

void project_bar(AppState& state) {
    auto& mission = state.mission;
    auto& editor = *mission.editor;
    if (begin_card("##project", "Project", {icons::LC_FOLDER})) {
        if (mission.project)
            dim_text("%s", state.ui.shown(path_utf8(mission.project->workspace_root)).c_str());
        else
            dim_text("Not saved yet. Save creates a project in %s",
                     state.ui.shown(path_utf8(default_project_workspace(state))).c_str());
        const bool unsaved = edits_unsaved(state);
        ImGui::BeginDisabled(!unsaved && mission.project);
        if (secondary_button((std::string(icons::LC_SAVE) + (unsaved ? " Save" : " Saved")).c_str()))
            save_mission_project(state);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (secondary_button("Save as...")) state.commands.run("file.save_mission_as");
        end_card();
    }
    if (state.authoring.project && begin_card("##map", "Map", {icons::LC_MOUNTAIN})) {
        const bool busy = authoring_pending(state);
        const auto findings = state.authoring.findings.size();
        if (busy)
            token_text(Token::inferred, "Building...");
        else if (findings)
            token_text(Token::warn, "%zu placements or actors are off their height rules", findings);
        else
            dim_text("Built from the Blender exports and the placements.");
        ImGui::BeginDisabled(busy);
        if (secondary_button("Rebuild map")) state.commands.run("mission.project_rebuild");
        ImGui::SameLine();
        if (secondary_button("Height report")) state.commands.run("mission.project_heights");
        ImGui::SameLine();
        ImGui::BeginDisabled(findings == 0);
        if (secondary_button("Resnap all")) state.commands.run("mission.project_resnap");
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (secondary_button((std::string(icons::LC_MOUNTAIN) + " Edit in Blender").c_str()))
            state.commands.run("build.edit_in_blender");
        end_card();
    }
    // An authoring project builds both archives (the Build panel's pipeline).
    if (!state.authoring.project && begin_card("##package", "Package", {icons::LC_PACKAGE})) {
        dim_text("Rebuilds the mission archive from the shipped one, replacing or adding only the changed files.");
        if (primary_button((std::string(icons::LC_PACKAGE) + " Export mission archive...").c_str()))
            state.ui.show_export_dialog = true;
        end_card();
    }
}

void history_body(AppState& state) {
    ImGui::BeginDisabled(!can_undo_edit(state));
    if (secondary_button((std::string(icons::LC_UNDO_2) + " Undo").c_str())) undo_edit(state);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_redo_edit(state));
    if (secondary_button((std::string(icons::LC_REDO_2) + " Redo").c_str())) redo_edit(state);
    ImGui::EndDisabled();
    ImGui::SameLine();
    dim_text("Ctrl+Z / Ctrl+Y; click a step to go back to it.");
    // Mission edits and project edits (placements, texts) in one list.
    const auto [labels, position] = edit_history(state);
    if (labels.empty()) {
        empty_state(icons::LC_HISTORY, "No edits yet. Select something in the viewport or the Outliner and change "
                                       "it in Properties, or move it with G and turn it with R.");
        return;
    }
    const auto go_to = [&state](const std::size_t target) {
        for (int guard = 0; guard < 10000; ++guard) {
            const auto current = edit_history(state).second;
            if (current > target && can_undo_edit(state))
                undo_edit(state);
            else if (current < target && can_redo_edit(state))
                redo_edit(state);
            else
                break;
        }
    };
    if (ImGui::Selectable("(original)", position == 0)) go_to(0);
    for (std::size_t i = 0; i < labels.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const bool undone = i >= position;
        if (undone) ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        if (ImGui::Selectable(labels[i].c_str(), i + 1 == position)) go_to(i + 1);
        if (undone) ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

void files_body(AppState& state) {
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

void mission_settings_body(AppState& state) {
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

// One placeable thing in the Assets panel: a class (this mission's or another
// mission's, imported on placing) or a building of the authoring project.
struct AssetRow {
    EntityKind kind{};
    std::optional<AuthoringTools::CatalogEntry> entry;
    std::string building;
    std::string label, detail;
};

// The Assets panel's chips: which kinds each shows.
bool in_category(const EntityKind kind, const int category) {
    switch (category) {
    case 1: return kind == EntityKind::player || kind == EntityKind::enemy || kind == EntityKind::animal;
    case 2: return kind == EntityKind::vehicle;
    case 3: return kind == EntityKind::prop || kind == EntityKind::usable || kind == EntityKind::helper;
    case 4: return kind == EntityKind::pickup;
    case 5: return kind == EntityKind::building;
    default: return true;
    }
}

bool armed(const AuthoringTools& tools, const AssetRow& row) {
    if (!row.building.empty()) return tools.place_building_asset == row.building;
    return tools.place_entry && row.entry && tools.place_entry->class_id == row.entry->class_id &&
           tools.place_entry->package == row.entry->package;
}

void asset_row(AppState& state, const AssetRow& row, const int id) {
    auto& tools = state.tools;
    const bool active = armed(tools, row) && state.preview.edit_tool() == GeometryPreview::EditTool::place;
    ImGui::PushID(id);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    // The row's name is its ID, so UI scripts can address it ("Assets::OFICIAL").
    if (ImGui::Selectable(("##" + row.label).c_str(), active, ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // Double click: at the view centre, as before the Place tool.
            if (!row.building.empty()) place_building(state, row.building);
            else if (row.entry) place_asset(state, *row.entry);
        } else if (!row.building.empty()) {
            tools.place_entry.reset();
            tools.place_building_asset = row.building;
            set_viewport_tool(state, GeometryPreview::EditTool::place);
        } else if (row.entry) {
            arm_place_tool(state, *row.entry);
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Click, then click in the viewport to place it (Shift keeps placing).\n"
                          "Drag it into the viewport, or double-click to place it at the view centre.");
    if (ImGui::BeginDragDropSource()) {
        tools.drag_entry = row.entry;
        tools.drag_building = row.building;
        ImGui::SetDragDropPayload("RWSMAN_ASSET", &id, sizeof(id));
        ImGui::TextUnformatted(row.label.c_str());
        ImGui::EndDragDropSource();
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::PushStyleColor(ImGuiCol_Text, kind_color(row.kind));
    ImGui::TextUnformatted(kind_icon(row.kind));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(row.label.c_str());
    if (!row.detail.empty()) {
        ImGui::SameLine();
        ImGui::PushFont(font(Font::caption));
        dim_text("%s", row.detail.c_str());
        ImGui::PopFont();
    }
    ImGui::PopID();
}

void assets_body(AppState& state) {
    auto& tools = state.tools;
    search_input("##asset_filter", "filter by ID, name, type or mission", tools.asset_filter.data(),
                 tools.asset_filter.size());
    ImGui::SameLine();
    help_marker("Click an asset, then click in the viewport to place it; drag it into the viewport; or "
                "double-click it to place it at the view centre. Classes from other missions are imported "
                "first (with their models, textures and animations); one undo step either way. Buildings are "
                "the project's own, built into the map.");
    static constexpr std::array<const char*, 6> categories{"All", "Characters", "Vehicles", "Props", "Pickups",
                                                           "Buildings"};
    // The chips wrap onto a second line in a narrow panel.
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < static_cast<int>(categories.size()); ++i) {
        const char* label = categories[static_cast<std::size_t>(i)];
        const float width = ImGui::CalcTextSize(label).x + 16.0F * ui_scale();
        if (i) {
            ImGui::SameLine(0.0F, 4.0F);
            if (ImGui::GetCursorScreenPos().x + width > right) ImGui::NewLine();
        }
        if (chip(label, tools.asset_category == i)) tools.asset_category = i;
    }
    const char* filter = tools.asset_filter.data();
    const auto kind_of = [](const std::string& type, const std::string& name) {
        return classify_actor({true, type, name, {}}, false);
    };
    int id = 0;
    std::size_t shown = 0;
    const auto show = [&](const AssetRow& row) {
        if (!in_category(row.kind, tools.asset_category) || !matches(row.label + "  " + row.detail, filter)) return;
        ++shown;
        asset_row(state, row, id++);
    };
    if (ImGui::BeginChild("##assets", {0, 0}, ImGuiChildFlags_Borders)) {
        if (const auto* project = state.authoring.project.get(); project && in_category(EntityKind::building, tools.asset_category)) {
            section("Project buildings");
            bool any = false;
            for (const auto& asset : project->assets) {
                if (asset.kind != csf::ProjectAsset::Kind::building) continue;
                any = true;
                std::size_t placed = 0;
                for (const auto& placement : project->placements) placed += placement.asset == asset.id;
                show({EntityKind::building, std::nullopt, asset.id, asset.id, std::to_string(placed) + " placed"});
            }
            if (!any) dim_text("Tag a mesh as a building in Blender and send it (CSF panel).");
        }
        if (tools.asset_category != 5) {
            section("This mission");
            if (state.mission.objects)
                for (const auto& definition : state.mission.objects->definitions()) {
                    if (!definition.class_id) continue;
                    const auto name = definition.name.value_or(""), type = definition.type.value_or("");
                    show({kind_of(type, name),
                          AuthoringTools::CatalogEntry{state.mission.editor->package_root(), "this mission",
                                                       *definition.class_id, name, type},
                          {}, name.empty() ? "class " + std::to_string(*definition.class_id) : name,
                          type + "  ·  " + std::to_string(*definition.class_id)});
                }
            section("Other missions");
            if (tools.catalog_root != state.settings.resource_root || tools.catalog.empty()) {
                ImGui::BeginDisabled(state.discovered.empty());
                if (secondary_button("List their classes")) build_asset_catalog(state);
                ImGui::EndDisabled();
                if (state.discovered.empty()) dim_text("Set the resource root in Preferences to find other missions.");
            } else {
                const std::size_t before = shown;
                for (const auto& entry : tools.catalog) {
                    if (shown - before >= 300) {
                        dim_text("Refine the filter to see more.");
                        break;
                    }
                    show({kind_of(entry.type, entry.name), entry, {}, entry.name,
                          entry.type + "  ·  " + std::to_string(entry.class_id) + "  ·  " + entry.package_name});
                }
            }
        }
        if (shown == 0 && filter[0]) dim_text("Nothing matches the filter.");
    }
    ImGui::EndChild();
}

void behaviours_body(AppState& state) {
    auto& tools = state.tools;
    using Preset = AuthoringTools::Preset;
    ImGui::TextUnformatted("Behaviour");
    help_marker("Behaviour presets write the actor, its navigation groups and its script together, as one undo "
                "step. Points are taken at the ground under the viewport centre.");
    static constexpr std::array<const char*, 5> names{"Guard on patrol", "Guard idling", "Animal on patrol",
                                                      "Cover group", "Walk grid"};
    int preset = static_cast<int>(tools.preset);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool picked_preset = ImGui::Combo("##preset", &preset, names.data(), static_cast<int>(names.size()));
    name_last_item("##preset");
    if (picked_preset) tools.preset = static_cast<Preset>(preset);
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
            begin_row("Preview");
            ImGui::Checkbox("##preview_grid", &tools.preview_grid);
            if (tools.preview_grid) {
                update_walk_grid_preview(state);
                ImGui::SameLine();
                dim_text("%zu points, %zu links in the viewport", tools.grid_points.size(), tools.grid_links.size());
            }
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

void flow_body(AppState& state) {
    const auto* flow = mission_flow(state);
    if (!flow) return;
    // The graph first (S3); the lists below it as Details.
    if (ImGui::BeginTabBar("##flow_views")) {
        if (ImGui::BeginTabItem("Graph")) {
            draw_flow_graph(state);
            ImGui::EndTabItem();
        }
        const bool details = ImGui::BeginTabItem("Details");
        if (!details) {
            ImGui::EndTabBar();
            return;
        }
        ImGui::EndTabItem();
        ImGui::EndTabBar();
    }
    ImGui::TextUnformatted("How the scripts connect");
    help_marker("Read from the programs as they are. Findings are evidence to check, not proof of what the game "
                "does.");
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

void objectives_body(AppState& state) {
    auto& tools = state.tools;
    const bool project = state.authoring.project != nullptr;
    // The objectives, equipment and tips already made: their recipes, editable.
    draw_component_cards(state, {"objective", "objectives", "kit", "equipment", "tips"});
    ImGui::TextUnformatted("New objectives");
    help_marker(project ? "Objectives, their completion scripts and the success check, as one undo step. Type the "
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
            ImGui::SetNextItemWidth(-ImGui::GetFrameHeightWithSpacing());
            if (const auto picked = filtered_combo("##target", current, items)) form.target = *picked;
            ImGui::SameLine(0.0F, 2.0F);
            const bool zone = form.kind == 0;
            pick_button(state, "##pick_target", {zone ? MissionRecordKey::Kind::area : MissionRecordKey::Kind::actor},
                        zone ? "Pick the objective's zone" : "Pick the objective's actor",
                        [&tools, i](const MissionRecordKey& key) {
                            if (i < tools.objectives.size()) tools.objectives[i].target = key.id;
                        });
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
        ImGui::SameLine(0.0F, 2.0F);
        pick_button(state, "##pick_kit_actor", {MissionRecordKey::Kind::actor}, "Pick the player this kit is for",
                    [&tools, i](const MissionRecordKey& key) {
                        if (i < tools.kits.size()) tools.kits[i].actor = key.id;
                    });
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

    // Triggers (E10): When -> If -> Do, each a component; Convert to script detaches one.
    section("Triggers");
    help_marker("Mission logic without script text: when something happens (the player enters a zone, an actor "
                "dies...), if an objective is or is not complete, do some actions. Each trigger is one script "
                "of a known shape; Convert to script turns it into ordinary script text.");
    draw_component_cards(state, {"trigger"});
    auto& draft = tools.trigger_draft;
    ImGui::PushID("new_trigger");
    if (begin_properties("##new_trigger")) {
        property_row("Name");
        std::string name;
        if (edit_text_value("##trigger_name", draft.script.name, name, "TRIGGER")) draft.script.name = name;
        trigger_fields(state, draft, [&state](const std::function<void(csf::Trigger&)>& edit) {
            edit(state.tools.trigger_draft);
        }, true);
        end_properties();
    }
    ImGui::Checkbox("Offer unverified events and actions", &tools.show_unverified);
    help_marker("Events and actions seen in the shipped missions but not yet played in a mission made here.");
    ImGui::BeginDisabled(draft.actions.empty());
    if (primary_button("Create trigger")) create_trigger(state);
    ImGui::EndDisabled();
    ImGui::PopID();
}

void texts_body(AppState& state) {
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

void cutscene_body(AppState& state) {
    auto& tools = state.tools;
    draw_component_cards(state, {"shot", "intro"});
    help_marker("Travelling shots for an intro cutscene: frame each shot in the viewport and capture it; the "
                "camera moves by the travel distance at constant height while keeping its target in view. Playing "
                "the shots here approximates the game (its field of view and timing differ).");
    ImGui::SameLine();
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

void import_body(AppState& state) {
    help_marker("Copies a class (with its models, collision, weapons, textures and animations) or an animation "
                "from another unpacked mission into this one, updating the package indexes.");
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

void draw_record_editor(AppState& state, const std::uint32_t entry) {
    if (!mission_editable(state)) return;
    const auto& scene = *state.mission.scene;
    const auto key = mission_record_key(scene, entry);
    if (key.kind == MissionRecordKey::Kind::none) return;
    ImGui::PushID(static_cast<int>(key.kind) * 1000003 + key.id * 1009 + key.sub_id);
    using Kind = MissionRecordKey::Kind;
    // Records other than actors edit in one card named after their kind.
    const auto card = [&](const char* title, const EntityKind kind, auto&& body) {
        if (!begin_card("##record", title, {kind_icon(kind), kind_color(kind)})) return;
        body();
        end_card();
    };
    switch (key.kind) {
    case Kind::actor:
        for (const auto& actor : scene.actors())
            if (actor.id == key.id) draw_actor_editor(state, actor);
        break;
    case Kind::dummy:
        for (const auto& dummy : scene.dummies())
            if (dummy.id == key.id) card("Marker", EntityKind::marker, [&] { draw_dummy_editor(state, dummy); });
        break;
    case Kind::light:
        for (const auto& light : scene.lights())
            if (light.id == key.id) card("Light", EntityKind::light, [&] { draw_light_editor(state, light); });
        break;
    case Kind::nav_point:
        if (const auto* point = scene.navigation_point(key.id, key.sub_id))
            card("Point", EntityKind::route, [&] { draw_nav_point_editor(state, *point); });
        break;
    case Kind::area:
        for (const auto& area : scene.areas())
            if (area.id == key.id) card("Zone", EntityKind::zone, [&] { draw_area_editor(state, area); });
        break;
    case Kind::nav_group:
        card("Points", EntityKind::route, [&] {
            if (secondary_button((std::string(icons::LC_PLUS) + " Add point at view center").c_str())) {
                const auto target = state.preview.view_target();
                std::int32_t added{};
                if (apply_mission_edit(state, state.mission.editor->add_navigation_point(
                                                  key.id, {target.x, target.y, target.z}, &added)))
                    select_after_refresh(state, {Kind::nav_point, key.id, added});
            }
        });
        break;
    default:
        break;
    }
    CardOptions raw{icons::LC_BRACES};
    raw.default_open = false;
    raw.help = "Every stored value of the record; edits keep the stored value kind.";
    if (begin_card("##all_fields", "All fields", raw)) {
        if (const auto* node = find_node(state.mission.document->roots(), entry))
            if (begin_fields("##raw_fields")) {
                draw_raw_scalars(state, *node, *state.mission.document, {});
                ImGui::EndTable();
            }
        end_card();
    }
    ImGui::PopID();
}

void draw_mission_edit_section(AppState& state, const std::uint32_t entry) {
    if (!mission_editable(state)) return;
    if (mission_record_key(*state.mission.scene, entry).kind == MissionRecordKey::Kind::none) return;
    if (!begin_section(state, "Edit", true)) return;
    draw_record_editor(state, entry);
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

namespace {

// Panels that edit the mission need an editable mission; otherwise they say so.
bool editing_ready(AppState& state) {
    if (mission_editable(state)) return true;
    if (state.mission.editor && state.mission.scene)
        dim_text("The mission is loading.");
    else
        empty_state(icons::LC_FILE, "Open a mission or a project to edit it.");
    return false;
}

} // namespace

void draw_changes(AppState& state) {
    section("Session changes");
    if (state.document && state.document->dirty())
        token_text(Token::dirty, "%s %s has unsaved byte edits", icons::LC_DOT,
                   path_utf8(state.document->source_path().filename()).c_str());
    else
        dim_text("No byte edits. Byte edits made in the Hex workspace appear here.");
    if (mission_editable(state)) {
        section("Mission files");
        files_body(state);
    }
}

void draw_history_panel(AppState& state) {
    if (editing_ready(state)) history_body(state);
}

void draw_mission_settings(AppState& state) {
    if (editing_ready(state)) mission_settings_body(state);
}

void draw_assets_panel(AppState& state) {
    if (!editing_ready(state)) return;
    if (ImGui::CollapsingHeader("Import a class without placing it")) import_body(state);
    assets_body(state);
}

void draw_behaviours(AppState& state) {
    if (editing_ready(state)) behaviours_body(state);
}

void draw_flow_panel(AppState& state) {
    if (editing_ready(state)) flow_body(state);
}

void draw_objectives_panel(AppState& state) {
    if (editing_ready(state)) objectives_body(state);
}

void draw_texts_panel(AppState& state) {
    if (editing_ready(state)) texts_body(state);
}

void draw_timeline(AppState& state) {
    if (editing_ready(state)) cutscene_body(state);
}

void draw_build(AppState& state) {
    if (!editing_ready(state)) return;
    project_bar(state);
    draw_project_pipeline(state);
    if (begin_card("##files", "Changed files", {icons::LC_FILE})) {
        files_body(state);
        end_card();
    }
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
                if (!edits_unsaved(state)) run();
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
    if (!state.ui.force_delete_reason.empty()) {
        ImGui::OpenPopup("Delete a record that is in use");
        ImGui::SetNextWindowSize({520.0F * ui_scale(), 0}, ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Delete a record that is in use", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(500.0F * ui_scale());
            ImGui::TextUnformatted(state.ui.force_delete_reason.c_str());
            ImGui::PopTextWrapPos();
            auto rows = collect_references(state, state.selection);
            std::erase_if(rows, [](const ReferenceRow& row) { return row.group != "Script uses"; });
            if (!rows.empty()) {
                ImGui::Spacing();
                dim_text("Deleting it leaves these references dangling:");
                for (std::size_t i = 0; i < rows.size() && i < 12; ++i)
                    ImGui::BulletText("%s  %s", rows[i].label.c_str(), rows[i].detail.c_str());
                if (rows.size() > 12) dim_text("... and %zu more", rows.size() - 12);
            }
            ImGui::Spacing();
            if (danger_button("Delete anyway")) {
                state.ui.force_delete_reason.clear();
                ImGui::CloseCurrentPopup();
                state.commands.run("mission.delete_force");
            }
            ImGui::SameLine();
            if (secondary_button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                state.ui.force_delete_reason.clear();
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
