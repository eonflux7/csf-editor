#include "mission_overlays.hpp"
#include "app_util.hpp"

#include "csf/cmo.hpp"
#include "rws/decoded.hpp"
#include "ui/theme.hpp"
#include "rws/physics_inspection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace rwsman {


rws::Vec3 rws_point(const csf::Vec3 value) {
    return {value.x, value.y, value.z};
}

MissionOverlays make_mission_overlays(const csf::MissionScene& scene) {
    using Kind = GeometryPreview::MissionOverlayKind;
    MissionOverlays result;
    const auto append_orientation = [&](const Kind kind, const std::uint32_t entry,
                                        const csf::Vec3 position, const float heading,
                                        const float pitch, const ImU32 color,
                                        const float length = 120.0F) {
        const auto origin = rws_point(position);
        result.lines.push_back(
            {kind, entry, origin,
             {origin.x + std::sin(heading) * std::cos(pitch) * length,
              origin.y - std::sin(pitch) * length,
              origin.z + std::cos(heading) * std::cos(pitch) * length},
             color, true});
    };
    for (const auto& actor : scene.actors())
        if (const auto spawn = scene.actor_spawn_position(actor)) {
            const auto color = ui::viewport_color(ui::Viewport::actor);
            result.points.push_back({Kind::actor, actor.source.entry_index,
                                     rws_point(*spawn), actor.name.value_or("Actor"), color});
            append_orientation(
                Kind::actor, actor.source.entry_index, *spawn,
                csf::mission_actor_angle_radians(actor.heading.value_or(0)),
                csf::mission_actor_angle_radians(actor.pitch.value_or(0)), color);
        }
    std::map<std::pair<std::int32_t, std::int32_t>, csf::Vec3> nav_points;
    for (const auto& group : scene.navigation())
        for (const auto& point : group.points)
            if (point.position && point.group_id && point.id) {
                nav_points[{*point.group_id, *point.id}] = *point.position;
                const auto color = group.type.value_or(0) == 0
                                       ? ui::viewport_color(ui::Viewport::nav_ground)
                                   : group.type == 1 ? ui::viewport_color(ui::Viewport::nav_climb)
                                                     : ui::viewport_color(ui::Viewport::nav_special);
                result.points.push_back({Kind::navigation_point, point.source.entry_index,
                                         rws_point(*point.position),
                                         point.name.value_or("Nav point"), color});
                append_orientation(Kind::navigation_point, point.source.entry_index,
                                   *point.position, point.heading.value_or(0),
                                   point.pitch.value_or(0), color, 70.0F);
            }
    auto add_connection = [&](const csf::NavConnection& connection) {
        if (!connection.valid) return;
        const auto origin = nav_points.find({*connection.origin_group, *connection.origin_point});
        const auto destination =
            nav_points.find({*connection.destination_group, *connection.destination_point});
        if (origin != nav_points.end() && destination != nav_points.end())
            result.lines.push_back({Kind::navigation_connection, connection.source.entry_index,
                                    rws_point(origin->second), rws_point(destination->second),
                                    ui::viewport_color(ui::Viewport::nav_link), true});
    };
    for (const auto& group : scene.navigation())
        for (const auto& connection : group.connections)
            add_connection(connection);
    for (const auto& connection : scene.cross_group_connections())
        add_connection(connection);
    for (const auto& dummy : scene.dummies())
        if (dummy.position) {
            const auto color = ui::viewport_color(ui::Viewport::dummy);
            result.points.push_back({Kind::dummy, dummy.source.entry_index,
                                     rws_point(*dummy.position), dummy.name.value_or("Dummy"),
                                     color});
            append_orientation(Kind::dummy, dummy.source.entry_index, *dummy.position,
                               dummy.heading.value_or(0), dummy.pitch.value_or(0), color);
        }
    for (const auto& area : scene.areas()) {
        for (std::size_t i = 0; i < area.points.size(); ++i) {
            const auto& a = area.points[i];
            const auto& b = area.points[(i + 1) % area.points.size()];
            result.lines.push_back({Kind::area, area.source.entry_index, rws_point(a), rws_point(b),
                                    ui::viewport_color(ui::Viewport::area)});
            if (area.height && std::isfinite(*area.height) && *area.height != 0) {
                const csf::Vec3 top_a{a.x, a.y + *area.height, a.z};
                const csf::Vec3 top_b{b.x, b.y + *area.height, b.z};
                result.lines.push_back({Kind::area, area.source.entry_index, rws_point(top_a),
                                        rws_point(top_b), ui::viewport_color(ui::Viewport::area, 0.71F)});
                result.lines.push_back({Kind::area, area.source.entry_index, rws_point(a),
                                        rws_point(top_a), ui::viewport_color(ui::Viewport::area, 0.52F)});
            }
        }
    }
    for (const auto& light : scene.lights())
        if (light.position) {
            const auto packed = light.color.value_or(0xFFF591U);
            const auto color = ui::rgb_u32(packed);
            result.points.push_back({Kind::light, light.source.entry_index,
                                     rws_point(*light.position), light.name.value_or("Light"),
                                     color});
            if (light.radius && *light.radius > 0 && std::isfinite(*light.radius)) {
                constexpr int segments = 24;
                for (int i = 0; i < segments; ++i) {
                    const float a = static_cast<float>(i) * 6.283185307F / segments;
                    const float b = static_cast<float>(i + 1) * 6.283185307F / segments;
                    const auto center = *light.position;
                    result.lines.push_back({Kind::light,
                                            light.source.entry_index,
                                            {center.x + std::cos(a) * *light.radius, center.y,
                                             center.z + std::sin(a) * *light.radius},
                                            {center.x + std::cos(b) * *light.radius, center.y,
                                             center.z + std::sin(b) * *light.radius},
                                            ui::with_alpha(color, 120)});
                }
            }
        }
    for (const auto& effect : scene.effects()) {
        if (!effect.dummy_id) continue;
        const auto dummy = std::ranges::find_if(
            scene.dummies(), [&](const auto& value) { return value.id == effect.dummy_id; });
        if (dummy == scene.dummies().end() || !dummy->position) continue;
        const auto color = ui::viewport_color(ui::Viewport::effect);
        result.points.push_back({Kind::effect, effect.source.entry_index,
                                 rws_point(*dummy->position), effect.name.value_or("Effect"),
                                 color});
        append_orientation(Kind::effect, effect.source.entry_index, *dummy->position,
                           dummy->heading.value_or(0), dummy->pitch.value_or(0), color, 90.0F);
    }
    return result;
}

