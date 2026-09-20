#pragma once

#include "rws/decoded.hpp"

#include <array>
#include <string>
#include <vector>

namespace rws {

// RenderWare affine matrix: right/up/at basis vectors followed by position.
struct Matrix34 {
    std::array<float, 12> values{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
};
struct Bounds {
    Vec3 minimum{}, maximum{};
    bool valid{};
};
struct PhysicsVolumeInstance {
    const PhysicsVolumeInfo* volume{};
    std::string path;
    Matrix34 world_transform;
    Bounds world_bounds;
};
struct PhysicsComparison {
    Bounds first, second;
    Vec3 center_delta, extent_delta;
    float first_volume{}, second_volume{}, gross_volume_ratio{};
};

[[nodiscard]] Matrix34 compose(const Matrix34& parent, const Matrix34& local) noexcept;
[[nodiscard]] Vec3 transform_point(const Matrix34& matrix, Vec3 point) noexcept;
[[nodiscard]] Bounds transform_bounds(const Bounds& bounds, const Matrix34& matrix) noexcept;
[[nodiscard]] Bounds physics_local_bounds(const PhysicsVolumeInfo& volume) noexcept;
[[nodiscard]] std::vector<PhysicsVolumeInstance>
flatten_physics_volumes(const PhysicsVolumeInfo& root, const Matrix34& object_transform = {});
[[nodiscard]] PhysicsComparison compare_bounds(const Bounds& first, const Bounds& second) noexcept;
[[nodiscard]] std::vector<Matrix34>
skeleton_rest_transforms(const FrameListInfo& frames,
                         std::vector<std::string>* diagnostics = nullptr);

} // namespace rws
