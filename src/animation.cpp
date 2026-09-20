#include "rws/animation.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace rws {
namespace {

struct Reader {
    std::span<const std::byte> bytes;
    std::uint64_t cursor{}, end{};
    bool u16(std::uint16_t& value) {
        if (end - cursor < 2) return false;
        const auto i = static_cast<std::size_t>(cursor);
        value = std::to_integer<std::uint16_t>(bytes[i]) |
                (std::to_integer<std::uint16_t>(bytes[i + 1]) << 8U);
        cursor += 2;
        return true;
    }
    bool u32(std::uint32_t& value) {
        if (end - cursor < 4) return false;
        const auto i = static_cast<std::size_t>(cursor);
        value = std::to_integer<std::uint32_t>(bytes[i]) |
                (std::to_integer<std::uint32_t>(bytes[i + 1]) << 8U) |
                (std::to_integer<std::uint32_t>(bytes[i + 2]) << 16U) |
                (std::to_integer<std::uint32_t>(bytes[i + 3]) << 24U);
        cursor += 4;
        return true;
    }
    bool f32(float& value) {
        std::uint32_t raw{};
        if (!u32(raw)) return false;
        value = std::bit_cast<float>(raw);
        return true;
    }
};

float half(const std::uint16_t value) {
    // Criterion's compressed HAnim scalar is not IEEE binary16: it uses one
    // sign bit, four exponent bits (bias 15), and eleven mantissa bits.
    const float sign = (value & 0x8000U) != 0 ? -1.0F : 1.0F;
    if ((value & 0x7FFFU) == 0) return std::copysign(0.0F, sign);
    const int exponent = static_cast<int>((value >> 11U) & 0xFU) - 15;
    const float mantissa = 1.0F + static_cast<float>(value & 0x07FFU) / 2048.0F;
    return sign * std::ldexp(mantissa, exponent);
}

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool finite(const Quaternion& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w);
}
float dot(const Quaternion& a, const Quaternion& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
Quaternion normalized(Quaternion q) {
    const float length = std::sqrt(std::max(0.0F, dot(q, q)));
    if (length > 1e-12F) {
        q.x /= length;
        q.y /= length;
        q.z /= length;
        q.w /= length;
    } else
        q = {0, 0, 0, 1};
    return q;
}
Quaternion slerp(Quaternion a, Quaternion b, float t) {
    a = normalized(a);
    b = normalized(b);
    float d = dot(a, b);
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > .9995F)
        return normalized({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                           a.w + (b.w - a.w) * t});
    const float angle = std::acos(std::clamp(d, -1.0F, 1.0F)), den = std::sin(angle);
    const float x = std::sin((1 - t) * angle) / den, y = std::sin(t * angle) / den;
    return {a.x * x + b.x * y, a.y * x + b.y * y, a.z * x + b.z * y, a.w * x + b.w * y};
}

std::array<float, 16> matrix(const Transform& t) {
    const auto q = normalized(t.rotation);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z,
                yz = q.y * q.z, wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    return {1 - 2 * (yy + zz), 2 * (xy + wz),     2 * (xz - wy),     0,
            2 * (xy - wz),     1 - 2 * (xx + zz), 2 * (yz + wx),     0,
            2 * (xz + wy),     2 * (yz - wx),     1 - 2 * (xx + yy), 0,
            t.translation.x,   t.translation.y,   t.translation.z,   1};
}
std::array<float, 16> multiply(const std::array<float, 16>& a, const std::array<float, 16>& b) {
    std::array<float, 16> o{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                o[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    return o;
}
Vec3 point(const std::array<float, 16>& m, const Vec3& v, float w) {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * w,
            m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * w,
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * w};
}

bool finite(const std::array<float, 16>& value) {
    return std::ranges::all_of(value,
                               [](const float component) { return std::isfinite(component); });
}

std::optional<std::array<float, 16>> inverse_affine(const std::array<float, 16>& value) {
    if (!finite(value)) return std::nullopt;
    const float a = value[0], b = value[4], c = value[8];
    const float d = value[1], e = value[5], f = value[9];
    const float g = value[2], h = value[6], i = value[10];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-8F) return std::nullopt;
    const float scale = 1.0F / determinant;
    std::array<float, 16> result{(e * i - f * h) * scale,
                                 (f * g - d * i) * scale,
                                 (d * h - e * g) * scale,
                                 0,
                                 (c * h - b * i) * scale,
                                 (a * i - c * g) * scale,
                                 (b * g - a * h) * scale,
                                 0,
                                 (b * f - c * e) * scale,
                                 (c * d - a * f) * scale,
                                 (a * e - b * d) * scale,
                                 0,
                                 0,
                                 0,
                                 0,
                                 1};
    const Vec3 translation{value[12], value[13], value[14]};
    const auto inverse_translation = point(result, translation, 0);
    result[12] = -inverse_translation.x;
    result[13] = -inverse_translation.y;
    result[14] = -inverse_translation.z;
    return result;
}

