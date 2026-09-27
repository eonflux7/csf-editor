// Mission overlays in the scene viewport: visibility and emphasis, the GPU marker
// pass (depth-aware x-ray, shapes, fading), and the ImGui decorations drawn on top
// (merged-marker badges, icons, labels, off-screen arrow, legend, minimap).
#include "geometry_preview.hpp"
#include "gl_api.hpp"

#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "rws/world_recovery.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstring>
#include <map>
#include <limits>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_set>

namespace rwsman {
namespace {

using Kind = GeometryPreview::MissionOverlayKind;
constexpr std::size_t kind_count = static_cast<std::size_t>(Kind::count);

constexpr std::array<const char*, kind_count> layer_names{
    "Actors", "Navigation points", "Navigation links", "Dummies",   "Cutscene cameras",
    "Areas",  "Lights",            "Effects",          "Actor CMO", "Actor physics"};
constexpr std::array<const char*, kind_count> layer_icons{
    ui::icons::LC_USER,  ui::icons::LC_MAP_PIN,   ui::icons::LC_ROUTE,    ui::icons::LC_DIAMOND,
    ui::icons::LC_CAMERA, ui::icons::LC_SQUARE,   ui::icons::LC_LIGHTBULB, ui::icons::LC_SPARKLES,
    ui::icons::LC_CUBOID, ui::icons::LC_PACKAGE};

// Marker shapes understood by the overlay fragment shader.
enum Shape : int { circle, diamond, square, triangle, ring, merged, halo, glow };
// Vertex modes understood by the overlay vertex shader.
enum Mode : int { point, segment, heading, arrow, world_ring, face };

constexpr std::array<Shape, kind_count> kind_shapes{circle, circle, circle, diamond, square,
                                                    square, ring,   triangle, square, square};
constexpr std::array<float, kind_count> kind_radius{5.5F, 3.5F, 3.0F, 4.5F, 5.0F,
                                                    4.0F, 5.0F, 5.0F, 4.0F, 4.0F};
constexpr std::array<float, kind_count> kind_heading{18.0F, 12.0F, 0.0F, 14.0F, 0.0F,
                                                     0.0F,  0.0F,  14.0F, 0.0F, 0.0F};

// Which member stands for a group of merged markers: actors first, nav points last.
constexpr std::array<int, kind_count> kind_rank{0, 5, 6, 3, 1, 6, 4, 2, 6, 6};

std::size_t index_of(const Kind kind) {
    return static_cast<std::size_t>(kind);
}

float distance_to_segment(const ImVec2 p, const ImVec2 a, const ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length_squared = dx * dx + dy * dy;
    float t = length_squared > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length_squared : 0.0F;
    t = std::clamp(t, 0.0F, 1.0F);
    const float x = a.x + dx * t - p.x, y = a.y + dy * t - p.y;
    return std::sqrt(x * x + y * y);
}

ImU32 scale_alpha(const ImU32 color, const float scale) {
    const auto alpha = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFU);
    const auto scaled = static_cast<ImU32>(std::clamp(alpha * scale, 0.0F, 255.0F));
    return (color & ~IM_COL32_A_MASK) | (scaled << IM_COL32_A_SHIFT);
}

ImU32 lighten(const ImU32 color, const float amount) {
    const auto channels = ui::unpack_rgba(color);
    const auto mix = [&](const std::uint8_t value) {
        return static_cast<int>(static_cast<float>(value) +
                                (255.0F - static_cast<float>(value)) * amount);
    };
    return ui::rgba_u32(mix(channels[0]), mix(channels[1]), mix(channels[2]), channels[3]);
}

constexpr const char* overlay_vertex_source = R"GLSL(#version 330 core
layout(location=0) in vec3 aA;
layout(location=1) in vec3 aB;
layout(location=2) in vec2 aCorner;
layout(location=3) in vec2 aSize;
layout(location=4) in vec4 aColor;
layout(location=5) in vec4 aParams;
uniform vec3 uCenter;
uniform float uYaw, uPitch, uDistance, uOrthographicScale;
uniform vec2 uPan;
uniform float uAspect, uTanHalfFov, uNear, uFar;
uniform bool uOrthographic;
uniform vec2 uViewport;
uniform vec2 uFade;
uniform float uFadeFloor;
out vec4 vColor;
out vec2 vLocal;
flat out float vMode;
flat out float vShape;
flat out float vRadius;
flat out float vFloor;
vec3 toView(vec3 w) {
    vec3 p=w-uCenter;
    float cy=cos(uYaw), sy=sin(uYaw), cp=cos(uPitch), sp=sin(uPitch);
    float rx=cy*p.x-sy*p.z, rz=sy*p.x+cy*p.z;
    return vec3(rx+uPan.x, cp*p.y-sp*rz+uPan.y, sp*p.y+cp*rz-uDistance);
}
vec4 toClip(vec3 v) {
    float f=1.0/uTanHalfFov;
    if (uOrthographic)
        return vec4(v.x/(uOrthographicScale*uAspect), v.y/uOrthographicScale,
                    (-2.0*v.z-uFar-uNear)/(uFar-uNear), 1.0);
    float z=((uFar+uNear)/(uNear-uFar))*v.z+(2.0*uFar*uNear)/(uNear-uFar);
    return vec4(v.x*f/uAspect, v.y*f, z, -v.z);
}
// World units covered by one pixel at view-space position v.
float pixelSize(vec3 v) {
    return uOrthographic ? 2.0*uOrthographicScale/uViewport.y
                         : 2.0*max(-v.z,uNear)*uTanHalfFov/uViewport.y;
}
// Pulls a marker toward the eye so the surface it stands on does not hide it.
vec3 towardCamera(vec3 v, float amount) {
    if (uOrthographic) return v+vec3(0.0,0.0,amount);
    float len=length(v);
    return len > amount*2.0 ? v-v/len*amount : v;
}
bool clipSegment(inout vec3 a, inout vec3 b) {
    if (uOrthographic) return true;
    float limit=-uNear*1.01;
    if (a.z > limit && b.z > limit) return false;
    if (a.z > limit) a=mix(a,b,(limit-a.z)/(b.z-a.z));
    if (b.z > limit) b=mix(b,a,(limit-b.z)/(a.z-b.z));
    return true;
}
void hide() { gl_Position=vec4(2.0,2.0,2.0,1.0); vColor.a=0.0; }
void main() {
    float mode=aParams.x;
    vMode=mode; vShape=aParams.y; vFloor=aParams.z; vRadius=aSize.x;
    vColor=aColor; vLocal=aCorner;
    vec3 va=toView(aA);
    float eye=length(va);
    if (mode < 0.5) {
        va=towardCamera(va, pixelSize(va)*aSize.x*1.5);
        vec4 c=toClip(va);
        float r=aSize.x+2.0;
        c.xy+=aCorner*r*2.0/uViewport*c.w;
        vLocal=aCorner*r;
        gl_Position=c;
    } else if (mode < 3.5) {
        vec3 vb=mode < 2.5 && mode > 1.5 ? toView(aA+aB*aSize.y*pixelSize(va)) : toView(aB);
        va=towardCamera(va, pixelSize(va)*2.0);
        vb=towardCamera(vb, pixelSize(vb)*2.0);
        if (!clipSegment(va,vb)) { hide(); return; }
        vec4 ca=toClip(va), cb=toClip(vb);
        vec2 sa=ca.xy/ca.w*uViewport*0.5, sb=cb.xy/cb.w*uViewport*0.5;
        vec2 d=sb-sa;
        float len=length(d);
        if (mode > 2.5) {
            if (len < 1e-3) { hide(); return; }
            vec2 u=d/len, n=vec2(-u.y,u.x);
            vec4 c=cb;
            c.xy+=(u*aCorner.x+n*aCorner.y)*2.0/uViewport*c.w;
            gl_Position=c;
            eye=length(vb);
        } else {
            vec2 n=len > 1e-4 ? vec2(-d.y,d.x)/len : vec2(0.0,1.0);
            float hw=aSize.x*0.5+1.0;
            vec4 c=aCorner.x < 0.5 ? ca : cb;
            c.xy+=n*aCorner.y*hw*2.0/uViewport*c.w;
            vLocal=vec2(aCorner.y*hw, aSize.x*0.5);
            gl_Position=c;
            eye=aCorner.x < 0.5 ? length(va) : length(vb);
        }
    } else if (mode < 4.5) {
        vec3 v=va;
        v.xy+=aCorner*aSize.x*1.08;
        vLocal=aCorner*1.08;
        gl_Position=toClip(v);
    } else {
        gl_Position=toClip(towardCamera(va, pixelSize(va)));
    }
    // Mirrors rwsman::marker_distance_fade, by distance from the eye.
    if (uFade.y > uFade.x && aParams.w < 0.5)
        vColor.a*=mix(1.0, uFadeFloor, clamp((eye-uFade.x)/(uFade.y-uFade.x), 0.0, 1.0));
})GLSL";

constexpr const char* overlay_fragment_source = R"GLSL(#version 330 core
in vec4 vColor;
in vec2 vLocal;
flat in float vMode;
flat in float vShape;
flat in float vRadius;
flat in float vFloor;
uniform float uAlphaScale;
uniform bool uOccludedPass;
uniform vec4 uOutline;
out vec4 FragColor;
float sdBox(vec2 p, vec2 b) {
    vec2 d=abs(p)-b;
    return length(max(d,0.0))+min(max(d.x,d.y),0.0);
}
float sdTriangle(vec2 p, float r) {
    const float k=1.7320508;
    p.y+=r*0.2;
    p.x=abs(p.x)-r;
    p.y=p.y+r/k;
    if (p.x+k*p.y > 0.0) p=vec2(p.x-k*p.y,-k*p.x-p.y)/2.0;
    p.x-=clamp(p.x,-2.0*r,0.0);
    return -length(p)*sign(p.y);
}
float sdShape(vec2 p, float r, float s) {
    if (s < 0.5) return length(p)-r;
    if (s < 1.5) return (abs(p.x)+abs(p.y))*0.70710678-r*0.9;
    if (s < 2.5) return sdBox(p, vec2(r*0.82));
    if (s < 3.5) return sdTriangle(p, r*1.1);
    if (s < 4.5) return abs(length(p)-r*0.75)-r*0.3;
    if (s < 5.5) return sdBox(p, vec2(r*0.7))-r*0.3;
    return abs(length(p)-r)-0.9;
}
void main() {
    vec4 color=vColor;
    if (vMode < 0.5 && vShape > 6.5) {
        float t=clamp(1.0-length(vLocal)/vRadius,0.0,1.0);  // a soft glow, no rim
        color.a*=t;
    } else if (vMode < 0.5) {
        float d=sdShape(vLocal, vRadius, vShape);
        float fill=clamp(0.5-d,0.0,1.0);
        float rim=clamp(0.5-(d-1.25),0.0,1.0);
        if (rim <= 0.0) discard;
        color.rgb=mix(uOutline.rgb,color.rgb,fill);
        color.a*=max(fill,rim*uOutline.a);
    } else if (vMode < 2.5) {
        color.a*=clamp(vLocal.y+0.5-abs(vLocal.x),0.0,1.0);
    } else if (vMode > 3.5 && vMode < 4.5) {
        float l=length(vLocal);
        float w=max(fwidth(l),1e-5);
        color.a*=clamp(1.25-abs(l-1.0)/w,0.0,1.0);
    }
    if (uOccludedPass) color.a*=max(uAlphaScale, vFloor);
    if (color.a < 0.004) discard;
    FragColor=color;
})GLSL";

} // namespace

