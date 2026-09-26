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

using MissionOverlays = GeometryPreview::MissionOverlaySet;

[[nodiscard]] rws::Vec3 rws_point(csf::Vec3 value);
// Actor markers take their kind's colour (player, enemy, prop...) when the
// class database is given.
[[nodiscard]] MissionOverlays make_mission_overlays(const csf::MissionScene& scene,
                                                    const csf::ObjectDatabase* objects = nullptr);
void append_cutscene_camera_overlays(
    const csf::MissionScene& scene,
    const std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>>& cutscenes,
    MissionOverlays& output);
void append_actor_collision_overlays(const csf::MissionScene& scene,
                                     const std::vector<csf::ActorAssociation>& associations,
                                     MissionOverlays& output);

} // namespace rwsman
