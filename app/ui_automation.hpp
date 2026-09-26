#pragma once

#include "rwsman/commands.hpp"
#include "rwsman/ui_script.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace rwsman {

// One widget ImGui registered in a frame (through the test-engine hooks).
struct UiItem {
    ImGuiID id{};
    std::string window;  // the window it was submitted in, as ImGui names it
    std::string label;   // as submitted, "Text##id" or "##id"
    ImRect rect, clip;
    ImGuiItemStatusFlags status{};
    ImGuiItemFlags flags{};

    [[nodiscard]] ImVec2 center() const { return rect.GetCenter(); }
    // Whether the item's centre is inside its window's clip rectangle.
    [[nodiscard]] bool visible() const { return clip.Contains(center()) && rect.GetWidth() > 0 && rect.GetHeight() > 0; }
};

// The UI test harness's view of ImGui (docs/plans/editor-ux-redesign.md, T3,
// T4, T7): records every widget each frame so scripts can address them by
// label, injects mouse, keyboard and text input as the platform backend
// would, and collects ImGui assertion failures and ID conflicts.
class UiAutomation {
public:
    // Installs the hooks on the current ImGui context.
    void enable();
    // Called before ImGui::NewFrame: the items of the frame that just ended
    // become the ones scripts search, and the next queued input event is
    // sent to ImGuiIO (after the backend's own events, so it wins). Returns
    // whether an input event was sent.
    bool before_new_frame();

    [[nodiscard]] const std::vector<UiItem>& items() const noexcept { return previous_; }
    // Visible items matching the target, the innermost (last submitted) first.
    [[nodiscard]] std::vector<const UiItem*> find(const UiTarget& target) const;

    // ImGui reports labels only for widgets inside the visible part of a
    // window. Scrolls every scrollable window the target's qualifier names by
    // most of a page (back to the top after the end). Returns false when no
    // window can scroll.
    bool scroll_towards(const UiTarget& target);

    void queue_hover(ImVec2 position);
    // `modifiers` (ImGuiMod_Shift, ...) are held from before the press to after the release.
    void queue_click(ImVec2 position, int clicks = 1, ImGuiKeyChord modifiers = 0);
    void queue_drag(ImVec2 from, ImVec2 to, int steps = 8, ImGuiKeyChord modifiers = 0);
    // Returns false when the shortcut does not parse or names an unknown key.
    bool queue_key(std::string_view shortcut);
    void queue_text(std::string text);
    [[nodiscard]] bool input_pending() const noexcept { return !events_.empty(); }

    // Assertion failures and duplicate widget IDs since the last call.
    [[nodiscard]] std::vector<std::string> take_failures();

private:
    struct Event {
        enum class Kind : std::uint8_t { move, button, key, text, modifiers };
        Kind kind{};
        ImVec2 position{};
        bool down{};
        ImGuiKey key{};
        ImGuiKeyChord modifiers{};
        std::string text;
        bool end_of_frame{};  // later events wait for the next frame
    };
    void hold_modifiers(ImGuiKeyChord modifiers, bool down);
    void push(Event event, bool end_of_frame) {
        event.end_of_frame = end_of_frame;
        events_.push_back(std::move(event));
    }
    static void on_item_add(ImGuiContext* context, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* data);
    static void on_item_info(ImGuiContext* context, ImGuiID id, const char* label, ImGuiItemStatusFlags flags);
    static bool on_assert(const char* expression, const char* file, int line);

    std::vector<UiItem> current_, previous_;
    std::deque<Event> events_;
    ImVec2 mouse_{-1.0F, -1.0F};
    std::vector<std::string> failures_;
};

} // namespace rwsman
