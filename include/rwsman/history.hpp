#pragma once

#include "rwsman/selection.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace rwsman {

// Plain-data snapshot of the viewport camera, so history can restore it without
// depending on the renderer.
struct CameraSnapshot {
    float yaw{}, pitch{}, distance{}, orthographic_scale{}, pan_x{}, pan_y{};
    float navigation[3]{};
    float center[3]{};
    float radius{};
    int projection{};
    bool valid{};
    [[nodiscard]] friend bool operator==(const CameraSnapshot&, const CameraSnapshot&) = default;
};

struct HistoryEntry {
    int workspace{};
    SelectionRef selection;
    CameraSnapshot camera;
};

// Bounded back/forward stack. Pushing an entry that has the same workspace and
// selection as the current one only refreshes its camera; pushing anything else
// discards the forward entries.
class NavigationHistory {
public:
    explicit NavigationHistory(std::size_t capacity = 64) : capacity_(capacity ? capacity : 1) {}

    void push(HistoryEntry entry);
    // Updates the camera stored for the current entry without creating a new one.
    void update_current_camera(const CameraSnapshot& camera);
    [[nodiscard]] const HistoryEntry* current() const noexcept;
    [[nodiscard]] bool can_back() const noexcept { return position_ > 0; }
    [[nodiscard]] bool can_forward() const noexcept { return position_ + 1 < entries_.size(); }
    // Returns the entry that becomes current, or null when there is none.
    const HistoryEntry* back() noexcept;
    const HistoryEntry* forward() noexcept;
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    std::vector<HistoryEntry> entries_;
    std::size_t position_{};
    std::size_t capacity_;
};

} // namespace rwsman
