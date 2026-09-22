#pragma once

#include "csf/document.hpp"
#include "csf/mission_scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rwsman {

// Finds the projected mission record for a CSFFBS entry and reports its kind
// and display label. Returns null when the entry is not a projected record.
const csf::CsfSourceId* find_mission_source(const csf::MissionScene& scene, std::uint32_t entry,
                                            std::string& kind, std::string& label);
const csf::Node* find_csf_node(const std::vector<csf::Node>& nodes, std::uint32_t entry);

} // namespace rwsman
