#include "rwsman/viewport_overlays.hpp"

#include "rwsman/fuzzy.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace rwsman {
namespace {

char lower(const char value) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
}

bool contains_insensitive(const std::string_view haystack, const std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        std::size_t i = 0;
        while (i < needle.size() && lower(haystack[start + i]) == lower(needle[i])) ++i;
        if (i == needle.size()) return true;
    }
    return false;
}

// A fuzzy match counts only when the matched characters stay close together, so a
// short query does not match every long label that happens to contain its letters.
bool compact_fuzzy(const std::string_view term, const std::string_view candidate) {
    if (term.size() < 3) return false;
    const auto match = fuzzy_match(term, candidate);
    if (!match || match->positions.empty()) return false;
    const auto span = match->positions.back() - match->positions.front() + 1;
    return span <= term.size() * 2;
}

float cross(const Point2& o, const Point2& a, const Point2& b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
}

bool point_in_triangle(const Point2& p, const Point2& a, const Point2& b, const Point2& c) {
    const float d1 = cross(a, b, p), d2 = cross(b, c, p), d3 = cross(c, a, p);
    const bool negative = d1 < 0 || d2 < 0 || d3 < 0;
    const bool positive = d1 > 0 || d2 > 0 || d3 > 0;
    return !(negative && positive);
}

} // namespace

void OverlayOptions::clamp() noexcept {
    const auto finite_or = [](float& value, const float fallback) {
        if (!std::isfinite(value)) value = fallback;
    };
    finite_or(occluded_opacity, 0.25F);
    finite_or(fade_distance, 3.0F);
    finite_or(merge_pixels, 6.0F);
    occluded_opacity = std::clamp(occluded_opacity, 0.0F, 1.0F);
    fade_distance = fade_distance <= 0.0F ? 0.0F : std::clamp(fade_distance, 0.5F, 50.0F);
    merge_pixels = std::clamp(merge_pixels, 0.0F, 64.0F);
    icon_limit = std::clamp(icon_limit, 0, 5000);
    if (static_cast<int>(labels) > static_cast<int>(Labels::all)) labels = Labels::hovered;
    if (static_cast<int>(headings) > static_cast<int>(Detail::all)) headings = Detail::all;
    if (static_cast<int>(details) > static_cast<int>(Detail::all)) details = Detail::selected;
    std::ranges::sort(hidden_layers);
    const auto duplicates = std::ranges::unique(hidden_layers);
    hidden_layers.erase(duplicates.begin(), duplicates.end());
}

bool OverlayOptions::layer_hidden(const std::string_view key) const noexcept {
    return std::ranges::find(hidden_layers, key) != hidden_layers.end();
}

void OverlayOptions::set_layer_hidden(const std::string_view key, const bool hidden) {
    const auto found = std::ranges::find(hidden_layers, key);
    if (hidden && found == hidden_layers.end()) {
        hidden_layers.emplace_back(key);
        std::ranges::sort(hidden_layers);
    } else if (!hidden && found != hidden_layers.end()) {
        hidden_layers.erase(found);
    }
}

MarkerFadeRange marker_fade_range(const float camera_to_scene_center, const float scene_radius,
                                  const float fade_distance) noexcept {
    if (!(scene_radius > 0.0F) || !(fade_distance > 0.0F) || !std::isfinite(camera_to_scene_center))
        return {};
    const float near = std::max(camera_to_scene_center - scene_radius, 0.0F);
    return {near, near + fade_distance * scene_radius};
}

float marker_distance_fade(const MarkerFadeRange range, const float distance) noexcept {
    if (!(range.far_distance > range.near_distance) || !(distance > range.near_distance)) return 1.0F;
    const float t = std::min((distance - range.near_distance) / (range.far_distance - range.near_distance), 1.0F);
    return (1.0F - t) + marker_fade_floor * t;
}

float marker_wall_fade(const int walls, const float occluded_opacity) noexcept {
    if (walls <= 0) return 1.0F;
    const float limit = std::clamp(occluded_opacity, 0.0F, 1.0F);
    // One wall: sqrt(limit); each further wall halves the remaining gap.
    return std::pow(limit, 1.0F - std::ldexp(1.0F, -std::min(walls, 16)));
}

