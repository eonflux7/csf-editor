#pragma once

#include "rws/decoded.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rws {

// An editable World (`0x0B`): parsed in the order the stream is read, and
// written back from its parts. Unlike the read-only recovered view
// (world_recovery.hpp) it owns its data, and unlike the Chunk tree it follows
// physical sizes, so the Pyro World Sector size overstatement
// (rws-format.md, "Pyro World Sector per-vertex data and size defect") is
// modelled instead of tolerated.

// One plug-in chunk inside an Extension, kept opaque.
struct WorldPlugin {
    std::uint32_t type{};
    std::vector<std::byte> payload;
};

inline constexpr std::uint32_t pyro_metadata_chunk = 0xFFFFFF00U;

// Pyro `0xFFFFFF00` on a World Sector: version, presence word, then one byte
// per triangle when present (KB-world-geometry-4).
struct PyroSectorMetadata {
    std::uint32_t version{1};
    bool present{};
    std::vector<std::uint8_t> triangle_bytes;
};

// World Sector triangle in stream order: three vertex indices, then the
// material index relative to the sector's material window.
struct WorldTriangle {
    std::array<std::uint16_t, 3> vertices{};
    std::uint16_t material{};
};

struct WorldModelSector {
    std::int32_t material_window_base{};
    Vec3 bounding_box_inf, bounding_box_sup;
    std::uint32_t collision_sector_present{};
    std::uint32_t unused{};
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> normals;   // packed, present when format & 0x10
    std::vector<std::uint32_t> prelight;  // RGBA, present when format & 0x08
    std::vector<std::vector<std::array<float, 2>>> texcoords;  // one array per set
    std::vector<WorldTriangle> triangles;
    std::vector<WorldPlugin> plugins;  // Extension children, in stream order
};

// A Plane Section; children are node indices into WorldModel::nodes.
struct WorldModelPlane {
    std::int32_t axis{};  // 0, 4, 8: source X, Y, Z
    float split{};
    float left_value{}, right_value{};
    std::size_t left{}, right{};
};

struct WorldModelNode {
    bool is_sector{};
    std::size_t index{};  // into planes or sectors
};

struct WorldModel {
    std::uint32_t library_id{};
    Vec3 inverse_origin;
    std::int32_t triangle_count{}, vertex_count{}, plane_sector_count{}, world_sector_count{};
    std::int32_t collision_sector_size{};
    std::uint32_t format{};
    Vec3 bounding_box_sup, bounding_box_inf;
    std::vector<std::byte> material_list;  // Material List (0x08) payload, opaque
    std::vector<WorldModelNode> nodes;     // nodes[0] is the root; preorder
    std::vector<WorldModelPlane> planes;
    std::vector<WorldModelSector> sectors;  // preorder leaf order
    std::vector<WorldPlugin> plugins;       // World Extension children
};

struct WorldWriteOptions {
    // Declare each World Sector's Pyro plug-in 4 bytes longer than written, as
    // the shipped exporter does (`0x006BF6C0` vs `0x006BEEB0`); every enclosing
    // size then includes the overstatement too. The reader never seeks a
    // matched plug-in, so both forms load (KB-world-geometry-5); authored
    // Worlds declare the actual size, and only round trips reproduce the defect.
    bool reproduce_pyro_size_overstatement{false};
};

// Parses the World chunk that begins at `offset`. On success `end` receives the
// physical end of the World in `bytes`.
[[nodiscard]] DecodeResult<WorldModel> parse_world_model(std::span<const std::byte> bytes,
                                                         std::uint64_t offset,
                                                         std::uint64_t* end = nullptr);
[[nodiscard]] std::vector<std::byte> write_world_model(const WorldModel& world,
                                                       const WorldWriteOptions& options = {});
