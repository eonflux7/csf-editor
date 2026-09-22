#include "ui/shortcuts.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace rwsman::ui {

std::optional<ImGuiKey> key_from_name(const std::string_view name) {
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'A' && c <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'A'));
        if (c >= 'a' && c <= 'z') return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'a'));
        if (c >= '0' && c <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
        if (c == ',') return ImGuiKey_Comma;
        if (c == '.') return ImGuiKey_Period;
        if (c == '/') return ImGuiKey_Slash;
        if (c == '+' || c == '=') return ImGuiKey_Equal;
        if (c == '-') return ImGuiKey_Minus;
    }
    if (name.size() >= 2 && name.size() <= 3 && (name[0] == 'F' || name[0] == 'f')) {
        const int number = std::atoi(std::string(name.substr(1)).c_str());
        if (number >= 1 && number <= 12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (number - 1));
    }
    struct Named {
        std::string_view name;
        ImGuiKey key;
    };
    static constexpr Named named[] = {
        {"Left", ImGuiKey_LeftArrow},  {"Right", ImGuiKey_RightArrow}, {"Up", ImGuiKey_UpArrow},
        {"Down", ImGuiKey_DownArrow},  {"Home", ImGuiKey_Home},        {"End", ImGuiKey_End},
        {"Space", ImGuiKey_Space},     {"Comma", ImGuiKey_Comma},      {"Period", ImGuiKey_Period},
        {"Tab", ImGuiKey_Tab},         {"Escape", ImGuiKey_Escape},    {"Enter", ImGuiKey_Enter},
        {"Delete", ImGuiKey_Delete},   {"Backspace", ImGuiKey_Backspace},
        {"PageUp", ImGuiKey_PageUp},   {"PageDown", ImGuiKey_PageDown},
        {"Numpad1", ImGuiKey_Keypad1}, {"Numpad3", ImGuiKey_Keypad3}, {"Numpad5", ImGuiKey_Keypad5},
        {"Numpad7", ImGuiKey_Keypad7},
    };
    for (const auto& entry : named)
        if (entry.name == name) return entry.key;
    return std::nullopt;
}

void dispatch_shortcuts(AppState& state) {
    const auto& io = ImGui::GetIO();
    // Mouse back/forward buttons work anywhere except while typing.
    if (!io.WantTextInput) {
        if (ImGui::IsMouseClicked(3, false)) state.commands.run("edit.back");
        if (ImGui::IsMouseClicked(4, false)) state.commands.run("edit.forward");
    }
    if (io.WantTextInput) return;
    // While the palette is open it owns the keyboard.
    if (state.ui.palette != UiState::PaletteMode::closed) return;

    const bool viewport_hovered = state.preview.viewport_hovered();
    for (const auto& command : state.commands.commands()) {
        if (command.shortcut.empty() || !command.run) continue;
        if (command.scope == ShortcutScope::viewport && !viewport_hovered) continue;
        const auto shortcut = parse_shortcut(command.shortcut);
        if (!shortcut) continue;
        const auto key = key_from_name(shortcut->key);
        if (!key) continue;
        if (io.KeyCtrl != shortcut->ctrl || io.KeyShift != shortcut->shift ||
            io.KeyAlt != shortcut->alt)
            continue;
        if (!ImGui::IsKeyPressed(*key, false)) continue;
        // Bare keys must not fire when a widget wants the keyboard (menus, popups).
        if (!shortcut->ctrl && !shortcut->alt && io.WantCaptureKeyboard) continue;
        if (state.commands.run(command.id)) return;
    }
}

} // namespace rwsman::ui
