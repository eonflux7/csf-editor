#include "ui_automation.hpp"

#include "imgui_hooks.hpp"
#include "ui/shortcuts.hpp"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace rwsman {
namespace {

UiAutomation* active = nullptr;

} // namespace

void UiAutomation::enable() {
    active = this;
    auto& handlers = imgui_hooks::handlers();
    handlers.item_add = &UiAutomation::on_item_add;
    handlers.item_info = &UiAutomation::on_item_info;
    handlers.assert_failed = &UiAutomation::on_assert;
    ImGui::GetCurrentContext()->TestEngineHookItems = true;
}

void UiAutomation::on_item_add(ImGuiContext* context, const ImGuiID id, const ImRect& bb,
                               const ImGuiLastItemData* data) {
    if (!active || id == 0 || !context->CurrentWindow) return;
    UiItem item;
    item.id = id;
    item.window = context->CurrentWindow->Name;
    item.rect = bb;
    item.clip = context->CurrentWindow->ClipRect;
    if (data) {
        item.flags = data->ItemFlags;
        item.status = data->StatusFlags;
        if (data->StatusFlags & ImGuiItemStatusFlags_HasClipRect) item.clip = data->ClipRect;
    }
    active->current_.push_back(std::move(item));
}

void UiAutomation::on_item_info(ImGuiContext* context, const ImGuiID id, const char* label,
                                const ImGuiItemStatusFlags flags) {
    if (!active || id == 0 || !label) return;
    auto& items = active->current_;
    // The item was added just before (ItemAdd, then ItemInfo); search back a little.
    for (std::size_t i = items.size(), steps = 0; i-- > 0 && steps < 16; ++steps)
        if (items[i].id == id) {
            if (items[i].label.empty()) {
                items[i].label = label;
                items[i].status |= flags;
                return;
            }
            break;
        }
    // Items registered without a preceding ItemAdd (a window's own entry).
    UiItem item;
    item.id = id;
    item.window = context->CurrentWindow ? context->CurrentWindow->Name : "";
    item.label = label;
    item.rect = context->LastItemData.ID == id ? context->LastItemData.Rect : ImRect{};
    item.clip = context->CurrentWindow ? context->CurrentWindow->ClipRect : ImRect{};
    item.status = flags;
    items.push_back(std::move(item));
}

bool UiAutomation::on_assert(const char* expression, const char* file, const int line) {
    if (!active) return false;
    char text[1024];
    std::snprintf(text, sizeof(text), "ImGui assertion failed: %s (%s:%d)", expression, file, line);
    active->failures_.emplace_back(text);
    return true;
}

bool UiAutomation::before_new_frame() {
    // Two labelled widgets with one ID in a frame share input and state: ImGui
    // reports it only while hovering one of them, the harness in every frame.
    std::unordered_map<ImGuiID, std::size_t> seen;
    for (std::size_t i = 0; i < current_.size(); ++i) {
        const auto& item = current_[i];
        if (item.label.empty() || (item.flags & ImGuiItemFlags_AllowDuplicateId) != 0) continue;
        const auto [found, inserted] = seen.emplace(item.id, i);
        if (inserted) continue;
        const auto& first = current_[found->second];
        if (first.rect.Min.x == item.rect.Min.x && first.rect.Min.y == item.rect.Min.y) continue;  // re-registered
        const auto message = "ID conflict in " + item.window + ": '" + first.label + "' and '" + item.label + "'";
        if (std::ranges::find(failures_, message) == failures_.end()) failures_.push_back(message);
    }
    previous_ = std::move(current_);
    current_.clear();

    auto& io = ImGui::GetIO();
    const bool sent = !events_.empty();
    while (!events_.empty()) {
        auto event = std::move(events_.front());
        events_.pop_front();
        switch (event.kind) {
        case Event::Kind::move:
            mouse_ = event.position;
            io.AddMousePosEvent(event.position.x, event.position.y);
            break;
        case Event::Kind::button:
            io.AddMousePosEvent(mouse_.x, mouse_.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, event.down);
            break;
        case Event::Kind::modifiers:
        case Event::Kind::key:
            for (const auto modifier : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt})
                if (event.modifiers & modifier) io.AddKeyEvent(static_cast<ImGuiKey>(modifier), event.down);
            for (const auto key : {ImGuiKey_LeftCtrl, ImGuiKey_LeftShift, ImGuiKey_LeftAlt}) {
                const auto modifier = key == ImGuiKey_LeftCtrl    ? ImGuiMod_Ctrl
                                      : key == ImGuiKey_LeftShift ? ImGuiMod_Shift
                                                                  : ImGuiMod_Alt;
                if (event.modifiers & modifier) io.AddKeyEvent(key, event.down);
            }
            if (event.kind == Event::Kind::key) io.AddKeyEvent(event.key, event.down);
            break;
        case Event::Kind::text:
            io.AddInputCharactersUTF8(event.text.c_str());
            break;
        }
        if (event.end_of_frame) break;
    }
    // Keep the pointer where the script left it; the platform backend reports
    // the real cursor (outside the hidden window) otherwise.
    if (mouse_.x >= 0.0F) io.AddMousePosEvent(mouse_.x, mouse_.y);
    return sent;
}