std::string_view overlay_labels_name(const OverlayOptions::Labels value) noexcept {
    switch (value) {
    case OverlayOptions::Labels::off: return "off";
    case OverlayOptions::Labels::selected: return "selected";
    case OverlayOptions::Labels::hovered: return "hovered";
    case OverlayOptions::Labels::nearby: return "nearby";
    case OverlayOptions::Labels::all: return "all";
    }
    return "hovered";
}

std::optional<OverlayOptions::Labels> parse_overlay_labels(const std::string_view text) noexcept {
    for (int i = 0; i <= static_cast<int>(OverlayOptions::Labels::all); ++i) {
        const auto value = static_cast<OverlayOptions::Labels>(i);
        if (overlay_labels_name(value) == text) return value;
    }
    return std::nullopt;
}

std::string_view overlay_detail_name(const OverlayOptions::Detail value) noexcept {
    switch (value) {
    case OverlayOptions::Detail::off: return "off";
    case OverlayOptions::Detail::selected: return "selected";
    case OverlayOptions::Detail::all: return "all";
    }
    return "selected";
}

std::optional<OverlayOptions::Detail> parse_overlay_detail(const std::string_view text) noexcept {
    for (int i = 0; i <= static_cast<int>(OverlayOptions::Detail::all); ++i) {
        const auto value = static_cast<OverlayOptions::Detail>(i);
        if (overlay_detail_name(value) == text) return value;
    }
    return std::nullopt;
}

std::span<const OverlayPreset> builtin_overlay_presets() {
    static const std::vector<OverlayPreset> presets{
        {"All", {}},
        {"Navigation",
         {"actor", "dummy", "cutscene_camera", "area", "light", "effect", "actor_cmo",
          "actor_physics"}},
        {"Scripting", {"nav_point", "nav_link", "cutscene_camera", "light", "actor_cmo", "actor_physics"}},
        {"Cinematics",
         {"nav_point", "nav_link", "area", "light", "effect", "actor_cmo", "actor_physics"}},
        {"Lighting",
         {"actor", "nav_point", "nav_link", "dummy", "cutscene_camera", "area", "effect",
          "actor_cmo", "actor_physics"}},
        {"Collision", {"nav_point", "nav_link", "dummy", "cutscene_camera", "light", "effect"}},
    };
    return presets;
}

std::vector<MarkerCluster> cluster_markers(const std::span<const ScreenMarker> markers,
                                           const float radius) {
    std::vector<MarkerCluster> clusters;
    clusters.reserve(markers.size());
    if (radius <= 0.0F) {
        for (std::uint32_t i = 0; i < markers.size(); ++i)
            clusters.push_back({markers[i].x, markers[i].y, {i}});
        return clusters;
    }
    // Anchors are bucketed by `radius` cells, so a marker checks the 3x3 cells around it.
    const auto cell = [&](const float value) {
        return static_cast<std::int64_t>(std::floor(value / radius));
    };
    const auto key = [](const std::int64_t x, const std::int64_t y) {
        return (static_cast<std::uint64_t>(x) << 32) ^ static_cast<std::uint64_t>(y & 0xFFFFFFFF);
    };
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;
    const float radius_squared = radius * radius;
    for (std::uint32_t i = 0; i < markers.size(); ++i) {
        const auto& marker = markers[i];
        if (!std::isfinite(marker.x) || !std::isfinite(marker.y)) continue;
        const auto cx = cell(marker.x), cy = cell(marker.y);
        std::optional<std::uint32_t> joined;
        if (!marker.pinned)
            for (std::int64_t dy = -1; dy <= 1 && !joined; ++dy)
                for (std::int64_t dx = -1; dx <= 1 && !joined; ++dx) {
                    const auto found = grid.find(key(cx + dx, cy + dy));
                    if (found == grid.end()) continue;
                    for (const auto index : found->second) {
                        const auto& cluster = clusters[index];
                        const float ox = cluster.x - marker.x, oy = cluster.y - marker.y;
                        if (ox * ox + oy * oy <= radius_squared) {
                            joined = index;
                            break;
                        }
                    }
                }
        if (joined) {
            clusters[*joined].members.push_back(i);
            continue;
        }
        const auto index = static_cast<std::uint32_t>(clusters.size());
        clusters.push_back({marker.x, marker.y, {i}});
        if (!marker.pinned) grid[key(cx, cy)].push_back(index); // Pinned anchors take no members.
    }
    return clusters;
}