// ---- State ------------------------------------------------------------------

void GeometryPreview::set_overlay_options(OverlayOptions options) {
    options.clamp();
    overlay_options_ = std::move(options);
}

void GeometryPreview::cycle_label_mode() noexcept {
    const auto next = (static_cast<int>(overlay_options_.labels) + 1) %
                      (static_cast<int>(OverlayOptions::Labels::all) + 1);
    overlay_options_.labels = static_cast<OverlayOptions::Labels>(next);
}

void GeometryPreview::toggle_floor_slice() {
    if (slice_.enabled) {
        slice_.enabled = false;
        return;
    }
    float y = center_.y + navigation_offset_.y;
    if (selected_mission_entry_)
        for (const auto& point : mission_points_)
            if (point.source_entry == *selected_mission_entry_) {
                y = point.position.y;
                break;
            }
    // A storey is a small fraction of a level; keep a sensible default for tiny scenes.
    if (slice_height_ <= 0.0F) slice_height_ = std::clamp(all_radius_ * 0.04F, 50.0F, 600.0F);
    slice_.low = y - slice_height_ * 0.3F;
    slice_.high = y + slice_height_ * 0.7F;
    slice_.enabled = true;
}

void GeometryPreview::rebuild_overlay_indexes() {
    entry_kinds_.clear();
    layer_entry_counts_.fill(0);
    for (auto& list : layer_sublayers_) list.clear();
    std::array<std::unordered_set<std::uint32_t>, kind_count> entries;
    std::array<std::map<std::string, std::unordered_set<std::uint32_t>>, kind_count> sublayers;
    overlay_bounds_valid_ = false;
    const auto include = [&](const rws::Vec3 p) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return;
        if (!overlay_bounds_valid_) {
            overlay_bounds_min_ = overlay_bounds_max_ = p;
            overlay_bounds_valid_ = true;
            return;
        }
        overlay_bounds_min_ = {std::min(overlay_bounds_min_.x, p.x), std::min(overlay_bounds_min_.y, p.y),
                               std::min(overlay_bounds_min_.z, p.z)};
        overlay_bounds_max_ = {std::max(overlay_bounds_max_.x, p.x), std::max(overlay_bounds_max_.y, p.y),
                               std::max(overlay_bounds_max_.z, p.z)};
    };
    const auto add = [&](const Kind kind, const std::uint32_t entry, const std::string& sublayer) {
        entry_kinds_.try_emplace(entry, kind);
        entries[index_of(kind)].insert(entry);
        if (!sublayer.empty()) sublayers[index_of(kind)][sublayer].insert(entry);
    };
    for (const auto& point : mission_points_) {
        add(point.kind, point.source_entry, point.sublayer);
        include(point.position);
    }
    for (const auto& line : mission_lines_) {
        add(line.kind, line.source_entry, line.sublayer);
        if (line.partner_entry) add(line.kind, *line.partner_entry, line.sublayer);
        include(line.first);
        include(line.second);
    }
    for (std::size_t i = 0; i < kind_count; ++i) {
        layer_entry_counts_[i] = entries[i].size();
        for (const auto& [name, members] : sublayers[i])
            layer_sublayers_[i].emplace_back(name, members.size());
    }
    point_occlusion_.assign(mission_points_.size(), -1);
    point_occlusion_generation_.assign(mission_points_.size(), 0);
    focus_source_entry_.reset();
    focus_entries_.clear();
    filter_matches_query_ = std::string(1, '\x01'); // Never equal to a typed query.
    hovered_mission_entry_.reset();
    hovered_cluster_.reset();
    overlay_markers_.clear();
    overlay_clusters_.clear();
    overlay_screen_lines_.clear();
    overlay_vertices_.clear();
    overlay_picker_entries_.clear();
}

bool GeometryPreview::overlay_layer_visible(const Kind kind, const std::string& sublayer) const {
    if (overlay_options_.layer_hidden(overlay_layer_keys[index_of(kind)])) return false;
    return sublayer.empty() || !hidden_sublayers_.contains({static_cast<std::uint8_t>(kind), sublayer});
}

bool GeometryPreview::overlay_entry_matches_filter(const std::uint32_t entry) const {
    return overlay_filter_[0] == '\0' || filter_matches_.contains(entry);
}

bool GeometryPreview::overlay_position_visible(const rws::Vec3 point) const {
    if (!rws::collision_point_visible(point, clips_)) return false;
    return !slice_.enabled || (point.y >= slice_.low && point.y <= slice_.high);
}

rwsman::MarkerFadeRange GeometryPreview::overlay_fade_range() const {
    // Perspective only: depth doesn't imply apparent distance in an orthographic view.
    if (projection_ != 0) return {};
    const auto center = view_point(all_center_);
    const float to_center = std::sqrt(center.x * center.x + center.y * center.y + center.z * center.z);
    return rwsman::marker_fade_range(to_center, all_radius_, overlay_options_.fade_distance);
}

float GeometryPreview::overlay_eye_distance(const rws::Vec3 point) const {
    const auto v = view_point(point);
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float GeometryPreview::overlay_occlusion_alpha(const std::uint32_t point_index) {
    if (const auto walls = overlay_point_walls(point_index))
        return rwsman::marker_wall_fade(*walls, overlay_options_.occluded_opacity);
    return overlay_point_occluded(point_index).value_or(false) ? overlay_options_.occluded_opacity : 1.0F;
}

GeometryPreview::Emphasis GeometryPreview::overlay_emphasis(const std::uint32_t entry) const {
    if (selected_mission_entry_ == entry || extra_selected_.contains(entry)) return Emphasis::selected;
    if (hovered_mission_entry_ == entry) return Emphasis::hovered;
    if (selected_mission_entry_ && focus_entries_.contains(entry)) return Emphasis::related;
    if (focus_mode_ && selected_mission_entry_) return Emphasis::dimmed;
    if (overlay_options_.dim_filtered && !overlay_entry_matches_filter(entry)) return Emphasis::dimmed;
    return Emphasis::normal;
}

rws::Vec3 GeometryPreview::view_point(const rws::Vec3 point) const {
    const auto [cy, sy, cp, sp] = view_rotation();
    const rws::Vec3 p{point.x - center_.x - navigation_offset_.x,
                      point.y - center_.y - navigation_offset_.y,
                      point.z - center_.z - navigation_offset_.z};
    const float rx = cy * p.x - sy * p.z, rz = sy * p.x + cy * p.z;
    const float view_scale = projection_ == 0 ? distance_ : orthographic_scale_;
    return {rx + pan_x_ * view_scale, cp * p.y - sp * rz - pan_y_ * view_scale,
            sp * p.y + cp * rz - distance_};
}

std::vector<rws::CollisionClipPlane> GeometryPreview::active_clip_planes() const {
    std::vector<rws::CollisionClipPlane> planes(clips_.begin(), clips_.end());
    if (slice_.enabled && slice_.clip_geometry && scene_mode_) {
        planes.push_back({true, 1, true, slice_.low});
        planes.push_back({true, 1, false, slice_.high});
    }
    return planes;
}

std::optional<int> GeometryPreview::overlay_point_walls(const std::uint32_t point_index) {
    if (point_index >= point_occlusion_.size()) return std::nullopt;
    // Without collision there is nothing to count; the GPU depth test still dims.
    if (!collision_document_ || collision_worlds_.empty() || (!show_collision_ && !show_visual_))
        return std::nullopt;
    auto& cached = point_occlusion_[point_index];
    const auto stale = [&] { return cached < 0 ? std::nullopt : std::optional<int>(cached); };
    if (point_occlusion_generation_[point_index] == occlusion_generation_) return stale();
    // Stale from here on: the frame loop keeps drawing until every count is current.
    occlusion_pending_ = true;
    if (!occlusion_settled_) return stale();
    const auto now = std::chrono::steady_clock::now();
    // A frame always makes progress with one count; further counts start only
    // while their typical cost still fits the slice, so one slow ray cannot push
    // the frame far past its budget. The camera is still here, so a few
    // milliseconds per frame cost no visible smoothness and finish sooner.
    if (occlusion_deadline_ == std::chrono::steady_clock::time_point{})
        occlusion_deadline_ = now + std::chrono::microseconds(3000);
    else if (now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::duration<double>(occlusion_count_seconds_)) >= occlusion_deadline_)
        return stale();
    point_occlusion_generation_[point_index] = occlusion_generation_;
    const auto position = mission_points_[point_index].position;
    const auto screen = project_point(position);
    const auto ray = screen ? viewport_ray(screen->x, screen->y) : std::nullopt;
    if (!ray) {
        cached = 0;
        return 0;
    }
    const rws::Vec3 offset{position.x - ray->origin.x, position.y - ray->origin.y,
                           position.z - ray->origin.z};
    const float along =
        offset.x * ray->direction.x + offset.y * ray->direction.y + offset.z * ray->direction.z;
    // The surface a marker stands on is not a wall in front of it.
    const float reach = along - std::max(along * 0.02F, 20.0F);
    const int walls = rws::count_collision_walls(collision_worlds_, collision_document_->bytes(), *ray, reach,
                                                 std::max(along * 0.001F, 2.0F), active_clip_planes());
    const double cost = std::chrono::duration<double>(std::chrono::steady_clock::now() - now).count();
    occlusion_count_seconds_ = occlusion_count_seconds_ == 0.0 ? cost : occlusion_count_seconds_ * 0.9 + cost * 0.1;
    cached = static_cast<std::int8_t>(std::min(walls, 127));
    return cached;
}

std::optional<bool> GeometryPreview::overlay_point_occluded(const std::uint32_t point_index) {
    if (point_index >= point_occlusion_.size()) return std::nullopt;
    if (!collision_document_ || collision_worlds_.empty() || (!show_collision_ && !show_visual_))
        return false;
    const auto walls = overlay_point_walls(point_index);
    return walls ? std::optional<bool>(*walls > 0) : std::nullopt;
}

// ---- Per-frame preparation --------------------------------------------------