std::array<float, 16> frame_matrix(const FrameInfo& frame) {
    const auto& r = frame.rotation;
    return {r[0],
            r[1],
            r[2],
            0,
            r[3],
            r[4],
            r[5],
            0,
            r[6],
            r[7],
            r[8],
            0,
            frame.position.x,
            frame.position.y,
            frame.position.z,
            1};
}
void diagnostic(AnimationClip& clip, AnimationDiagnostic::Severity severity, std::uint64_t offset,
                std::string code, std::string message) {
    clip.diagnostics.push_back({severity, offset, std::move(code), std::move(message)});
}
std::string escape(const std::string& v) {
    std::string out;
    for (char c : v) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}
void write_file(const std::filesystem::path& p, std::span<const std::byte> b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot create " + p.string());
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) throw std::runtime_error("Cannot write " + p.string());
}
void write_text(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot create " + p.string());
    f << s;
    if (!f) throw std::runtime_error("Cannot write " + p.string());
}
template <class T>
std::uint32_t append(std::vector<std::byte>& out, const T& value) {
    const auto offset = static_cast<std::uint32_t>(out.size());
    const auto raw = std::as_bytes(std::span(&value, 1));
    out.insert(out.end(), raw.begin(), raw.end());
    return offset;
}
template <class T>
std::uint32_t append_many(std::vector<std::byte>& out, const std::vector<T>& values) {
    while (out.size() % 4)
        out.push_back(std::byte{});
    const auto offset = static_cast<std::uint32_t>(out.size());
    const auto raw = std::as_bytes(std::span(values));
    out.insert(out.end(), raw.begin(), raw.end());
    return offset;
}

} // namespace

bool AnimationClip::valid() const noexcept {
    return supported() && std::ranges::none_of(diagnostics, [](const auto& d) {
               return d.severity == AnimationDiagnostic::Severity::error;
           });
}
bool HAnimBinding::complete() const noexcept {
    return !matrix_to_frame.empty() &&
           std::ranges::none_of(matrix_to_frame, [](const auto value) { return value < 0; });
}
const char* animation_layout_name(const AnimationLayout value) noexcept {
    switch (value) {
    case AnimationLayout::hanim_uncompressed_36:
        return "HAnim uncompressed (36-byte)";
    case AnimationLayout::hanim_compressed_22:
        return "HAnim compressed (22-byte + scale)";
    default:
        return "unsupported/raw";
    }
}

