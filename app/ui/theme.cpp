#include "ui/theme.hpp"

#include "ui/icons.hpp"

#include "rwsman/contrast.hpp"

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
    rgb(0x878E9A),        // text_dim (AA 4.5:1 on bg2)
    rgb(0xE8A33D),        // accent
    rgb(0xE8A33D, 0.22F), // accent_dim
    rgb(0x6CC56C),        // ok
    rgb(0x5FB3D9),        // inferred
    rgb(0xA58BD8),        // raw
    rgb(0xE0B341),        // warn
    rgb(0xF05F57),        // error
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

// EntityKind colours, dark and high-contrast (0xRRGGBB).
constexpr std::array<unsigned, static_cast<std::size_t>(EntityKind::count)> dark_kinds{
    0x6CC56C, // player
    0xE8705B, // enemy
    0xC9955A, // animal
    0x94A7BD, // vehicle
    0x5FD1C0, // pickup
    0xE0B341, // usable
    0xA7ADB7, // prop
    0x878E9A, // helper
    0xE5534B, // unresolved
    0xC7925F, // building
    0x74B85F, // vegetation
    0x3CCDFF, // route
    0x55E091, // cover
    0x4E9BC9, // walk_grid
    0xF06AD6, // camera_path
    0xF2CC4B, // zone
    0xB77DF5, // marker
    0xF5E78A, // light
    0xFF6E9F, // effect
    0xF0A94A, // objective
    0xA58BD8, // trigger
    0x8FB8E8, // script
};
constexpr std::array<unsigned, static_cast<std::size_t>(EntityKind::count)> high_contrast_kinds{
    0x7CFF7C, 0xFF7A66, 0xE0AA6A, 0xB8CCE0, 0x6FFFEA, 0xFFD84D, 0xD0D4DC, 0xB8B8B8, 0xFF6B63, 0xE5A870, 0x8CE070,
    0x66E0FF, 0x66FFA8, 0x70C0F0, 0xFF80E8, 0xFFE066, 0xD39CFF, 0xFFF3A0, 0xFF8AB6, 0xFFBE5C, 0xC9B0FF, 0xA8D0FF,
};

// Syntax colours (plain, opcode, tag, number, string, label, punctuation).
constexpr std::array<unsigned, static_cast<std::size_t>(Syntax::count)> dark_syntax{
    0xD4D7DD, 0xE8A33D, 0x5FB3D9, 0xA58BD8, 0x6CC56C, 0x878E9A, 0x6B7280};
constexpr std::array<unsigned, static_cast<std::size_t>(Syntax::count)> high_contrast_syntax{
    0xFFFFFF, 0xFFB84D, 0x66D9FF, 0xC9A8FF, 0x66FF66, 0xB8B8B8, 0x9A9A9A};
const std::array<unsigned, static_cast<std::size_t>(Syntax::count)>* syntax_palette = &dark_syntax;

const Palette* palette = &dark_palette;
const std::array<unsigned, static_cast<std::size_t>(EntityKind::count)>* kind_palette = &dark_kinds;
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
    IM_COL32(255, 215, 70, 34),   // area_fill
    IM_COL32(10, 12, 16, 230),    // marker_outline
    IM_COL32(255, 255, 255, 255), // hover
    IM_COL32(120, 230, 255, 255), // related
    IM_COL32(52, 58, 70, 235),    // cluster
    IM_COL32(235, 238, 245, 255), // cluster_text
    IM_COL32(12, 14, 18, 205),    // label_background
    IM_COL32(232, 163, 61, 240),  // offscreen
    IM_COL32(12, 14, 18, 210),    // minimap_background
    IM_COL32(232, 163, 61, 200),  // minimap_view
    IM_COL32(120, 230, 255, 170), // slice_band
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
        kind_palette = &high_contrast_kinds;
        syntax_palette = &high_contrast_syntax;
        current_theme = "high-contrast";
    } else {
        palette = &dark_palette;
        syntax_palette = &dark_syntax;
        kind_palette = &dark_kinds;
        current_theme = "dark";
    }
}

std::string_view theme_name() {
    return current_theme;
}

ImVec4 kind_color(const EntityKind kind, const float alpha) {
    const auto index = static_cast<std::size_t>(kind);
    return rgb(index < kind_palette->size() ? (*kind_palette)[index] : 0x878E9A, alpha);
}

