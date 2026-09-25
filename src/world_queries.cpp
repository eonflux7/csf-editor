#include "rws/world_queries.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <utility>

namespace rws {
namespace {

using Point = std::array<double, 3>;

void append_u32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}

void append_f32(std::vector<std::byte>& out, const double value) {
    append_u32(out, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
}

bool convex_xz(const std::vector<Point>& vertices, const std::vector<std::uint32_t>& loop) {
    int sign = 0;
    const auto n = loop.size();
    for (std::size_t k = 0; k < n; ++k) {
        const auto& a = vertices[loop[k]];
        const auto& b = vertices[loop[(k + 1) % n]];
        const auto& c = vertices[loop[(k + 2) % n]];
        const auto cross = (b[0] - a[0]) * (c[2] - b[2]) - (b[2] - a[2]) * (c[0] - b[0]);
        if (std::abs(cross) < 1e-6) return false;
        const int s = cross > 0 ? 1 : -1;
        if (sign != 0 && s != sign) return false;
        sign = s;
    }
    return true;
}

} // namespace

SectorMap build_sector_map(const WorldSource& source) {
    // One vertex per distinct position (rounded to 0.1 cm, halves to even), so
    // neighbouring faces share edges.
    std::map<std::array<long long, 3>, std::uint32_t> index;
    std::vector<Point> vertices;
    std::vector<std::uint32_t> remap;
    remap.reserve(source.vertices.size());
    for (std::size_t i = 0; i < source.vertices.size(); ++i) {
        const auto& position = source.vertices[i].position;
        const Point p = i < source.exact_positions.size()
                            ? source.exact_positions[i]
                            : Point{position.x, position.y, position.z};
        const std::array<long long, 3> key{std::llrint(p[0] * 10), std::llrint(p[1] * 10),
                                           std::llrint(p[2] * 10)};
        const auto [found, inserted] = index.try_emplace(key, static_cast<std::uint32_t>(vertices.size()));
        if (inserted) vertices.push_back(p);
        remap.push_back(found->second);
    }
    std::vector<std::array<std::uint32_t, 3>> faces;
    for (const auto& face : source.faces) {
        if (!face.collision) continue;
        const std::array<std::uint32_t, 3> f{remap.at(face.vertices[0]), remap.at(face.vertices[1]),
                                             remap.at(face.vertices[2])};
        if (f[0] != f[1] && f[1] != f[2] && f[0] != f[2]) faces.push_back(f);
    }

    // Pair each triangle with an unused neighbour when the quad is convex.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> edge_face;
    for (std::size_t i = 0; i < faces.size(); ++i)
        for (int k = 0; k < 3; ++k) edge_face[{faces[i][k], faces[i][(k + 1) % 3]}] = i;
    std::vector<bool> used(faces.size());
    std::vector<std::vector<std::uint32_t>> polygons;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        if (used[i]) continue;
        used[i] = true;
        std::vector<std::uint32_t> merged;
        for (int k = 0; k < 3 && merged.empty(); ++k) {
            const auto a = faces[i][k], b = faces[i][(k + 1) % 3], c = faces[i][(k + 2) % 3];
            const auto found = edge_face.find({b, a});
            if (found == edge_face.end() || used[found->second]) continue;
            std::vector<std::uint32_t> other;
            for (const auto v : faces[found->second])
                if (v != a && v != b) other.push_back(v);
            if (other.size() != 1) continue;
            std::vector<std::uint32_t> loop{a, other[0], b, c};  // edge a-b becomes a-d-b
            if (convex_xz(vertices, loop)) {
                used[found->second] = true;
                merged = std::move(loop);
            }
        }
        polygons.push_back(merged.empty() ? std::vector<std::uint32_t>(faces[i].begin(), faces[i].end())
                                          : std::move(merged));
    }

    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> owner;
    for (std::size_t s = 0; s < polygons.size(); ++s)
        for (std::size_t k = 0; k < polygons[s].size(); ++k)
            owner[{polygons[s][k], polygons[s][(k + 1) % polygons[s].size()]}] = static_cast<std::uint32_t>(s);
    SectorMap result;
    auto& out = result.bytes;
    append_u32(out, 0);
    append_u32(out, static_cast<std::uint32_t>(vertices.size()));
    for (const auto& p : vertices)
        for (const auto c : p) append_f32(out, c);
    append_u32(out, static_cast<std::uint32_t>(polygons.size()));
    for (std::size_t s = 0; s < polygons.size(); ++s) {
        const auto& loop = polygons[s];
        append_u32(out, static_cast<std::uint32_t>(loop.size()));
        append_u32(out, static_cast<std::uint32_t>(s));
        append_u32(out, 0);
        const auto& p0 = vertices[loop[0]];
        const auto& p1 = vertices[loop[1]];
        const auto& p2 = vertices[loop[2]];
        const Point u{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const Point w{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        Point n{u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]};
        auto length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length == 0) length = 1.0;
        for (auto& c : n) c /= length;
        // Summed from +0.0 as Python's sum() does, so a -0.0 total gives d = -0.0.
        const auto d = -(0.0 + n[0] * p0[0] + n[1] * p0[1] + n[2] * p0[2]);
        for (const auto* values : std::array<const Point*, 3>{&p0, &n, &n})  // point, normal, cached normal
            for (const auto c : *values) append_f32(out, c);
        append_f32(out, d);
        append_u32(out, 1);  // flag byte 1 and three zero pad bytes
        for (std::size_t k = 0; k < loop.size(); ++k) {
            const auto a = loop[k], b = loop[(k + 1) % loop.size()];
            const auto neighbour = owner.find({b, a});
            append_u32(out, a);
            append_u32(out, b);
            append_u32(out, neighbour == owner.end() ? 0xFFFFFFFFU : neighbour->second);
        }
    }
    result.vertex_count = vertices.size();
    result.sector_count = polygons.size();
    return result;
}