AnimationClip decode_animation(const Chunk& chunk, const std::span<const std::byte> bytes) {
    AnimationClip clip;
    clip.source_offset = chunk.offset;
    clip.source_size = chunk.available_size;
    clip.library_id = chunk.library_id;
    if (chunk.type != 0x1B) {
        diagnostic(clip, AnimationDiagnostic::Severity::error, chunk.offset, "not-animation",
                   "Chunk is not Animation Animation (0x1B)");
        return clip;
    }
    if (chunk.payload_offset > bytes.size() ||
        chunk.available_size > bytes.size() - chunk.payload_offset) {
        diagnostic(clip, AnimationDiagnostic::Severity::error, chunk.offset, "out-of-bounds",
                   "Animation payload is outside source bytes");
        return clip;
    }
    Reader reader{bytes, chunk.payload_offset, chunk.payload_offset + chunk.available_size};
    if (!reader.u32(clip.version) || !reader.u32(clip.interpolation_type) ||
        !reader.u32(clip.declared_keyframe_count) || !reader.u32(clip.flags) ||
        !reader.f32(clip.duration)) {
        diagnostic(clip, AnimationDiagnostic::Severity::error, chunk.payload_offset,
                   "truncated-header", "Animation header is truncated");
        return clip;
    }
    if (clip.version != 0x100)
        diagnostic(clip, AnimationDiagnostic::Severity::warning, chunk.payload_offset, "version",
                   "Animation stream version is not 0x100");
    if (!std::isfinite(clip.duration) || clip.duration < 0)
        diagnostic(clip, AnimationDiagnostic::Severity::error, chunk.payload_offset + 16,
                   "duration", "Duration is non-finite or negative");
    if (clip.declared_keyframe_count > 10'000'000U) {
        diagnostic(clip, AnimationDiagnostic::Severity::error, chunk.payload_offset + 8,
                   "keyframe-count", "Keyframe count exceeds the safety limit");
        return clip;
    }
    if (clip.interpolation_type == 1) {
        clip.layout = AnimationLayout::hanim_uncompressed_36;
        clip.serialized_record_size = 36;
        clip.logical_record_stride = 36;
    } else if (clip.interpolation_type == 2) {
        clip.layout = AnimationLayout::hanim_compressed_22;
        clip.serialized_record_size = 22;
        clip.logical_record_stride = 24;
    } else {
        diagnostic(clip, AnimationDiagnostic::Severity::warning, chunk.payload_offset + 4,
                   "unsupported-interpolator",
                   "Interpolator is retained as raw payload and is not evaluated");
        clip.trailing_bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(reader.cursor),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(reader.end));
        return clip;
    }
    const std::uint64_t records_size =
        static_cast<std::uint64_t>(clip.declared_keyframe_count) * clip.serialized_record_size;
    const std::uint64_t trailer_size =
        clip.layout == AnimationLayout::hanim_compressed_22 ? 24U : 0U;
    if (records_size > reader.end - reader.cursor ||
        trailer_size > reader.end - reader.cursor - records_size) {
        diagnostic(clip, AnimationDiagnostic::Severity::error, reader.cursor, "truncated-keyframes",
                   "Keyframe records or compressed translation trailer are truncated");
        return clip;
    }
    std::map<std::uint32_t, std::uint32_t> logical_offsets;
    clip.keyframes.reserve(clip.declared_keyframe_count);
    for (std::uint32_t i = 0; i < clip.declared_keyframe_count; ++i) {
        AnimationKeyframe frame;
        frame.source_offset = reader.cursor;
        frame.source_size = clip.serialized_record_size;
        logical_offsets.emplace(i * clip.logical_record_stride, i);
        if (!reader.f32(frame.time)) {
            break;
        }
        if (clip.layout == AnimationLayout::hanim_uncompressed_36) {
            if (!reader.f32(frame.rotation.x) || !reader.f32(frame.rotation.y) ||
                !reader.f32(frame.rotation.z) || !reader.f32(frame.rotation.w) ||
                !reader.f32(frame.translation.x) || !reader.f32(frame.translation.y) ||
                !reader.f32(frame.translation.z) || !reader.u32(frame.raw_previous))
                break;
        } else {
            std::uint16_t q[4]{}, p[3]{};
            for (auto& v : q)
                reader.u16(v);
            for (auto& v : p)
                reader.u16(v);
            reader.u32(frame.raw_previous);
            frame.rotation = {half(q[0]), half(q[1]), half(q[2]), half(q[3])};
            frame.translation = {half(p[0]), half(p[1]), half(p[2])};
        }
        frame.raw.assign(bytes.begin() + static_cast<std::ptrdiff_t>(frame.source_offset),
                         bytes.begin() + static_cast<std::ptrdiff_t>(reader.cursor));
        clip.keyframes.push_back(std::move(frame));
    }
    if (clip.layout == AnimationLayout::hanim_compressed_22) {
        reader.f32(clip.translation_offset.x);
        reader.f32(clip.translation_offset.y);
        reader.f32(clip.translation_offset.z);
        reader.f32(clip.translation_scale.x);
        reader.f32(clip.translation_scale.y);
        reader.f32(clip.translation_scale.z);
        for (auto& f : clip.keyframes) {
            f.translation.x =
                f.translation.x * clip.translation_scale.x + clip.translation_offset.x;
            f.translation.y =
                f.translation.y * clip.translation_scale.y + clip.translation_offset.y;
            f.translation.z =
                f.translation.z * clip.translation_scale.z + clip.translation_offset.z;
        }
    }
    if (reader.cursor < reader.end) {
        clip.trailing_bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(reader.cursor),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(reader.end));
        diagnostic(clip, AnimationDiagnostic::Severity::warning, reader.cursor, "trailing-bytes",
                   "Animation has preserved trailing bytes");
    }
    std::map<std::int32_t, std::size_t> track_lookup;
    std::int32_t next_track = -1;
    for (std::uint32_t i = 0; i < clip.keyframes.size(); ++i) {
        auto& f = clip.keyframes[i];
        if (!std::isfinite(f.time) || f.time < 0 || f.time > clip.duration + 1e-3F)
            diagnostic(clip, AnimationDiagnostic::Severity::error, f.source_offset, "keyframe-time",
                       "Keyframe time is invalid or outside the clip duration");
        if (!finite(f.translation) || !finite(f.rotation))
            diagnostic(clip, AnimationDiagnostic::Severity::error, f.source_offset,
                       "non-finite-transform", "Keyframe contains a non-finite transform");
        const float qlen = std::sqrt(std::max(0.0F, dot(f.rotation, f.rotation)));
        if (std::isfinite(qlen) && std::abs(qlen - 1) > 0.025F)
            diagnostic(clip, AnimationDiagnostic::Severity::warning, f.source_offset,
                       "quaternion-length",
                       "Quaternion length differs from one; evaluation normalizes it");
        std::int32_t track = -1;
        if (const auto previous = logical_offsets.find(f.raw_previous);
            previous != logical_offsets.end() && previous->second < i) {
            f.previous_keyframe = static_cast<std::int32_t>(previous->second);
            track = clip.keyframes[previous->second].node_index.value_or(-1);
        } else {
            if ((f.raw_previous & 0x3F000000U) == 0 && f.raw_previous != 0xFFFFFFFFU)
                diagnostic(clip, AnimationDiagnostic::Severity::error, f.source_offset,
                           "invalid-previous",
                           "Previous-keyframe offset does not identify an earlier record; the "
                           "chain may be forward, cyclic, or out of range");
            track = ++next_track;
        }
        if (track < 0) track = ++next_track;
        f.node_index = track;
        auto [found, inserted] = track_lookup.emplace(track, clip.tracks.size());
        if (inserted) clip.tracks.push_back({track, std::nullopt, std::nullopt, {}});
        clip.tracks[found->second].keyframes.push_back(i);
    }
    for (const auto& track : clip.tracks) {
        float previous = -std::numeric_limits<float>::infinity();
        for (const auto index : track.keyframes) {
            if (clip.keyframes[index].time + 1e-6F < previous)
                diagnostic(clip, AnimationDiagnostic::Severity::error,
                           clip.keyframes[index].source_offset, "nonmonotonic-track",
                           "Track keyframe times are not monotonic");
            previous = clip.keyframes[index].time;
        }
    }
    return clip;
}

