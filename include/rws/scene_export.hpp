#pragma once

#include "rws/document.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rws {

struct SceneExportStats {
    std::uint64_t clumps{};
    std::uint64_t atomic_instances{};
    std::uint64_t custom_instances{};
    std::uint64_t unresolved_instances{};
    std::uint64_t world_sectors{};
    std::uint64_t recovered_world_sectors{};
    std::uint64_t recovered_world_vertices{};
    std::uint64_t recovered_world_triangles{};
    std::uint64_t materials{};
    std::uint64_t vertices{};
    std::uint64_t triangles{};
    std::uint64_t skipped{};
};

// An image file a glTF material can use for a texture name: its URI relative
// to the .gltf, and how the material treats its alpha.
struct SceneTexture {
    enum class Alpha { opaque, mask, blend };
    std::string uri;
    Alpha alpha{Alpha::opaque};
};
// Resolves a material's texture name; nullopt leaves the texture unbound.
using SceneTextureResolver = std::function<std::optional<SceneTexture>(std::string_view name)>;

// Exports the assembled, Y-up scene to glTF 2.0. Clump frame transforms are
// baked into vertex positions, World Sectors remain separate nodes, and UV0/
// UV1 are exported as TEXCOORD_0/TEXCOORD_1. A sibling manifest records the
// original RWS offsets and base/lightmap texture names for round-trip tooling.
// With `textures`, base textures are bound as baseColorTexture on TEXCOORD_0
// (alpha from the SceneTexture) and a lightmap's URI is the material's
// `lightmap_uri` extra, for TEXCOORD_1: glTF has no lightmap slot.
[[nodiscard]] SceneExportStats export_scene_gltf(const std::vector<Chunk>& chunks,
                                                 std::span<const SceneInstance> instances,
                                                 std::span<const std::byte> bytes,
                                                 const std::filesystem::path& output_path,
                                                 const SceneTextureResolver& textures = {});

// Exports every Atomic belonging to one Clump, with the same layout and
// manifest as the whole-scene export.
[[nodiscard]] SceneExportStats export_clump_gltf(const Chunk& clump,
                                                 std::span<const std::byte> bytes,
                                                 const std::filesystem::path& output_path,
                                                 const SceneTextureResolver& textures = {});

[[nodiscard]] SceneExportStats export_collision_gltf(const std::vector<Chunk>& chunks,
                                                     std::span<const std::byte> bytes,
                                                     const std::filesystem::path& output_path);

// Prototype id of a map Clump: 1000 + its first valid Pyro Atomic object
// index, the number scene-instance records name (FUN_006C3C60).
[[nodiscard]] std::optional<std::uint32_t> clump_prototype_id(const Chunk& clump,
                                                              std::span<const std::byte> bytes);

// Game-space triangles of a map Clump placed by a scene-instance record: the
// record Matrix replaces the Clump root frame (FUN_006C4090), as in the scene
// export and the viewport.
[[nodiscard]] std::vector<std::array<Vec3, 3>>
placed_clump_triangles(const Chunk& clump, std::span<const std::byte> bytes,
                       const SceneInstance& instance);

} // namespace rws