[[nodiscard]] std::uint32_t world_texcoord_sets(std::uint32_t format) noexcept;
[[nodiscard]] DecodeResult<PyroSectorMetadata>
decode_pyro_sector_metadata(const WorldPlugin& plugin, std::size_t triangle_count);
[[nodiscard]] WorldPlugin encode_pyro_sector_metadata(const PyroSectorMetadata& metadata);
// Offset of the first top-level World in a map .rws, walking Clumps and
// scene-instance records (0x16FC0, whose payload runs 12 bytes past its size).
[[nodiscard]] std::optional<std::uint64_t> find_map_world(std::span<const std::byte> bytes);
// Checks node links and array sizes; returns one message per problem.
[[nodiscard]] std::vector<std::string> check_world_model(const WorldModel& world);
// Sets the header's aggregate counts from the parts (bounds are left alone).
void update_world_counts(WorldModel& world);

// Material List payloads: split into self-contained Material (0x07) chunks
// (a non-negative index entry is resolved to a copy of the chunk it names),
// and composed back into the all-inline form the shipped Worlds use.
[[nodiscard]] DecodeResult<std::vector<std::vector<std::byte>>>
split_material_list(std::span<const std::byte> payload, std::uint32_t library_id);
[[nodiscard]] std::vector<std::byte>
compose_material_list(std::span<const std::vector<std::byte>> materials, std::uint32_t library_id);

// The largest World Sector the u16 vertex indices and counts allow. Visual
// Worlds are built with it: the game occlusion-culls each visual sector's box
// against the scene's area occluders, and small sectors (1024 triangles)
// flickered at their borders on ST07A; shipped visual Worlds have 1-40
// sectors of ~30k triangles, collision Worlds <= 1024 (the builder's default).
inline constexpr std::size_t visual_sector_triangles = 0xFFFFU / 3U;

// Building a World from a triangle soup (game units, source Y up, triangles
// counter-clockwise seen from their front, as in the shipped Worlds).
struct WorldBuildVertex {
    Vec3 position;
    Vec3 normal;                                     // used when format & 0x10
    std::uint32_t prelight{0xFFFFFFFFU};             // used when format & 0x08
    std::array<std::array<float, 2>, 2> texcoords{};  // sets beyond the format's are ignored
};
struct WorldBuildTriangle {
    std::array<WorldBuildVertex, 3> vertices;
    std::uint16_t material{};  // index into the World Material List
    std::uint8_t pyro{};       // per-triangle Pyro byte (KB-world-geometry-4)
};
struct WorldBuildOptions {
    std::uint32_t library_id{0x1C020037U};  // RenderWare 3.7.0.2 build 55, all shipped maps
    // Shipped values: visual 0x400200B0 (normals, two UV sets), collision
    // 0x40000040. 0x40000000 is set in every shipped World and matches
    // RenderWare's overlapping-sectors flag, which the builder relies on.
    std::uint32_t format{0x40000040U};
    std::vector<std::byte> material_list;  // Material List payload
    std::size_t max_sector_triangles{1024};
    // Plug-ins every shipped World Sector carries: Bin Mesh (triangle list),
    // User Data FVF.UserData = 0x3003 (copied, meaning unexplained) and the Pyro
    // metadata; visual Worlds add Right To Render (MatFX pipeline) and MatFX
    // enabled. Pyro bytes are written only when some triangle has one.
    bool visual_plugins{};
};
// RenderWare Collision plug-in (0x11D) for a World Sector: version 0x37002 and
// an embedded Coll Tree (0x2C) over the sector's triangles (rws-format.md,
// reader 0x0056F030, writer 0x0056EEE0). Every shipped collision sector with
// triangles carries one; the layout below matches all 2323 shipped trees:
// header {flags = 1 (remap present), inf, sup, triangle count, split count},
// preorder splits of two 8-byte children {type = axis (0/4/8) | 1 on the left,
// contents = 0xFF for a child split (index = split) or the leaf's triangle
// count (index = first remap slot), value = left upper / right lower bound},
// then a u16 remap from leaf order to sector triangle index.
[[nodiscard]] WorldPlugin build_collision_tree(const WorldModelSector& sector,
                                               std::uint32_t library_id);

// The triangles of an existing World in build form (absolute materials,
// per-triangle Pyro bytes), e.g. to rebuild or merge it.
[[nodiscard]] std::vector<WorldBuildTriangle> world_build_triangles(const WorldModel& world);
[[nodiscard]] DecodeResult<WorldModel> build_world(std::span<const WorldBuildTriangle> triangles,
                                                   const WorldBuildOptions& options);

} // namespace rws