DecodeResult<HAnimBinding> decode_hanim_binding(const Chunk& frame_list,
                                                const std::span<const std::byte> bytes) {
    const auto frames = decode_frame_list(frame_list, bytes);
    if (!frames) return {std::nullopt, frames.error};
    HAnimBinding result;
    result.frame_node_ids.assign(frames.value->frames.size(), -1);
    std::optional<HAnimInfo> hierarchy;
    std::size_t frame_index{};
    for (const auto& child : frame_list.children) {
        if (child.type != 0x03) continue;
        if (frame_index >= result.frame_node_ids.size()) {
            result.diagnostics.emplace_back(
                "Frame List contains more frame extensions than frames");
            break;
        }
        const auto* plugin = find_child(child, 0x11E);
        if (plugin) {
            const auto decoded = decode_hanim(*plugin, bytes);
            if (!decoded) return {std::nullopt, decoded.error};
            result.frame_node_ids[frame_index] = decoded.value->hierarchy_id;
            if (!decoded.value->nodes.empty() &&
                (!hierarchy || decoded.value->nodes.size() > hierarchy->nodes.size()))
                hierarchy = *decoded.value;
        }
        ++frame_index;
    }
    if (!hierarchy) return {std::nullopt, "Frame List extensions contain no HAnim hierarchy"};
    result.hierarchy = std::move(*hierarchy);
    std::int32_t maximum = -1;
    for (const auto& node : result.hierarchy.nodes)
        maximum = std::max(maximum, node.node_index);
    if (maximum < 0) return {std::nullopt, "HAnim hierarchy has no non-negative matrix indices"};
    result.matrix_to_frame.assign(static_cast<std::size_t>(maximum) + 1U, -1);
    for (const auto& node : result.hierarchy.nodes) {
        if (node.node_index < 0) {
            result.diagnostics.emplace_back("HAnim node has a negative matrix index");
            continue;
        }
        const auto found = std::ranges::find(result.frame_node_ids, node.node_id);
        if (found == result.frame_node_ids.end()) {
            result.diagnostics.emplace_back("HAnim node ID " + std::to_string(node.node_id) +
                                            " has no Frame List extension");
            continue;
        }
        const auto mapped = static_cast<std::int32_t>(found - result.frame_node_ids.begin());
        auto& slot = result.matrix_to_frame[static_cast<std::size_t>(node.node_index)];
        if (slot >= 0 && slot != mapped)
            result.diagnostics.emplace_back("HAnim matrix index " +
                                            std::to_string(node.node_index) +
                                            " maps to multiple frames");
        else
            slot = mapped;
    }
    return {std::move(result), {}};
}

namespace {
AnimationCompatibility
map_animation_tracks_impl(AnimationClip& clip, const HAnimInfo& hierarchy,
                          const std::span<const std::int32_t> matrix_to_frame,
                          const std::size_t frame_count) {
    AnimationCompatibility result;
    result.track_to_frame.assign(clip.tracks.size(), -1);
    if (clip.tracks.size() > matrix_to_frame.size())
        result.diagnostics.emplace_back("Animation has more tracks than the HAnim matrix array");
    if (clip.tracks.size() > frame_count)
        result.diagnostics.emplace_back("Animation has more tracks than model frames");
    for (std::size_t i = 0; i < clip.tracks.size(); ++i) {
        clip.tracks[i].node_id.reset();
        clip.tracks[i].frame_index.reset();
        if (i >= matrix_to_frame.size() || matrix_to_frame[i] < 0 ||
            static_cast<std::size_t>(matrix_to_frame[i]) >= frame_count)
            continue;
        result.track_to_frame[i] = matrix_to_frame[i];
        clip.tracks[i].frame_index = matrix_to_frame[i];
        for (const auto& node : hierarchy.nodes)
            if (node.node_index == static_cast<std::int32_t>(i)) {
                clip.tracks[i].node_id = node.node_id;
                break;
            }
    }
    if (hierarchy.nodes.size() != clip.tracks.size())
        result.diagnostics.emplace_back(
            "HAnim node count differs from animation track count; index mapping is partial");
    result.compatible =
        std::ranges::none_of(result.track_to_frame, [](int value) { return value < 0; }) &&
        !result.track_to_frame.empty();
    return result;
}
} // namespace

