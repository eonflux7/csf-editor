// Viewport editing: the move/rotate gizmo for the selected mission record.
//
// The gizmo only produces poses. While a drag is active the preview moves the
// record's actor model and markers itself, so the app sees a live pose every
// frame and applies one mission edit when the drag ends.
#include "geometry_preview.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace rwsman {
namespace {

constexpr float ring_pixels = 56.0F;  // Rotation ring radius.
constexpr float grab_pixels = 9.0F;   // Pick tolerance around handles.
constexpr float center_pixels = 13.0F;

float distance_to_segment(const ImVec2 p, const ImVec2 a, const ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = dx * dx + dy * dy;
    float t = length > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length : 0.0F;
    t = std::clamp(t, 0.0F, 1.0F);
    const float x = a.x + t * dx - p.x, y = a.y + t * dy - p.y;
    return std::sqrt(x * x + y * y);
}

float length(const ImVec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// Intersection of a ray with the horizontal plane y = height.
std::optional<rws::Vec3> ray_plane(const rws::CollisionRay& ray, const float height) {
    if (std::abs(ray.direction.y) < 1e-5F) return std::nullopt;
    const float t = (height - ray.origin.y) / ray.direction.y;
    if (t < 0) return std::nullopt;
    return rws::Vec3{ray.origin.x + ray.direction.x * t, height, ray.origin.z + ray.direction.z * t};
}

float wrap_angle(float value) {
    constexpr float tau = 2.0F * std::numbers::pi_v<float>;
    value = std::fmod(value, tau);
    if (value > std::numbers::pi_v<float>) value -= tau;
    if (value < -std::numbers::pi_v<float>) value += tau;
    return value;
}

} // namespace

void GeometryPreview::set_edit_handle(std::optional<EditHandle> handle) {
    if (edit_drag_) return; // The drag owns the pose until it ends.
    edit_handle_ = handle;
}

std::optional<GeometryPreview::EditDrag> GeometryPreview::take_edit_drag() {
    if (edit_result_) return std::exchange(edit_result_, std::nullopt);
    if (!edit_drag_) return std::nullopt;
    const auto& start = edit_drag_->start;
    return EditDrag{EditDrag::Phase::active, start.source_entry, edit_drag_->position, edit_drag_->heading,
                    start.map_instance, start.rotation, start.heading_radians};
}

namespace {

// The instance basis turned about the world Y axis by `angle`; forward
// (0, 0, 1) turns to (sin, 0, cos), matching mission headings.
std::array<float, 9> yaw_rotated(const std::array<float, 9>& basis, const float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    auto result = basis;
    for (std::size_t column = 0; column < 3; ++column) {
        const float x = basis[column * 3], z = basis[column * 3 + 2];
        result[column * 3] = c * x + s * z;
        result[column * 3 + 2] = -s * x + c * z;
    }
    return result;
}

} // namespace

std::array<float, 9> GeometryPreview::instance_rotation_for(const EditDrag& drag) {
    return yaw_rotated(drag.rotation, drag.heading_radians - drag.start_heading_radians);
}

std::optional<rws::Vec3> GeometryPreview::surface_point(const ImVec2 screen) const {
    if (const auto hit = pick_collision(screen.x, screen.y)) return hit->position;
    return std::nullopt;
}

std::optional<rws::Vec3> GeometryPreview::placement_point(const ImVec2 screen) const {
    const auto collision = pick_collision(screen.x, screen.y);
    if (!show_visual_) return collision ? std::optional(collision->position) : std::nullopt;
    const auto ray = viewport_ray(screen.x, screen.y);
    if (!ray) return collision ? std::optional(collision->position) : std::nullopt;
    const auto o = ray->origin, d = ray->direction;
    const auto clips = active_clip_planes();
    // The nearest visible World triangle (Moller-Trumbore), batches outside
    // the ray skipped by their bounding spheres.
    float closest = collision ? collision->distance : std::numeric_limits<float>::max();
    std::optional<rws::Vec3> best;
    bool level = false;
    for (std::size_t index = 0; index < draw_batches_.size(); ++index) {
        const auto& batch = draw_batches_[index];
        if (batch.layer != PreviewLayer::visual_world) continue;
        if (index < batch_bounds_.size() && batch_bounds_[index].radius >= 0.0F) {
            const auto& sphere = batch_bounds_[index];
            const rws::Vec3 to{sphere.center.x - o.x, sphere.center.y - o.y, sphere.center.z - o.z};
            const float along = to.x * d.x + to.y * d.y + to.z * d.z;
            const float square = to.x * to.x + to.y * to.y + to.z * to.z - along * along;
            if (square > sphere.radius * sphere.radius) continue;
        }
        const auto end = static_cast<std::size_t>(batch.first) + batch.count;
        for (std::size_t i = batch.first; i + 2 < end; i += 3) {
            const auto& a = gpu_vertices_[i];
            const auto& b = gpu_vertices_[i + 1];
            const auto& c = gpu_vertices_[i + 2];
            const rws::Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
            const rws::Vec3 p{d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x};
            const float determinant = e1.x * p.x + e1.y * p.y + e1.z * p.z;
            if (std::abs(determinant) < 1e-6F) continue;
            const float inverse = 1.0F / determinant;
            const rws::Vec3 s{o.x - a.x, o.y - a.y, o.z - a.z};
            const float u = (s.x * p.x + s.y * p.y + s.z * p.z) * inverse;
            if (u < 0.0F || u > 1.0F) continue;
            const rws::Vec3 q{s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
            const float v = (d.x * q.x + d.y * q.y + d.z * q.z) * inverse;
            if (v < 0.0F || u + v > 1.0F) continue;
            const float t = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inverse;
            // A few centimetres short of the collision hit: the floor's own
            // visible triangles lie on it and must not count as furniture.
            if (t <= 0.0F || t >= closest - 2.0F) continue;
            const rws::Vec3 hit{o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
            if (!rws::collision_point_visible(hit, clips)) continue;
            const rws::Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
            const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            closest = t;
            best = hit;
            level = length > 0.0F && std::abs(n.y) / length > 0.7F;
        }
    }
    // A wall or a slope in front, or something far above the floor (a tree's
    // crown, a roof): the collision surface decides.
    if (best && level && (!collision || best->y - collision->position.y <= 200.0F)) return best;
    return collision ? std::optional(collision->position) : std::nullopt;
}

rws::Vec3 GeometryPreview::view_target() const {
    const rws::Vec3 target{center_.x + navigation_offset_.x, center_.y + navigation_offset_.y,
                           center_.z + navigation_offset_.z};
    // Prefer the ground under the view center; fall back to the orbit target.
    if (canvas_width_ > 0 && canvas_height_ > 0)
        if (const auto ground = surface_point({canvas_x_ + canvas_width_ * 0.5F,
                                               canvas_y_ + canvas_height_ * 0.5F}))
            return *ground;
    return target;
}

void GeometryPreview::move_mission_markers(const std::uint32_t source_entry, const rws::Vec3 position,
                                           const float heading_radians) {
    for (auto& point : mission_points_) {
        if (point.source_entry != source_entry) continue;
        point.position = position;
        if (point.heading)
            point.heading = rws::Vec3{std::sin(heading_radians), point.heading->y, std::cos(heading_radians)};
    }
}

void GeometryPreview::apply_drag_pose(const ActiveDrag& drag) {
    const auto& start = drag.start;
    if (start.map_instance) {
        place_scene_instance(*start.map_instance,
                             yaw_rotated(start.rotation, drag.heading - start.heading_radians), drag.position);
        return;
    }
    place_mission_actor(start.source_entry, drag.position, drag.heading, drag.pitch);
    move_mission_markers(start.source_entry, drag.position, drag.heading);
}

bool GeometryPreview::update_edit_gizmo() {
    const auto& io = ImGui::GetIO();
    const auto finish = [&](const EditDrag::Phase phase) {
        auto& drag = *edit_drag_;
        if (phase == EditDrag::Phase::cancelled) {
            drag.position = drag.start.position;
            drag.heading = drag.start.heading_radians;
            apply_drag_pose(drag);
        }
        edit_result_ = EditDrag{phase, drag.start.source_entry, drag.position, drag.heading,
                                drag.start.map_instance, drag.start.rotation, drag.start.heading_radians};
        edit_handle_ = drag.start;
        edit_handle_->position = drag.position;
        edit_handle_->heading_radians = drag.heading;
        edit_drag_.reset();
    };

    if (edit_drag_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            finish(EditDrag::Phase::cancelled);
            return true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            finish(EditDrag::Phase::finished);
            return true;
        }
        auto& drag = *edit_drag_;
        const auto& start = drag.start;
        const auto center = project_point(start.position);
        const auto axis_length = [&] {
            // A world length that stays a similar size on screen at any zoom.
            const float distance = std::max(overlay_eye_distance(start.position), 1.0F);
            return projection_ == 0 ? distance * 0.12F : orthographic_scale_ * 0.2F;
        }();
        switch (drag.mode) {
        case ActiveDrag::Mode::plane: {
            const bool snap = snap_to_surface_ != io.KeyAlt;
            // A moved record lands on furniture as a placed one does.
            const auto surface = snap ? placement_point(io.MousePos) : std::nullopt;
            if (surface) {
                drag.position = *surface;
            } else if (const auto ray = viewport_ray(io.MousePos.x, io.MousePos.y)) {
                if (const auto hit = ray_plane(*ray, start.position.y))
                    drag.position = {start.position.x + hit->x - drag.plane_start.x, start.position.y,
                                     start.position.z + hit->z - drag.plane_start.z};
            }
            break;
        }
        case ActiveDrag::Mode::axis_x:
        case ActiveDrag::Mode::axis_y:
        case ActiveDrag::Mode::axis_z: {
            const rws::Vec3 axis = drag.mode == ActiveDrag::Mode::axis_x   ? rws::Vec3{1, 0, 0}
                                   : drag.mode == ActiveDrag::Mode::axis_y ? rws::Vec3{0, 1, 0}
                                                                           : rws::Vec3{0, 0, 1};
            const auto tip = project_point({start.position.x + axis.x * axis_length,
                                            start.position.y + axis.y * axis_length,
                                            start.position.z + axis.z * axis_length});
            if (center && tip) {
                const ImVec2 screen_axis{tip->x - center->x, tip->y - center->y};
                const float squared = screen_axis.x * screen_axis.x + screen_axis.y * screen_axis.y;
                if (squared > 1.0F) {
                    const ImVec2 moved{io.MousePos.x - drag.start_mouse.x, io.MousePos.y - drag.start_mouse.y};
                    const float t = (moved.x * screen_axis.x + moved.y * screen_axis.y) / squared * axis_length;
                    drag.position = {start.position.x + axis.x * t, start.position.y + axis.y * t,
                                     start.position.z + axis.z * t};
                }
            }
            break;
        }
        case ActiveDrag::Mode::rotate: {
            float angle = drag.start_angle;
            const auto ray = viewport_ray(io.MousePos.x, io.MousePos.y);
            const auto hit = ray ? ray_plane(*ray, start.position.y) : std::nullopt;
            if (hit && projection_ != 2 && projection_ != 3)
                angle = std::atan2(hit->x - start.position.x, hit->z - start.position.z);
            else
                angle = drag.start_angle + (io.MousePos.x - drag.start_mouse.x) * 0.01F;
            float heading = start.heading_radians + wrap_angle(angle - drag.start_angle);
            if (io.KeyCtrl) {
                constexpr float step = std::numbers::pi_v<float> / 12.0F; // 15 degrees
                heading = std::round(heading / step) * step;
            }
            drag.heading = wrap_angle(heading);
            break;
        }
        }
        apply_drag_pose(drag);
        animating_frame_ = ImGui::GetFrameCount();
        return true;
    }

    if ((edit_tool_ != EditTool::move && edit_tool_ != EditTool::rotate) || !edit_handle_ || !canvas_hovered_ ||
        !ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return false;
    const auto& handle = *edit_handle_;
    const auto center = project_point(handle.position);
    if (!center) return false;
    const ImVec2 mouse = io.MousePos;
    std::optional<ActiveDrag::Mode> mode;
    if (edit_tool_ == EditTool::move) {
        const float distance = std::max(overlay_eye_distance(handle.position), 1.0F);
        const float axis_length = projection_ == 0 ? distance * 0.12F : orthographic_scale_ * 0.2F;
        const std::array<std::pair<ActiveDrag::Mode, rws::Vec3>, 3> axes{
            {{ActiveDrag::Mode::axis_x, {1, 0, 0}}, {ActiveDrag::Mode::axis_y, {0, 1, 0}},
             {ActiveDrag::Mode::axis_z, {0, 0, 1}}}};
        if (length({mouse.x - center->x, mouse.y - center->y}) <= center_pixels) {
            mode = ActiveDrag::Mode::plane;
        } else {
            for (const auto& [axis_mode, axis] : axes) {
                const auto tip = project_point({handle.position.x + axis.x * axis_length,
                                                handle.position.y + axis.y * axis_length,
                                                handle.position.z + axis.z * axis_length});
                if (tip && length({tip->x - center->x, tip->y - center->y}) > 4.0F &&
                    distance_to_segment(mouse, *center, *tip) <= grab_pixels) {
                    mode = axis_mode;
                    break;
                }
            }
        }
    } else if (handle.rotatable &&
               length({mouse.x - center->x, mouse.y - center->y}) <= ring_pixels + grab_pixels) {
        mode = ActiveDrag::Mode::rotate;
    }
    if (!mode) return false;
    ActiveDrag drag;
    drag.mode = *mode;
    drag.start = handle;
    drag.start_mouse = mouse;
    drag.position = handle.position;
    drag.heading = handle.heading_radians;
    const auto actor = std::ranges::find(mission_actor_models_, handle.source_entry,
                                         &MissionActorModel::source_entry);
    if (actor != mission_actor_models_.end()) drag.pitch = actor->pitch_radians;
    if (const auto ray = viewport_ray(mouse.x, mouse.y))
        if (const auto hit = ray_plane(*ray, handle.position.y)) {
            drag.plane_start = *hit;
            drag.start_angle = std::atan2(hit->x - handle.position.x, hit->z - handle.position.z);
        }
    edit_drag_ = drag;
    return true;
}

void GeometryPreview::draw_edit_gizmo(ImDrawList* draw_list) {
    if ((edit_tool_ != EditTool::move && edit_tool_ != EditTool::rotate) || !edit_handle_) return;
    const auto position = edit_drag_ ? edit_drag_->position : edit_handle_->position;
    const auto heading = edit_drag_ ? edit_drag_->heading : edit_handle_->heading_radians;
    const auto center = project_point(position);
    if (!center) return;
    const auto active = [&](const ActiveDrag::Mode mode) { return edit_drag_ && edit_drag_->mode == mode; };
    const ImU32 highlight = ui::viewport_color(ui::Viewport::selection);
    if (edit_tool_ == EditTool::move) {
        const float distance = std::max(overlay_eye_distance(position), 1.0F);
        const float axis_length = projection_ == 0 ? distance * 0.12F : orthographic_scale_ * 0.2F;
        const std::array<std::tuple<ActiveDrag::Mode, rws::Vec3, ui::Viewport>, 3> axes{
            {{ActiveDrag::Mode::axis_x, {1, 0, 0}, ui::Viewport::axis_x},
             {ActiveDrag::Mode::axis_y, {0, 1, 0}, ui::Viewport::axis_y},
             {ActiveDrag::Mode::axis_z, {0, 0, 1}, ui::Viewport::axis_z}}};
        for (const auto& [mode, axis, color] : axes) {
            const auto tip = project_point({position.x + axis.x * axis_length, position.y + axis.y * axis_length,
                                            position.z + axis.z * axis_length});
            if (!tip) continue;
            const auto stroke = active(mode) ? highlight : ui::viewport_color(color);
            draw_list->AddLine(*center, *tip, stroke, active(mode) ? 4.0F : 2.5F);
            draw_list->AddCircleFilled(*tip, 5.0F, stroke);
        }
        const auto plane = active(ActiveDrag::Mode::plane) ? highlight
                                                            : ui::viewport_color(ui::Viewport::hud_text);
        draw_list->AddCircle(*center, center_pixels, plane, 24, 2.0F);
        draw_list->AddCircleFilled(*center, 3.0F, plane);
    } else if (edit_handle_->rotatable) {
        const auto ring = active(ActiveDrag::Mode::rotate) ? highlight
                                                            : ui::viewport_color(ui::Viewport::axis_y);
        draw_list->AddCircle(*center, ring_pixels, ring, 48, 2.5F);
        // Facing tick: forward is (sin h, 0, cos h) in the map's X/Z plane.
        const float distance = std::max(overlay_eye_distance(position), 1.0F);
        const float reach = projection_ == 0 ? distance * 0.1F : orthographic_scale_ * 0.15F;
        if (const auto facing = project_point({position.x + std::sin(heading) * reach, position.y,
                                               position.z + std::cos(heading) * reach})) {
            const ImVec2 direction{facing->x - center->x, facing->y - center->y};
            const float scale = length(direction) > 1.0F ? ring_pixels / length(direction) : 0.0F;
            const ImVec2 end{center->x + direction.x * scale, center->y + direction.y * scale};
            draw_list->AddLine(*center, end, ring, 2.5F);
            draw_list->AddCircleFilled(end, 5.0F, ring);
        }
    }
}

} // namespace rwsman
