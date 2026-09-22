#pragma once

#include "csf/animation_catalog.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "csf/program.hpp"
#include "rwsman/chunk_lookup.hpp"
#include "rwsman/search_index.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rwsman {

// Text of a program operand value ("" for none), shared by the listing and the
// script search haystack.
[[nodiscard]] std::string program_operand_text(const csf::ProgramOperand& operand);

void index_chunks(SearchIndex& index, const std::vector<rws::Chunk>& chunks,
                  const ChunkDisplayNames& names);
void index_scene_instances(SearchIndex& index, std::span<const rws::SceneInstance> instances);

struct MissionIndexInputs {
    const csf::MissionScene* scene{};
    const csf::MissionGraph* graph{};
    const csf::ObjectDatabase* objects{};
    const csf::AnimationCatalog* animations{};
    const std::vector<std::pair<std::filesystem::path, csf::ProgramDocument>>* programs{};
};

// Adds actors, navigation, spatial records, effects, scripts, variables, BDD
// classes, animations, and mission resources.
void index_mission(SearchIndex& index, const MissionIndexInputs& inputs);

} // namespace rwsman
