#include "ui/pickers.hpp"

#include "animation_preview.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "rwsman/entity_kind.hpp"
#include "ui/icons.hpp"

#include "csf/animation_catalog.hpp"
#include "csf/mission_flow.hpp"
#include "csf/mission_recipes.hpp"
#include "csf/object_database.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <unordered_map>

namespace rwsman::ui {
namespace {

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// The Spanish names of the weapons and tools a kit gives, in English.
std::string weapon_english(const std::string& name) {
    static const std::map<std::string, std::string> names{
        {"cuchillo lanzable", "throwing knife"}, {"cuerda piano", "garrotte wire"}, {"prismaticos", "binoculars"},
        {"disfraz", "disguise"},                 {"moneda", "coin"},                {"desarmado", "unarmed"},
        {"desarmado advance", "unarmed"},        {"pulso", "punch"},                {"sdk mg", "vehicle machine gun"},
        {"luger_npc", "Luger (soldiers)"},       {"cuchillo", "knife"},             {"jeringa", "syringe"},
        {"granada", "grenade"},                  {"botiquin", "first aid kit"},     {"tabaco", "cigarettes"}};
    const auto found = names.find(lower(name));
    return found == names.end() ? std::string{} : found->second;
}

} // namespace

char* combo_filter(const ImGuiID id, std::size_t& size) {
    static std::unordered_map<ImGuiID, std::array<char, 128>> filters;
    auto& buffer = filters[id];
    size = buffer.size();
    return buffer.data();
}

bool combo_filter_matches(const std::string& label, const char* filter) {
    return !filter || !*filter || lower(label).find(lower(filter)) != std::string::npos;
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

std::vector<std::pair<std::int32_t, std::string>> character_class_items(const AppState& state) {
    std::vector<std::pair<std::int32_t, std::string>> items;
    if (!state.mission.objects) return items;
    for (const auto& definition : state.mission.objects->definitions()) {
        if (!definition.class_id) continue;
        const auto kind = classify_actor(class_facts(state.mission.objects.get(), definition.class_id), false);
        if (kind != EntityKind::enemy && kind != EntityKind::animal) continue;
        items.emplace_back(*definition.class_id, definition.name.value_or("(unnamed)") + "  " +
                                                     std::to_string(*definition.class_id));
    }
    std::ranges::sort(items, {}, &std::pair<std::int32_t, std::string>::second);
    return items;
}

std::string class_label(const AppState& state, const std::optional<std::int32_t> id) {
    if (!id) return "(none)";
    if (state.mission.objects)
        if (const auto found = state.mission.objects->find_class(*id); found.size() == 1)
            return std::to_string(*id) + "  " + found.front()->name.value_or("(unnamed)");
    return std::to_string(*id) + "  (not in Objetos.bdd)";
}

namespace {

// The body model of a class (the character model for player classes).
std::optional<std::string> class_model(const csf::ObjectDefinition& definition) {
    for (const auto& reference : definition.references)
        if (reference.kind == csf::ObjectReference::Kind::visual_model) return reference.path;
    return std::nullopt;
}

} // namespace

std::vector<std::pair<std::int32_t, std::string>> model_items(const AppState& state) {
    std::map<std::string, std::pair<std::int32_t, std::string>> models;
    if (!state.mission.objects) return {};
    for (const auto& definition : state.mission.objects->definitions()) {
        const auto model = class_model(definition);
        if (!definition.class_id || !model) continue;
        models.try_emplace(lower(*model), *definition.class_id,
                           *model + "  (" + std::to_string(*definition.class_id) + " " + definition.name.value_or("") +
                               ")");
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
        if (record.id) items.emplace_back(*record.id, record.logical_name + "  " + std::to_string(*record.id));
    return items;
}

std::string animation_label(const AppState& state, const std::int32_t id) {
    if (state.mission.animations)
        if (const auto* record = state.mission.animations->find_id(id))
            return record->logical_name + "  " + std::to_string(id);
    return std::to_string(id) + "  (not in Anims.bdd)";
}

WeaponStance class_stance(const AppState& state, const std::optional<std::int32_t> class_id) {
    if (!class_id || !state.mission.objects || !state.mission.weapons) return WeaponStance::unknown;
    const auto found = state.mission.objects->find_class(*class_id);
    if (found.size() != 1) return WeaponStance::unknown;
    for (const auto weapon : found.front()->weapon_ids)
        if (const auto* definition = state.mission.weapons->find_id(weapon); definition && definition->name)
            if (const auto stance = weapon_stance(*definition->name); stance != WeaponStance::unknown) return stance;
    return WeaponStance::unknown;
}

std::optional<std::int32_t> animation_combo(AppState& state, const char* id, const std::int32_t current,
                                            const bool loops_first, const std::optional<std::int32_t> soldier_class) {
    std::vector<std::pair<std::int32_t, std::string>> items;
    const auto stance = class_stance(state, soldier_class);
    std::size_t hidden = 0;
    if (state.mission.animations) {
        const auto& records = state.mission.animations->records();
        for (const int pass : {0, 1})
            for (const auto& record : records) {
                if (!record.id) continue;
                const bool loops = record.loop.value_or(false);
                if (loops_first && (pass == 0) != loops) continue;
                if (!loops_first && pass == 1) continue;
                if (!animation_fits(stance, record.logical_name) && *record.id != current) {
                    ++hidden;
                    continue;
                }
                items.emplace_back(*record.id, record.logical_name + "  " + std::to_string(*record.id) +
                                                   (loops ? "  \xC2\xB7 loops" : ""));
            }
    }
    const auto preview = current ? animation_label(state, current) : std::string("Pick an animation...");
    const auto picked = filtered_combo(id, preview, items);
    if (hidden > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("This soldier carries a %s: %zu clips made for other weapons are left out.",
                          weapon_stance_name(stance), hidden);
    return picked;
}

void animation_preview_button(AppState& state, const char* id, const std::int32_t actor_id, const std::int32_t animation) {
    const auto entry = mission_record_entry(*state.mission.scene, {MissionRecordKey::Kind::actor, actor_id, 0});
    const auto* record = state.mission.animations ? state.mission.animations->find_id(animation) : nullptr;
    const bool playing = entry && state.mission.animated_actor == entry && state.mission.animation_playing && record &&
                         state.mission.active_animation == record->logical_name;
    ImGui::BeginDisabled(!entry || !record);
    if (icon_button(id, playing ? icons::LC_SQUARE : icons::LC_PLAY,
                    playing ? "Stop the preview" : "Preview it on the actor in the viewport (the game's AI does not run)",
                    playing)) {
        if (playing) {
            stop_actor_animation(state);
        } else if (const auto error = preview_actor_animation(state, *entry, *record)) {
            state.notify(LogLevel::warn, "Cannot preview " + record->logical_name + ": " + *error);
        }
    }
    ImGui::EndDisabled();
}

std::vector<std::pair<std::int32_t, std::string>> weapon_items(const AppState& state) {
    std::vector<std::pair<std::int32_t, std::string>> items;
    if (!state.mission.weapons) return items;
    for (const auto& definition : state.mission.weapons->definitions())
        if (definition.id) items.emplace_back(*definition.id, weapon_label(state, *definition.id));
    return items;
}

std::string weapon_label(const AppState& state, const std::int32_t id) {
    if (state.mission.weapons)
        if (const auto* definition = state.mission.weapons->find_id(id)) {
            const auto name = definition->name.value_or("weapon");
            const auto english = weapon_english(name);
            return name + (english.empty() ? "" : " (" + english + ")") + "  " + std::to_string(id);
        }
    return std::to_string(id) + "  (not in Armas.bdd)";
}

std::vector<std::pair<std::int32_t, std::string>> objective_items(AppState& state) {
    std::vector<std::pair<std::int32_t, std::string>> items;
    if (const auto* flow = state.mission.scene ? mission_flow(state) : nullptr)
        for (const auto& objective : flow->objectives())
            items.emplace_back(objective.number,
                               std::to_string(objective.number) + "  " + game_text(state, objective.label));
    return items;
}

std::vector<std::pair<std::int32_t, std::string>> player_items(AppState& state) {
    std::vector<std::pair<std::int32_t, std::string>> items;
    if (!state.mission.scene) return items;
    const auto& scene = *state.mission.scene;
    for (const auto& actor : scene.actors())
        if (actor.id && classify_mission_actor(scene, state.mission.objects.get(), actor) == EntityKind::player) {
            const auto facts = class_facts(state.mission.objects.get(), actor.class_id);
            items.emplace_back(*actor.id, actor.name.value_or("actor " + std::to_string(*actor.id)) +
                                              (facts.name.empty() ? "" : "  (" + facts.name + ")"));
        }
    return items;
}

std::vector<std::int32_t> enemy_actors(AppState& state) {
    std::vector<std::int32_t> actors;
    if (!state.mission.scene) return actors;
    const auto& scene = *state.mission.scene;
    for (const auto& actor : scene.actors())
        if (actor.id && classify_mission_actor(scene, state.mission.objects.get(), actor) == EntityKind::enemy)
            actors.push_back(*actor.id);
    return actors;
}

std::optional<std::string> event_combo(AppState& state, const char* id, const std::string& current, const char* none) {
    std::optional<std::string> picked;
    const std::string preview = current.empty() && none ? none : current.empty() ? "(pick an event)" : current;
    if (!ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        name_last_item(id);
        return picked;
    }
    if (none && ImGui::Selectable(none, current.empty())) picked = std::string();
    // Custom events: the ones some script raises, and the ones some script waits for.
    std::vector<const csf::FlowEvent*> events;
    if (const auto* flow = state.mission.scene ? mission_flow(state) : nullptr)
        for (const auto& event : flow->events())
            if (!event.builtin) events.push_back(&event);
    for (const auto* event : events) {
        const auto label = event->name + "  (raised by " + std::to_string(event->senders.size()) + " script" +
                           (event->senders.size() == 1 ? "" : "s") + ")";
        if (ImGui::Selectable(label.c_str(), event->name == current)) picked = event->name;
    }
    if (events.empty()) dim_text("No script raises an event yet.");
    ImGui::Separator();
    static std::array<char, 64> typed{};
    if (ImGui::IsWindowAppearing()) typed.fill('\0');
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##new_event", "a new event name, then Enter", typed.data(), typed.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue) &&
        typed[0]) {
        auto name = csf::script_name_token(typed.data());
        std::ranges::transform(name, name.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
        picked = name;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndCombo();
    return picked;
}

std::string game_text(const AppState& state, const std::string& id) {
    if (const auto* project = state.authoring.project.get())
        for (const auto& string : project->strings)
            if (string.id == id) return string.text;
    return id;
}

std::optional<std::string> game_text_field(AppState& state, const char* id, const std::string& text_id,
                                           const char* hint) {
    const auto* project = state.authoring.project.get();
    std::string edited;
    if (!project) {
        if (edit_text_value(id, text_id, edited, hint ? hint : "FLI string ID") && !edited.empty() && edited != text_id)
            return edited;
        return std::nullopt;
    }
    const auto found = std::ranges::find(project->strings, text_id, &csf::ProjectText::id);
    const bool own = found != project->strings.end();
    const std::string shown = own ? found->text : std::string();
    const std::string placeholder = !own && !text_id.empty() ? "game text " + text_id : (hint ? hint : "the words");
    if (!edit_text_value(id, shown, edited, placeholder.c_str()) || edited.empty() || edited == shown) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && !text_id.empty())
            ImGui::SetTooltip(own ? "Text %s of the project" : "Text %s of the game (type to replace it with your own)",
                              text_id.c_str());
        return std::nullopt;
    }
    if (own) {
        set_project_text(state, text_id, edited);
        return std::nullopt;
    }
    if (auto new_id = game_text_id(state, edited)) return new_id;
    state.warn("The project's text ID range is full");
    return std::nullopt;
}

} // namespace rwsman::ui