void GeometryPreview::prepare_overlays(const ImVec2 origin, const ImVec2 size) {
    overlay_markers_.clear();
    overlay_clusters_.clear();
    overlay_screen_lines_.clear();
    overlay_vertices_.clear();
    occlusion_pending_ = false;
    visible_layer_counts_.fill(0);
    offscreen_indicator_rect_.reset();
    if (!scene_mode_ || !has_mission_overlays()) {
        hovered_mission_entry_.reset();
        hovered_cluster_.reset();
        return;
    }
    const auto& io = ImGui::GetIO();
    const float scale = ui::ui_scale();
    show_all_labels_ = canvas_hovered_ && !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_L);

    // Filter matches are recomputed only when the query changes.
    const std::string_view query(overlay_filter_.data());
    if (query != filter_matches_query_) {
        filter_matches_query_ = query;
        filter_matches_.clear();
        if (!query.empty()) {
            const auto test = [&](const Kind kind, const std::uint32_t entry,
                                  const std::string& sublayer, const std::string& label) {
                if (overlay_filter_matches(query, {overlay_layer_keys[index_of(kind)],
                                                   layer_names[index_of(kind)], sublayer, label, entry}))
                    filter_matches_.insert(entry);
            };
            for (const auto& point : mission_points_)
                test(point.kind, point.source_entry, point.sublayer, point.label);
            for (const auto& line : mission_lines_) {
                test(line.kind, line.source_entry, line.sublayer, {});
                if (line.partner_entry) test(line.kind, *line.partner_entry, line.sublayer, {});
            }
        }
    }
    // Focus set: the selection, its direct relations, and the far ends of its links.
    if (focus_source_entry_ != selected_mission_entry_) {
        focus_source_entry_ = selected_mission_entry_;
        focus_entries_.clear();
        if (selected_mission_entry_) {
            const auto add_neighbors = [&](const std::uint32_t entry, const bool follow_links,
                                           auto&& self) -> void {
                const auto found = mission_relations_.find(entry);
                if (found == mission_relations_.end()) return;
                for (const auto other : found->second) {
                    if (!focus_entries_.insert(other).second) continue;
                    const auto kind = entry_kinds_.find(other);
                    if (follow_links && kind != entry_kinds_.end() &&
                        kind->second == Kind::navigation_connection)
                        self(other, false, self);
                }
            };
            focus_entries_.insert(*selected_mission_entry_);
            add_neighbors(*selected_mission_entry_, true, add_neighbors);
        }
    }
    // Wall counts stay valid while the camera is still. Counting is too slow to
    // keep up with a moving camera, so markers keep their last counts until it
    // settles, then refresh within a small time slice per frame.
    const std::array<float, 12> camera_key{yaw_, pitch_, distance_, orthographic_scale_, pan_x_,
                                           pan_y_, center_.x + navigation_offset_.x,
                                           center_.y + navigation_offset_.y,
                                           center_.z + navigation_offset_.z,
                                           static_cast<float>(projection_), size.x, size.y};
    const auto now = std::chrono::steady_clock::now();
    if (camera_key != occlusion_camera_key_) {
        occlusion_camera_key_ = camera_key;
        occlusion_camera_changed_ = now;
        ++occlusion_generation_;
    }
    occlusion_settled_ = now - occlusion_camera_changed_ >= std::chrono::milliseconds(150);
    occlusion_deadline_ = {};

    const auto hide_filtered = !overlay_options_.dim_filtered && overlay_filter_[0] != '\0';
    const auto entry_shown = [&](const std::uint32_t entry) {
        if (isolate_selected_actor_ && selected_mission_entry_ && selected_mission_entry_ != entry)
            return false;
        if (!mission_entry_visible(entry)) return false;
        return !hide_filtered || overlay_entry_matches_filter(entry);
    };
    const bool build_minimap = overlay_options_.show_minimap && overlay_bounds_valid_;
    if (build_minimap) minimap_density_.assign(minimap_cells * minimap_cells, 0);
    const float span_x = std::max(overlay_bounds_max_.x - overlay_bounds_min_.x, 1.0F);
    const float span_z = std::max(overlay_bounds_max_.z - overlay_bounds_min_.z, 1.0F);
    const ImVec2 lo{origin.x - 24.0F, origin.y - 24.0F}, hi{origin.x + size.x + 24.0F,
                                                            origin.y + size.y + 24.0F};
    auto& counted = overlay_counted_;
    counted.clear();
    const auto count_visible = [&](const Kind kind, const std::uint32_t entry) {
        if (counted.insert((static_cast<std::uint64_t>(index_of(kind)) << 32) | entry).second)
            ++visible_layer_counts_[index_of(kind)];
    };

    // Markers on screen.
    for (std::uint32_t i = 0; i < mission_points_.size(); ++i) {
        const auto& point = mission_points_[i];
        if (!overlay_layer_visible(point.kind, point.sublayer) || !entry_shown(point.source_entry) ||
            !overlay_position_visible(point.position))
            continue;
        if (build_minimap) {
            const int cx = std::clamp(static_cast<int>((point.position.x - overlay_bounds_min_.x) /
                                                       span_x * minimap_cells),
                                      0, minimap_cells - 1);
            const int cz = std::clamp(static_cast<int>((overlay_bounds_max_.z - point.position.z) /
                                                       span_z * minimap_cells),
                                      0, minimap_cells - 1);
            auto& cell = minimap_density_[static_cast<std::size_t>(cz * minimap_cells + cx)];
            if (cell < std::numeric_limits<std::uint16_t>::max()) ++cell;
        }
        const auto screen = project_point(point.position);
        if (!screen || screen->x < lo.x || screen->y < lo.y || screen->x > hi.x || screen->y > hi.y)
            continue;
        // The selection, its relations in focus mode, and filter matches never merge.
        const bool pinned = selected_mission_entry_ == point.source_entry ||
                            (focus_mode_ && selected_mission_entry_ && focus_entries_.contains(point.source_entry)) ||
                            (overlay_filter_[0] != '\0' && filter_matches_.contains(point.source_entry));
        overlay_markers_.push_back({i, *screen, -view_point(point.position).z, pinned});
        count_visible(point.kind, point.source_entry);
    }
    // Merge markers that crowd the same pixels. The selection stays on its own.
    {
        std::vector<ScreenMarker> screen;
        screen.reserve(overlay_markers_.size());
        for (const auto& marker : overlay_markers_)
            screen.push_back({marker.screen.x, marker.screen.y, marker.pinned});
        for (auto& cluster : cluster_markers(screen, overlay_options_.merge_pixels * scale)) {
            const auto best = std::ranges::min_element(cluster.members, {}, [&](const std::uint32_t index) {
                return kind_rank[index_of(mission_points_[overlay_markers_[index].point].kind)];
            });
            const auto representative = *best;
            overlay_clusters_.push_back({{cluster.x, cluster.y}, std::move(cluster.members), representative});
        }
    }
    // Lines on screen (for hover, picking, and counts).
    for (std::uint32_t i = 0; i < mission_lines_.size(); ++i) {
        const auto& line = mission_lines_[i];
        if (!overlay_layer_visible(line.kind, line.sublayer) || !entry_shown(line.source_entry) ||
            !overlay_position_visible(line.first) || !overlay_position_visible(line.second))
            continue;
        const auto a = project_point(line.first), b = project_point(line.second);
        if (!a || !b) continue;
        if (std::max(a->x, b->x) < lo.x || std::min(a->x, b->x) > hi.x ||
            std::max(a->y, b->y) < lo.y || std::min(a->y, b->y) > hi.y)
            continue;
        overlay_screen_lines_.push_back({i, *a, *b});
        count_visible(line.kind, line.source_entry);
    }

    // Hover: a merged marker, else the nearest marker or line.
    hovered_mission_entry_.reset();
    hovered_cluster_.reset();
    const bool pointer_idle = !io.MouseDown[0] && !io.MouseDown[1] && !io.MouseDown[2];
    if (canvas_hovered_ && pointer_idle) {
        hovered_cluster_ = overlay_cluster_at(io.MousePos);
        if (!hovered_cluster_) {
            const auto candidates = overlay_candidates(io.MousePos);
            if (!candidates.empty()) hovered_mission_entry_ = candidates.front();
        }
    }

    // ---- GPU vertices: faces, then lines, then markers, emphasized markers last.
    const auto push = [&](const rws::Vec3 a, const rws::Vec3 b, const float cx, const float cy,
                          const float size0, const float size1, const ImU32 color, const Mode mode,
                          const float shape, const float floor, const bool no_fade) {
        overlay_vertices_.push_back({{a.x, a.y, a.z},
                                     {b.x, b.y, b.z},
                                     {cx, cy},
                                     {size0, size1},
                                     color,
                                     {static_cast<float>(mode), shape, floor, no_fade ? 1.0F : 0.0F}});
    };
    const auto push_quad = [&](const rws::Vec3 a, const rws::Vec3 b, const float size0,
                               const float size1, const ImU32 color, const Mode mode,
                               const float shape, const float floor, const bool no_fade) {
        constexpr std::array<std::array<float, 2>, 6> corners{
            {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}};
        for (const auto& c : corners) push(a, b, c[0], c[1], size0, size1, color, mode, shape, floor, no_fade);
    };
    const auto push_segment = [&](const rws::Vec3 a, const rws::Vec3 b, const float thickness,
                                  const float length, const ImU32 color, const Mode mode,
                                  const float floor, const bool no_fade) {
        constexpr std::array<std::array<float, 2>, 6> corners{
            {{0, -1}, {1, -1}, {1, 1}, {0, -1}, {1, 1}, {0, 1}}};
        for (const auto& c : corners)
            push(a, b, c[0], c[1], thickness * scale, length * scale, color, mode, 0, floor, no_fade);
    };
    const auto push_arrow = [&](const rws::Vec3 a, const rws::Vec3 b, const float size0,
                                const ImU32 color, const float floor, const bool no_fade) {
        const float s = size0 * scale, back = 5.0F * scale;
        push(a, b, -back, 0, 0, 0, color, Mode::arrow, 0, floor, no_fade);
        push(a, b, -back - s, s * 0.55F, 0, 0, color, Mode::arrow, 0, floor, no_fade);
        push(a, b, -back - s, -s * 0.55F, 0, 0, color, Mode::arrow, 0, floor, no_fade);
    };
    const auto floor_for = [](const Emphasis emphasis) {
        return emphasis == Emphasis::selected  ? 0.75F
               : emphasis == Emphasis::hovered ? 0.6F
               : emphasis == Emphasis::related ? 0.45F
                                               : 0.0F;
    };
    const auto emphasized = [](const Emphasis emphasis) {
        return emphasis == Emphasis::selected || emphasis == Emphasis::hovered ||
               emphasis == Emphasis::related;
    };
    const auto detail_shown = [&](const OverlayOptions::Detail mode, const Emphasis emphasis) {
        return mode == OverlayOptions::Detail::all ||
               (mode == OverlayOptions::Detail::selected && emphasized(emphasis));
    };
    const auto styled = [&](const ImU32 color, const Emphasis emphasis) {
        switch (emphasis) {
        case Emphasis::dimmed: return scale_alpha(color, 0.22F);
        case Emphasis::hovered: return lighten(color, 0.45F);
        case Emphasis::selected: return lighten(color, 0.25F);
        default: return color;
        }
    };

    for (const auto& face : mission_faces_) {
        if (!overlay_layer_visible(face.kind, {}) || !entry_shown(face.source_entry)) continue;
        if (!overlay_position_visible(face.a) || !overlay_position_visible(face.b) ||
            !overlay_position_visible(face.c))
            continue;
        // Zone fills stay faint until the zone is selected or hovered, so
        // overlapping zones do not tint the whole view.
        const auto emphasis = overlay_emphasis(face.source_entry);
        const float boost = emphasis == Emphasis::selected  ? 3.0F
                            : emphasis == Emphasis::hovered ? 2.2F
                            : emphasis == Emphasis::related ? 1.2F
                            : emphasis == Emphasis::dimmed  ? 0.15F
                                                            : 0.45F;
        const auto color = scale_alpha(face.color, boost);
        for (const auto& p : {face.a, face.b, face.c})
            push(p, p, 0, 0, 0, 0, color, Mode::face, 0, floor_for(emphasis) * 0.3F, emphasized(emphasis));
    }
    // Lines are sorted so emphasized ones draw over the rest.
    std::vector<std::pair<Emphasis, std::uint32_t>> line_order;
    line_order.reserve(overlay_screen_lines_.size());
    for (std::uint32_t i = 0; i < mission_lines_.size(); ++i) {
        const auto& line = mission_lines_[i];
        if (!overlay_layer_visible(line.kind, line.sublayer) || !entry_shown(line.source_entry) ||
            !overlay_position_visible(line.first) || !overlay_position_visible(line.second))
            continue;
        auto emphasis = overlay_emphasis(line.source_entry);
        if (line.partner_entry)
            emphasis = std::max(emphasis, overlay_emphasis(*line.partner_entry));
        if (line.detail && !detail_shown(overlay_options_.details, emphasis)) continue;
        line_order.emplace_back(emphasis, i);
    }
    std::ranges::stable_sort(line_order, {}, &std::pair<Emphasis, std::uint32_t>::first);
    for (const auto& [emphasis, index] : line_order) {
        const auto& line = mission_lines_[index];
        const float thickness = emphasis == Emphasis::selected  ? 3.0F
                                : emphasis == Emphasis::hovered ? 2.6F
                                : emphasis == Emphasis::related ? 2.2F
                                : emphasis == Emphasis::dimmed  ? 1.0F
                                                                : 1.5F;
        const auto color = emphasis == Emphasis::related && line.kind == Kind::navigation_connection
                               ? ui::viewport_color(ui::Viewport::related)
                               : styled(line.color, emphasis);
        const float floor = floor_for(emphasis);
        push_segment(line.first, line.second, thickness, 0, color, Mode::segment, floor, emphasized(emphasis));
        if (line.directed && emphasis != Emphasis::dimmed)
            push_arrow(line.first, line.second, 7.0F, color, floor, emphasized(emphasis));
    }
    // Headings and light ranges belong to single markers.
    std::vector<std::pair<Emphasis, std::uint32_t>> marker_order;
    for (std::uint32_t c = 0; c < overlay_clusters_.size(); ++c) {
        const auto& cluster = overlay_clusters_[c];
        if (cluster.markers.size() != 1) continue;
        const auto& marker = overlay_markers_[cluster.markers.front()];
        const auto& point = mission_points_[marker.point];
        const auto emphasis = overlay_emphasis(point.source_entry);
        marker_order.emplace_back(emphasis, marker.point);
        const float floor = floor_for(emphasis);
        const auto color = styled(point.color, emphasis);
        if (point.heading && kind_heading[index_of(point.kind)] > 0.0F &&
            detail_shown(overlay_options_.headings, emphasis))
            push_segment(point.position, *point.heading, emphasis == Emphasis::selected ? 2.2F : 1.5F,
                         kind_heading[index_of(point.kind)] * (emphasis == Emphasis::selected ? 1.6F : 1.0F),
                         color, Mode::heading, floor, emphasized(emphasis));
        if (point.radius > 0.0F && detail_shown(overlay_options_.details, emphasis))
            push_quad(point.position, point.position, point.radius, 0, scale_alpha(color, 0.6F),
                      Mode::world_ring, 0, floor * 0.5F, emphasized(emphasis));
    }
    // Merged markers.
    for (const auto& cluster : overlay_clusters_) {
        if (cluster.markers.size() < 2) continue;
        const auto& first = mission_points_[overlay_markers_[cluster.representative].point];
        const auto kind = index_of(first.kind);
        const float radius =
            (kind_radius[kind] + 1.0F + 0.6F * std::min(std::log2(static_cast<float>(cluster.markers.size())), 4.0F)) *
            scale;
        bool dimmed = true;
        for (const auto index : cluster.markers)
            dimmed = dimmed && overlay_emphasis(mission_points_[overlay_markers_[index].point].source_entry) ==
                                   Emphasis::dimmed;
        float alpha = dimmed ? 0.3F : 1.0F;
        float floor = 0.0F;
        if (const auto walls = overlay_point_walls(overlay_markers_[cluster.representative].point)) {
            alpha *= rwsman::marker_wall_fade(*walls, overlay_options_.occluded_opacity);
            floor = 1.0F; // Walls counted; the depth test must not dim again.
        }
        // The representative's shape, ringed to show that more markers share the spot.
        push_quad(first.position, first.position, radius, 0, scale_alpha(first.color, alpha), Mode::point,
                  static_cast<float>(kind_shapes[kind]), floor, false);
        push_quad(first.position, first.position, radius + 3.0F * scale, 0,
                  scale_alpha(ui::viewport_color(ui::Viewport::cluster_text), 0.7F * alpha), Mode::point,
                  static_cast<float>(Shape::halo), floor, false);
    }
    std::ranges::stable_sort(marker_order, {}, &std::pair<Emphasis, std::uint32_t>::first);
    // Contact disks (V7): a shadow on the ground under each actor, so a marker
    // reads as standing somewhere; drawn before every marker. On the ground
    // plane, but as wide as the marker on screen at any distance.
    for (const auto& [emphasis, index] : marker_order) {
        const auto& point = mission_points_[index];
        if (point.kind != Kind::actor || emphasis == Emphasis::dimmed) continue;
        constexpr int segments = 20;
        // Centimetres per pixel here: a metre either way on the ground, projected.
        const auto at = project_point(point.position);
        const auto east = project_point({point.position.x + 100.0F, point.position.y, point.position.z});
        const auto north = project_point({point.position.x, point.position.y, point.position.z + 100.0F});
        if (!at || !east || !north) continue;
        const float span = std::max(std::hypot(east->x - at->x, east->y - at->y), std::hypot(north->x - at->x, north->y - at->y));
        if (span < 0.5F) continue;
        const float pixel = 100.0F / span;
        const float disk_radius = (kind_radius[index_of(point.kind)] + 16.0F) * scale * pixel;
        const rws::Vec3 centre{point.position.x, point.position.y + 2.0F * pixel, point.position.z};
        const auto shadow = ui::viewport_color(ui::Viewport::marker_outline, 0.6F);
        for (int k = 0; k < segments; ++k) {
            const float a0 = 6.2831853F * static_cast<float>(k) / segments;
            const float a1 = 6.2831853F * static_cast<float>(k + 1) / segments;
            for (const auto& p : {centre,
                                  rws::Vec3{centre.x + disk_radius * std::cos(a0), centre.y, centre.z + disk_radius * std::sin(a0)},
                                  rws::Vec3{centre.x + disk_radius * std::cos(a1), centre.y, centre.z + disk_radius * std::sin(a1)}})
                push(p, p, 0, 0, 0, 0, shadow, Mode::face, 0, 0.8F, false);  // ground may rise past the feet
        }
    }
    for (const auto& [emphasis, index] : marker_order) {
        const auto& point = mission_points_[index];
        const auto kind = index_of(point.kind);
        const float grow = emphasis == Emphasis::selected ? 3.0F : emphasis == Emphasis::hovered ? 2.0F : 0.0F;
        const float radius = (kind_radius[kind] + grow) * scale;
        // Each wall in front dims the marker further; emphasized ones keep their floor.
        float alpha = 1.0F, floor = floor_for(emphasis);
        if (const auto walls = overlay_point_walls(index)) {
            alpha = std::max(rwsman::marker_wall_fade(*walls, overlay_options_.occluded_opacity), floor);
            floor = 1.0F;
        }
        // The selection's soft glow (V7), behind the marker.
        if (emphasis == Emphasis::selected)
            push_quad(point.position, point.position, radius + 18.0F * scale, 0,
                      ui::viewport_color(ui::Viewport::selection, 0.6F * alpha), Mode::point,
                      static_cast<float>(Shape::glow), floor, true);
        push_quad(point.position, point.position, radius, 0, scale_alpha(styled(point.color, emphasis), alpha),
                  Mode::point, static_cast<float>(kind_shapes[kind]), floor, emphasized(emphasis));
        if (emphasis == Emphasis::selected || emphasis == Emphasis::hovered || emphasis == Emphasis::related) {
            const auto ring = emphasis == Emphasis::selected ? ui::viewport_color(ui::Viewport::selection)
                              : emphasis == Emphasis::hovered ? ui::viewport_color(ui::Viewport::hover)
                                                              : ui::viewport_color(ui::Viewport::related);
            push_quad(point.position, point.position, radius + (emphasis == Emphasis::related ? 3.0F : 4.5F) * scale,
                      0, scale_alpha(ring, alpha), Mode::point, static_cast<float>(Shape::halo), floor, true);
            // A thicker outline for the selection: a second ring just outside.
            if (emphasis == Emphasis::selected)
                push_quad(point.position, point.position, radius + 6.0F * scale, 0, scale_alpha(ring, alpha),
                          Mode::point, static_cast<float>(Shape::halo), floor, true);
        }
    }
}

