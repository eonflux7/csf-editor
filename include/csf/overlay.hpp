#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace csf {

enum class OverlayPrimitiveKind { point, segment };

struct ScreenOverlayPrimitive {
    std::uint32_t source_entry{};
    OverlayPrimitiveKind kind{OverlayPrimitiveKind::point};
    float x1{}, y1{}, x2{}, y2{};
    std::uint8_t priority{};
    bool visible{true};
    bool clipped{};
};

struct OverlayPick {
    std::uint32_t source_entry{};
    float distance_pixels{};
};

// Inputs are already projected into framebuffer coordinates. The threshold is
// therefore stable under scene zoom and perspective/orthographic changes.
[[nodiscard]] std::optional<OverlayPick>
pick_overlay(std::span<const ScreenOverlayPrimitive> primitives, float mouse_x, float mouse_y,
             float threshold_pixels = 10.0F) noexcept;

} // namespace csf
