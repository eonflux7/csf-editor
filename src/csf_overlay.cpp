#include "csf/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace csf {

std::optional<OverlayPick> pick_overlay(const std::span<const ScreenOverlayPrimitive> primitives,
                                        const float mouse_x, const float mouse_y,
                                        const float threshold_pixels) noexcept {
    if (!std::isfinite(mouse_x) || !std::isfinite(mouse_y) || !std::isfinite(threshold_pixels) ||
        threshold_pixels < 0) return std::nullopt;
    float best_squared = threshold_pixels * threshold_pixels;
    std::uint8_t best_priority = std::numeric_limits<std::uint8_t>::max();
    std::optional<std::uint32_t> best;
    for (const auto& primitive : primitives) {
        if (!primitive.visible || primitive.clipped) continue;
        float closest_x = primitive.x1, closest_y = primitive.y1;
        if (primitive.kind == OverlayPrimitiveKind::segment) {
            const float vx = primitive.x2 - primitive.x1, vy = primitive.y2 - primitive.y1;
            const float length_squared = vx * vx + vy * vy;
            const float t = length_squared > 0 ? std::clamp(
                ((mouse_x - primitive.x1) * vx + (mouse_y - primitive.y1) * vy) / length_squared,
                0.0F, 1.0F) : 0.0F;
            closest_x += t * vx; closest_y += t * vy;
        }
        const float dx = closest_x - mouse_x, dy = closest_y - mouse_y;
        const float distance_squared = dx * dx + dy * dy;
        if (distance_squared < best_squared ||
            (distance_squared == best_squared && primitive.priority < best_priority)) {
            best_squared = distance_squared;
            best_priority = primitive.priority;
            best = primitive.source_entry;
        }
    }
    if (!best) return std::nullopt;
    return OverlayPick{*best, std::sqrt(best_squared)};
}

} // namespace csf
