#include "app_state.hpp"
#include "ui/ui.hpp"

#include "animation_preview.hpp"
#include "app_util.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rwsman::ui {

void draw_actor_animation(AppState& state, const csf::ActorAssociation& association,
                          const std::uint32_t entry) {
    auto& mission = state.mission;
    auto& geometry_preview = state.preview;
    const auto actor_record = std::ranges::find_if(mission.scene->actors(), [&](const auto& value) {
        return value.source.entry_index == entry;
    });
    std::map<const csf::AnimationRecord*, std::vector<std::string>> traced;
    if (actor_record != mission.scene->actors().end()) {
        for (const auto* use :
             mission.script_animations.for_actor(actor_record->script_ids, actor_record->id))
            if (const auto* record = mission.animations->find_id(use->animation_id))
                traced[record].push_back("GSC script " + std::to_string(use->script_id) + " (" +
                                         use->script_name + ") / " + use->opcode + " at entry " +
                                         std::to_string(use->source.entry_index));
    }
    if (actor_record != mission.scene->actors().end())
        for (const auto& binding : actor_record->animations)
            if (binding.id)
                if (const auto* record = mission.animations->find_id(*binding.id))
                    traced[record].push_back("Scene .ANIMACIONES override, slot " +
                                             binding.type.value_or("(unnamed)"));
    const auto asset_key = [](const std::string& value) {
        auto key = std::filesystem::path(value).stem().string();
        std::ranges::transform(key, key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return key;
    };
    for (const auto& evidence : association.animations)
        for (const auto& record : mission.animations->records())
            for (const auto& variant : record.variants)
                if (asset_key(evidence.resolution.original_reference) ==
                    asset_key(variant.reference))
                    traced[&record].push_back("Objetos.bdd " + evidence.field + " at entry " +
                                              std::to_string(evidence.source.entry_index));
    if (actor_record != mission.scene->actors().end() && !actor_record->script_ids.empty()) {
        std::string ids;
        for (const auto id : actor_record->script_ids) {
            if (!ids.empty()) ids += ", ";
            ids += std::to_string(id);
        }
        ImGui::TextWrapped("Actor scripts: %s", ids.c_str());
    }
    const auto assign_animation = [&](const csf::AnimationRecord& animation) {
        if (const auto error = preview_actor_animation(state, entry, animation))
            return state.error("Animation unchanged: " + *error);
        mission.animation_playing = false;
        state.ok("Assigned " + animation.logical_name + " to selected actor (AI remains disabled)");
    };
    if (mission.active_clip && mission.animated_actor == entry) {
        ImGui::TextWrapped("Clip: %s", mission.active_animation.c_str());
        ImGui::Text("%.3f / %.3f s", mission.animation_time, mission.active_clip->duration);
        if (ImGui::Button(mission.animation_playing ? "Pause##actor_anim" : "Play##actor_anim"))
            mission.animation_playing = !mission.animation_playing;
        ImGui::SameLine();
        if (ImGui::Button("Reset##actor_anim")) {
            mission.animation_time = 0;
            mission.animation_playing = false;
            static_cast<void>(geometry_preview.set_mission_actor_animation(
                *mission.animated_actor, mission.active_clip, 0, mission.animation_loop));
        }
        ImGui::SameLine();
        if (ImGui::Button("Step -##actor_anim")) {
            mission.animation_time = std::max(
                0.0F, mission.animation_time - 1.0F / static_cast<float>(mission.animation_fps));
            mission.animation_playing = false;
            static_cast<void>(geometry_preview.set_mission_actor_animation(
                *mission.animated_actor, mission.active_clip, mission.animation_time,
                mission.animation_loop));
        }
        ImGui::SameLine();
        if (ImGui::Button("Step +##actor_anim")) {
            mission.animation_time =
                std::min(mission.active_clip->duration,
                         mission.animation_time + 1.0F / static_cast<float>(mission.animation_fps));
            mission.animation_playing = false;
            static_cast<void>(geometry_preview.set_mission_actor_animation(
                *mission.animated_actor, mission.active_clip, mission.animation_time,
                mission.animation_loop));
        }
        const auto seek_key = [&](const bool next) {
            std::optional<float> target;
            for (const auto& key : mission.active_clip->keyframes) {
                if (next && key.time > mission.animation_time + 1.0e-5F &&
                    (!target || key.time < *target))
                    target = key.time;
                if (!next && key.time < mission.animation_time - 1.0e-5F &&
                    (!target || key.time > *target))
                    target = key.time;
            }
            if (!target) target = next ? mission.active_clip->duration : 0;
            mission.animation_time = *target;
            mission.animation_playing = false;
            static_cast<void>(geometry_preview.set_mission_actor_animation(
                *mission.animated_actor, mission.active_clip, mission.animation_time,
                mission.animation_loop));
        };
        if (ImGui::Button("Previous key##actor_anim")) seek_key(false);
        ImGui::SameLine();
        if (ImGui::Button("Next key##actor_anim")) seek_key(true);
        if (ImGui::Checkbox("Loop##actor_anim", &mission.animation_loop))
            static_cast<void>(geometry_preview.set_mission_actor_animation(
                *mission.animated_actor, mission.active_clip, mission.animation_time,
                mission.animation_loop));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderFloat("Speed##actor_anim", &mission.animation_speed, -4, 4, "%.2fx");
        constexpr const char* preview_rates[] = {"15 FPS", "24 FPS", "25 FPS", "30 FPS", "60 FPS"};
        constexpr int preview_rate_values[] = {15, 24, 25, 30, 60};
        int preview_rate_index{};
        for (int i = 0; i < static_cast<int>(std::size(preview_rate_values)); ++i)
            if (preview_rate_values[i] == mission.animation_fps) preview_rate_index = i;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::Combo("##actor_anim_fps", &preview_rate_index, preview_rates,
                         static_cast<int>(std::size(preview_rates))))
            mission.animation_fps = preview_rate_values[preview_rate_index];
        if (ImGui::SliderFloat("Time##actor_anim", &mission.animation_time, 0,
                               std::max(.001F, mission.active_clip->duration), "%.3f s")) {
            mission.animation_playing = false;
            static_cast<void>(geometry_preview.set_mission_actor_animation(
                *mission.animated_actor, mission.active_clip, mission.animation_time,
                mission.animation_loop));
        }
    }
    ImGui::SeparatorText("Explicit mission animation references");
    if (traced.empty())
        ImGui::TextWrapped("None. This actor has no direct Objetos.bdd animation, "
                           "attached SCN script animation, or GSC action explicitly "
                           "targeting its ID. Normal AI locomotion/combat animation "
                           "selection is runtime behavior and is not an SCN "
                           "assignment.");
    for (const auto& [animation, evidence] : traced) {
        ImGui::PushID(static_cast<int>(animation->source.entry_index));
        const bool resolvable = std::ranges::any_of(animation->variants, [](const auto& v) {
            return v.resolution && v.resolution->candidate_indices.size() == 1 &&
                   v.resolution->status != csf::ResolutionStatus::ambiguous;
        });
        ImGui::BeginDisabled(!resolvable);
        if (ImGui::Selectable(animation->logical_name.c_str(),
                              mission.active_animation == animation->logical_name))
            assign_animation(*animation);
        ImGui::EndDisabled();
        for (const auto& line : evidence) {
            ImGui::Indent();
            ImGui::TextDisabled("%s", line.c_str());
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (traced.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);
    const auto playable_label =
        "Playable catalog clips (validated when selected) (" +
        std::to_string(mission.animations->records().size() - traced.size()) + ")";
    if (ImGui::TreeNode(playable_label.c_str())) {
        const auto compatible_animations = mission.animations->compatible(
            association.visual_models.size() == 1 && association.visual_models.front().resolved_path
                ? path_utf8(association.visual_models.front().resolved_path->filename())
                : std::string{});
        for (const auto* animation : compatible_animations) {
            if (traced.contains(animation)) continue;
            const bool resolvable = std::ranges::any_of(animation->variants, [](const auto& v) {
                return v.resolution && v.resolution->candidate_indices.size() == 1 &&
                       v.resolution->status != csf::ResolutionStatus::ambiguous;
            });
            ImGui::PushID(static_cast<int>(animation->source.entry_index));
            ImGui::BeginDisabled(!resolvable);
            if (ImGui::Selectable(animation->logical_name.c_str(),
                                  mission.active_animation == animation->logical_name))
                assign_animation(*animation);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    ImGui::TextDisabled("Evidence list: direct object metadata and PLAY_ANMBDD actions "
                        "in scripts attached to this actor.");
    ImGui::TextDisabled("Catalog clips are candidates, not proof of an "
                        "in-game assignment; exact HAnim/frame "
                        "compatibility is validated on selection.");
    ImGui::TextDisabled("Only this actor is evaluated. No AI or "
                        "runtime-state inference.");
}

} // namespace rwsman::ui
