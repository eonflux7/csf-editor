#pragma once

#include "csf/animation_catalog.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "geometry_preview.hpp"

#include <filesystem>
#include <utility>
#include <vector>

namespace rwsman {

struct MissionOverlays {
    std::vector<GeometryPreview::MissionOverlayPoint> points;
    std::vector<GeometryPreview::MissionOverlayLine> lines;
};

[[nodiscard]] rws::Vec3 rws_point(csf::Vec3 value);
[[nodiscard]] MissionOverlays make_mission_overlays(const csf::MissionScene& scene);
void append_cutscene_camera_overlays(
    const csf::MissionScene& scene,
    const std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>>& cutscenes,
    MissionOverlays& output);
void append_actor_collision_overlays(const csf::MissionScene& scene,
                                     const std::vector<csf::ActorAssociation>& associations,
                                     MissionOverlays& output);

} // namespace rwsman
