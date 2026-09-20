#pragma once

#include "rws/decoded.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rws {

struct Quaternion {
    float x{}, y{}, z{}, w{1.0F};
};

struct AnimationDiagnostic {
    enum class Severity { note, warning, error };
    Severity severity{Severity::warning};
    std::uint64_t offset{};
    std::string code;
    std::string message;
};

struct AnimationKeyframe {
    std::uint64_t source_offset{};
    std::uint32_t source_size{};
    Quaternion rotation;
    Vec3 translation;
    float time{};
    std::int32_t previous_keyframe{-1};
    std::uint32_t raw_previous{};
    std::optional<std::int32_t> node_index;
    std::vector<std::byte> raw;
};

struct AnimationTrack {
    std::int32_t track_index{};
    std::optional<std::int32_t> node_id;
    std::optional<std::int32_t> frame_index;
    std::vector<std::uint32_t> keyframes;
};

enum class AnimationLayout { unsupported, hanim_uncompressed_36, hanim_compressed_22 };

struct AnimationClip {
    std::uint64_t source_offset{};
    std::uint64_t source_size{};
    std::uint32_t library_id{};
    std::uint32_t version{};
    std::uint32_t interpolation_type{};
    std::uint32_t declared_keyframe_count{};
    std::uint32_t flags{};
    float duration{};
    AnimationLayout layout{AnimationLayout::unsupported};
    std::uint32_t serialized_record_size{};
    std::uint32_t logical_record_stride{};
    Vec3 translation_offset{};
    Vec3 translation_scale{1.0F, 1.0F, 1.0F};
    std::vector<AnimationKeyframe> keyframes;
    std::vector<AnimationTrack> tracks;
    std::vector<std::byte> trailing_bytes;
    std::vector<AnimationDiagnostic> diagnostics;
    [[nodiscard]] bool supported() const noexcept { return layout != AnimationLayout::unsupported; }
    [[nodiscard]] bool valid() const noexcept;
};

struct Transform {
    Quaternion rotation;
    Vec3 translation;
};

struct Pose {
    float sampled_time{};
    std::vector<Transform> local;
    std::vector<std::array<float, 16>> world;
    std::vector<AnimationDiagnostic> diagnostics;
};

struct SkinBindPose {
    std::vector<std::array<float, 16>> local;
    std::vector<std::array<float, 16>> world;
};

struct AnimationCompatibility {
    bool compatible{};
    std::vector<std::int32_t> track_to_frame;
    std::vector<std::string> diagnostics;
};

// RenderWare serializes one Extension per Frame List frame.  The HAnim
// hierarchy stores animation/skin matrix indices, while the individual frame
// extensions carry the node IDs needed to associate those indices with Frame
// List entries.  They are not generally the same numeric index.
struct HAnimBinding {
    HAnimInfo hierarchy;
    std::vector<std::int32_t> matrix_to_frame;
    std::vector<std::int32_t> frame_node_ids;
    std::vector<std::string> diagnostics;
    [[nodiscard]] bool complete() const noexcept;
};

struct SkinVertex {
    Vec3 position;
    Vec3 normal;
    std::array<std::uint8_t, 4> bones{};
    std::array<float, 4> weights{};
};

[[nodiscard]] AnimationClip decode_animation(const Chunk&, std::span<const std::byte>);
[[nodiscard]] DecodeResult<HAnimBinding> decode_hanim_binding(const Chunk& frame_list,
                                                              std::span<const std::byte>);
[[nodiscard]] AnimationCompatibility map_animation_tracks(AnimationClip&, const HAnimInfo&,
                                                          std::size_t frame_count);
[[nodiscard]] AnimationCompatibility map_animation_tracks(AnimationClip&, const HAnimBinding&,
                                                          std::size_t frame_count);
[[nodiscard]] Pose evaluate_pose(const AnimationClip&, const FrameListInfo&, float time, bool loop);
[[nodiscard]] Pose evaluate_pose(const AnimationClip&, const FrameListInfo&,
                                 std::span<const std::array<float, 16>> base_local, float time,
                                 bool loop);
// Skin inverse binds include the atomic's bind transform. Recover the bone bind
// hierarchy so tracks that animate only part of a skeleton leave all other
// bones in the pose expected by the Skin plugin.
[[nodiscard]] DecodeResult<SkinBindPose>
recover_skin_bind_pose(const FrameListInfo&, const HAnimBinding&,
                       std::span<const std::array<float, 16>> inverse_bind,
                       const std::array<float, 16>& atomic_bind);
[[nodiscard]] std::vector<Vec3> extract_root_motion(const AnimationClip&, std::size_t samples,
                                                    bool loop = false);
[[nodiscard]] std::vector<SkinVertex> cpu_skin(std::span<const SkinVertex>,
                                               std::span<const std::array<float, 16>> inverse_bind,
                                               std::span<const std::array<float, 16>> bone_world);
[[nodiscard]] std::vector<std::array<float, 16>>
decode_inverse_bind_matrices(const SkinInfo&, std::span<const std::byte>);
[[nodiscard]] const char* animation_layout_name(AnimationLayout) noexcept;

// Exports a portable glTF skeleton and animation. Skin joint/inverse-bind data is
// emitted when supplied; raw RenderWare metadata is retained in a sibling manifest.
void export_animation_gltf(const AnimationClip&, const FrameListInfo&, const HAnimInfo&,
                           std::span<const std::array<float, 16>> inverse_bind,
                           const std::filesystem::path& output_path);

} // namespace rws