std::vector<std::optional<ScreenRect>> place_labels(const std::span<const LabelRequest> requests,
                                                    const ScreenRect bounds, const float gap,
                                                    const std::span<const ScreenRect> obstacles) {
    std::vector<std::optional<ScreenRect>> result(requests.size());
    std::vector<std::uint32_t> order(requests.size());
    std::iota(order.begin(), order.end(), 0U);
    std::ranges::stable_sort(order, [&](const std::uint32_t a, const std::uint32_t b) {
        return requests[a].priority > requests[b].priority;
    });
    // Placed rectangles are bucketed in coarse cells to keep dense scenes linear.
    constexpr float cell_size = 96.0F;
    const auto cell = [](const float value) {
        return static_cast<std::int64_t>(std::floor(value / cell_size));
    };
    const auto key = [](const std::int64_t x, const std::int64_t y) {
        return (static_cast<std::uint64_t>(x) << 32) ^ static_cast<std::uint64_t>(y & 0xFFFFFFFF);
    };
    std::unordered_map<std::uint64_t, std::vector<ScreenRect>> grid;
    const auto insert = [&](const ScreenRect& rect) {
        for (auto y = cell(rect.y0); y <= cell(rect.y1); ++y)
            for (auto x = cell(rect.x0); x <= cell(rect.x1); ++x) grid[key(x, y)].push_back(rect);
    };
    const auto collides = [&](const ScreenRect& rect) {
        for (auto y = cell(rect.y0); y <= cell(rect.y1); ++y)
            for (auto x = cell(rect.x0); x <= cell(rect.x1); ++x) {
                const auto found = grid.find(key(x, y));
                if (found == grid.end()) continue;
                for (const auto& other : found->second)
                    if (rect.overlaps(other)) return true;
            }
        return false;
    };
    for (const auto& obstacle : obstacles) insert(obstacle);
    for (const auto index : order) {
        const auto& request = requests[index];
        const float w = request.width, h = request.height;
        const float ax = request.anchor_x, ay = request.anchor_y;
        const std::array<ScreenRect, 4> candidates{{
            {ax + gap, ay - h - gap * 0.5F, ax + gap + w, ay - gap * 0.5F},
            {ax + gap, ay + gap * 0.5F, ax + gap + w, ay + gap * 0.5F + h},
            {ax - gap - w, ay - h - gap * 0.5F, ax - gap, ay - gap * 0.5F},
            {ax - gap - w, ay + gap * 0.5F, ax - gap, ay + gap * 0.5F + h},
        }};
        for (const auto& candidate : candidates) {
            if (!bounds.contains(candidate) || collides(candidate)) continue;
            result[index] = candidate;
            insert(candidate);
            break;
        }
    }
    return result;
}

std::optional<EdgeIndicator> edge_indicator(const float dx, const float dy, const ScreenRect viewport,
                                            const float inset) noexcept {
    const float length = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(length) || length < 1e-6F) return std::nullopt;
    const float cx = (viewport.x0 + viewport.x1) * 0.5F, cy = (viewport.y0 + viewport.y1) * 0.5F;
    const float half_w = (viewport.x1 - viewport.x0) * 0.5F - inset;
    const float half_h = (viewport.y1 - viewport.y0) * 0.5F - inset;
    if (half_w <= 0.0F || half_h <= 0.0F) return std::nullopt;
    const float ux = dx / length, uy = dy / length;
    const float tx = std::abs(ux) > 1e-6F ? half_w / std::abs(ux) : std::numeric_limits<float>::max();
    const float ty = std::abs(uy) > 1e-6F ? half_h / std::abs(uy) : std::numeric_limits<float>::max();
    const float t = std::min(tx, ty);
    return EdgeIndicator{cx + ux * t, cy + uy * t, std::atan2(uy, ux)};
}