GroundQuery::GroundQuery(const WorldSource& source) {
    for (const auto& face : source.faces)
        if (face.collision)
            triangles_.push_back({source.vertices.at(face.vertices[0]).position,
                                  source.vertices.at(face.vertices[1]).position,
                                  source.vertices.at(face.vertices[2]).position});
    if (triangles_.empty()) return;
    float maximum_x = triangles_.front().a.x, maximum_z = triangles_.front().a.z;
    minimum_x_ = maximum_x;
    minimum_z_ = maximum_z;
    for (const auto& t : triangles_)
        for (const auto* p : {&t.a, &t.b, &t.c}) {
            minimum_x_ = std::min(minimum_x_, p->x);
            minimum_z_ = std::min(minimum_z_, p->z);
            maximum_x = std::max(maximum_x, p->x);
            maximum_z = std::max(maximum_z, p->z);
        }
    // About two triangles per cell on an even mesh.
    const auto area = std::max((maximum_x - minimum_x_) * (maximum_z - minimum_z_), 1.0F);
    cell_size_ = std::max(std::sqrt(2.0F * area / static_cast<float>(triangles_.size())), 1.0F);
    columns_ = static_cast<std::size_t>((maximum_x - minimum_x_) / cell_size_) + 1;
    rows_ = static_cast<std::size_t>((maximum_z - minimum_z_) / cell_size_) + 1;
    cells_.resize(columns_ * rows_);
    const auto column = [&](const float x) {
        return std::min(static_cast<std::size_t>((x - minimum_x_) / cell_size_), columns_ - 1);
    };
    const auto row = [&](const float z) {
        return std::min(static_cast<std::size_t>((z - minimum_z_) / cell_size_), rows_ - 1);
    };
    for (std::uint32_t i = 0; i < triangles_.size(); ++i) {
        const auto& t = triangles_[i];
        const auto x0 = column(std::min({t.a.x, t.b.x, t.c.x})), x1 = column(std::max({t.a.x, t.b.x, t.c.x}));
        const auto z0 = row(std::min({t.a.z, t.b.z, t.c.z})), z1 = row(std::max({t.a.z, t.b.z, t.c.z}));
        for (auto r = z0; r <= z1; ++r)
            for (auto c = x0; c <= x1; ++c) cells_[r * columns_ + c].push_back(i);
    }
}

std::optional<GroundQuery::Hit> GroundQuery::highest(const float x, const float z) const {
    return query(x, z, std::nullopt);
}

std::optional<GroundQuery::Hit> GroundQuery::below(const float x, const float y, const float z) const {
    return query(x, z, y + 1.0F);
}

std::optional<GroundQuery::Hit> GroundQuery::query(const float x, const float z,
                                                   const std::optional<float> ceiling) const {
    if (cells_.empty() || x < minimum_x_ || z < minimum_z_) return std::nullopt;
    const auto c = static_cast<std::size_t>((x - minimum_x_) / cell_size_);
    const auto r = static_cast<std::size_t>((z - minimum_z_) / cell_size_);
    if (c >= columns_ || r >= rows_) return std::nullopt;
    std::optional<Hit> best;
    for (const auto i : cells_[r * columns_ + c]) {
        const auto& t = triangles_[i];
        // Barycentric coordinates in XZ, with a small tolerance for shared edges.
        const double d = (double(t.b.z) - t.c.z) * (double(t.a.x) - t.c.x) +
                         (double(t.c.x) - t.b.x) * (double(t.a.z) - t.c.z);
        if (std::abs(d) < 1e-9) continue;  // vertical in XZ
        const double l0 = ((double(t.b.z) - t.c.z) * (x - double(t.c.x)) +
                           (double(t.c.x) - t.b.x) * (z - double(t.c.z))) / d;
        const double l1 = ((double(t.c.z) - t.a.z) * (x - double(t.c.x)) +
                           (double(t.a.x) - t.c.x) * (z - double(t.c.z))) / d;
        const double l2 = 1.0 - l0 - l1;
        constexpr double epsilon = -1e-6;
        if (l0 < epsilon || l1 < epsilon || l2 < epsilon) continue;
        const auto height = static_cast<float>(l0 * t.a.y + l1 * t.b.y + l2 * t.c.y);
        if (ceiling && height > *ceiling) continue;
        if (best && best->height >= height) continue;
        const Vec3 u{t.b.x - t.a.x, t.b.y - t.a.y, t.b.z - t.a.z};
        const Vec3 v{t.c.x - t.a.x, t.c.y - t.a.y, t.c.z - t.a.z};
        Vec3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        auto length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (n.y < 0) length = -length;  // point up whatever the winding
        if (length != 0) n = {n.x / length, n.y / length, n.z / length};
        best = Hit{height, n};
    }
    return best;
}

} // namespace rws
