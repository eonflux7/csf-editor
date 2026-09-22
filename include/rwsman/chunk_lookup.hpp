#pragma once

#include "rws/decoded.hpp"
#include "rws/document.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace rwsman {

using ChunkDisplayNames = std::unordered_map<std::uint64_t, std::string>;

const rws::Chunk* find_chunk(const std::vector<rws::Chunk>& chunks, std::uint64_t offset);
const rws::SceneInstance* find_instance(std::span<const rws::SceneInstance> instances,
                                        std::uint64_t offset);
const rws::Chunk* find_first_chunk(const std::vector<rws::Chunk>& chunks, std::uint32_t type);
const rws::Chunk* find_enclosing_clump(const std::vector<rws::Chunk>& chunks,
                                       std::uint64_t offset, const rws::Chunk* clump = nullptr);
const rws::Chunk* find_owning_geometry(const std::vector<rws::Chunk>& chunks, std::uint64_t offset,
                                       const rws::Chunk* geometry = nullptr);
const rws::Chunk* find_preview_geometry(const rws::Chunk& selected,
                                        const std::vector<rws::Chunk>& all_chunks);
const rws::Chunk* find_owning_object(const std::vector<rws::Chunk>& chunks, std::uint64_t offset,
                                     const rws::Chunk* owner = nullptr);

ChunkDisplayNames resolve_chunk_display_names(const std::vector<rws::Chunk>& chunks,
                                              std::span<const std::byte> bytes,
                                              std::span<const rws::SceneInstance> instances);

} // namespace rwsman
