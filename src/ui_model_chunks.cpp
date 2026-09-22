#include "rwsman/chunk_lookup.hpp"

#include <algorithm>
#include <optional>

namespace rwsman {

const rws::Chunk* find_chunk(const std::vector<rws::Chunk>& chunks, const std::uint64_t offset) {
    for (const auto& chunk : chunks) {
        if (chunk.offset == offset) return &chunk;
        if (const auto* child = find_chunk(chunk.children, offset)) return child;
    }
    return nullptr;
}

const rws::SceneInstance* find_instance(const std::span<const rws::SceneInstance> instances,
                                        const std::uint64_t offset) {
    const auto found = std::find_if(
        instances.begin(), instances.end(),
        [offset](const rws::SceneInstance& instance) { return instance.offset == offset; });
    return found == instances.end() ? nullptr : &*found;
}

const rws::Chunk* find_first_chunk(const std::vector<rws::Chunk>& chunks,
                                   const std::uint32_t type) {
    for (const auto& chunk : chunks) {
        if (chunk.type == type) return &chunk;
        if (const auto* child = find_first_chunk(chunk.children, type)) return child;
    }
    return nullptr;
}

const rws::Chunk* find_enclosing_clump(const std::vector<rws::Chunk>& chunks,
                                       const std::uint64_t offset,
                                       const rws::Chunk* clump) {
    for (const auto& chunk : chunks) {
        const auto* current = chunk.type == 0x10 ? &chunk : clump;
        if (chunk.offset == offset) return current;
        if (const auto* found = find_enclosing_clump(chunk.children, offset, current)) return found;
    }
    return nullptr;
}


const rws::Chunk* find_preview_geometry(const rws::Chunk& selected,
                                        const std::vector<rws::Chunk>& all_chunks) {
    if (selected.type == 0x0F) return &selected;
    if (selected.type == 0x10 || selected.type == 0x1A)
        return find_first_chunk(selected.children, 0x0F);
    return find_owning_geometry(all_chunks, selected.offset, nullptr);
}

const rws::Chunk* find_owning_object(const std::vector<rws::Chunk>& chunks,
                                     const std::uint64_t offset,
                                     const rws::Chunk* owner) {
    for (const auto& chunk : chunks) {
        if (chunk.offset == offset) return owner;
        const auto* child_owner = chunk.type == 0x03 ? owner : &chunk;
        if (const auto* found = find_owning_object(chunk.children, offset, child_owner))
            return found;
    }
    return nullptr;
}

const rws::Chunk* find_owning_geometry(const std::vector<rws::Chunk>& chunks,
                                       const std::uint64_t offset,
                                       const rws::Chunk* geometry) {
    for (const auto& chunk : chunks) {
        const auto* current = chunk.type == 0x0F ? &chunk : geometry;
        if (chunk.offset == offset) return current;
        if (const auto* found = find_owning_geometry(chunk.children, offset, current)) return found;
    }
    return nullptr;
}


std::optional<rws::PyroExtensionInfo> decode_pyro_metadata(const rws::Chunk& owner,
                                                           const std::span<const std::byte> bytes) {
    const auto* extension = rws::find_child(owner, 0x03);
    const auto* metadata = extension ? rws::find_child(*extension, 0xFFFFFF00U) : nullptr;
    if (!metadata) return std::nullopt;
    const auto decoded = rws::decode_pyro_extension(*metadata, owner.type, bytes);
    return decoded ? decoded.value : std::nullopt;
}

ChunkDisplayNames resolve_chunk_display_names(const std::vector<rws::Chunk>& chunks,
                                              const std::span<const std::byte> bytes,
                                              const std::span<const rws::SceneInstance> instances) {
    ChunkDisplayNames names;
    std::unordered_map<std::uint32_t, std::string> prototype_names;
    for (const auto& instance : instances) {
        if (!instance.prototype_name.empty())
            prototype_names.try_emplace(instance.prototype_id, instance.prototype_name);
    }

    auto visit = [&](auto&& self, const std::vector<rws::Chunk>& siblings) -> void {
        for (const auto& chunk : siblings) {
            if (chunk.type == 0x06) {
                const auto texture = rws::decode_texture(chunk, bytes);
                if (texture && !texture.value->name.empty())
                    names.emplace(chunk.offset, texture.value->name);
            } else if (chunk.type == 0x07 || chunk.type == 0x14) {
                const auto metadata = decode_pyro_metadata(chunk, bytes);
                if (metadata && !metadata->object_name().empty())
                    names.emplace(chunk.offset, metadata->object_name());
            } else if (chunk.type == 0x10) {
                for (const auto& child : chunk.children) {
                    if (child.type != 0x14) continue;
                    const auto metadata = decode_pyro_metadata(child, bytes);
                    if (!metadata) continue;
                    if (!metadata->object_name().empty()) {
                        names.emplace(chunk.offset, metadata->object_name());
                        break;
                    }
                    const auto object_index = metadata->atomic_object_index();
                    if (!object_index) continue;
                    const auto found = prototype_names.find(1000U + *object_index);
                    if (found != prototype_names.end()) {
                        names.emplace(chunk.offset, found->second);
                        break;
                    }
                }
            } else if (chunk.type == 0x0E) {
                const auto* metadata_chunk = find_first_chunk(chunk.children, 0xFFFFFF00U);
                if (metadata_chunk) {
                    const auto metadata = rws::decode_pyro_extension(*metadata_chunk, 0x0E, bytes);
                    if (metadata && !metadata.value->object_name().empty())
                        names.emplace(chunk.offset, metadata.value->object_name());
                }
            }
            self(self, chunk.children);
        }
    };
    visit(visit, chunks);
    return names;
}

} // namespace rwsman
