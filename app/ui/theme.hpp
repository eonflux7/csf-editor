#pragma once

#include <imgui.h>

#include <array>
#include <cstdint>
#include <string_view>

// All colors in the GUI come from this header. ImGui color literals (IM_COL32,
// ImVec4 constructors) are only written in theme.cpp; everything else asks for a
// token or one of the named viewport colors below.
namespace rwsman::ui {

enum class Token : std::uint8_t {
    bg0,        // Window background, viewport clear.
    bg1,        // Panels, child regions.
    bg2,        // Headers, hovered rows, inputs.
    line,       // 1 px borders and separators.
    text,       // Primary text.
    text_dim,   // Labels, secondary info.
    accent,     // Selection, active tab, focus ring (amber).
    accent_dim, // Selected row background.
    ok,         // Proven: resolved, validated, exact.
    inferred,   // Candidate or untyped relationship.
    raw,        // Unknown field, preserved bytes.
    warn,       // Warnings, ambiguous resolution.
    error,      // Errors, missing, truncated.
    dirty,      // Modified in this session or staged.
    count
};

// What a displayed value is known to be. Badges come only from resolver and
// scene status fields, never from UI heuristics.
enum class Provenance : std::uint8_t {
    proven,   // Exact, validated, or resolved uniquely.
    inferred, // Candidate or untyped relationship.
    unknown,  // Unknown or raw: preserved bytes with no decoded meaning.
    diagnosed // Carries a diagnostic.
};

[[nodiscard]] ImVec4 color(Token token);
[[nodiscard]] ImVec4 color(Token token, float alpha);
[[nodiscard]] ImU32 color_u32(Token token, float alpha = 1.0F);
// Fully transparent color (icon buttons, invisible backgrounds).
[[nodiscard]] ImVec4 transparent();
[[nodiscard]] Token provenance_token(Provenance provenance);
[[nodiscard]] const char* provenance_glyph(Provenance provenance);
[[nodiscard]] const char* provenance_name(Provenance provenance);

// "dark" or "high-contrast". Unknown names fall back to "dark".
void set_theme(std::string_view name);
[[nodiscard]] std::string_view theme_name();
// Rebuilds ImGuiStyle from the current theme. `scale` is the combined OS and user
// UI scale; sizes are multiplied by it.
void apply_theme(float scale);

// Heat scale for size gradients: neutral at 0, accent at 1.
[[nodiscard]] ImVec4 heat(float t);

// Colors of things drawn in the 3D viewport and its overlays. They are data
// colors (one per kind of scene record), not semantic status colors.
enum class Viewport : std::uint8_t {
    clear,             // Framebuffer clear color.
    canvas,            // Viewport background.
    canvas_border,
    hud_text,
    hud_background,
    grid_minor,
    grid_major,
    axis_x,
    axis_y,
    axis_z,
    selection,         // Selected object outline / marker.
    selection_secondary,
    measurement,
    error_text,
    bounds,
    actor,
    nav_ground,
    nav_climb,
    nav_special,
    nav_link,
    dummy,
    area,
    cutscene_camera,
    effect,
    actor_collision,
    actor_physics,
    skeleton,
    skeleton_label,
    collision_selected,
    bsp_path,
    leaf_bounds,
    measure_a,
    measure_b,
    point_ring,
    bsp_x,
    bsp_y,
    bsp_z,
    hud_text_dim,
    warning_background,
    warning_text,
    clip_plane,
    physics_center,
    physics_axis,
    physics_body,
    physics_ragdoll,
    physics_joint,
    count
};
[[nodiscard]] ImU32 viewport_color(Viewport color, float alpha = 1.0F);
[[nodiscard]] ImVec4 viewport_color_f(Viewport color, float alpha = 1.0F);
// Packs 0..255 channels, and unpacks a packed color back into channels.
[[nodiscard]] ImU32 rgba_u32(int red, int green, int blue, int alpha = 255);
[[nodiscard]] std::array<std::uint8_t, 4> unpack_rgba(ImU32 color);
// An RGB color packed as 0xRRGGBB (mission light colors) as an opaque ImU32.
[[nodiscard]] ImU32 rgb_u32(std::uint32_t rgb, std::uint8_t alpha = 255);
// The same color with a different alpha.
[[nodiscard]] ImU32 with_alpha(ImU32 color, std::uint8_t alpha);

} // namespace rwsman::ui