std::vector<const UiItem*> UiAutomation::find(const UiTarget& target) const {
    std::vector<const UiItem*> found;
    for (auto it = previous_.rbegin(); it != previous_.rend(); ++it)
        if (it->visible() && ui_target_matches(target, it->window, it->label)) found.push_back(&*it);
    return found;
}

bool UiAutomation::scroll_towards(const UiTarget& target) {
    if (target.window.empty()) return false;
    bool scrolled = false;
    for (auto* window : ImGui::GetCurrentContext()->Windows) {
        if (!window->WasActive || window->ScrollMax.y <= 0.0F || !ui_window_matches(target.window, window->Name))
            continue;
        const float page = std::max(window->InnerRect.GetHeight() * 0.8F, 20.0F);
        const float next = window->Scroll.y >= window->ScrollMax.y - 1.0F ? 0.0F : window->Scroll.y + page;
        ImGui::SetScrollY(window, std::min(next, window->ScrollMax.y));
        scrolled = true;
    }
    return scrolled;
}

void UiAutomation::queue_hover(const ImVec2 position) { push({Event::Kind::move, position}, true); }

void UiAutomation::hold_modifiers(const ImGuiKeyChord modifiers, const bool down) {
    if (!modifiers) return;
    Event event{Event::Kind::modifiers};
    event.modifiers = modifiers;
    event.down = down;
    push(event, true);
}

void UiAutomation::queue_click(const ImVec2 position, const int clicks, const ImGuiKeyChord modifiers) {
    hold_modifiers(modifiers, true);
    push({Event::Kind::move, position}, true);
    for (int i = 0; i < clicks; ++i) {
        Event down{Event::Kind::button, position};
        down.down = true;
        push(down, true);
        push({Event::Kind::button, position}, true);
    }
    hold_modifiers(modifiers, false);
}

void UiAutomation::queue_drag(const ImVec2 from, const ImVec2 to, const int steps, const ImGuiKeyChord modifiers) {
    hold_modifiers(modifiers, true);
    push({Event::Kind::move, from}, true);
    Event down{Event::Kind::button, from};
    down.down = true;
    push(down, true);
    for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        push({Event::Kind::move, {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t}}, true);
    }
    push({Event::Kind::button, to}, true);
    hold_modifiers(modifiers, false);
}

bool UiAutomation::queue_key(const std::string_view shortcut) {
    const auto parsed = parse_shortcut(shortcut);
    if (!parsed) return false;
    const auto key = ui::key_from_name(parsed->key);
    if (!key) return false;
    Event event{Event::Kind::key};
    event.key = *key;
    event.modifiers = (parsed->ctrl ? ImGuiMod_Ctrl : 0) | (parsed->shift ? ImGuiMod_Shift : 0) |
                      (parsed->alt ? ImGuiMod_Alt : 0);
    event.down = true;
    push(event, true);
    event.down = false;
    push(event, true);
    return true;
}

void UiAutomation::queue_text(std::string text) {
    Event event{Event::Kind::text};
    event.text = std::move(text);
    push(std::move(event), true);
}

std::vector<std::string> UiAutomation::take_failures() { return std::exchange(failures_, {}); }

} // namespace rwsman