std::optional<std::size_t> GeometryPreview::overlay_cluster_at(const ImVec2 mouse) const {
    const float scale = ui::ui_scale();
    std::optional<std::size_t> nearest;
    float best = 8.0F * scale * 8.0F * scale;
    for (std::size_t i = 0; i < overlay_clusters_.size(); ++i) {
        const auto& cluster = overlay_clusters_[i];
        if (cluster.markers.size() < 2) continue;
        const float dx = cluster.screen.x - mouse.x, dy = cluster.screen.y - mouse.y;
        if (dx * dx + dy * dy < best) {
            best = dx * dx + dy * dy;
            nearest = i;
        }
    }
    // A single marker nearer than the merged one wins.
    const auto candidates = overlay_candidates(mouse);
    if (nearest && !candidates.empty())
        for (const auto& marker : overlay_markers_)
            if (mission_points_[marker.point].source_entry == candidates.front()) {
                const float dx = marker.screen.x - mouse.x, dy = marker.screen.y - mouse.y;
                if (dx * dx + dy * dy < best) nearest.reset();
                break;
            }
    return nearest;
}

std::vector<std::uint32_t> GeometryPreview::overlay_candidates(const ImVec2 mouse) const {
    const float scale = ui::ui_scale();
    std::vector<std::pair<float, std::uint32_t>> hits;
    const float point_radius = 8.0F * scale, line_radius = 5.0F * scale;
    for (const auto& cluster : overlay_clusters_) {
        if (cluster.markers.size() != 1) continue;
        const auto& marker = overlay_markers_[cluster.markers.front()];
        const float dx = marker.screen.x - mouse.x, dy = marker.screen.y - mouse.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance < point_radius) hits.emplace_back(distance, mission_points_[marker.point].source_entry);
    }
    for (const auto& projected : overlay_screen_lines_) {
        const float distance = distance_to_segment(mouse, projected.a, projected.b);
        if (distance >= line_radius) continue;
        const auto& line = mission_lines_[projected.line];
        // Lines rank behind markers at equal distance; faces are not pickable.
        hits.emplace_back(distance + 3.0F * scale, line.source_entry);
        if (line.partner_entry) hits.emplace_back(distance + 3.5F * scale, *line.partner_entry);
    }
    std::ranges::stable_sort(hits, {}, &std::pair<float, std::uint32_t>::first);
    std::vector<std::uint32_t> result;
    for (const auto& [distance, entry] : hits)
        if (!locked_mission_entries_.contains(entry) && std::ranges::find(result, entry) == result.end())
            result.push_back(entry);
    return result;
}