AnimationCompatibility map_animation_tracks(AnimationClip& clip, const HAnimInfo& hierarchy,
                                            const std::size_t frame_count) {
    std::int32_t maximum =
        clip.tracks.empty() ? -1 : static_cast<std::int32_t>(clip.tracks.size() - 1);
    for (const auto& node : hierarchy.nodes)
        maximum = std::max(maximum, node.node_index);
    std::vector<std::int32_t> identity(static_cast<std::size_t>(maximum + 1), -1);
    for (const auto& node : hierarchy.nodes)
        if (node.node_index >= 0 && static_cast<std::size_t>(node.node_index) < identity.size())
            identity[static_cast<std::size_t>(node.node_index)] = node.node_index;
    return map_animation_tracks_impl(clip, hierarchy, identity, frame_count);
}

AnimationCompatibility map_animation_tracks(AnimationClip& clip, const HAnimBinding& binding,
                                            const std::size_t frame_count) {
    auto result =
        map_animation_tracks_impl(clip, binding.hierarchy, binding.matrix_to_frame, frame_count);
    result.diagnostics.insert(result.diagnostics.end(), binding.diagnostics.begin(),
                              binding.diagnostics.end());
    if (!binding.complete()) result.compatible = false;
    return result;
}

Pose evaluate_pose(const AnimationClip& clip, const FrameListInfo& frames,
                   const std::span<const std::array<float, 16>> base_local, float time,
                   const bool loop) {
    Pose pose;
    pose.local.resize(frames.frames.size());
    pose.world.resize(frames.frames.size());
    std::vector<std::array<float, 16>> local_matrices(frames.frames.size());
    if (!std::isfinite(time)) time = 0;
    if (clip.duration > 0)
        time = loop ? std::fmod(std::max(0.0F, time), clip.duration)
                    : std::clamp(time, 0.0F, clip.duration);
    else
        time = 0;
    pose.sampled_time = time;
    for (std::size_t i = 0; i < frames.frames.size(); ++i) {
        pose.local[i].translation = frames.frames[i].position;
        const auto& r = frames.frames[i].rotation;
        const float trace = r[0] + r[4] + r[8];
        Quaternion q;
        if (trace > 0) {
            const float s = std::sqrt(trace + 1) * 2;
            q = {(r[7] - r[5]) / s, (r[2] - r[6]) / s, (r[3] - r[1]) / s, .25F * s};
        } else if (r[0] > r[4] && r[0] > r[8]) {
            const float s = std::sqrt(std::max(0.0F, 1 + r[0] - r[4] - r[8])) * 2;
            q = {.25F * s, (r[1] + r[3]) / s, (r[2] + r[6]) / s, (r[7] - r[5]) / s};
        } else if (r[4] > r[8]) {
            const float s = std::sqrt(std::max(0.0F, 1 + r[4] - r[0] - r[8])) * 2;
            q = {(r[1] + r[3]) / s, .25F * s, (r[5] + r[7]) / s, (r[2] - r[6]) / s};
        } else {
            const float s = std::sqrt(std::max(0.0F, 1 + r[8] - r[0] - r[4])) * 2;
            q = {(r[2] + r[6]) / s, (r[5] + r[7]) / s, .25F * s, (r[3] - r[1]) / s};
        }
        pose.local[i].rotation = normalized(q);
        local_matrices[i] =
            base_local.size() == frames.frames.size() ? base_local[i] : matrix(pose.local[i]);
    }
    for (const auto& track : clip.tracks) {
        const auto frame = track.frame_index.value_or(track.track_index);
        if (frame < 0 || static_cast<std::size_t>(frame) >= pose.local.size() ||
            track.keyframes.empty())
            continue;
        const AnimationKeyframe *a = &clip.keyframes[track.keyframes.front()], *b = a;
        for (const auto index : track.keyframes) {
            const auto& f = clip.keyframes[index];
            if (f.time <= time) a = &f;
            if (f.time >= time) {
                b = &f;
                break;
            }
            b = &f;
        }
        float alpha = b->time > a->time ? (time - a->time) / (b->time - a->time) : 0;
        pose.local[static_cast<std::size_t>(frame)] = {
            slerp(a->rotation, b->rotation, alpha),
            {a->translation.x + (b->translation.x - a->translation.x) * alpha,
             a->translation.y + (b->translation.y - a->translation.y) * alpha,
             a->translation.z + (b->translation.z - a->translation.z) * alpha}};
        local_matrices[static_cast<std::size_t>(frame)] =
            matrix(pose.local[static_cast<std::size_t>(frame)]);
    }
    std::vector<std::uint8_t> state(frames.frames.size());
    const auto resolve = [&](auto&& self, std::size_t i) -> void {
        if (state[i] == 2) return;
        if (state[i] == 1) {
            pose.diagnostics.push_back({AnimationDiagnostic::Severity::error, 0, "frame-cycle",
                                        "Model frame hierarchy contains a cycle"});
            pose.world[i] = local_matrices[i];
            state[i] = 2;
            return;
        }
        state[i] = 1;
        const auto parent = frames.frames[i].parent;
        if (parent >= 0 && static_cast<std::size_t>(parent) < frames.frames.size()) {
            self(self, static_cast<std::size_t>(parent));
            pose.world[i] =
                multiply(pose.world[static_cast<std::size_t>(parent)], local_matrices[i]);
        } else
            pose.world[i] = local_matrices[i];
        state[i] = 2;
    };
    for (std::size_t i = 0; i < frames.frames.size(); ++i)
        resolve(resolve, i);
    return pose;
}

