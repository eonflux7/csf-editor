#include "rws/physics_inspection.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace rws {
namespace {
Matrix34 as_matrix(const std::array<float, 12>& values) {
    return {values};
}
Bounds unite(Bounds a, const Bounds& b) {
    if (!b.valid) return a;
    if (!a.valid) return b;
    a.minimum = {std::min(a.minimum.x, b.minimum.x), std::min(a.minimum.y, b.minimum.y),
                 std::min(a.minimum.z, b.minimum.z)};
    a.maximum = {std::max(a.maximum.x, b.maximum.x), std::max(a.maximum.y, b.maximum.y),
                 std::max(a.maximum.z, b.maximum.z)};
    return a;
}
float bounds_volume(const Bounds& b) {
    return b.valid ? std::max(0.0F, b.maximum.x - b.minimum.x) *
                         std::max(0.0F, b.maximum.y - b.minimum.y) *
                         std::max(0.0F, b.maximum.z - b.minimum.z)
                   : 0.0F;
}
} // namespace

Vec3 transform_point(const Matrix34& m, const Vec3 p) noexcept {
    return {m.values[0] * p.x + m.values[3] * p.y + m.values[6] * p.z + m.values[9],
            m.values[1] * p.x + m.values[4] * p.y + m.values[7] * p.z + m.values[10],
            m.values[2] * p.x + m.values[5] * p.y + m.values[8] * p.z + m.values[11]};
}
Matrix34 compose(const Matrix34& a, const Matrix34& b) noexcept {
    Matrix34 r;
    for (int column = 0; column < 3; ++column)
        for (int row = 0; row < 3; ++row)
            r.values[column * 3 + row] = a.values[row] * b.values[column * 3] +
                                         a.values[3 + row] * b.values[column * 3 + 1] +
                                         a.values[6 + row] * b.values[column * 3 + 2];
    const auto p = transform_point(a, {b.values[9], b.values[10], b.values[11]});
    r.values[9] = p.x;
    r.values[10] = p.y;
    r.values[11] = p.z;
    return r;
}
Bounds transform_bounds(const Bounds& b, const Matrix34& m) noexcept {
    if (!b.valid) return {};
    Bounds result;
    result.minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    result.maximum = {-result.minimum.x, -result.minimum.y, -result.minimum.z};
    result.valid = true;
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
            for (int z = 0; z < 2; ++z) {
                const auto p = transform_point(m, {x ? b.maximum.x : b.minimum.x,
                                                   y ? b.maximum.y : b.minimum.y,
                                                   z ? b.maximum.z : b.minimum.z});
                result.minimum = {std::min(result.minimum.x, p.x), std::min(result.minimum.y, p.y),
                                  std::min(result.minimum.z, p.z)};
                result.maximum = {std::max(result.maximum.x, p.x), std::max(result.maximum.y, p.y),
                                  std::max(result.maximum.z, p.z)};
            }
    return result;
}
Bounds physics_local_bounds(const PhysicsVolumeInfo& v) noexcept {
    const float f = std::max(0.0F, v.fatness);
    Vec3 e{f, f, f};
    if (v.kind == 0x0F && v.capsule_half_height)
        e = {f, f + std::max(0.0F, *v.capsule_half_height), f};
    else if (v.kind == 0x10 && v.box_half_extents)
        e = {std::max(0.0F, v.box_half_extents->x) + f, std::max(0.0F, v.box_half_extents->y) + f,
             std::max(0.0F, v.box_half_extents->z) + f};
    else if (v.kind == 0x11 && v.cylinder_radius && v.cylinder_half_height)
        e = {std::max(0.0F, *v.cylinder_radius) + f, std::max(0.0F, *v.cylinder_half_height) + f,
             std::max(0.0F, *v.cylinder_radius) + f};
    if (v.kind == 0x13) {
        Bounds total;
        for (const auto& child : v.children)
            total = unite(total,
                          transform_bounds(physics_local_bounds(child), as_matrix(child.matrix)));
        return total;
    }
    return {{-e.x, -e.y, -e.z}, {e.x, e.y, e.z}, true};
}
std::vector<PhysicsVolumeInstance> flatten_physics_volumes(const PhysicsVolumeInfo& root,
                                                           const Matrix34& object) {
    std::vector<PhysicsVolumeInstance> result;
    std::function<void(const PhysicsVolumeInfo&, const Matrix34&, const std::string&)> visit =
        [&](const auto& v, const auto& parent, const std::string& path) {
            const auto world = compose(parent, as_matrix(v.matrix));
            result.push_back({&v, path, world, transform_bounds(physics_local_bounds(v), world)});
            for (std::size_t i = 0; i < v.children.size(); ++i)
                visit(v.children[i], world, path + "/" + std::to_string(i));
        };
    visit(root, object, "0");
    return result;
}
PhysicsComparison compare_bounds(const Bounds& a, const Bounds& b) noexcept {
    PhysicsComparison r;
    r.first = a;
    r.second = b;
    if (!a.valid || !b.valid) return r;
    const Vec3 ac{(a.minimum.x + a.maximum.x) / 2, (a.minimum.y + a.maximum.y) / 2,
                  (a.minimum.z + a.maximum.z) / 2};
    const Vec3 bc{(b.minimum.x + b.maximum.x) / 2, (b.minimum.y + b.maximum.y) / 2,
                  (b.minimum.z + b.maximum.z) / 2};
    r.center_delta = {bc.x - ac.x, bc.y - ac.y, bc.z - ac.z};
    r.extent_delta = {(b.maximum.x - b.minimum.x) - (a.maximum.x - a.minimum.x),
                      (b.maximum.y - b.minimum.y) - (a.maximum.y - a.minimum.y),
                      (b.maximum.z - b.minimum.z) - (a.maximum.z - a.minimum.z)};
    r.first_volume = bounds_volume(a);
    r.second_volume = bounds_volume(b);
    r.gross_volume_ratio = r.first_volume > 0 ? r.second_volume / r.first_volume : 0;
    return r;
}
std::vector<Matrix34> skeleton_rest_transforms(const FrameListInfo& frames,
                                               std::vector<std::string>* diagnostics) {
    std::vector<Matrix34> result(frames.frames.size());
    std::vector<unsigned char> state(frames.frames.size());
    std::function<void(std::size_t)> solve = [&](std::size_t i) {
        if (state[i] == 2) return;
        if (state[i] == 1) {
            if (diagnostics) diagnostics->push_back("Cycle at frame " + std::to_string(i));
            result[i] = {};
            state[i] = 2;
            return;
        }
        state[i] = 1;
        const auto& f = frames.frames[i];
        Matrix34 local;
        for (std::size_t j = 0; j < 9; ++j)
            local.values[j] = f.rotation[j];
        local.values[9] = f.position.x;
        local.values[10] = f.position.y;
        local.values[11] = f.position.z;
        if (f.parent >= 0 && static_cast<std::size_t>(f.parent) < frames.frames.size()) {
            solve(static_cast<std::size_t>(f.parent));
            result[i] = compose(result[static_cast<std::size_t>(f.parent)], local);
        } else {
            result[i] = local;
            if (f.parent >= 0 && diagnostics)
                diagnostics->push_back("Frame " + std::to_string(i) + " has invalid parent " +
                                       std::to_string(f.parent));
        }
        state[i] = 2;
    };
    for (std::size_t i = 0; i < result.size(); ++i)
        solve(i);
    return result;
}
} // namespace rws