bool GeometryPreview::overlay_click(const ImVec2 mouse, const bool cycle) {
    if (offscreen_indicator_rect_) {
        const auto& r = *offscreen_indicator_rect_;
        if (mouse.x >= r.x && mouse.x <= r.z && mouse.y >= r.y && mouse.y <= r.w) {
            frame_selection(std::nullopt);
            return true;
        }
    }
    // Test cluster proximity directly rather than trusting hovered_cluster_: that field is only
    // refreshed while the pointer is idle, so it is stale on the frame a click is released.
    if (const auto clicked_cluster = overlay_cluster_at(mouse)) {
        overlay_picker_entries_.clear();
        for (const auto index : overlay_clusters_[*clicked_cluster].markers) {
            if (index >= overlay_markers_.size()) continue;
            const auto entry = mission_points_[overlay_markers_[index].point].source_entry;
            if (std::ranges::find(overlay_picker_entries_, entry) == overlay_picker_entries_.end())
                overlay_picker_entries_.push_back(entry);
        }
        open_overlay_picker_ = !overlay_picker_entries_.empty();
        return open_overlay_picker_;
    }
    const auto candidates = overlay_candidates(mouse);
    if (candidates.empty()) {
        last_overlay_click_ = {-1e9F, -1e9F};
        return false;
    }
    // Clicking the same spot again steps to the next marker under the pointer.
    const float dx = mouse.x - last_overlay_click_.x, dy = mouse.y - last_overlay_click_.y;
    overlay_click_cycle_ = cycle && dx * dx + dy * dy < 16.0F ? overlay_click_cycle_ + 1 : 0;
    last_overlay_click_ = mouse;
    selected_mission_entry_ = candidates[overlay_click_cycle_ % candidates.size()];
    return true;
}

// ---- GPU pass ---------------------------------------------------------------

