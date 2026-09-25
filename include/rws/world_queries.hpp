#pragma once

#include "rws/world_source.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rws {

// `.sec` sector map for the collision faces of a World source (KB-scn-6,
// KB-scn-8): u32 version 0, u32 vertex count, vec3 vertices, u32 sector count,
// then per sector u32 entry count, u32 sector index, u32 0, a 0x2c plane
// {point, unit normal, cached normal, d, u8 flag 1, 3 pad} and one 12-byte
// directed half-edge {A, B, neighbour sector or 0xFFFFFFFF} per polygon vertex,
// in loop order. With flag 1 the game recomputes d from the point and normal.
//
// Vertices are merged by position (0.1 cm), adjacent triangles are paired into
// quads that are convex in XZ, and neighbour links join sectors across shared
// edges. Replaces hello world's build_sec.py, whose output it reproduces
// byte for byte (v13's Convoy.sec).
struct SectorMap {
    std::vector<std::byte> bytes;
    std::size_t vertex_count{}, sector_count{};
};
[[nodiscard]] SectorMap build_sector_map(const WorldSource& source);

// Heights of the collision faces of a World source over the XZ plane.
class GroundQuery {
public:
    explicit GroundQuery(const WorldSource& source);

    struct Hit {
        float height{};
        Vec3 normal;  // unit, pointing up
    };
    // The highest surface over (x, z), or the highest at or below `y` (plus a
    // 1 cm tolerance), so a query from inside a building finds its floor.
    [[nodiscard]] std::optional<Hit> highest(float x, float z) const;
    [[nodiscard]] std::optional<Hit> below(float x, float y, float z) const;

private:
    struct Triangle {
        Vec3 a, b, c;
    };
    [[nodiscard]] std::optional<Hit> query(float x, float z, std::optional<float> ceiling) const;

    std::vector<Triangle> triangles_;
    std::vector<std::vector<std::uint32_t>> cells_;
    float minimum_x_{}, minimum_z_{}, cell_size_{1.0F};
    std::size_t columns_{}, rows_{};
};

} // namespace rws