void append_cutscene_camera_overlays(
    const csf::MissionScene& scene,
    const std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>>& cutscenes,
    MissionOverlays& output) {
    using Kind = GeometryPreview::MissionOverlayKind;
    std::set<std::uint32_t> added;
    for (const auto& [path, timeline] : cutscenes)
        for (const auto& script : timeline.scripts())
            for (const auto& action : script.actions) {
                if (action.kind != csf::CutsceneActionKind::camera || !action.numeric_value)
                    continue;
                const auto id = static_cast<std::int32_t>(*action.numeric_value);
                const auto dummy = std::ranges::find_if(
                    scene.dummies(), [&](const auto& value) { return value.id == id; });
                if (dummy == scene.dummies().end() || !dummy->position ||
                    !added.insert(dummy->source.entry_index).second)
                    continue;
                const float heading = dummy->heading.value_or(0);
                const float pitch = dummy->pitch.value_or(0);
                const auto origin = rws_point(*dummy->position);
                const rws::Vec3 forward{std::sin(heading) * std::cos(pitch), -std::sin(pitch),
                                        std::cos(heading) * std::cos(pitch)};
                const rws::Vec3 right{std::cos(heading), 0, -std::sin(heading)};
                constexpr float length = 300.0F, width = 100.0F;
                const rws::Vec3 tip{origin.x + forward.x * length, origin.y + forward.y * length,
                                    origin.z + forward.z * length};
                const rws::Vec3 left{tip.x - right.x * width, tip.y, tip.z - right.z * width};
                const rws::Vec3 right_tip{tip.x + right.x * width, tip.y, tip.z + right.z * width};
                const auto color = ui::viewport_color(ui::Viewport::cutscene_camera);
                output.points.push_back({Kind::cutscene_camera, dummy->source.entry_index, origin,
                                         dummy->name.value_or("Cutscene camera") + " [" +
                                             path_utf8(path.filename()) + "]",
                                         color});
                output.lines.push_back(
                    {Kind::cutscene_camera, dummy->source.entry_index, origin, tip, color});
                output.lines.push_back(
                    {Kind::cutscene_camera, dummy->source.entry_index, origin, left, color});
                output.lines.push_back(
                    {Kind::cutscene_camera, dummy->source.entry_index, origin, right_tip, color});
                output.lines.push_back(
                    {Kind::cutscene_camera, dummy->source.entry_index, left, right_tip, color});
            }
}

