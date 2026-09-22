#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rwsman {

// A keyboard shortcut in a portable form. `key` is a display name such as "P",
// "F12", "Left", "1", "Space", "Home". The UI layer maps names to ImGui keys.
struct Shortcut {
    bool ctrl{}, shift{}, alt{};
    std::string key;

    [[nodiscard]] bool empty() const noexcept { return key.empty(); }
    [[nodiscard]] friend bool operator==(const Shortcut&, const Shortcut&) = default;
};

// Parses "Ctrl+Shift+P", "Alt+Left", "F12". Modifier names are case-insensitive;
// the key keeps its canonical case (single letters upper-case). Returns nullopt
// for an empty or malformed string.
[[nodiscard]] std::optional<Shortcut> parse_shortcut(std::string_view text);
[[nodiscard]] std::string format_shortcut(const Shortcut& shortcut);

enum class ShortcutScope {
    global,   // Active unless a text field has keyboard focus.
    viewport, // Active only while the 3D viewport is hovered.
};

struct Command {
    std::string id;       // Stable, dotted: "view.workspace.mission".
    std::string label;    // Menu and palette text.
    std::string category; // "File", "View", "Navigate", ...
    std::string shortcut; // "Ctrl+Shift+P", empty when unbound.
    ShortcutScope scope{ShortcutScope::global};
    std::function<bool()> enabled; // Null means always enabled.
    std::function<void()> run;
    // Extra palette keywords that are not part of the label.
    std::string keywords;
    // Shown in menus with a check mark when set.
    std::function<bool()> checked;
    // Commands that only exist for menus and Help (for example a bare-digit
    // viewport alias) can be hidden from the palette.
    bool in_palette{true};
    // Menu layout hints: a submenu name inside the category menu, and whether a
    // separator precedes this command.
    std::string submenu;
    bool separator_before{};
    // Commands that only drive the menu bar (shortcut hints in Help) can be
    // hidden from menus.
    bool in_menu{true};
    // Bookmark and alias commands are summarized in the cheat sheet by hand.
    bool in_help{true};
};

struct ShortcutConflict {
    std::string first, second, shortcut;
};

class CommandRegistry {
public:
    // Adds a command. Returns false, without adding, when the id is empty or
    // already registered.
    bool add(Command command);
    // Most recently added command, for builders that fill optional fields.
    [[nodiscard]] Command& back() noexcept { return commands_.back(); }
    [[nodiscard]] const Command* find(std::string_view id) const noexcept;
    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }
    [[nodiscard]] bool is_enabled(std::string_view id) const noexcept;
    // Runs the command when it exists and is enabled. Returns whether it ran.
    bool run(std::string_view id) const;
    // Every pair of commands that bind the same shortcut in the same scope, and
    // any command whose shortcut string does not parse.
    [[nodiscard]] std::vector<ShortcutConflict> conflicts() const;
    // Commands in registration order, grouped by category in first-seen order.
    [[nodiscard]] std::vector<std::pair<std::string, std::vector<const Command*>>>
    by_category() const;

private:
    std::vector<Command> commands_;
};

} // namespace rwsman