bool overlay_filter_matches(const std::string_view query, const OverlayFilterSubject& subject) {
    std::string haystack;
    std::size_t position = 0;
    while (position < query.size()) {
        while (position < query.size() && std::isspace(static_cast<unsigned char>(query[position])))
            ++position;
        if (position >= query.size()) break;
        auto end = position;
        while (end < query.size() && !std::isspace(static_cast<unsigned char>(query[end]))) ++end;
        const auto term = query.substr(position, end - position);
        position = end;
        if (term.front() == '#') {
            std::uint32_t entry{};
            const auto digits = term.substr(1);
            const auto [ptr, error] =
                std::from_chars(digits.data(), digits.data() + digits.size(), entry);
            if (error != std::errc{} || ptr != digits.data() + digits.size() ||
                entry != subject.entry)
                return false;
            continue;
        }
        if (term.size() > 5 && lower(term[0]) == 'k' && lower(term[1]) == 'i' &&
            lower(term[2]) == 'n' && lower(term[3]) == 'd' && term[4] == ':') {
            const auto kind = term.substr(5);
            if (!contains_insensitive(subject.layer_key, kind) &&
                !contains_insensitive(subject.layer_name, kind))
                return false;
            continue;
        }
        if (haystack.empty()) {
            haystack.reserve(subject.label.size() + subject.sublayer.size() +
                             subject.layer_name.size() + 2);
            haystack.append(subject.label).append(" ").append(subject.sublayer).append(" ").append(
                subject.layer_name);
        }
        if (!contains_insensitive(haystack, term) && !compact_fuzzy(term, haystack)) return false;
    }
    return true;
}

bool point_in_polygon(const std::span<const Point2> polygon, const float x, const float y) noexcept {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const auto& a = polygon[i];
        const auto& b = polygon[j];
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0])
            inside = !inside;
    }
    return inside;
}

std::vector<std::array<std::uint32_t, 3>> triangulate_polygon(const std::span<const Point2> polygon) {
    std::vector<std::array<std::uint32_t, 3>> triangles;
    const auto n = static_cast<std::uint32_t>(polygon.size());
    if (n < 3) return triangles;
    float area = 0.0F;
    for (std::uint32_t i = 0, j = n - 1; i < n; j = i++)
        area += polygon[j][0] * polygon[i][1] - polygon[i][0] * polygon[j][1];
    if (!std::isfinite(area) || std::abs(area) < 1e-9F) return triangles;
    const float winding = area > 0.0F ? 1.0F : -1.0F;
    std::vector<std::uint32_t> remaining(n);
    std::iota(remaining.begin(), remaining.end(), 0U);
    // Each pass removes one ear; a pass that finds none means the polygon is not
    // simple, and the rest is fanned so the caller still gets a closed surface.
    std::size_t guard = static_cast<std::size_t>(n) * n;
    while (remaining.size() > 3 && guard-- > 0) {
        bool clipped = false;
        const auto count = remaining.size();
        for (std::size_t i = 0; i < count; ++i) {
            const auto prev = remaining[(i + count - 1) % count];
            const auto curr = remaining[i];
            const auto next = remaining[(i + 1) % count];
            const auto& a = polygon[prev];
            const auto& b = polygon[curr];
            const auto& c = polygon[next];
            if (cross(a, b, c) * winding <= 0.0F) continue; // Reflex or collinear.
            bool contains_other = false;
            for (const auto other : remaining) {
                if (other == prev || other == curr || other == next) continue;
                if (point_in_triangle(polygon[other], a, b, c)) {
                    contains_other = true;
                    break;
                }
            }
            if (contains_other) continue;
            triangles.push_back({prev, curr, next});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    for (std::size_t i = 1; i + 1 < remaining.size(); ++i)
        triangles.push_back({remaining[0], remaining[i], remaining[i + 1]});
    return triangles;
}

} // namespace rwsman
