#include "rwsman/history.hpp"

#include <cstddef>
#include <utility>

namespace rwsman {

const char* selection_kind_name(const SelectionRef::Kind kind) noexcept {
    switch (kind) {
    case SelectionRef::Kind::none:
        return "none";
    case SelectionRef::Kind::chunk:
        return "chunk";
    case SelectionRef::Kind::scene_instance:
        return "scene instance";
    case SelectionRef::Kind::mission_entry:
        return "mission entry";
    case SelectionRef::Kind::program_script:
        return "script";
    case SelectionRef::Kind::program_instruction:
        return "instruction";
    case SelectionRef::Kind::database_record:
        return "database record";
    case SelectionRef::Kind::resource_path:
        return "resource";
    }
    return "none";
}

void NavigationHistory::push(HistoryEntry entry) {
    if (!entries_.empty()) {
        auto& current = entries_[position_];
        if (current.workspace == entry.workspace && current.selection == entry.selection) {
            if (entry.camera.valid) current.camera = entry.camera;
            return;
        }
        entries_.resize(position_ + 1);
    }
    entries_.push_back(std::move(entry));
    if (entries_.size() > capacity_)
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - capacity_));
    position_ = entries_.size() - 1;
}

void NavigationHistory::update_current_camera(const CameraSnapshot& camera) {
    if (!entries_.empty()) entries_[position_].camera = camera;
}

const HistoryEntry* NavigationHistory::current() const noexcept {
    return entries_.empty() ? nullptr : &entries_[position_];
}

const HistoryEntry* NavigationHistory::back() noexcept {
    if (!can_back()) return nullptr;
    return &entries_[--position_];
}

const HistoryEntry* NavigationHistory::forward() noexcept {
    if (!can_forward()) return nullptr;
    return &entries_[++position_];
}

void NavigationHistory::clear() noexcept {
    entries_.clear();
    position_ = 0;
}

} // namespace rwsman