Pose evaluate_pose(const AnimationClip& clip, const FrameListInfo& frames, const float time,
                   const bool loop) {
    return evaluate_pose(clip, frames, {}, time, loop);
}

DecodeResult<SkinBindPose>
recover_skin_bind_pose(const FrameListInfo& frames, const HAnimBinding& binding,
                       const std::span<const std::array<float, 16>> inverse_bind,
                       const std::array<float, 16>& atomic_bind) {
    DecodeResult<SkinBindPose> result;
    if (frames.frames.empty()) {
        result.error = "Cannot recover a Skin bind pose without model frames";
        return result;
    }
    if (inverse_bind.empty()) {
        result.error = "Cannot recover a Skin bind pose without inverse-bind matrices";
        return result;
    }
    if (!finite(atomic_bind)) {
        result.error = "The atomic bind transform contains non-finite values";
        return result;
    }
    if (binding.matrix_to_frame.size() < inverse_bind.size()) {
        result.error = "The HAnim binding has fewer matrix mappings than the Skin";
        return result;
    }

    SkinBindPose pose;
    pose.local.resize(frames.frames.size());
    pose.world.resize(frames.frames.size());
    std::vector<std::uint8_t> known(frames.frames.size());
    for (std::size_t bone = 0; bone < inverse_bind.size(); ++bone) {
        const auto mapped = binding.matrix_to_frame[bone];
        if (mapped < 0 || static_cast<std::size_t>(mapped) >= frames.frames.size()) {
            result.error = "A Skin matrix does not map to a valid model frame";
            return result;
        }
        const auto inverse = inverse_affine(inverse_bind[bone]);
        if (!inverse) {
            result.error = "A Skin inverse-bind matrix is singular or non-finite";
            return result;
        }
        const auto frame = static_cast<std::size_t>(mapped);
        if (known[frame]) {
            result.error = "Multiple Skin matrices map to the same model frame";
            return result;
        }
        pose.world[frame] = multiply(atomic_bind, *inverse);
        known[frame] = 1;
    }

    std::vector<std::uint8_t> state(frames.frames.size());
    const auto resolve_world = [&](auto&& self, const std::size_t frame) -> bool {
        if (state[frame] == 2) return true;
        if (state[frame] == 1) return false;
        state[frame] = 1;
        const auto parent = frames.frames[frame].parent;
        if (parent >= 0 && (static_cast<std::size_t>(parent) >= frames.frames.size() ||
                            !self(self, static_cast<std::size_t>(parent))))
            return false;
        if (!known[frame]) {
            if (parent >= 0) {
                pose.world[frame] = multiply(pose.world[static_cast<std::size_t>(parent)],
                                             frame_matrix(frames.frames[frame]));
            } else {
                pose.world[frame] = frame_matrix(frames.frames[frame]);
            }
        }
        state[frame] = 2;
        return true;
    };
    for (std::size_t frame = 0; frame < frames.frames.size(); ++frame)
        if (!resolve_world(resolve_world, frame)) {
            result.error = "The model frame hierarchy contains a cycle or invalid parent";
            return result;
        }

    for (std::size_t frame = 0; frame < frames.frames.size(); ++frame) {
        const auto parent = frames.frames[frame].parent;
        if (parent < 0) {
            pose.local[frame] = pose.world[frame];
            continue;
        }
        if (static_cast<std::size_t>(parent) >= frames.frames.size()) {
            result.error = "A model frame has an invalid parent";
            return result;
        }
        const auto inverse_parent = inverse_affine(pose.world[static_cast<std::size_t>(parent)]);
        if (!inverse_parent) {
            result.error = "A parent bind transform is singular or non-finite";
            return result;
        }
        pose.local[frame] = multiply(*inverse_parent, pose.world[frame]);
    }
    result.value = std::move(pose);
    return result;
}

