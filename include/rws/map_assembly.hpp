#pragma once

#include "rws/document.hpp"
#include "rws/world_model.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rws {

// Scene-instance record (0x16FC0) bytes, the inverse of Document's reader
// (writer FUN_006C47C0; rws-format.md, "CSF scene-instance record").
[[nodiscard]] std::vector<std::byte> encode_scene_instance(const SceneInstance& instance,
                                                           std::uint32_t library_id);

// A vanilla prop placed on a new map: one or more of the donor map's scene
// instances (e.g. a tree's trunk and canopy), turned `yaw_degrees` about +Y
// from their donor orientation and moved together so the bottom centre of
// their combined placed bounds lands on `position`.
struct PlacedProp {
    std::vector<std::uint32_t> donor_instance_ids;
    Vec3 position;
    float yaw_degrees{};
};

struct AssembledProps {
    std::vector<std::byte> prefix;  // the used Clumps, then the instance records
    std::vector<WorldBuildTriangle> collision;
    std::vector<std::string> notes;
};

// Copies each placed prop's Clump (once per prototype, in donor order) and
// writes its instance record. Shipped prop collision is baked into _col.rws,
// so the donor collision triangles lying inside the donor instance's bounds
// are moved with the prop and returned for the new collision World.
[[nodiscard]] DecodeResult<AssembledProps> assemble_props(const Document& donor_map,
                                                          const WorldModel& donor_collision,
                                                          std::span<const PlacedProp> props);

} // namespace rws