bool GeometryPreview::create_overlay_program() {
    if (overlay_program_failed_) return false;
    auto& gl = gl_api();
    if (!gl.create_shader) {
        overlay_program_failed_ = true;
        return false;
    }
    const auto compile = [&](const GLenum type, const char* source) -> GLuint {
        const GLuint shader = gl.create_shader(type);
        gl.shader_source(shader, 1, &source, nullptr);
        gl.compile_shader(shader);
        GLint okay{};
        gl.get_shader_iv(shader, gl_compile_status, &okay);
        if (!okay) {
            std::array<char, 1024> log{};
            gl.get_shader_log(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            std::fprintf(stderr, "csf-editor: overlay shader compile failed: %s\n", log.data());
            gl.delete_shader(shader);
            return 0;
        }
        return shader;
    };
    const GLuint vertex = compile(gl_vertex_shader, overlay_vertex_source);
    const GLuint fragment = vertex ? compile(gl_fragment_shader, overlay_fragment_source) : 0;
    if (!vertex || !fragment) {
        if (vertex) gl.delete_shader(vertex);
        overlay_program_failed_ = true;
        return false;
    }
    overlay_program_ = gl.create_program();
    gl.attach_shader(overlay_program_, vertex);
    gl.attach_shader(overlay_program_, fragment);
    gl.link_program(overlay_program_);
    gl.delete_shader(vertex);
    gl.delete_shader(fragment);
    GLint linked{};
    gl.get_program_iv(overlay_program_, gl_link_status, &linked);
    if (!linked) {
        std::array<char, 1024> log{};
        gl.get_program_log(overlay_program_, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::fprintf(stderr, "csf-editor: overlay shader link failed: %s\n", log.data());
        destroy_overlay_program();
        overlay_program_failed_ = true;
        return false;
    }
    gl.gen_vertex_arrays(1, &overlay_vertex_array_);
    gl.gen_buffers(1, &overlay_vertex_buffer_);
    gl.bind_vertex_array(overlay_vertex_array_);
    gl.bind_buffer(gl_array_buffer, overlay_vertex_buffer_);
    const auto attribute = [&](const GLuint location, const GLint components, const GLenum type,
                               const GLboolean normalized, const std::size_t offset) {
        gl.enable_vertex_attrib_array(location);
        gl.vertex_attrib_pointer(location, components, type, normalized, sizeof(OverlayVertex),
                                 reinterpret_cast<void*>(offset));
    };
    attribute(0, 3, GL_FLOAT, GL_FALSE, offsetof(OverlayVertex, a));
    attribute(1, 3, GL_FLOAT, GL_FALSE, offsetof(OverlayVertex, b));
    attribute(2, 2, GL_FLOAT, GL_FALSE, offsetof(OverlayVertex, corner));
    attribute(3, 2, GL_FLOAT, GL_FALSE, offsetof(OverlayVertex, size));
    attribute(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, offsetof(OverlayVertex, color));
    attribute(5, 4, GL_FLOAT, GL_FALSE, offsetof(OverlayVertex, params));
    gl.bind_vertex_array(0);
    overlay_buffer_capacity_ = 0;
    return true;
}

void GeometryPreview::destroy_overlay_program() {
    auto& gl = gl_api();
    if (overlay_vertex_buffer_ && gl.delete_buffers) gl.delete_buffers(1, &overlay_vertex_buffer_);
    if (overlay_vertex_array_ && gl.delete_vertex_arrays)
        gl.delete_vertex_arrays(1, &overlay_vertex_array_);
    if (overlay_program_ && gl.delete_program) gl.delete_program(overlay_program_);
    overlay_vertex_buffer_ = overlay_vertex_array_ = overlay_program_ = 0;
    overlay_buffer_capacity_ = 0;
}

void GeometryPreview::render_overlays_gpu(const int viewport_width, const int viewport_height) {
    if (!scene_mode_ || overlay_vertices_.empty()) return;
    if (!overlay_program_ && !create_overlay_program()) return;
    auto& gl = gl_api();
    gl.use_program(overlay_program_);
    gl.bind_vertex_array(overlay_vertex_array_);
    gl.bind_buffer(gl_array_buffer, overlay_vertex_buffer_);
    const auto bytes = overlay_vertices_.size() * sizeof(OverlayVertex);
    if (bytes > overlay_buffer_capacity_) {
        overlay_buffer_capacity_ = bytes + bytes / 2;
        gl.buffer_data(gl_array_buffer, static_cast<GlSizePtr>(overlay_buffer_capacity_), nullptr,
                       gl_stream_draw);
    }
    gl.buffer_sub_data(gl_array_buffer, 0, static_cast<GlSizePtr>(bytes), overlay_vertices_.data());

    const auto location = [&](const char* name) { return gl.get_uniform_location(overlay_program_, name); };
    const float view_scale = projection_ == 0 ? distance_ : orthographic_scale_;
    const float near_plane = std::max(distance_ * 0.001F, std::max(radius_ * 0.000001F, 0.001F));
    const float navigation_distance = std::sqrt(navigation_offset_.x * navigation_offset_.x +
                                                navigation_offset_.y * navigation_offset_.y +
                                                navigation_offset_.z * navigation_offset_.z);
    float far_plane = std::max(distance_ + radius_ * 3.0F + navigation_distance, near_plane + 1.0F);
    float clip_near = near_plane;
    if (projection_ != 0) { // Linear depth spanning the scene, as in the scene shader.
        const float reach = std::max(all_radius_, radius_) * 3.0F + navigation_distance;
        clip_near = distance_ - reach;
        far_plane = distance_ + reach;
    }
    gl.uniform_3f(location("uCenter"), center_.x + navigation_offset_.x, center_.y + navigation_offset_.y,
                  center_.z + navigation_offset_.z);
    gl.uniform_1f(location("uYaw"), projection_ == 3 ? 1.57079632679F : (projection_ == 0 ? yaw_ : 0.0F));
    gl.uniform_1f(location("uPitch"), projection_ == 1 ? -1.57079632679F : (projection_ == 0 ? pitch_ : 0.0F));
    gl.uniform_1f(location("uDistance"), distance_);
    gl.uniform_1f(location("uOrthographicScale"), orthographic_scale_);
    gl.uniform_2f(location("uPan"), pan_x_ * view_scale, -pan_y_ * view_scale);
    gl.uniform_1f(location("uAspect"), static_cast<float>(viewport_width) / static_cast<float>(viewport_height));
    gl.uniform_1f(location("uTanHalfFov"), std::tan(25.0F * 3.14159265358979323846F / 180.0F));
    gl.uniform_1f(location("uNear"), clip_near);
    gl.uniform_1f(location("uFar"), far_plane);
    gl.uniform_1i(location("uOrthographic"), projection_ != 0);
    // Pixel sizes are in ImGui points; the framebuffer may be scaled.
    gl.uniform_2f(location("uViewport"), canvas_width_, canvas_height_);
    const auto fade = overlay_fade_range();
    gl.uniform_2f(location("uFade"), fade.near_distance, fade.far_distance);
    gl.uniform_1f(location("uFadeFloor"), rwsman::marker_fade_floor);
    const auto outline = ui::unpack_rgba(ui::viewport_color(ui::Viewport::marker_outline));
    gl.uniform_4f(location("uOutline"), outline[0] / 255.0F, outline[1] / 255.0F, outline[2] / 255.0F,
                  outline[3] / 255.0F);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    const auto count = static_cast<GLsizei>(overlay_vertices_.size());
    // Behind geometry: faded. In front: full strength. Each fragment passes exactly one test.
    glDepthFunc(GL_GREATER);
    gl.uniform_1i(location("uOccludedPass"), 1);
    gl.uniform_1f(location("uAlphaScale"), overlay_options_.occluded_opacity);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDepthFunc(GL_LEQUAL);
    gl.uniform_1i(location("uOccludedPass"), 0);
    gl.uniform_1f(location("uAlphaScale"), 1.0F);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_BLEND);
    gl.bind_vertex_array(0);
}

// ---- Decorations ------------------------------------------------------------

void GeometryPreview::draw_overlay_decorations(ImDrawList* draw_list, const ImVec2 origin, const ImVec2 size) {
    if (!scene_mode_ || !has_mission_overlays()) return;
    const float scale = ui::ui_scale();
    const auto& io = ImGui::GetIO();
    const float font_size = ImGui::GetFontSize();
    auto* font = ImGui::GetFont();
    // Icons and badges dim with depth exactly like the GPU markers under them.
    const auto fade_range = overlay_fade_range();
    const auto distance_fade = [&](const std::uint32_t point) {
        return rwsman::marker_distance_fade(fade_range, overlay_eye_distance(mission_points_[point].position));
    };

    // Count badges for merged markers, largest first, skipped where they would overlap.
    std::vector<ScreenRect> badges;
    {
        std::vector<LabelRequest> requests;
        std::vector<std::string> texts;
        std::vector<float> alphas;
        const float small = font_size * 0.78F;
        for (const auto& cluster : overlay_clusters_) {
            if (cluster.markers.size() < 3) continue; // A pair reads as one merged marker.
            if (std::ranges::all_of(cluster.markers, [&](const std::uint32_t index) {
                    return overlay_emphasis(mission_points_[overlay_markers_[index].point].source_entry) ==
                           Emphasis::dimmed;
                }))
                continue;
            // Groups behind geometry get badges as faint as their markers.
            const auto point = overlay_markers_[cluster.representative].point;
            const float alpha = overlay_occlusion_alpha(point) * distance_fade(point);
            if (alpha <= 0.02F) continue;
            alphas.push_back(alpha);
            texts.push_back(cluster.markers.size() > 99 ? std::string("99+")
                                                        : std::to_string(cluster.markers.size()));
            const auto extent = font->CalcTextSizeA(small, FLT_MAX, 0.0F, texts.back().c_str());
            requests.push_back({cluster.screen.x, cluster.screen.y, extent.x + 6.0F, extent.y + 2.0F,
                                static_cast<float>(cluster.markers.size())});
        }
        const ScreenRect bounds{origin.x, origin.y, origin.x + size.x, origin.y + size.y};
        const auto placed = place_labels(requests, bounds, 4.0F * scale);
        for (std::size_t i = 0; i < placed.size(); ++i) {
            if (!placed[i]) continue;
            const auto& rect = *placed[i];
            draw_list->AddRectFilled({rect.x0, rect.y0}, {rect.x1, rect.y1},
                                     ui::viewport_color(ui::Viewport::cluster, alphas[i]), 4.0F);
            draw_list->AddText(font, small, {rect.x0 + 3.0F, rect.y0 + 1.0F},
                               ui::viewport_color(ui::Viewport::cluster_text, alphas[i]), texts[i].c_str());
            badges.push_back(rect);
        }
    }

    // Icon badges for single markers when few are on screen, and always for the
    // selected and hovered ones.
    std::size_t singles = 0;
    for (const auto& cluster : overlay_clusters_) singles += cluster.markers.size() == 1 ? 1 : 0;
    const bool icons = static_cast<int>(singles) <= overlay_options_.icon_limit;
    for (const auto& cluster : overlay_clusters_) {
        if (cluster.markers.size() != 1) continue;
        const auto& marker = overlay_markers_[cluster.markers.front()];
        const auto& point = mission_points_[marker.point];
        const auto emphasis = overlay_emphasis(point.source_entry);
        if (!icons && emphasis != Emphasis::selected && emphasis != Emphasis::hovered) continue;
        if (point.kind == Kind::navigation_point && emphasis != Emphasis::selected &&
            emphasis != Emphasis::hovered)
            continue; // Nav points are too dense for badges.
        float alpha = emphasis == Emphasis::dimmed ? 0.3F : 1.0F;
        alpha *= std::max(overlay_occlusion_alpha(marker.point), emphasis == Emphasis::selected ? 0.75F : 0.0F);
        if (emphasis != Emphasis::selected && emphasis != Emphasis::hovered)
            alpha *= distance_fade(marker.point);
        if (alpha <= 0.02F) continue;
        const float radius = 9.0F * scale;
        const ImVec2 c{marker.screen.x, marker.screen.y - radius - 6.0F * scale};
        draw_list->AddCircleFilled(c, radius, scale_alpha(ui::viewport_color(ui::Viewport::label_background), alpha), 16);
        draw_list->AddCircle(c, radius, scale_alpha(point.color, alpha), 16, 1.5F);
        const char* icon = layer_icons[index_of(point.kind)];
        const float icon_size = font_size * 0.85F;
        const auto icon_extent = font->CalcTextSizeA(icon_size, FLT_MAX, 0.0F, icon);
        draw_list->AddText(font, icon_size, {c.x - icon_extent.x * 0.5F, c.y - icon_extent.y * 0.5F},
                           scale_alpha(point.color, alpha), icon);
        badges.push_back({c.x - radius, c.y - radius, c.x + radius, c.y + radius});
    }

    // Labels.
    using Labels = OverlayOptions::Labels;
    const auto mode = show_all_labels_ ? Labels::all : overlay_options_.labels;
    if (mode != Labels::off) {
        struct Candidate {
            std::uint32_t point;
            ImVec2 screen;
            float priority;
            Emphasis emphasis;
            std::string text;
        };
        std::vector<Candidate> candidates;
        const float nearby = 180.0F * scale;
        for (const auto& cluster : overlay_clusters_) {
            const auto& marker = overlay_markers_[cluster.representative];
            const auto& point = mission_points_[marker.point];
            if (point.label.empty()) continue;
            const auto emphasis = overlay_emphasis(point.source_entry);
            float priority = 1.0F / (1.0F + std::max(marker.depth, 0.0F));
            bool wanted = false;
            if (emphasis == Emphasis::selected) {
                wanted = true;
                priority += 1000.0F;
            } else if (emphasis == Emphasis::hovered && mode >= Labels::hovered) {
                wanted = true;
                priority += 500.0F;
            } else if (emphasis == Emphasis::related && focus_mode_ && mode >= Labels::hovered) {
                wanted = true;
                priority += 100.0F;
            } else if (mode == Labels::all && emphasis != Emphasis::dimmed) {
                wanted = true;
            } else if (mode == Labels::nearby && canvas_hovered_ && emphasis != Emphasis::dimmed) {
                const float dx = marker.screen.x - io.MousePos.x, dy = marker.screen.y - io.MousePos.y;
                if (dx * dx + dy * dy < nearby * nearby) {
                    wanted = true;
                    priority += 10.0F / (1.0F + std::sqrt(dx * dx + dy * dy));
                }
            }
            if (!wanted) continue;
            // "+N" counts the other markers here, not the representative's own
            // parts (an actor's spawn point, an effect's dummy).
            auto text = point.label;
            const auto related = mission_relations_.find(point.source_entry);
            std::size_t others = 0;
            for (const auto index : cluster.markers) {
                const auto entry = mission_points_[overlay_markers_[index].point].source_entry;
                if (entry == point.source_entry) continue;
                if (related != mission_relations_.end() && std::ranges::find(related->second, entry) != related->second.end())
                    continue;
                ++others;
            }
            if (others > 0) text += " +" + std::to_string(others);
            candidates.push_back({marker.point, marker.screen, priority, emphasis, std::move(text)});
        }
        constexpr std::size_t label_cap = 600;
        if (candidates.size() > label_cap) {
            std::ranges::nth_element(candidates, candidates.begin() + label_cap, std::greater{},
                                     &Candidate::priority);
            candidates.resize(label_cap);
        }
        std::vector<LabelRequest> requests;
        requests.reserve(candidates.size());
        const float pad = 3.0F * scale;
        for (const auto& candidate : candidates) {
            const auto text = ImGui::CalcTextSize(candidate.text.c_str());
            requests.push_back({candidate.screen.x, candidate.screen.y, text.x + pad * 2.0F,
                                text.y + pad, candidate.priority});
        }
        const ScreenRect bounds{origin.x + 2.0F, origin.y + 2.0F, origin.x + size.x - 2.0F, origin.y + size.y - 2.0F};
        const auto placed = place_labels(requests, bounds, 8.0F * scale, badges);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (!placed[i]) continue;
            const auto& candidate = candidates[i];
            float alpha = 1.0F;
            if (candidate.emphasis != Emphasis::selected)
                alpha = std::max(overlay_occlusion_alpha(candidate.point), 0.35F);
            const auto& rect = *placed[i];
            draw_list->AddRectFilled({rect.x0, rect.y0}, {rect.x1, rect.y1},
                                     scale_alpha(ui::viewport_color(ui::Viewport::label_background), alpha), 3.0F);
            const auto color = candidate.emphasis == Emphasis::selected ? ui::viewport_color(ui::Viewport::selection)
                               : candidate.emphasis == Emphasis::hovered ? ui::viewport_color(ui::Viewport::hover)
                               : candidate.emphasis == Emphasis::related ? ui::viewport_color(ui::Viewport::related)
                                                                         : ui::viewport_color(ui::Viewport::hud_text);
            draw_list->AddText({rect.x0 + pad, rect.y0 + pad * 0.5F}, scale_alpha(color, alpha),
                               candidate.text.c_str());
        }
    }

    // Arrow toward the selection when it is off screen.
    if (selected_mission_entry_) {
        std::optional<rws::Vec3> target;
        std::string label;
        for (const auto& point : mission_points_)
            if (point.source_entry == *selected_mission_entry_) {
                target = point.position;
                label = point.label;
                break;
            }
        if (!target)
            for (const auto& line : mission_lines_)
                if (line.source_entry == *selected_mission_entry_ || line.partner_entry == *selected_mission_entry_) {
                    target = rws::Vec3{(line.first.x + line.second.x) * 0.5F, (line.first.y + line.second.y) * 0.5F,
                                       (line.first.z + line.second.z) * 0.5F};
                    label = line.sublayer.empty() ? layer_names[index_of(line.kind)] : line.sublayer;
                    break;
                }
        if (target) {
            const auto screen = project_point(*target);
            const float inset = 22.0F * scale;
            const bool inside = screen && screen->x >= origin.x + 4.0F && screen->y >= origin.y + 4.0F &&
                                screen->x <= origin.x + size.x - 4.0F && screen->y <= origin.y + size.y - 4.0F;
            if (!inside) {
                const ImVec2 middle{origin.x + size.x * 0.5F, origin.y + size.y * 0.5F};
                const auto view = view_point(*target);
                const bool in_front = projection_ != 0 || view.z < 0.0F;
                const float dx = in_front && screen ? screen->x - middle.x : view.x;
                const float dy = in_front && screen ? screen->y - middle.y : -view.y;
                if (const auto edge = edge_indicator(dx, dy, {origin.x, origin.y, origin.x + size.x, origin.y + size.y}, inset)) {
                    const float c = std::cos(edge->angle), s = std::sin(edge->angle);
                    const float length = 14.0F * scale, width = 8.0F * scale;
                    const ImVec2 tip{edge->x + c * length * 0.5F, edge->y + s * length * 0.5F};
                    const ImVec2 left{edge->x - c * length * 0.5F - s * width, edge->y - s * length * 0.5F + c * width};
                    const ImVec2 right{edge->x - c * length * 0.5F + s * width, edge->y - s * length * 0.5F - c * width};
                    const auto color = ui::viewport_color(ui::Viewport::offscreen);
                    draw_list->AddTriangleFilled(tip, left, right, color);
                    draw_list->AddTriangle(tip, left, right, ui::viewport_color(ui::Viewport::marker_outline), 1.5F);
                    if (!label.empty()) {
                        const auto text = ImGui::CalcTextSize(label.c_str());
                        ImVec2 at{edge->x - c * (length + 6.0F * scale) - text.x * 0.5F,
                                  edge->y - s * (length + 6.0F * scale) - text.y * 0.5F};
                        at.x = std::clamp(at.x, origin.x + 4.0F, origin.x + size.x - text.x - 4.0F);
                        at.y = std::clamp(at.y, origin.y + 4.0F, origin.y + size.y - text.y - 4.0F);
                        draw_list->AddRectFilled({at.x - 4.0F, at.y - 2.0F}, {at.x + text.x + 4.0F, at.y + text.y + 2.0F},
                                                 ui::viewport_color(ui::Viewport::label_background), 3.0F);
                        draw_list->AddText(at, color, label.c_str());
                    }
                    const float hit = 16.0F * scale;
                    offscreen_indicator_rect_ = ImVec4{edge->x - hit, edge->y - hit, edge->x + hit, edge->y + hit};
                }
            }
        }
    }
}