void append_actor_collision_overlays(const csf::MissionScene& scene,
                                     const std::vector<csf::ActorAssociation>& associations,
                                     MissionOverlays& output) {
    using Kind = GeometryPreview::MissionOverlayKind;
    std::unordered_map<std::string, std::shared_ptr<const csf::CmoDocument>> cmo_cache;
    std::unordered_map<std::string, std::shared_ptr<const rws::Document>> physics_cache;
    const auto place = [&](const csf::MissionActor& actor, const rws::Vec3 local) {
        const float h = csf::mission_actor_angle_radians(actor.heading.value_or(0));
        const float p = csf::mission_actor_angle_radians(actor.pitch.value_or(0));
        const float cy = std::cos(h), sy = std::sin(h), cp = std::cos(p), sp = std::sin(p);
        const auto origin = rws_point(*scene.actor_spawn_position(actor));
        return rws::Vec3{origin.x + cy * local.x + sy * sp * local.y + sy * cp * local.z,
                         origin.y + cp * local.y - sp * local.z,
                         origin.z - sy * local.x + cy * sp * local.y + cy * cp * local.z};
    };
    const auto add_box = [&](const csf::MissionActor& actor, const std::uint32_t entry,
                             const rws::Vec3 center, const rws::Vec3 extent, const Kind kind,
                             const ImU32 color) {
        const std::array<rws::Vec3, 8> p{
            {{center.x - extent.x, center.y - extent.y, center.z - extent.z},
             {center.x + extent.x, center.y - extent.y, center.z - extent.z},
             {center.x - extent.x, center.y + extent.y, center.z - extent.z},
             {center.x + extent.x, center.y + extent.y, center.z - extent.z},
             {center.x - extent.x, center.y - extent.y, center.z + extent.z},
             {center.x + extent.x, center.y - extent.y, center.z + extent.z},
             {center.x - extent.x, center.y + extent.y, center.z + extent.z},
             {center.x + extent.x, center.y + extent.y, center.z + extent.z}}};
        constexpr std::array<std::array<int, 2>, 12> edges{{{0, 1},
                                                            {0, 2},
                                                            {0, 4},
                                                            {1, 3},
                                                            {1, 5},
                                                            {2, 3},
                                                            {2, 6},
                                                            {3, 7},
                                                            {4, 5},
                                                            {4, 6},
                                                            {5, 7},
                                                            {6, 7}}};
        for (const auto& edge : edges)
            output.lines.push_back(
                {kind, entry, place(actor, p[edge[0]]), place(actor, p[edge[1]]), color});
    };
    for (std::size_t actor_index = 0;
         actor_index < scene.actors().size() && actor_index < associations.size(); ++actor_index) {
        const auto& actor = scene.actors()[actor_index];
        const auto& association = associations[actor_index];
        if (!scene.actor_spawn_position(actor)) continue;
        if (association.collision_models.size() == 1 &&
            association.collision_models.front().resolved_path) {
            const auto path = *association.collision_models.front().resolved_path;
            const auto key = csf::ResourceIndex::normalize_path(path);
            auto found = cmo_cache.find(key);
            if (found == cmo_cache.end()) try {
                    found = cmo_cache
                                .emplace(key, std::make_shared<csf::CmoDocument>(
                                                  csf::CmoDocument::load(path)))
                                .first;
                } catch (const std::exception&) {}
            if (found != cmo_cache.end())
                for (const auto& shape : found->second->shapes()) {
                    if (shape.bone_index) continue;
                    rws::Vec3 center{};
                    if (shape.center) center = {shape.center->x, shape.center->y, shape.center->z};
                    if (shape.offset) {
                        center.x += shape.offset->x;
                        center.y += shape.offset->y;
                        center.z += shape.offset->z;
                    }
                    if (shape.kind == csf::CmoShapeKind::box ||
                        shape.kind == csf::CmoShapeKind::ellipsoid) {
                        rws::Vec3 extent{0.25F, 0.25F, 0.25F};
                        if (shape.dimensions)
                            extent = {shape.dimensions->x * .5F, shape.dimensions->y * .5F,
                                      shape.dimensions->z * .5F};
                        add_box(actor, actor.source.entry_index, center, extent, Kind::actor_cmo,
                                ui::viewport_color(ui::Viewport::actor_collision));
                    } else {
                        const float radius = shape.radius.value_or(
                            shape.dimensions ? std::max({shape.dimensions->x, shape.dimensions->y,
                                                         shape.dimensions->z}) *
                                                   .5F
                                             : 0.25F);
                        constexpr int segments = 20;
                        for (int axis = 0; axis < 3; ++axis)
                            for (int i = 0; i < segments; ++i) {
                                const float a = static_cast<float>(i) * 6.283185307F / segments,
                                            b = static_cast<float>(i + 1) * 6.283185307F / segments;
                                const auto point = [&](float angle) {
                                    if (axis == 0)
                                        return rws::Vec3{center.x + std::cos(angle) * radius,
                                                         center.y,
                                                         center.z + std::sin(angle) * radius};
                                    if (axis == 1)
                                        return rws::Vec3{center.x + std::cos(angle) * radius,
                                                         center.y + std::sin(angle) * radius,
                                                         center.z};
                                    return rws::Vec3{center.x, center.y + std::cos(angle) * radius,
                                                     center.z + std::sin(angle) * radius};
                                };
                                output.lines.push_back({Kind::actor_cmo, actor.source.entry_index,
                                                        place(actor, point(a)),
                                                        place(actor, point(b)),
                                                        ui::viewport_color(ui::Viewport::actor_collision)});
                            }
                    }
                }
        }
        if (association.physics_models.size() == 1 &&
            association.physics_models.front().resolved_path) {
            const auto path = *association.physics_models.front().resolved_path;
            const auto key = csf::ResourceIndex::normalize_path(path);
            auto found = physics_cache.find(key);
            if (found == physics_cache.end()) try {
                    found = physics_cache
                                .emplace(key,
                                         std::make_shared<rws::Document>(rws::Document::load(path)))
                                .first;
                } catch (const std::exception&) {}
            if (found != physics_cache.end()) {
                const auto collect = [&](auto&& self,
                                         const std::vector<rws::Chunk>& chunks) -> void {
                    for (const auto& chunk : chunks) {
                        if (chunk.type == 0x907) {
                            const auto body =
                                rws::decode_physics_body_def(chunk, found->second->bytes());
                            if (body)
                                for (const auto& volume :
                                     rws::flatten_physics_volumes(body.value->volume)) {
                                    const auto& b = volume.world_bounds;
                                    if (b.valid)
                                        add_box(actor, actor.source.entry_index,
                                                {(b.minimum.x + b.maximum.x) * .5F,
                                                 (b.minimum.y + b.maximum.y) * .5F,
                                                 (b.minimum.z + b.maximum.z) * .5F},
                                                {(b.maximum.x - b.minimum.x) * .5F,
                                                 (b.maximum.y - b.minimum.y) * .5F,
                                                 (b.maximum.z - b.minimum.z) * .5F},
                                                Kind::actor_physics, ui::viewport_color(ui::Viewport::actor_physics));
                                }
                        }
                        self(self, chunk.children);
                    }
                };
                collect(collect, found->second->chunks());
            }
        }
    }
}

} // namespace rwsman
