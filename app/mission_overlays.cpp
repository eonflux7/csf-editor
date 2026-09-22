#include "mission_overlays.hpp"
#include "app_util.hpp"

#include "csf/cmo.hpp"
#include "rws/decoded.hpp"
#include "ui/theme.hpp"
#include "rws/physics_inspection.hpp"
#include "rwsman/viewport_overlays.hpp"

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
    const auto facing = [](const float heading, const float pitch) {
        return rws::Vec3{std::sin(heading) * std::cos(pitch), -std::sin(pitch),
                         std::cos(heading) * std::cos(pitch)};
    };
    const auto relate = [&](const std::uint32_t a, const std::uint32_t b) {
        if (a != b) result.relations.emplace_back(a, b);
    };
    // Things an area can contain, for area membership relations.
    struct Placed {
        std::uint32_t entry;
        csf::Vec3 position;
    };
    std::vector<Placed> placed;

    std::map<std::pair<std::int32_t, std::int32_t>, std::pair<csf::Vec3, std::uint32_t>> nav_points;
    std::map<std::int32_t, std::string> group_names;
    for (const auto& group : scene.navigation()) {
        const auto type = group.type.value_or(0);
        const char* type_name = type == 0 ? "ground" : type == 1 ? "climb" : "special";
        std::string name = group.name && !group.name->empty()
                               ? *group.name
                               : "Group " + (group.id ? std::to_string(*group.id) : std::string("?"));
        name += std::string(" (") + type_name + ")";
        if (group.id) group_names[*group.id] = name;
        const auto color = type == 0   ? ui::viewport_color(ui::Viewport::nav_ground)
                           : type == 1 ? ui::viewport_color(ui::Viewport::nav_climb)
                                       : ui::viewport_color(ui::Viewport::nav_special);
        for (const auto& point : group.points)
            if (point.position && point.group_id && point.id) {
                nav_points[{*point.group_id, *point.id}] = {*point.position, point.source.entry_index};
                GeometryPreview::MissionOverlayPoint marker{
                    Kind::navigation_point, point.source.entry_index, rws_point(*point.position),
                    point.name.value_or("Nav point"), color, name};
                if (point.heading || point.pitch)
                    marker.heading = facing(point.heading.value_or(0), point.pitch.value_or(0));
                result.points.push_back(std::move(marker));
                placed.push_back({point.source.entry_index, *point.position});
            }
    }
    for (const auto& actor : scene.actors())
        if (const auto spawn = scene.actor_spawn_position(actor)) {
            std::string sublayer = actor.faction && !actor.faction->empty()
                                       ? "Faction: " + *actor.faction
                                   : actor.class_id ? "Class " + std::to_string(*actor.class_id)
                                                    : std::string("Unclassified");
            GeometryPreview::MissionOverlayPoint marker{
                Kind::actor, actor.source.entry_index, rws_point(*spawn), actor.name.value_or("Actor"),
                ui::viewport_color(ui::Viewport::actor), std::move(sublayer)};
            marker.heading = facing(csf::mission_actor_angle_radians(actor.heading.value_or(0)),
                                    csf::mission_actor_angle_radians(actor.pitch.value_or(0)));
            result.points.push_back(std::move(marker));
            placed.push_back({actor.source.entry_index, *spawn});
            if (actor.group && actor.cell)
                if (const auto found = nav_points.find({*actor.group, *actor.cell});
                    found != nav_points.end())
                    relate(actor.source.entry_index, found->second.second);
        }
    // A link and its reverse become one undirected line.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> link_lines;
    auto add_connection = [&](const csf::NavConnection& connection) {
        if (!connection.valid) return;
        const auto origin = nav_points.find({*connection.origin_group, *connection.origin_point});
        const auto destination =
            nav_points.find({*connection.destination_group, *connection.destination_point});
        if (origin == nav_points.end() || destination == nav_points.end()) return;
        const auto entry = connection.source.entry_index;
        const auto from = origin->second.second, to = destination->second.second;
        relate(entry, from);
        relate(entry, to);
        if (const auto reverse = link_lines.find({to, from}); reverse != link_lines.end()) {
            auto& line = result.lines[reverse->second];
            if (!line.partner_entry) {
                line.partner_entry = entry;
                line.directed = false;
                relate(line.source_entry, entry);
                return;
            }
        }
        link_lines[{from, to}] = result.lines.size();
        const auto group = group_names.find(*connection.origin_group);
        GeometryPreview::MissionOverlayLine line{Kind::navigation_connection, entry,
                                                 rws_point(origin->second.first),
                                                 rws_point(destination->second.first),
                                                 ui::viewport_color(ui::Viewport::nav_link), true};
        if (group != group_names.end()) line.sublayer = group->second;
        result.lines.push_back(std::move(line));
    };
    for (const auto& group : scene.navigation())
        for (const auto& connection : group.connections)
            add_connection(connection);
    for (const auto& connection : scene.cross_group_connections())
        add_connection(connection);
    for (const auto& dummy : scene.dummies())
        if (dummy.position) {
            GeometryPreview::MissionOverlayPoint marker{
                Kind::dummy, dummy.source.entry_index, rws_point(*dummy.position),
                dummy.name.value_or("Dummy"), ui::viewport_color(ui::Viewport::dummy)};
            marker.heading = facing(dummy.heading.value_or(0), dummy.pitch.value_or(0));
            result.points.push_back(std::move(marker));
            placed.push_back({dummy.source.entry_index, *dummy.position});
        }
    for (const auto& light : scene.lights())
        if (light.position) {
            GeometryPreview::MissionOverlayPoint marker{
                Kind::light, light.source.entry_index, rws_point(*light.position),
                light.name.value_or("Light"), ui::rgb_u32(light.color.value_or(0xFFF591U))};
            if (light.radius && *light.radius > 0 && std::isfinite(*light.radius))
                marker.radius = *light.radius;
            result.points.push_back(std::move(marker));
            placed.push_back({light.source.entry_index, *light.position});
        }
    for (const auto& effect : scene.effects()) {
        if (!effect.dummy_id) continue;
        const auto dummy = std::ranges::find_if(
            scene.dummies(), [&](const auto& value) { return value.id == effect.dummy_id; });
        if (dummy == scene.dummies().end() || !dummy->position) continue;
        GeometryPreview::MissionOverlayPoint marker{
            Kind::effect, effect.source.entry_index, rws_point(*dummy->position),
            effect.name.value_or("Effect"), ui::viewport_color(ui::Viewport::effect),
            effect.class_id ? "Class " + std::to_string(*effect.class_id) : std::string{}};
        marker.heading = facing(dummy->heading.value_or(0), dummy->pitch.value_or(0));
        result.points.push_back(std::move(marker));
        relate(effect.source.entry_index, dummy->source.entry_index);
    }
    for (const auto& area : scene.areas()) {
        if (area.points.size() < 2) continue;
        const auto entry = area.source.entry_index;
        const auto edge = ui::viewport_color(ui::Viewport::area);
        const bool has_height = area.height && std::isfinite(*area.height) && *area.height != 0;
        const float height = has_height ? *area.height : 0.0F;
        const auto raised = [&](const csf::Vec3& p) { return rws::Vec3{p.x, p.y + height, p.z}; };
        for (std::size_t i = 0; i < area.points.size(); ++i) {
            const auto& a = area.points[i];
            const auto& b = area.points[(i + 1) % area.points.size()];
            result.lines.push_back({Kind::area, entry, rws_point(a), rws_point(b), edge});
            if (!has_height) continue;
            result.lines.push_back({Kind::area, entry, raised(a), raised(b),
                                    ui::viewport_color(ui::Viewport::area, 0.71F)});
            result.lines.push_back({Kind::area, entry, rws_point(a), raised(a),
                                    ui::viewport_color(ui::Viewport::area, 0.52F)});
            // Side wall.
            const auto fill = ui::viewport_color(ui::Viewport::area_fill);
            result.faces.push_back({Kind::area, entry, rws_point(a), rws_point(b), raised(b), fill});
            result.faces.push_back({Kind::area, entry, rws_point(a), raised(b), raised(a), fill});
        }
        // Floor and roof caps, triangulated on the X/Z plane.
        std::vector<Point2> outline;
        outline.reserve(area.points.size());
        for (const auto& p : area.points) outline.push_back({p.x, p.z});
        const auto fill = ui::viewport_color(ui::Viewport::area_fill);
        for (const auto& t : triangulate_polygon(outline)) {
            const auto &a = area.points[t[0]], &b = area.points[t[1]], &c = area.points[t[2]];
            result.faces.push_back({Kind::area, entry, rws_point(a), rws_point(b), rws_point(c), fill});
            if (has_height)
                result.faces.push_back({Kind::area, entry, raised(a), raised(b), raised(c), fill});
        }
        // Membership: inside the outline and within the vertical extent (a flat
        // area counts everything above or below it).
        float low = area.points.front().y, high = low;
        for (const auto& p : area.points) {
            low = std::min(low, p.y);
            high = std::max(high, p.y);
        }
        if (has_height) {
            low = std::min(low, low + height);
            high = std::max(high, high + height);
        }
        for (const auto& item : placed) {
            if (has_height && (item.position.y < low - 1.0F || item.position.y > high + 1.0F))
                continue;
            if (point_in_polygon(outline, item.position.x, item.position.z)) relate(entry, item.entry);
        }
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
                const auto cutscene = path_utf8(path.filename());
                output.points.push_back({Kind::cutscene_camera, dummy->source.entry_index, origin,
                                         dummy->name.value_or("Cutscene camera") + " [" + cutscene +
                                             "]",
                                         color, cutscene, forward});
                for (const auto& [a, b] : {std::pair{origin, tip}, std::pair{origin, left},
                                           std::pair{origin, right_tip}, std::pair{left, right_tip}}) {
                    GeometryPreview::MissionOverlayLine line{Kind::cutscene_camera,
                                                             dummy->source.entry_index, a, b, color};
                    line.detail = true;
                    line.sublayer = cutscene;
                    output.lines.push_back(std::move(line));
                }
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