std::vector<Vec3> extract_root_motion(const AnimationClip& clip, const std::size_t samples,
                                      const bool loop) {
    std::vector<Vec3> out;
    if (samples == 0 || clip.tracks.empty()) return out;
    const auto& track = clip.tracks.front();
    for (std::size_t s = 0; s < samples; ++s) {
        const float time =
            samples == 1 ? 0
                         : clip.duration * static_cast<float>(s) / static_cast<float>(samples - 1);
        const AnimationKeyframe *a = &clip.keyframes[track.keyframes.front()], *b = a;
        for (auto index : track.keyframes) {
            const auto& f = clip.keyframes[index];
            if (f.time <= time) a = &f;
            if (f.time >= time) {
                b = &f;
                break;
            }
            b = &f;
        }
        const float t = b->time > a->time ? (time - a->time) / (b->time - a->time) : 0;
        out.push_back({a->translation.x + (b->translation.x - a->translation.x) * t,
                       a->translation.y + (b->translation.y - a->translation.y) * t,
                       a->translation.z + (b->translation.z - a->translation.z) * t});
    }
    if (loop && out.size() > 1) {
        const auto base = out.front();
        for (auto& v : out) {
            v.x -= base.x;
            v.y -= base.y;
            v.z -= base.z;
        }
    }
    return out;
}

std::vector<SkinVertex> cpu_skin(const std::span<const SkinVertex> input,
                                 const std::span<const std::array<float, 16>> inverse_bind,
                                 const std::span<const std::array<float, 16>> bone_world) {
    std::vector<SkinVertex> out(input.begin(), input.end());
    for (std::size_t i = 0; i < input.size(); ++i) {
        Vec3 p{}, n{};
        float total{};
        for (std::size_t j = 0; j < 4; ++j) {
            const float w = input[i].weights[j];
            const auto bone = input[i].bones[j];
            if (w <= 0 || bone >= inverse_bind.size() || bone >= bone_world.size()) continue;
            const auto m = multiply(bone_world[bone], inverse_bind[bone]);
            const auto pp = point(m, input[i].position, 1), nn = point(m, input[i].normal, 0);
            p.x += pp.x * w;
            p.y += pp.y * w;
            p.z += pp.z * w;
            n.x += nn.x * w;
            n.y += nn.y * w;
            n.z += nn.z * w;
            total += w;
        }
        if (total > 0) {
            out[i].position = {p.x / total, p.y / total, p.z / total};
            const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (length > 1e-8F) out[i].normal = {n.x / length, n.y / length, n.z / length};
        }
    }
    return out;
}

std::vector<std::array<float, 16>>
decode_inverse_bind_matrices(const SkinInfo& skin, const std::span<const std::byte> bytes) {
    const auto size = static_cast<std::uint64_t>(skin.bone_count) * 64U;
    if (skin.inverse_matrices_offset > bytes.size() ||
        size > bytes.size() - skin.inverse_matrices_offset)
        return {};
    std::vector<std::array<float, 16>> result(skin.bone_count);
    Reader reader{bytes, skin.inverse_matrices_offset, skin.inverse_matrices_offset + size};
    for (auto& value : result) {
        for (auto& component : value)
            if (!reader.f32(component)) return {};
        // RenderWare serializes an RwMatrix as four padded RwV3d values.  The
        // final words are structure padding, not a homogeneous fourth row.
        // Normalize that storage representation before using it in 4x4 math;
        // in particular, m[15] must be one so a following bone-world matrix
        // contributes its translation.
        value[3] = value[7] = value[11] = 0;
        value[15] = 1;
    }
    return result;
}

