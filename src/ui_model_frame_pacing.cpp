#include "rwsman/frame_pacing.hpp"

#include <algorithm>
#include <cmath>

namespace rwsman {

FramePacing decide_frame_pacing(const FramePacingSettings& settings, const FramePacingInput& input) {
    FramePacing pacing;
    const auto cap = [&](const int fps) {
        if (fps > 0) pacing.min_frame_time = std::max(pacing.min_frame_time, 1.0 / fps);
    };
    cap(settings.fps_limit);
    if (!input.focused) cap(settings.background_fps_limit);
    if (input.iconified) {
        // Nothing is visible: keep polling for background work, slowly.
        pacing.wait_for_events = true;
        pacing.wait_timeout = 0.25;
        return pacing;
    }
    if (!settings.idle_redraw || input.animating ||
        input.now - input.last_input < idle_linger_seconds)
        return pacing;
    pacing.wait_for_events = true;
    pacing.wait_timeout = input.background_work ? 0.1 : (input.text_input ? 0.3 : 0.5);
    return pacing;
}

void FrameRateCounter::add(const double interval_seconds) {
    if (!(interval_seconds > 0.0) || !std::isfinite(interval_seconds)) return;
    intervals_[next_] = interval_seconds;
    next_ = (next_ + 1) % intervals_.size();
    count_ = std::min(count_ + 1, intervals_.size());
}

double FrameRateCounter::fps() const noexcept {
    double total = 0.0;
    for (std::size_t i = 0; i < count_; ++i) total += intervals_[i];
    return total > 0.0 ? static_cast<double>(count_) / total : 0.0;
}

bool sphere_in_view(const ViewVolume& volume, const float x, const float y, const float z,
                    const float radius) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(radius))
        return true; // Never hide what cannot be tested.
    const float depth = -z;
    if (depth + radius < volume.near_plane || depth - radius > volume.far_plane) return false;
    if (volume.orthographic) {
        const float half_height = volume.orthographic_scale;
        return std::abs(x) - half_height * volume.aspect <= radius &&
               std::abs(y) - half_height <= radius;
    }
    // Distance from the center to each side plane through the eye.
    const auto inside = [&](const float offset, const float slope) {
        return (std::abs(offset) - slope * depth) / std::sqrt(1.0F + slope * slope) <= radius;
    };
    return inside(x, volume.tan_half_fov * volume.aspect) && inside(y, volume.tan_half_fov);
}

} // namespace rwsman
