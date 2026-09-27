#pragma once

#include "app_state.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace csf {
struct AnimationRecord;
}

// Plays a catalog animation on a placed actor in the viewport: a preview of
// the clip on the actor's model, without the game's AI. The Inspect mode's
// Animation playback and the Mission mode's animation pickers share it.
namespace rwsman {

// Starts `animation` on the actor at scene entry `entry`, playing and
// looping. Returns why it cannot (no unique ANM file, a model the clip does
// not fit, ...).
[[nodiscard]] std::optional<std::string> preview_actor_animation(AppState& state, std::uint32_t entry,
                                                                 const csf::AnimationRecord& animation);
// The actor back in its rest pose.
void stop_actor_animation(AppState& state);
// Advances a playing preview; once a frame.
void tick_actor_animation(AppState& state);

} // namespace rwsman
