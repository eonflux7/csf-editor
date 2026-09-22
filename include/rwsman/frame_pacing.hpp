#pragma once

#include <array>
#include <cstddef>

// GUI-free helpers behind the frame loop's pacing (when to redraw and how
// often), the viewport's frame statistics, and view-volume culling.
namespace rwsman {

struct FramePacingSettings {
    // Stop redrawing while nothing changes; input or an animation wakes the loop.
    bool idle_redraw{true};
    int fps_limit{};            // 0: the display refresh rate (vsync) only.
    int background_fps_limit{}; // Cap while the window is unfocused; 0 for none.
};

struct FramePacingInput {
    double now{};        // Seconds on a monotonic clock.
    double last_input{}; // When the latest input event arrived.
    // Something on screen changes by itself: a camera easing, a playing animation, a toast.
    bool animating{};
    // Work finishes off-screen (a mission load, an open file dialog) and must be polled.
    bool background_work{};
    bool text_input{}; // A focused text field: its caret blinks.
    bool focused{true};
    bool iconified{};
};

struct FramePacing {
    // Sleep until an input event arrives or `wait_timeout` passes, instead of
    // drawing the next frame straight away.
    bool wait_for_events{};
    double wait_timeout{};
    // Shortest time between frame starts; 0 leaves pacing to vsync.
    double min_frame_time{};
};

// Frames keep coming this long after the last input, so hover delays, tooltips,
// and ImGui's own settling play out before the loop goes idle.
inline constexpr double idle_linger_seconds = 0.6;

[[nodiscard]] FramePacing decide_frame_pacing(const FramePacingSettings& settings,
                                              const FramePacingInput& input);

// Average frame rate over the latest active frames. Idle waits are left out, so
// the rate reads as what the scene sustains while it changes.
class FrameRateCounter {
public:
    void add(double interval_seconds);
    void reset() noexcept { count_ = next_ = 0; }
    [[nodiscard]] double fps() const noexcept;

private:
    std::array<double, 32> intervals_{};
    std::size_t count_{}, next_{};
};

// The viewport's view volume in view space: the camera at the origin looking
// down -Z, as the scene shader builds it.
struct ViewVolume {
    bool orthographic{};
    float tan_half_fov{0.4663077F}; // tan(25 degrees).
    float aspect{1.0F};
    float near_plane{0.01F}, far_plane{1000.0F};
    float orthographic_scale{1.0F}; // Half the visible height.
};

// True when a sphere around the view-space `center` may be visible.
[[nodiscard]] bool sphere_in_view(const ViewVolume& volume, float x, float y, float z,
                                  float radius) noexcept;

} // namespace rwsman