void GeometryPreview::draw_overlay_hover_tooltip() {
    if (!canvas_hovered_ || ImGui::IsPopupOpen("##overlay_picker")) return;
    const auto describe = [&](const std::uint32_t entry) {
        struct Description {
            Kind kind{Kind::actor};
            std::string label, sublayer;
            std::optional<std::uint32_t> point;
        } result;
        for (std::uint32_t i = 0; i < mission_points_.size(); ++i)
            if (mission_points_[i].source_entry == entry) {
                result = {mission_points_[i].kind, mission_points_[i].label, mission_points_[i].sublayer, i};
                return result;
            }
        for (const auto& line : mission_lines_)
            if (line.source_entry == entry || line.partner_entry == entry) {
                result = {line.kind, {}, line.sublayer, std::nullopt};
                return result;
            }
        return result;
    };
    if (hovered_cluster_ && *hovered_cluster_ < overlay_clusters_.size()) {
        const auto& cluster = overlay_clusters_[*hovered_cluster_];
        ImGui::BeginTooltip();
        ImGui::Text("%zu merged markers", cluster.markers.size());
        ImGui::Separator();
        std::size_t shown = 0;
        for (const auto index : cluster.markers) {
            if (shown == 12) break;
            const auto& point = mission_points_[overlay_markers_[index].point];
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(point.color));
            ImGui::TextUnformatted(layer_icons[index_of(point.kind)]);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::Text("%s  #%u", point.label.c_str(), point.source_entry);
            ++shown;
        }
        if (cluster.markers.size() > shown) ImGui::TextDisabled("... and %zu more", cluster.markers.size() - shown);
        ImGui::TextDisabled("Click to choose one; zoom in to separate them.");
        ImGui::EndTooltip();
        return;
    }
    if (!hovered_mission_entry_) return;
    const auto info = describe(*hovered_mission_entry_);
    ImGui::BeginTooltip();
    ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::Token::text_dim));
    ImGui::Text("%s %s", layer_icons[index_of(info.kind)], layer_names[index_of(info.kind)]);
    ImGui::PopStyleColor();
    if (!info.label.empty()) ImGui::TextUnformatted(info.label.c_str());
    if (!info.sublayer.empty() && info.sublayer != info.label) ImGui::TextUnformatted(info.sublayer.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::Token::text_dim));
    ImGui::Text("entry #%u", *hovered_mission_entry_);
    if (const auto related = mission_relations_.find(*hovered_mission_entry_); related != mission_relations_.end())
        ImGui::Text("%zu related", related->second.size());
    if (const auto walls = info.point ? overlay_point_walls(*info.point) : std::nullopt; walls && *walls > 0)
        ImGui::Text(*walls == 1 ? "Behind %d wall" : "Behind %d walls", *walls);
    else if (info.point && overlay_point_occluded(*info.point).value_or(false))
        ImGui::TextUnformatted("Behind geometry");
    ImGui::PopStyleColor();
    ImGui::EndTooltip();
}

