#include "animation_preview.hpp"

#include "csf/animation_catalog.hpp"
#include "rws/animation.hpp"
#include "rws/decoded.hpp"
#include "rws/document.hpp"
#include "rwsman/chunk_lookup.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace rwsman {

std::optional<std::string> preview_actor_animation(AppState& state, const std::uint32_t entry,
                                                   const csf::AnimationRecord& animation) {
    auto& mission = state.mission;
    try {
        const auto association = std::ranges::find_if(mission.actor_associations, [&](const auto& value) {
            return value.actor.entry_index == entry;
        });
        if (association == mission.actor_associations.end()) throw std::runtime_error("The actor has no model");
        const auto variant = std::ranges::find_if(animation.variants, [](const auto& value) {
            return value.resolution && value.resolution->candidate_indices.size() == 1 &&
                   value.resolution->status != csf::ResolutionStatus::ambiguous;
        });
        if (variant == animation.variants.end())
            throw std::runtime_error("The animation has no uniquely resolved ANM file");
        const auto& resource = mission.resources.resources()[variant->resolution->candidate_indices.front()];
        const auto animation_document = rws::Document::load(resource.path);
        if (animation_document.chunks().empty() || animation_document.chunks().front().type != 0x1B)
            throw std::runtime_error("The resolved file is not an animation");
        auto clip = std::make_shared<rws::AnimationClip>(
            rws::decode_animation(animation_document.chunks().front(), animation_document.bytes()));
        if (!clip->valid()) throw std::runtime_error("The clip is unsupported or failed validation");
        if (association->visual_models.size() != 1 || !association->visual_models.front().resolved_path)
            throw std::runtime_error("The actor has no uniquely resolved model");
        const auto model_document = rws::Document::load(*association->visual_models.front().resolved_path);
        const auto* frame_chunk = find_first_chunk(model_document.chunks(), 0x0E);
        if (!frame_chunk) throw std::runtime_error("The actor's model has no skeleton");
        const auto frames = rws::decode_frame_list(*frame_chunk, model_document.bytes());
        const auto binding = rws::decode_hanim_binding(*frame_chunk, model_document.bytes());
        if (!frames || !binding) throw std::runtime_error("The actor's skeleton could not be decoded");
        const auto compatibility = rws::map_animation_tracks(*clip, *binding.value, frames.value->frames.size());
        if (!compatibility.compatible)
            throw std::runtime_error("The clip does not fit this actor's skeleton" +
                                     (compatibility.diagnostics.empty() ? std::string{}
                                                                        : ": " + compatibility.diagnostics.front()));
        if (mission.animated_actor && *mission.animated_actor != entry) stop_actor_animation(state);
        if (!state.preview.set_mission_actor_animation(entry, clip, 0, mission.animation_loop))
            throw std::runtime_error("The actor's model is not loaded in the viewport");
        mission.active_clip = std::move(clip);
        mission.animated_actor = entry;
        mission.active_animation = animation.logical_name;
        mission.animation_time = 0;
        mission.animation_playing = true;
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
}

void stop_actor_animation(AppState& state) {
    auto& mission = state.mission;
    if (mission.animated_actor) (void)state.preview.set_mission_actor_animation(*mission.animated_actor, nullptr, 0, true);
    mission.active_clip.reset();
    mission.animated_actor.reset();
    mission.active_animation.clear();
    mission.animation_playing = false;
}

void tick_actor_animation(AppState& state) {
    auto& mission = state.mission;
    if (!mission.active_clip || !mission.animated_actor || !mission.animation_playing) return;
    state.ui.animating = true;
    const float delta = ImGui::GetIO().DeltaTime;
    mission.animation_accumulator += delta;
    mission.animation_time += delta * mission.animation_speed;
    const float duration = mission.active_clip->duration;
    if (mission.animation_loop && duration > 0) {
        mission.animation_time = std::fmod(mission.animation_time, duration);
        if (mission.animation_time < 0) mission.animation_time += duration;
    } else if (mission.animation_time >= duration) {
        mission.animation_time = duration;
        mission.animation_playing = false;
    } else if (mission.animation_time <= 0) {
        mission.animation_time = 0;
        mission.animation_playing = false;
    }
    if (mission.animation_accumulator >= 1.0F / static_cast<float>(mission.animation_fps)) {
        (void)state.preview.set_mission_actor_animation(*mission.animated_actor, mission.active_clip,
                                                        mission.animation_time, mission.animation_loop);
        mission.animation_accumulator = 0;
    }
}

} // namespace rwsman
