#pragma once

#include "rws/map_assembly.hpp"
#include "rws/world_model.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rws {

// `.csfworld`: the text interchange a modelling tool (tools/blender/
// export_csf_world.py) writes and `csf-mod world-build` compiles into a visual
// World and a collision World. Coordinates are game units (centimetres,
// source Y up); faces are counter-clockwise seen from their front.
//
//   csfworld 1
//   material <texture> <surface> [<shade 0-255> [<lightmap>]]   (declaration order = index)
//   v <x> <y> <z> <nx> <ny> <nz> <u0> <v0> [<u1> <v1>]
//   f <a> <b> <c> <material> <visual|collision|both>
//   prop <donor-instance-id>[,<id>...] <x> <y> <z> [<yaw-degrees>]
//   piece <x0> <y0> <z0> <x1> <y1> <z1> <x> <y> <z> [<yaw-degrees>]
//
// A prop copies a donor scene instance (and its Clump); a piece copies the
// donor Worlds' own triangles lying wholly inside a box, visual and collision,
// with their donor materials. Both are turned about +Y and moved so the bottom
// centre of their donor bounds (a piece: of its box) lands on <x> <y> <z>.
//
// Lines starting with '#' and blank lines are ignored. Texture names select a
// donor visual material, surfaces a donor collision material by its Pyro name.

struct WorldSourceMaterial {
    std::string texture;
    std::string surface;
    std::uint8_t shade{228};  // collision per-triangle byte; the most common shipped value
    // A lightmap texture of the World's own (e.g. TERRAIN_Lm): the donor
    // material is copied with its Material Effects lightmap renamed, and the
    // vertices' second UV set samples it. Empty: the donor's lightmap.
    std::string lightmap;
};

struct WorldSourceFace {
    std::array<std::uint32_t, 3> vertices{};
    std::uint32_t material{};
    bool visual{true}, collision{true};
};

struct WorldPiece {
    Vec3 inf, sup;  // donor box
    Vec3 position;
    float yaw_degrees{};
};

struct WorldSource {
    std::vector<WorldSourceMaterial> materials;
    std::vector<WorldBuildVertex> vertices;
    std::vector<bool> has_second_uv;  // per vertex: the v line gave u1 v1
    // Per vertex: the position as written, before rounding to float (the
    // sector map's planes are computed from these).
    std::vector<std::array<double, 3>> exact_positions;
    std::vector<WorldSourceFace> faces;
    std::vector<PlacedProp> props;
    std::vector<WorldPiece> pieces;
};

[[nodiscard]] DecodeResult<WorldSource> parse_world_source(std::string_view text);

struct WorldCompileOptions {
    std::size_t max_sector_triangles{1024};
    // Second UV set for every visual vertex that has none: the donor materials
    // keep their dual-pass lightmap, so this picks one lightmap texel.
    bool constant_lightmap_uv{true};
    std::array<float, 2> lightmap_uv{0.5F, 0.5F};
};

struct CompiledWorlds {
    WorldModel visual, collision;
    std::vector<std::string> notes;  // what was resolved from where
};

// Materials are copied from the donor Worlds (the map .rws and its _col.rws):
// visual materials by texture name, collision materials by surface name.
// `extra_collision` (e.g. assembled prop collision) is added to the collision
// World as is; its materials index the donor collision Material List.
[[nodiscard]] DecodeResult<CompiledWorlds>
compile_world_source(const WorldSource& source, const WorldModel& donor_visual,
                     const WorldModel& donor_collision, const WorldCompileOptions& options = {},
                     std::span<const WorldBuildTriangle> extra_collision = {});

// Complete map files for a World source: `map` is <map>.rws (the placed props'
// Clumps and instance records, or with `keep_donor_props` the donor's, then
// the visual World) and `collision` is <map>_col.rws. Materials, props and
// pieces come from the donor files. Both results are checked to recover as
// complete through the viewer's reader.
struct BuiltMap {
    std::vector<std::byte> map, collision;
    std::vector<std::string> notes;
    std::uint32_t visual_triangles{}, visual_sectors{}, collision_triangles{}, collision_sectors{};
};
[[nodiscard]] DecodeResult<BuiltMap> build_map_files(const WorldSource& source,
                                                     std::span<const std::byte> donor_map,
                                                     std::span<const std::byte> donor_collision,
                                                     const WorldCompileOptions& options = {},
                                                     bool keep_donor_props = false);

// `.csfworld` text for a source; parsing it gives the same source (positions
// keep their exact values).
[[nodiscard]] std::string write_world_source(const WorldSource& source);

// Appends `part` to `target`: its vertices, faces (materials merged by
// texture, surface and shade), props and pieces. With `placed`, the part's
// vertices and normals are turned `yaw_degrees` about +Y (as props and pieces
// are) and moved by `offset`.
struct WorldSourcePlacement {
    Vec3 offset;
    float yaw_degrees{};
};
void append_world_source(WorldSource& target, const WorldSource& part,
                         const std::optional<WorldSourcePlacement>& placed = std::nullopt);

// A Material (0x07) chunk with the texture of its Material Effects (0x120)
// dual pass, the lightmap, renamed; fails when it has none.
[[nodiscard]] DecodeResult<std::vector<std::byte>> replace_material_lightmap(std::span<const std::byte> material,
                                                                             std::string_view lightmap);
[[nodiscard]] std::string material_lightmap_name(std::span<const std::byte> material);

// Texture name of a Material (0x07) chunk, or its Pyro surface name.
[[nodiscard]] std::string material_texture_name(std::span<const std::byte> material);
[[nodiscard]] std::string material_surface_name(std::span<const std::byte> material);

} // namespace rws
