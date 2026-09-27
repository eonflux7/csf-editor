#pragma once

#include "app_state.hpp"
#include "rwsman/animation_slots.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Pickers for the things a mission author chooses by name rather than by ID:
// classes, animations, weapons, objectives and game texts. Every authoring
// card uses these instead of number fields (docs/plans/editor-ux-redesign.md,
// Phase 7).
namespace rwsman::ui {

// The filter text of the open filtered combo with `id`.
char* combo_filter(ImGuiID id, std::size_t& size);
[[nodiscard]] bool combo_filter_matches(const std::string& label, const char* filter);

// A combo with a filter box. `items` are (value, label); returns the pick.
template <class T>
std::optional<T> filtered_combo(const char* id, const std::string& preview,
                                const std::vector<std::pair<T, std::string>>& items) {
    std::optional<T> picked;
    if (!ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        name_last_item(id);  // for UI scripts
        return picked;
    }
    std::size_t size{};
    char* filter = combo_filter(ImGui::GetID("filter"), size);
    if (ImGui::IsWindowAppearing()) {
        filter[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", "filter", filter, size);
    std::size_t shown = 0;
    for (const auto& [value, label] : items) {
        if (!combo_filter_matches(label, filter)) continue;
        if (++shown > 400) {
            dim_text("Refine the filter to see more.");
            break;
        }
        if (ImGui::Selectable(label.c_str(), label == preview)) picked = value;
    }
    ImGui::EndCombo();
    return picked;
}

// Classes of this mission's Objetos.bdd: "36  Infanteria Oficial Pistola".
[[nodiscard]] std::vector<std::pair<std::int32_t, std::string>> class_items(const AppState& state);
[[nodiscard]] std::string class_label(const AppState& state, std::optional<std::int32_t> id);
// Classes that are soldiers or other characters (the ones a guard can be).
[[nodiscard]] std::vector<std::pair<std::int32_t, std::string>> character_class_items(const AppState& state);
// One entry per distinct model, naming the first class that has it.
[[nodiscard]] std::vector<std::pair<std::int32_t, std::string>> model_items(const AppState& state);
[[nodiscard]] std::string model_label(const AppState& state, std::optional<std::int32_t> class_id);

// Anims.bdd: "LUGERidle  1113".
[[nodiscard]] std::vector<std::pair<std::int32_t, std::string>> animation_items(const AppState& state);
[[nodiscard]] std::string animation_label(const AppState& state, std::int32_t id);
// An animation combo: looping clips first when `loops_first` (idle loops),
// each marked "loops". With a soldier's class, clips made for another weapon
// stance (a rifle's SF* for an MP40 soldier) are left out, and the tooltip
// says how many. Returns the pick.
std::optional<std::int32_t> animation_combo(AppState& state, const char* id, std::int32_t current, bool loops_first,
                                            std::optional<std::int32_t> soldier_class = std::nullopt);
// How a class holds its weapon (its first Armas.bdd weapon), unknown without one.
[[nodiscard]] WeaponStance class_stance(const AppState& state, std::optional<std::int32_t> class_id);
// A small play button that previews `animation` on actor `actor_id` in the
// viewport, or stops the preview when it is playing.
void animation_preview_button(AppState& state, const char* id, std::int32_t actor_id, std::int32_t animation);

// Armas.bdd weapons: "Luger  16".
[[nodiscard]] std::vector<std::pair<std::int32_t, std::string>> weapon_items(const AppState& state);
[[nodiscard]] std::string weapon_label(const AppState& state, std::int32_t id);

// The mission's objectives by number: "1  Reach the farm".
[[nodiscard]] std::vector<std::pair<std::int32_t, std::string>> objective_items(AppState& state);

// Actors of the mission the player controls ("9  Actor_9 (Espia)").
[[nodiscard]] std::vector<std::pair<std::int32_t, std::string>> player_items(AppState& state);
// The mission's enemy soldiers (not animals), by actor ID.
[[nodiscard]] std::vector<std::int32_t> enemy_actors(AppState& state);

// A mission event picked from the ones the mission's scripts raise
// (SEND_EVENT), or typed as a new name; returns the pick. `none` names a
// first entry that returns "" (the default, such as "the mission starts").
std::optional<std::string> event_combo(AppState& state, const char* id, const std::string& current,
                                       const char* none = nullptr);

// The words of a game text: the project's string for `id`, or the ID itself
// (a shipped text such as g014) without a project.
[[nodiscard]] std::string game_text(const AppState& state, const std::string& id);
// A field showing a game text. With an authoring project the user types the
// words: an existing project string is edited in place (a project edit), and
// words that are not one of its strings yet get a new ID, which is returned so
// the caller stores it. Without a project it edits the FLI ID and returns it.
std::optional<std::string> game_text_field(AppState& state, const char* id, const std::string& text_id,
                                           const char* hint = nullptr);

} // namespace rwsman::ui