void export_animation_gltf(const AnimationClip& clip, const FrameListInfo& frames,
                           const HAnimInfo& hierarchy,
                           const std::span<const std::array<float, 16>> inverse_bind,
                           const std::filesystem::path& output_path) {
    if (!clip.valid())
        throw std::runtime_error("Cannot export an unsupported or invalid animation");
    if (frames.frames.empty())
        throw std::runtime_error("Cannot export animation without model frames");
    std::vector<std::byte> bin;
    struct View {
        std::uint32_t offset{}, length{}, target{};
    };
    struct Accessor {
        int view{};
        std::uint32_t type{}, count{};
        const char* shape{};
    };
    std::vector<View> views;
    std::vector<Accessor> accessors;
    auto add = [&]<class T>(const std::vector<T>& v, std::uint32_t type, const char* shape) {
        const auto offset = append_many(bin, v);
        views.push_back({offset, static_cast<std::uint32_t>(v.size() * sizeof(T)), 0});
        accessors.push_back({static_cast<int>(views.size() - 1), type,
                             static_cast<std::uint32_t>(v.size()), shape});
        return static_cast<int>(accessors.size() - 1);
    };
    std::vector<std::array<float, 16>> bind(inverse_bind.begin(), inverse_bind.end());
    const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bind.resize(frames.frames.size(), identity);
    const int bind_accessor = add(bind, 5126, "MAT4");
    struct Channel {
        int time{}, translation{}, rotation{}, node{};
    };
    std::vector<Channel> channels;
    for (const auto& track : clip.tracks) {
        const auto frame = track.frame_index.value_or(track.track_index);
        if (frame < 0 || static_cast<std::size_t>(frame) >= frames.frames.size()) continue;
        std::vector<float> times;
        std::vector<Vec3> translations;
        std::vector<std::array<float, 4>> rotations;
        for (auto index : track.keyframes) {
            const auto& k = clip.keyframes[index];
            times.push_back(k.time);
            translations.push_back(k.translation);
            rotations.push_back({k.rotation.x, k.rotation.y, k.rotation.z, k.rotation.w});
        }
        channels.push_back({add(times, 5126, "SCALAR"), add(translations, 5126, "VEC3"),
                            add(rotations, 5126, "VEC4"), frame});
    }
    auto bin_path = output_path;
    bin_path.replace_extension(".bin");
    std::ostringstream json;
    json << "{\n  \"asset\": {\"version\": \"2.0\", \"generator\": \"rws-man\"},\n  \"buffers\": "
            "[{\"uri\": \""
         << escape(bin_path.filename().string()) << "\", \"byteLength\": " << bin.size()
         << "}],\n  \"bufferViews\": [\n";
    for (std::size_t i = 0; i < views.size(); ++i)
        json << "    {\"buffer\":0,\"byteOffset\":" << views[i].offset
             << ",\"byteLength\":" << views[i].length << "}" << (i + 1 < views.size() ? "," : "")
             << "\n";
    json << "  ],\n  \"accessors\": [\n";
    for (std::size_t i = 0; i < accessors.size(); ++i)
        json << "    {\"bufferView\":" << accessors[i].view
             << ",\"componentType\":" << accessors[i].type << ",\"count\":" << accessors[i].count
             << ",\"type\":\"" << accessors[i].shape << "\"}"
             << (i + 1 < accessors.size() ? "," : "") << "\n";
    json << "  ],\n  \"nodes\": [\n";
    for (std::size_t i = 0; i < frames.frames.size(); ++i) {
        const auto& f = frames.frames[i];
        json << "    {\"name\":\"bone_" << i;
        if (i < hierarchy.nodes.size()) json << "_id_" << hierarchy.nodes[i].node_id;
        json << "\",\"translation\":[" << f.position.x << ',' << f.position.y << ',' << f.position.z
             << ']';
        std::vector<std::size_t> children;
        for (std::size_t c = 0; c < frames.frames.size(); ++c)
            if (frames.frames[c].parent == static_cast<int>(i)) children.push_back(c);
        if (!children.empty()) {
            json << ",\"children\":[";
            for (std::size_t c = 0; c < children.size(); ++c)
                json << children[c] << (c + 1 < children.size() ? "," : "");
            json << ']';
        }
        json << '}' << (i + 1 < frames.frames.size() ? "," : "") << "\n";
    }
    json << "  ],\n  \"skins\":[{\"inverseBindMatrices\":" << bind_accessor << ",\"joints\":[";
    for (std::size_t i = 0; i < frames.frames.size(); ++i)
        json << i << (i + 1 < frames.frames.size() ? "," : "");
    json << "]}],\n  \"animations\":[{\"name\":\"animation\",\"samplers\":[";
    bool first = true;
    for (const auto& c : channels) {
        if (!first) json << ',';
        first = false;
        json << "{\"input\":" << c.time << ",\"output\":" << c.translation
             << ",\"interpolation\":\"LINEAR\"},{\"input\":" << c.time
             << ",\"output\":" << c.rotation << ",\"interpolation\":\"LINEAR\"}";
    }
    json << "],\"channels\":[";
    first = true;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (!first) json << ',';
        first = false;
        json << "{\"sampler\":" << i * 2 << ",\"target\":{\"node\":" << channels[i].node
             << ",\"path\":\"translation\"}},{\"sampler\":" << i * 2 + 1
             << ",\"target\":{\"node\":" << channels[i].node << ",\"path\":\"rotation\"}}";
    }
    json << "]}],\n  \"scenes\":[{\"nodes\":[";
    bool root_first = true;
    for (std::size_t i = 0; i < frames.frames.size(); ++i)
        if (frames.frames[i].parent < 0) {
            if (!root_first) json << ',';
            root_first = false;
            json << i;
        }
    json << "]}],\"scene\":0\n}\n";
    write_file(bin_path, bin);
    write_text(output_path, json.str());
    auto manifest = output_path;
    manifest.replace_extension(".manifest.json");
    std::ostringstream meta;
    meta << "{\n  \"format\": \"rws-man-animation-manifest-v1\",\n  \"source_offset\": "
         << clip.source_offset << ",\n  \"library_id\": " << clip.library_id
         << ",\n  \"interpolator\": " << clip.interpolation_type << ",\n  \"flags\": " << clip.flags
         << ",\n  \"duration\": " << clip.duration << ",\n  \"tracks\": " << clip.tracks.size()
         << ",\n  \"keyframes\": " << clip.keyframes.size()
         << ",\n  \"coordinate_rule\": \"RenderWare local transforms preserved; glTF meters/Y-up "
            "conversion is not inferred\"\n}\n";
    write_text(manifest, meta.str());
}

} // namespace rws
