#pragma once

#include <imgui.h>

struct GLFWwindow;

namespace rwsman::ui {

enum class Font {
    sans,      // Menus, buttons, labels.
    sans_bold, // Headers and section titles.
    mono,      // IDs, offsets, hashes, paths, values, the script listing.
    title,     // Start page and Home headings.
    caption,   // Small secondary text: counts, hints under fields, badges.
};

[[nodiscard]] ImFont* font(Font which);

// Rebuilds the font atlas and the ImGui style when the effective scale changed:
// the OS content scale, the framebuffer scale, or the user override. Must be
// called between frames, before ImGui_ImplOpenGL3_NewFrame(). Returns true when
// anything was rebuilt (always true on the first call).
bool update_fonts_and_style(GLFWwindow* window, float user_scale);

// Scale currently applied to sizes: OS content scale times the user override.
[[nodiscard]] float ui_scale();

struct FontScope {
    explicit FontScope(Font which) { ImGui::PushFont(font(which)); }
    ~FontScope() { ImGui::PopFont(); }
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};

} // namespace rwsman::ui
