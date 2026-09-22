#include "rwsman/commands.hpp"

#include <algorithm>
#include <cctype>

namespace rwsman {
namespace {

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string canonical_key(std::string_view key) {
    std::string result(key);
    if (result.size() == 1) {
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
        return result;
    }
    // Named keys: capitalize the first letter, keep the rest ("F12", "Left").
    if (!result.empty() && std::islower(static_cast<unsigned char>(result[0])))
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}

} // namespace

std::optional<Shortcut> parse_shortcut(const std::string_view text) {
    Shortcut result;
    std::size_t start = 0;
    bool have_key = false;
    while (start <= text.size()) {
        const auto plus = text.find('+', start);
        // A trailing "+" is the plus key itself ("Ctrl++").
        auto part = text.substr(start, plus == std::string_view::npos ? plus : plus - start);
        if (part.empty() && plus != std::string_view::npos && plus + 1 == text.size()) {
            part = "+";
            start = text.size() + 1;
        } else {
            start = plus == std::string_view::npos ? text.size() + 1 : plus + 1;
        }
        if (part.empty()) return std::nullopt;
        if (have_key) return std::nullopt; // Something follows the key.
        const auto name = lower(part);
        if (name == "ctrl" || name == "control")
            result.ctrl = true;
        else if (name == "shift")
            result.shift = true;
        else if (name == "alt")
            result.alt = true;
        else {
            result.key = canonical_key(part);
            have_key = true;
        }
    }
    if (!have_key) return std::nullopt;
    return result;
}

std::string format_shortcut(const Shortcut& shortcut) {
    std::string result;
    if (shortcut.ctrl) result += "Ctrl+";
    if (shortcut.shift) result += "Shift+";
    if (shortcut.alt) result += "Alt+";
    return result + shortcut.key;
}

bool CommandRegistry::add(Command command) {
    if (command.id.empty() || find(command.id)) return false;
    commands_.push_back(std::move(command));
    return true;
}

const Command* CommandRegistry::find(const std::string_view id) const noexcept {
    const auto found = std::ranges::find_if(commands_, [&](const Command& c) { return c.id == id; });
    return found == commands_.end() ? nullptr : &*found;
}

bool CommandRegistry::is_enabled(const std::string_view id) const noexcept {
    const auto* command = find(id);
    return command && (!command->enabled || command->enabled());
}

bool CommandRegistry::run(const std::string_view id) const {
    const auto* command = find(id);
    if (!command || !command->run || (command->enabled && !command->enabled())) return false;
    command->run();
    return true;
}

std::vector<ShortcutConflict> CommandRegistry::conflicts() const {
    std::vector<ShortcutConflict> result;
    for (std::size_t i = 0; i < commands_.size(); ++i) {
        if (commands_[i].shortcut.empty()) continue;
        const auto first = parse_shortcut(commands_[i].shortcut);
        if (!first) {
            result.push_back({commands_[i].id, {}, commands_[i].shortcut});
            continue;
        }
        for (std::size_t j = i + 1; j < commands_.size(); ++j) {
            if (commands_[j].shortcut.empty() || commands_[j].scope != commands_[i].scope)
                continue;
            const auto second = parse_shortcut(commands_[j].shortcut);
            if (second && *first == *second)
                result.push_back({commands_[i].id, commands_[j].id, format_shortcut(*first)});
        }
    }
    return result;
}

std::vector<std::pair<std::string, std::vector<const Command*>>>
CommandRegistry::by_category() const {
    std::vector<std::pair<std::string, std::vector<const Command*>>> groups;
    for (const auto& command : commands_) {
        auto group = std::ranges::find_if(
            groups, [&](const auto& entry) { return entry.first == command.category; });
        if (group == groups.end()) {
            groups.emplace_back(command.category, std::vector<const Command*>{});
            group = groups.end() - 1;
        }
        group->second.push_back(&command);
    }
    return groups;
}

} // namespace rwsman
