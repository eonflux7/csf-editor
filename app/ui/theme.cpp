#include "ui/theme.hpp"

#include "ui/icons.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace rwsman::ui {
namespace {

constexpr ImVec4 rgb(const unsigned hex, const float alpha = 1.0F) {
    return {static_cast<float>((hex >> 16U) & 0xFFU) / 255.0F,
            static_cast<float>((hex >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>(hex & 0xFFU) / 255.0F, alpha};
}

using Palette = std::array<ImVec4, static_cast<std::size_t>(Token::count)>;

constexpr Palette dark_palette{
    rgb(0x0F1114),        // bg0
    rgb(0x15181D),        // bg1
    rgb(0x1C2027),        // bg2
    rgb(0x2A2F38),        // line
    rgb(0xD4D7DD),        // text
    rgb(0x7D8490),        // text_dim
    rgb(0xE8A33D),        // accent
    rgb(0xE8A33D, 0.22F), // accent_dim
    rgb(0x6CC56C),        // ok
    rgb(0x5FB3D9),        // inferred
    rgb(0xA58BD8),        // raw
    rgb(0xE0B341),        // warn
    rgb(0xE5534B),        // error
    rgb(0xE8A33D),        // dirty
};

constexpr Palette high_contrast_palette{
    rgb(0x000000),        // bg0
    rgb(0x080808),        // bg1
    rgb(0x1A1A1A),        // bg2
    rgb(0x8C8C8C),        // line
    rgb(0xFFFFFF),        // text
    rgb(0xB8B8B8),        // text_dim
    rgb(0xFFB84D),        // accent
    rgb(0xFFB84D, 0.35F), // accent_dim
    rgb(0x66FF66),        // ok
    rgb(0x66D9FF),        // inferred
    rgb(0xC9A8FF),        // raw
    rgb(0xFFDD44),        // warn
    rgb(0xFF6B63),        // error
    rgb(0xFFB84D),        // dirty
};

const Palette* palette = &dark_palette;
std::string current_theme = "dark";

ImVec4 mix(const ImVec4& a, const ImVec4& b, const float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t};
}

ImVec4 with_a(ImVec4 value, const float alpha) {
    value.w = alpha;
    return value;
}

constexpr std::array<ImU32, static_cast<std::size_t>(Viewport::count)> viewport_colors{
    IM_COL32(8, 9, 11, 255),      // clear
    IM_COL32(22, 25, 31, 255),    // canvas
    IM_COL32(70, 76, 88, 255),    // canvas_border
    IM_COL32(225, 229, 238, 255), // hud_text
    IM_COL32(10, 12, 16, 185),    // hud_background
    IM_COL32(95, 103, 118, 55),   // grid_minor
    IM_COL32(125, 134, 150, 95),  // grid_major
    IM_COL32(229, 83, 75, 255),   // axis_x
    IM_COL32(108, 197, 108, 255), // axis_y
    IM_COL32(95, 179, 217, 255),  // axis_z
    IM_COL32(232, 163, 61, 255),  // selection
    IM_COL32(255, 255, 255, 255), // selection_secondary
    IM_COL32(255, 230, 100, 235), // measurement
    IM_COL32(229, 83, 75, 255),   // error_text
    IM_COL32(125, 134, 150, 160), // bounds
    IM_COL32(255, 150, 60, 255),  // actor
    IM_COL32(60, 205, 255, 255),  // nav_ground
    IM_COL32(85, 235, 145, 255),  // nav_climb
    IM_COL32(245, 135, 245, 255), // nav_special
    IM_COL32(50, 175, 225, 180),  // nav_link
    IM_COL32(190, 105, 255, 255), // dummy
    IM_COL32(255, 215, 70, 210),  // area
    IM_COL32(255, 85, 220, 255),  // cutscene_camera
    IM_COL32(255, 95, 150, 255),  // effect
    IM_COL32(255, 105, 215, 225), // actor_collision
    IM_COL32(70, 225, 255, 220),  // actor_physics
    IM_COL32(100, 255, 155, 235), // skeleton
    IM_COL32(175, 255, 205, 255), // skeleton_label
    IM_COL32(255, 45, 210, 255),  // collision_selected
    IM_COL32(255, 120, 210, 255), // bsp_path
    IM_COL32(245, 215, 70, 210),  // leaf_bounds
    IM_COL32(65, 210, 255, 255),  // measure_a
    IM_COL32(255, 190, 55, 255),  // measure_b
    IM_COL32(15, 18, 22, 220),    // point_ring
    IM_COL32(240, 75, 75, 210),   // bsp_x
    IM_COL32(80, 225, 95, 210),   // bsp_y
    IM_COL32(75, 135, 245, 210),  // bsp_z
    IM_COL32(165, 174, 190, 255), // hud_text_dim
    IM_COL32(92, 59, 15, 220),    // warning_background
    IM_COL32(255, 202, 105, 255), // warning_text
    IM_COL32(232, 163, 61, 150),  // clip_plane
    IM_COL32(255, 235, 80, 255),  // physics_center
    IM_COL32(255, 90, 220, 255),  // physics_axis
    IM_COL32(80, 220, 255, 235),  // physics_body
    IM_COL32(255, 120, 190, 235), // physics_ragdoll
    IM_COL32(255, 220, 95, 245),  // physics_joint
};

} // namespace

ImVec4 color(const Token token) {
    return (*palette)[static_cast<std::size_t>(token)];
}

ImVec4 transparent() {
    return {0.0F, 0.0F, 0.0F, 0.0F};
}

ImVec4 color(const Token token, const float alpha) {
    auto value = color(token);
    value.w *= alpha;
    return value;
}

ImU32 color_u32(const Token token, const float alpha) {
    auto value = color(token);
    value.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(value);
}

Token provenance_token(const Provenance provenance) {
    switch (provenance) {
    case Provenance::proven:
        return Token::ok;
    case Provenance::inferred:
        return Token::inferred;
    case Provenance::unknown:
        return Token::raw;
    case Provenance::diagnosed:
        return Token::warn;
    }
    return Token::text_dim;
}

const char* provenance_glyph(const Provenance provenance) {
    switch (provenance) {
    case Provenance::proven:
        return icons::LC_CHECK;
    case Provenance::inferred:
        return "~";
    case Provenance::unknown:
        return "?";
    case Provenance::diagnosed:
        return icons::LC_TRIANGLE_ALERT;
    }
    return "?";
}

const char* provenance_name(const Provenance provenance) {
    switch (provenance) {
    case Provenance::proven:
        return "proven";
    case Provenance::inferred:
        return "inferred";
    case Provenance::unknown:
        return "unknown / raw";
    case Provenance::diagnosed:
        return "diagnosed";
    }
    return "";
}

void set_theme(const std::string_view name) {
    if (name == "high-contrast") {
        palette = &high_contrast_palette;
        current_theme = "high-contrast";
    } else {
        palette = &dark_palette;
        current_theme = "dark";
    }
}

std::string_view theme_name() {
    return current_theme;
}

ImVec4 heat(const float t) {
    return mix(color(Token::text_dim), color(Token::accent), std::clamp(t, 0.0F, 1.0F));
}

ImU32 viewport_color(const Viewport which, const float alpha) {
    const ImU32 packed = viewport_colors[static_cast<std::size_t>(which)];
    if (alpha >= 1.0F) return packed;
    const auto scaled = static_cast<ImU32>(
        static_cast<float>((packed >> IM_COL32_A_SHIFT) & 0xFFU) * std::clamp(alpha, 0.0F, 1.0F));
    return (packed & ~IM_COL32_A_MASK) | (scaled << IM_COL32_A_SHIFT);
}

ImVec4 viewport_color_f(const Viewport which, const float alpha) {
    return ImGui::ColorConvertU32ToFloat4(viewport_color(which, alpha));
}

ImU32 rgba_u32(const int red, const int green, const int blue, const int alpha) {
    return IM_COL32(std::clamp(red, 0, 255), std::clamp(green, 0, 255), std::clamp(blue, 0, 255),
                    std::clamp(alpha, 0, 255));
}

std::array<std::uint8_t, 4> unpack_rgba(const ImU32 packed) {
    return {static_cast<std::uint8_t>((packed >> IM_COL32_R_SHIFT) & 0xFFU),
            static_cast<std::uint8_t>((packed >> IM_COL32_G_SHIFT) & 0xFFU),
            static_cast<std::uint8_t>((packed >> IM_COL32_B_SHIFT) & 0xFFU),
            static_cast<std::uint8_t>((packed >> IM_COL32_A_SHIFT) & 0xFFU)};
}

ImU32 rgb_u32(const std::uint32_t value, const std::uint8_t alpha) {
    return IM_COL32((value >> 16U) & 0xFFU, (value >> 8U) & 0xFFU, value & 0xFFU, alpha);
}

ImU32 with_alpha(const ImU32 packed, const std::uint8_t alpha) {
    return (packed & ~IM_COL32_A_MASK) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

void apply_theme(const float scale) {
    const auto bg0 = color(Token::bg0), bg1 = color(Token::bg1), bg2 = color(Token::bg2);
    const auto line = color(Token::line), text = color(Token::text);
    const auto dim = color(Token::text_dim), accent = color(Token::accent);
    const auto accent_dim = color(Token::accent_dim);
    const auto hover = mix(bg2, text, 0.06F);
    const auto pressed = mix(bg2, accent, 0.30F);

    ImGuiStyle style;
    style.Alpha = 1.0F;
    style.DisabledAlpha = 0.45F;
    style.WindowPadding = {8.0F, 6.0F};
    style.FramePadding = {6.0F, 3.0F};
    style.CellPadding = {6.0F, 2.0F};
    style.ItemSpacing = {6.0F, 4.0F};
    style.ItemInnerSpacing = {4.0F, 4.0F};
    style.IndentSpacing = 14.0F;
    style.ScrollbarSize = 11.0F;
    style.GrabMinSize = 9.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.TabBarOverlineSize = 2.0F;
    style.WindowRounding = 0.0F;
    style.ChildRounding = 0.0F;
    style.FrameRounding = 2.0F;
    style.PopupRounding = 2.0F;
    style.ScrollbarRounding = 0.0F;
    style.GrabRounding = 2.0F;
    style.TabRounding = 0.0F;
    style.WindowTitleAlign = {0.0F, 0.5F};
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextAlign = {0.0F, 0.5F};
    style.SeparatorTextPadding = {12.0F, 2.0F};
    style.DockingSeparatorSize = 2.0F;

    auto* c = style.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = dim;
    c[ImGuiCol_WindowBg] = bg1;
    c[ImGuiCol_ChildBg] = with_a(bg1, 0.0F);
    c[ImGuiCol_PopupBg] = mix(bg1, bg2, 0.5F);
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = with_a(bg0, 0.0F);
    c[ImGuiCol_FrameBg] = bg2;
    c[ImGuiCol_FrameBgHovered] = hover;
    c[ImGuiCol_FrameBgActive] = pressed;
    c[ImGuiCol_TitleBg] = bg1;
    c[ImGuiCol_TitleBgActive] = bg2;
    c[ImGuiCol_TitleBgCollapsed] = bg1;
    c[ImGuiCol_MenuBarBg] = bg0;
    c[ImGuiCol_ScrollbarBg] = with_a(bg0, 0.5F);
    c[ImGuiCol_ScrollbarGrab] = mix(line, text, 0.10F);
    c[ImGuiCol_ScrollbarGrabHovered] = mix(line, text, 0.30F);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = mix(accent, bg2, 0.35F);
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = hover;
    c[ImGuiCol_ButtonActive] = pressed;
    c[ImGuiCol_Header] = accent_dim;
    c[ImGuiCol_HeaderHovered] = hover;
    c[ImGuiCol_HeaderActive] = mix(accent_dim, accent, 0.35F);
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = mix(line, accent, 0.6F);
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = with_a(line, 0.6F);
    c[ImGuiCol_ResizeGripHovered] = mix(line, accent, 0.6F);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_TabHovered] = hover;
    c[ImGuiCol_Tab] = bg1;
    c[ImGuiCol_TabSelected] = bg2;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg1;
    c[ImGuiCol_TabDimmedSelected] = bg2;
    c[ImGuiCol_TabDimmedSelectedOverline] = with_a(accent, 0.45F);
    c[ImGuiCol_DockingPreview] = with_a(accent, 0.30F);
    c[ImGuiCol_DockingEmptyBg] = bg0;
    c[ImGuiCol_PlotLines] = mix(text, dim, 0.4F);
    c[ImGuiCol_PlotLinesHovered] = accent;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = mix(accent, text, 0.3F);
    c[ImGuiCol_TableHeaderBg] = bg2;
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = with_a(line, 0.55F);
    c[ImGuiCol_TableRowBg] = with_a(bg0, 0.0F);
    c[ImGuiCol_TableRowBgAlt] = with_a(text, 0.025F);
    c[ImGuiCol_TextLink] = color(Token::inferred);
    c[ImGuiCol_TextSelectedBg] = with_a(accent, 0.30F);
    c[ImGuiCol_DragDropTarget] = accent;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_NavWindowingHighlight] = with_a(text, 0.7F);
    c[ImGuiCol_NavWindowingDimBg] = with_a(bg0, 0.6F);
    c[ImGuiCol_ModalWindowDimBg] = with_a(bg0, 0.6F);

    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
}

} // namespace rwsman::ui