ImU32 kind_color_u32(const EntityKind kind, const float alpha) {
    return ImGui::ColorConvertFloat4ToU32(kind_color(kind, alpha));
}

ImU32 syntax_color(const Syntax syntax) {
    const auto index = static_cast<std::size_t>(syntax);
    return ImGui::ColorConvertFloat4ToU32(rgb(index < syntax_palette->size() ? (*syntax_palette)[index] : 0xD4D7DD));
}

const char* kind_icon(const EntityKind kind) {
    switch (kind) {
    case EntityKind::player: return icons::LC_USER;
    case EntityKind::enemy: return icons::LC_PERSON_STANDING;
    case EntityKind::animal: return icons::LC_DOG;
    case EntityKind::vehicle: return icons::LC_TRUCK;
    case EntityKind::pickup: return icons::LC_PACKAGE;
    case EntityKind::usable: return icons::LC_HAND;
    case EntityKind::prop: return icons::LC_BOX;
    case EntityKind::helper: return icons::LC_VIDEO;
    case EntityKind::unresolved: return icons::LC_CIRCLE_ALERT;
    case EntityKind::building: return icons::LC_BUILDING_2;
    case EntityKind::vegetation: return icons::LC_TREES;
    case EntityKind::route: return icons::LC_ROUTE;
    case EntityKind::cover: return icons::LC_SHIELD;
    case EntityKind::walk_grid: return icons::LC_GRID_3X3;
    case EntityKind::camera_path: return icons::LC_SPLINE;
    case EntityKind::zone: return icons::LC_SQUARE_DASHED;
    case EntityKind::marker: return icons::LC_MAP_PIN;
    case EntityKind::light: return icons::LC_LIGHTBULB;
    case EntityKind::effect: return icons::LC_SPARKLES;
    case EntityKind::objective: return icons::LC_FLAG;
    case EntityKind::trigger: return icons::LC_ZAP;
    case EntityKind::script: return icons::LC_SCROLL_TEXT;
    case EntityKind::count: break;
    }
    return icons::LC_DOT;
}

std::vector<std::string> theme_contrast_problems() {
    const auto packed = [](const Token token) {
        const auto value = color(token);
        const auto channel = [](const float c) { return static_cast<std::uint32_t>(std::clamp(c, 0.0F, 1.0F) * 255.0F + 0.5F); };
        return channel(value.x) << 16U | channel(value.y) << 8U | channel(value.z);
    };
    constexpr std::array<std::pair<Token, const char*>, 3> backgrounds{
        {{Token::bg0, "bg0"}, {Token::bg1, "bg1"}, {Token::bg2, "bg2"}}};
    constexpr std::array<std::pair<Token, const char*>, 6> foregrounds{{{Token::text, "text"},
                                                                       {Token::text_dim, "text_dim"},
                                                                       {Token::accent, "accent"},
                                                                       {Token::ok, "ok"},
                                                                       {Token::warn, "warn"},
                                                                       {Token::error, "error"}}};
    std::vector<std::string> problems;
    for (const auto& [foreground, foreground_name] : foregrounds)
        for (const auto& [background, background_name] : backgrounds)
            if (const double ratio = contrast_ratio(packed(foreground), packed(background)); ratio < 4.5)
                problems.push_back(current_theme + ": " + foreground_name + " on " + background_name + " is " +
                                   std::to_string(ratio).substr(0, 4) + ":1 (AA needs 4.5)");
    for (std::size_t i = 0; i < kind_palette->size(); ++i)
        for (const auto& [background, background_name] : backgrounds)
            if (const double ratio = contrast_ratio((*kind_palette)[i], packed(background)); ratio < 3.0)
                problems.push_back(current_theme + ": kind " + entity_kind_name(static_cast<EntityKind>(i)) +
                                   " on " + background_name + " is " + std::to_string(ratio).substr(0, 4) +
                                   ":1 (icons need 3)");
    return problems;
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
    style.WindowPadding = {10.0F, 8.0F};
    style.FramePadding = {8.0F, 4.0F};
    style.CellPadding = {7.0F, 3.0F};
    style.ItemSpacing = {8.0F, 5.0F};
    style.ItemInnerSpacing = {5.0F, 4.0F};
    style.IndentSpacing = 16.0F;
    style.ScrollbarSize = 12.0F;
    style.GrabMinSize = 10.0F;
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