void GeometryPreview::draw_overlay_picker() {
    if (open_overlay_picker_) {
        ImGui::OpenPopup("##overlay_picker");
        open_overlay_picker_ = false;
    }
    if (!ImGui::BeginPopup("##overlay_picker")) return;
    ImGui::TextDisabled("%zu markers here", overlay_picker_entries_.size());
    ImGui::Separator();
    const float height = std::min(static_cast<float>(overlay_picker_entries_.size()), 14.0F) *
                         ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("##picker_list", {320.0F * ui::ui_scale(), height}, ImGuiChildFlags_None)) {
        for (const auto entry : overlay_picker_entries_) {
            const MissionOverlayPoint* found = nullptr;
            for (const auto& point : mission_points_)
                if (point.source_entry == entry) {
                    found = &point;
                    break;
                }
            if (!found) continue;
            ImGui::PushID(static_cast<int>(entry));
            const auto text = std::string(layer_icons[index_of(found->kind)]) + "  " + found->label + "  #" +
                              std::to_string(entry);
            if (ImGui::Selectable(text.c_str(), selected_mission_entry_ == entry)) {
                selected_mission_entry_ = entry;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::IsItemHovered() && !found->sublayer.empty())
                ImGui::SetTooltip("%s\n%s", layer_names[index_of(found->kind)], found->sublayer.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void GeometryPreview::draw_overlay_legend(ImDrawList* draw_list, const ImVec2 origin, const ImVec2 size) {
    if (!scene_mode_ || !overlay_options_.show_legend || !has_mission_overlays()) return;
    const float scale = ui::ui_scale();
    const float line = ImGui::GetTextLineHeight() + 2.0F * scale;
    std::vector<std::pair<std::size_t, std::string>> rows;
    float width = 0.0F;
    for (std::size_t i = 0; i < kind_count; ++i) {
        if (layer_entry_counts_[i] == 0) continue;
        const bool hidden = overlay_options_.layer_hidden(overlay_layer_keys[i]);
        char text[96];
        if (hidden)
            std::snprintf(text, sizeof(text), "%s  hidden", layer_names[i]);
        else
            std::snprintf(text, sizeof(text), "%s  %zu / %zu", layer_names[i], visible_layer_counts_[i],
                          layer_entry_counts_[i]);
        width = std::max(width, ImGui::CalcTextSize(text).x);
        rows.emplace_back(i, text);
    }
    if (rows.empty()) return;
    std::string footer;
    if (overlay_filter_[0] != '\0') footer = std::string("filter: ") + overlay_filter_.data();
    if (slice_.enabled) {
        char text[64];
        std::snprintf(text, sizeof(text), "%sslice Y %.0f..%.0f", footer.empty() ? "" : "  ", slice_.low, slice_.high);
        footer += text;
    }
    if (focus_mode_) footer += footer.empty() ? "focus" : "  focus";
    if (!footer.empty()) width = std::max(width, ImGui::CalcTextSize(footer.c_str()).x);
    const float icon = 16.0F * scale;
    const float box_width = width + icon + 16.0F * scale;
    const float top = origin.y + 38.0F * scale;
    const ImVec2 lo{origin.x + size.x - box_width - 8.0F * scale, top};
    const float height = line * static_cast<float>(rows.size() + (footer.empty() ? 0 : 1)) + 8.0F * scale;
    draw_list->AddRectFilled(lo, {lo.x + box_width, lo.y + height}, ui::viewport_color(ui::Viewport::hud_background), 3.0F);
    float y = lo.y + 4.0F * scale;
    for (const auto& [kind, text] : rows) {
        const bool hidden = overlay_options_.layer_hidden(overlay_layer_keys[kind]);
        ImU32 color = ui::viewport_color(ui::Viewport::bounds);
        for (const auto& point : mission_points_)
            if (index_of(point.kind) == kind) {
                color = point.color;
                break;
            }
        if (color == ui::viewport_color(ui::Viewport::bounds))
            for (const auto& l : mission_lines_)
                if (index_of(l.kind) == kind) {
                    color = l.color;
                    break;
                }
        const auto text_color = ui::viewport_color(hidden ? ui::Viewport::hud_text_dim : ui::Viewport::hud_text);
        draw_list->AddText({lo.x + 6.0F * scale, y}, hidden ? scale_alpha(color, 0.4F) : color, layer_icons[kind]);
        draw_list->AddText({lo.x + 6.0F * scale + icon, y}, text_color, text.c_str());
        y += line;
    }
    if (!footer.empty())
        draw_list->AddText({lo.x + 6.0F * scale, y}, ui::viewport_color(ui::Viewport::related), footer.c_str());
}

void GeometryPreview::draw_overlay_minimap(const ImVec2 origin, const ImVec2 size) {
    if (!scene_mode_ || !overlay_options_.show_minimap || !overlay_bounds_valid_ ||
        minimap_density_.size() != static_cast<std::size_t>(minimap_cells * minimap_cells))
        return;
    const float scale = ui::ui_scale();
    const float span_x = std::max(overlay_bounds_max_.x - overlay_bounds_min_.x, 1.0F);
    const float span_z = std::max(overlay_bounds_max_.z - overlay_bounds_min_.z, 1.0F);
    const float side = std::min(180.0F * scale, std::min(size.x * 0.4F, size.y - 330.0F * scale));
    if (side < 60.0F) return;
    // Keep the level's aspect ratio inside a square box.
    const float map_w = span_x >= span_z ? side : side * span_x / span_z;
    const float map_h = span_x >= span_z ? side * span_z / span_x : side;
    // Above the axis gizmo, clear of the stats HUD in the bottom-left corner.
    const ImVec2 box{origin.x + size.x - side - 12.0F * scale, origin.y + size.y - side - 124.0F * scale};
    ImGui::SetCursorScreenPos(box);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui::viewport_color_f(ui::Viewport::minimap_background));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 3.0F);
    if (ImGui::BeginChild("##overlay_minimap", {side, side}, ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_NoSavedSettings)) {
        auto* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 map{box.x + (side - map_w) * 0.5F, box.y + (side - map_h) * 0.5F};
        const auto to_map = [&](const float x, const float z) {
            return ImVec2{map.x + (x - overlay_bounds_min_.x) / span_x * map_w,
                          map.y + (overlay_bounds_max_.z - z) / span_z * map_h};
        };
        const auto peak = static_cast<float>(*std::ranges::max_element(minimap_density_));
        const float cell_w = map_w / minimap_cells, cell_h = map_h / minimap_cells;
        const auto density_color = ui::viewport_color(ui::Viewport::related);
        for (int z = 0; z < minimap_cells; ++z)
            for (int x = 0; x < minimap_cells; ++x) {
                const auto count = minimap_density_[static_cast<std::size_t>(z * minimap_cells + x)];
                if (count == 0 || peak <= 0.0F) continue;
                const float t = std::sqrt(static_cast<float>(count) / peak);
                const ImVec2 lo{map.x + x * cell_w, map.y + z * cell_h};
                draw_list->AddRectFilled(lo, {lo.x + cell_w + 0.5F, lo.y + cell_h + 0.5F},
                                         scale_alpha(density_color, 0.15F + 0.75F * t));
            }
        if (slice_.enabled) draw_list->AddText({box.x + 4.0F, box.y + 2.0F}, ui::viewport_color(ui::Viewport::slice_band), "slice");
        // Camera target and viewing direction.
        const rws::Vec3 target{center_.x + navigation_offset_.x, center_.y + navigation_offset_.y,
                               center_.z + navigation_offset_.z};
        const auto at = to_map(target.x, target.z);
        const auto offset = camera_offset(yaw_, pitch_);
        const float forward_x = -offset.x, forward_z = -offset.z;
        const float length = std::sqrt(forward_x * forward_x + forward_z * forward_z);
        const auto view_color = ui::viewport_color(ui::Viewport::minimap_view);
        if (projection_ == 0 && length > 1e-4F) {
            const float reach = 28.0F * scale;
            const float ux = forward_x / length, uz = forward_z / length;
            const float spread = 0.45F;
            const auto ray = [&](const float angle) {
                const float c = std::cos(angle), s = std::sin(angle);
                const float rx = ux * c - uz * s, rz = ux * s + uz * c;
                return ImVec2{at.x + rx * reach, at.y - rz * reach};
            };
            draw_list->AddTriangleFilled(at, ray(-spread), ray(spread), scale_alpha(view_color, 0.35F));
            draw_list->AddLine(at, ray(-spread), view_color, 1.0F);
            draw_list->AddLine(at, ray(spread), view_color, 1.0F);
        }
        draw_list->AddCircleFilled(at, 3.5F * scale, view_color);
        if (selected_mission_entry_)
            for (const auto& point : mission_points_)
                if (point.source_entry == *selected_mission_entry_) {
                    const auto p = to_map(point.position.x, point.position.z);
                    draw_list->AddCircle(p, 5.0F * scale, ui::viewport_color(ui::Viewport::selection), 12, 2.0F);
                    break;
                }
        // Click or drag to move the camera target.
        ImGui::SetCursorScreenPos(box);
        ImGui::InvisibleButton("##minimap_input", {side, side});
        if (ImGui::IsItemActive()) {
            const auto mouse = ImGui::GetIO().MousePos;
            const float x = overlay_bounds_min_.x + (mouse.x - map.x) / map_w * span_x;
            const float z = overlay_bounds_max_.z - (mouse.y - map.y) / map_h * span_z;
            target_navigation_offset_.x = x - center_.x;
            target_navigation_offset_.z = z - center_.z;
            pan_x_ = pan_y_ = 0.0F;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Marker density (top view). Click or drag to move the camera.");
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---- Toolbar popups ---------------------------------------------------------

void GeometryPreview::draw_layers_popup(const bool has_visual, const bool has_collision) {
    ImGui::BeginDisabled(!has_visual);
    ImGui::Checkbox("Visual scene", &show_visual_);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!has_collision);
    ImGui::Checkbox("Level collision", &show_collision_);
    ImGui::EndDisabled();
    if (has_mission_overlays()) {
        ImGui::Separator();
        ImGui::TextDisabled("Presets");
        const auto apply = [&](const OverlayPreset& preset) {
            overlay_options_.hidden_layers = preset.hidden_layers;
            overlay_options_.clamp();
        };
        int shown = 0;
        const auto preset_button = [&](const OverlayPreset& preset, const bool user) {
            if (shown++ % 4 != 0) ImGui::SameLine();
            ImGui::PushID(user ? 1 : 0);
            ImGui::PushID(preset.name.c_str());
            const bool active = overlay_options_.hidden_layers == preset.hidden_layers;
            if (active) ImGui::PushStyleColor(ImGuiCol_Text, ui::color(ui::Token::accent));
            if (ImGui::SmallButton(preset.name.c_str())) apply(preset);
            if (active) ImGui::PopStyleColor();
            if (user && ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Delete preset"))
                    std::erase_if(overlay_options_.presets,
                                  [&](const OverlayPreset& other) { return other.name == preset.name; });
                ImGui::EndPopup();
            }
            if (user && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Right-click to delete");
            ImGui::PopID();
            ImGui::PopID();
        };
        for (const auto& preset : builtin_overlay_presets())
            if (std::ranges::none_of(overlay_options_.presets,
                                     [&](const OverlayPreset& user) { return user.name == preset.name; }))
                preset_button(preset, false);
        const auto user_presets = overlay_options_.presets; // The loop may delete one.
        for (const auto& preset : user_presets) preset_button(preset, true);
        ImGui::SetNextItemWidth(150.0F * ui::ui_scale());
        ImGui::InputTextWithHint("##preset_name", "New preset name", new_preset_name_.data(), new_preset_name_.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(new_preset_name_[0] == '\0');
        if (ImGui::SmallButton("Save")) {
            OverlayPreset preset{new_preset_name_.data(), overlay_options_.hidden_layers};
            std::erase_if(overlay_options_.presets,
                          [&](const OverlayPreset& other) { return other.name == preset.name; });
            overlay_options_.presets.push_back(std::move(preset));
            new_preset_name_.fill('\0');
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextDisabled("Mission layers  (Alt+click: solo)");
        for (std::size_t i = 0; i < kind_count; ++i) {
            if (layer_entry_counts_[i] == 0) continue;
            const auto key = overlay_layer_keys[i];
            ImU32 color = ui::viewport_color(ui::Viewport::bounds);
            for (const auto& point : mission_points_)
                if (index_of(point.kind) == i) {
                    color = point.color;
                    break;
                }
            if (color == ui::viewport_color(ui::Viewport::bounds))
                for (const auto& line : mission_lines_)
                    if (index_of(line.kind) == i) {
                        color = line.color;
                        break;
                    }
            ImGui::PushID(static_cast<int>(i));
            const auto& sublayers = layer_sublayers_[i];
            bool open = false;
            if (sublayers.size() > 1) {
                open = ImGui::TreeNodeEx("##sublayers", ImGuiTreeNodeFlags_NoTreePushOnOpen);
                ImGui::SameLine();
            } else {
                ImGui::Dummy({ImGui::GetTreeNodeToLabelSpacing() - ImGui::GetStyle().ItemSpacing.x, 0.0F});
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
            ImGui::TextUnformatted(layer_icons[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            bool visible = !overlay_options_.layer_hidden(key);
            const auto text = std::string(layer_names[i]) + " (" + std::to_string(layer_entry_counts_[i]) + ")";
            if (ImGui::Checkbox(text.c_str(), &visible)) {
                if (ImGui::GetIO().KeyAlt) {
                    for (const auto other : overlay_layer_keys) overlay_options_.set_layer_hidden(other, other != key);
                } else {
                    overlay_options_.set_layer_hidden(key, !visible);
                }
            }
            if (open) {
                ImGui::Indent();
                const bool scroll = sublayers.size() > 12;
                if (scroll)
                    ImGui::BeginChild("##sublayer_list", {0.0F, ImGui::GetTextLineHeightWithSpacing() * 12.0F},
                                      ImGuiChildFlags_AutoResizeX);
                for (const auto& [name, count] : sublayers) {
                    const std::pair<std::uint8_t, std::string> id{static_cast<std::uint8_t>(i), name};
                    bool sub_visible = !hidden_sublayers_.contains(id);
                    const auto sub_text = name + " (" + std::to_string(count) + ")";
                    if (ImGui::Checkbox(sub_text.c_str(), &sub_visible)) {
                        if (ImGui::GetIO().KeyAlt) {
                            for (const auto& [other, unused] : sublayers)
                                if (other == name)
                                    hidden_sublayers_.erase({static_cast<std::uint8_t>(i), other});
                                else
                                    hidden_sublayers_.insert({static_cast<std::uint8_t>(i), other});
                        } else if (sub_visible) {
                            hidden_sublayers_.erase(id);
                        } else {
                            hidden_sublayers_.insert(id);
                        }
                    }
                }
                if (scroll) ImGui::EndChild();
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Show all")) {
            overlay_options_.hidden_layers.clear();
            hidden_sublayers_.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Hide all"))
            for (const auto key : overlay_layer_keys) overlay_options_.set_layer_hidden(key, true);
        if (!hidden_mission_entries_.empty()) {
            ImGui::SameLine();
            const auto text = "Unhide " + std::to_string(hidden_mission_entries_.size()) + " entries";
            if (ImGui::SmallButton(text.c_str())) hidden_mission_entries_.clear();
        }
    }
    if (!skeleton_lines_.empty()) {
        ImGui::Separator();
        ImGui::Checkbox("Skeleton", &show_skeleton_);
        if (show_skeleton_) ImGui::Checkbox("Bone IDs", &show_skeleton_labels_);
    }
    if (!physics_lines_.empty()) ImGui::Checkbox("Physics", &show_physics_);
}

void GeometryPreview::draw_markers_popup() {
    auto& o = overlay_options_;
    const float width = 190.0F * ui::ui_scale();
    const auto combo = [&](const char* label, int& value, const char* items, const char* tooltip) {
        ImGui::SetNextItemWidth(width);
        const bool changed = ImGui::Combo(label, &value, items);
        if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
        return changed;
    };
    int labels = static_cast<int>(o.labels);
    if (combo("Labels", labels, "Off\0Selected\0Hovered + selected\0Near the pointer\0All\0",
              "Hold L over the viewport to show all labels for a moment."))
        o.labels = static_cast<OverlayOptions::Labels>(labels);
    int headings = static_cast<int>(o.headings);
    if (combo("Heading ticks", headings, "Off\0Selected / related\0All\0", nullptr))
        o.headings = static_cast<OverlayOptions::Detail>(headings);
    int details = static_cast<int>(o.details);
    if (combo("Frusta & light radii", details, "Off\0Selected / related\0All\0",
              "Cutscene camera frusta and light ranges."))
        o.details = static_cast<OverlayOptions::Detail>(details);
    ImGui::SetNextItemWidth(width);
    float occluded = o.occluded_opacity * 100.0F;
    if (ImGui::SliderFloat("Behind geometry", &occluded, 0.0F, 100.0F, "%.0f%%")) o.occluded_opacity = occluded / 100.0F;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Opacity of markers deep behind level geometry; each wall in front dims a marker further toward it. 100%% ignores depth.");
    ImGui::SetNextItemWidth(width);
    ImGui::SliderFloat("Fade beyond", &o.fade_distance, 0.0F, 20.0F,
                       o.fade_distance <= 0.0F ? "off" : "%.1fx scene radius", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Markers dim with distance past the near side of the scene, reaching their faintest this many scene radii further on (perspective).");
    ImGui::SetNextItemWidth(width);
    ImGui::SliderFloat("Merge within", &o.merge_pixels, 0.0F, 24.0F, o.merge_pixels <= 0.0F ? "off" : "%.0f px");
    ImGui::SetNextItemWidth(width);
    ImGui::SliderInt("Icon badges up to", &o.icon_limit, 0, 1000, "%d markers");
    ImGui::Checkbox("Legend", &o.show_legend);
    ImGui::SameLine();
    ImGui::Checkbox("Minimap (M)", &o.show_minimap);
    ImGui::Checkbox("Filter dims instead of hiding", &o.dim_filtered);
    ImGui::Checkbox("Focus mode (Z)", &focus_mode_);
    o.clamp();

    ImGui::Separator();
    ImGui::TextDisabled("Height slice (Y)");
    bool slice = slice_.enabled;
    if (ImGui::Checkbox("Enabled", &slice)) {
        if (slice != slice_.enabled) toggle_floor_slice();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Current floor")) {
        slice_.enabled = false;
        toggle_floor_slice();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Center the band on the selected marker, or the camera target.");
    ImGui::BeginDisabled(!slice_.enabled);
    ImGui::SetNextItemWidth(width);
    const float speed = std::max(all_radius_ * 0.001F, 0.5F);
    if (ImGui::DragFloatRange2("Y range", &slice_.low, &slice_.high, speed, 0.0F, 0.0F, "%.0f", "%.0f"))
        slice_height_ = std::max(slice_.high - slice_.low, 1.0F);
    ImGui::Checkbox("Cut level geometry", &slice_.clip_geometry);
    ImGui::EndDisabled();
}

} // namespace rwsman
