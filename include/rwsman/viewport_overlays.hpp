#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// GUI-free helpers behind the viewport's mission overlays: the persisted display
// options, screen-space marker merging, label placement, the text filter, and
// small polygon utilities. Screen coordinates are pixels with Y down.
namespace rwsman {

// Stable keys of the overlay layers, in `GeometryPreview::MissionOverlayKind` order.
inline constexpr std::array<std::string_view, 10> overlay_layer_keys{
    "actor", "nav_point",     "nav_link", "dummy",     "cutscene_camera",
    "area",  "light",         "effect",   "actor_cmo", "actor_physics"};

struct OverlayPreset {
    std::string name;
    std::vector<std::string> hidden_layers; // Keys from `overlay_layer_keys`.
    [[nodiscard]] friend bool operator==(const OverlayPreset&, const OverlayPreset&) = default;
};

struct OverlayOptions {
    enum class Labels : std::uint8_t { off, selected, hovered, nearby, all };
    // Heading ticks, cutscene camera frusta, and light radii.
    enum class Detail : std::uint8_t { off, selected, all };

    Labels labels{Labels::hovered};
    Detail headings{Detail::all};
    Detail details{Detail::selected};
    // Opacity of markers deep behind level geometry; a single wall dims less.
    // 0 hides occluded markers, 1 ignores depth.
    float occluded_opacity{0.25F};
    // Markers dim with distance past the near side of the scene, reaching their
    // faintest this many scene radii further on (perspective only). 0 disables.
    float fade_distance{3.0F};
    // Markers closer than this many pixels merge into one counted marker. 0 disables.
    float merge_pixels{6.0F};
    // Icon badges appear when at most this many single markers are on screen.
    int icon_limit{120};
    bool show_legend{true};
    bool show_minimap{false};
    // The filter dims non-matching markers; otherwise it hides them.
    bool dim_filtered{true};
    std::vector<std::string> hidden_layers;
    std::vector<OverlayPreset> presets;

    [[nodiscard]] friend bool operator==(const OverlayOptions&, const OverlayOptions&) = default;
    void clamp() noexcept;
    [[nodiscard]] bool layer_hidden(std::string_view key) const noexcept;
    void set_layer_hidden(std::string_view key, bool hidden);
};

// Marker dimming. Distance: full strength up to the near side of the scene as
// seen from the camera (or the camera itself when inside it), then fading
// linearly to `marker_fade_floor` over `fade_distance` scene radii. Walls: each
// wall between the camera and a marker dims it further, approaching
// `occluded_opacity`. Markers dim rather than vanish unless that opacity is 0.
inline constexpr float marker_fade_floor = 0.3F;
struct MarkerFadeRange {
    float near_distance{}, far_distance{}; // From the camera; far_distance <= near_distance disables fading.
};
[[nodiscard]] MarkerFadeRange marker_fade_range(float camera_to_scene_center, float scene_radius,
                                                float fade_distance) noexcept;
[[nodiscard]] float marker_distance_fade(MarkerFadeRange range, float distance) noexcept;
[[nodiscard]] float marker_wall_fade(int walls, float occluded_opacity) noexcept;

[[nodiscard]] std::string_view overlay_labels_name(OverlayOptions::Labels value) noexcept;
[[nodiscard]] std::optional<OverlayOptions::Labels> parse_overlay_labels(std::string_view text) noexcept;
[[nodiscard]] std::string_view overlay_detail_name(OverlayOptions::Detail value) noexcept;
[[nodiscard]] std::optional<OverlayOptions::Detail> parse_overlay_detail(std::string_view text) noexcept;

// Presets that ship with the app; a user preset with the same name replaces one.
[[nodiscard]] std::span<const OverlayPreset> builtin_overlay_presets();

struct ScreenMarker {
    float x{}, y{};
    // Markers that never merge (the selection, the hovered marker).
    bool pinned{};
};

struct MarkerCluster {
    float x{}, y{};                     // Position of the first member.
    std::vector<std::uint32_t> members; // Indices into the input, in input order.
};

// Greedy merge in input order: each marker joins the first cluster whose anchor lies
// within `radius` pixels, or starts a new one. Pinned markers stay single. A radius
// of 0 or less returns one cluster per marker.
[[nodiscard]] std::vector<MarkerCluster> cluster_markers(std::span<const ScreenMarker> markers,
                                                         float radius);

struct ScreenRect {
    float x0{}, y0{}, x1{}, y1{};
    [[nodiscard]] bool overlaps(const ScreenRect& other) const noexcept {
        return x0 < other.x1 && other.x0 < x1 && y0 < other.y1 && other.y0 < y1;
    }
    [[nodiscard]] bool contains(const ScreenRect& other) const noexcept {
        return other.x0 >= x0 && other.x1 <= x1 && other.y0 >= y0 && other.y1 <= y1;
    }
};

struct LabelRequest {
    float anchor_x{}, anchor_y{};
    float width{}, height{};
    float priority{}; // Higher places first.
};

// Places labels beside their anchors without overlapping each other or the
// `obstacles`, trying right-above, right-below, left-above, then left-below.
// Returns each request's rectangle, or nullopt when every candidate collides or
// leaves `bounds`.
[[nodiscard]] std::vector<std::optional<ScreenRect>>
place_labels(std::span<const LabelRequest> requests, ScreenRect bounds, float gap,
             std::span<const ScreenRect> obstacles = {});

// Where an arrow pointing at an off-screen target sits on the viewport edge, for a
// target in direction (dx, dy) from the viewport center. `angle` is in radians,
// 0 pointing right, increasing clockwise (Y down).
struct EdgeIndicator {
    float x{}, y{}, angle{};
};
[[nodiscard]] std::optional<EdgeIndicator> edge_indicator(float dx, float dy, ScreenRect viewport,
                                                          float inset) noexcept;

// Overlay text filter. Whitespace-separated terms must all match:
//   #123        the source entry index
//   kind:nav    a layer key or layer name containing the text
//   anything    a fuzzy match against "<label> <sublayer> <layer name>"
// An empty or blank query matches everything.
struct OverlayFilterSubject {
    std::string_view layer_key, layer_name, sublayer, label;
    std::uint32_t entry{};
};
[[nodiscard]] bool overlay_filter_matches(std::string_view query, const OverlayFilterSubject& subject);

using Point2 = std::array<float, 2>;
// Even-odd rule; points on an edge may go either way.
[[nodiscard]] bool point_in_polygon(std::span<const Point2> polygon, float x, float y) noexcept;
// Ear-clipping triangulation of a simple polygon in either winding. Degenerate
// input (fewer than 3 points, zero area) yields no triangles; a polygon that is
// not simple falls back to a fan.
[[nodiscard]] std::vector<std::array<std::uint32_t, 3>> triangulate_polygon(std::span<const Point2> polygon);

} // namespace rwsman
