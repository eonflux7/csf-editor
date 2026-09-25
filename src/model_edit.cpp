#include "rws/model_edit.hpp"

#include "rws/document.hpp"

#include <bit>
#include <cmath>
#include <functional>

namespace rws {
namespace {

float read_f32(const std::vector<std::byte>& bytes, const std::size_t at) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) value = value << 8U | std::to_integer<std::uint32_t>(bytes[at + static_cast<std::size_t>(i)]);
    return std::bit_cast<float>(value);
}

void write_f32(std::vector<std::byte>& bytes, const std::size_t at, const float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) bytes[at + static_cast<std::size_t>(i)] = static_cast<std::byte>(bits >> (8 * i) & 0xFFU);
}

std::uint32_t read_u32(const std::vector<std::byte>& bytes, const std::size_t at) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) value = value << 8U | std::to_integer<std::uint32_t>(bytes[at + static_cast<std::size_t>(i)]);
    return value;
}

} // namespace

DecodeResult<std::vector<std::byte>> scale_clump(const std::span<const std::byte> bytes, const float scale) {
    DecodeResult<std::vector<std::byte>> result;
    if (!(scale > 0) || !std::isfinite(scale)) {
        result.error = "the scale must be positive";
        return result;
    }
    const auto document = Document::from_bytes({bytes.begin(), bytes.end()});
    std::vector<std::byte> out(bytes.begin(), bytes.end());
    const auto scale_at = [&](const std::size_t at, const std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) write_f32(out, at + i * 4, read_f32(out, at + i * 4) * scale);
    };
    std::size_t frames = 0, geometries = 0;
    std::function<void(const Chunk&)> visit = [&](const Chunk& chunk) {
        if (chunk.type == 0x0EU && !chunk.children.empty() && chunk.children.front().type == 0x01U) {
            // Frame List struct: count, then per frame a 3x3 rotation, the
            // translation, the parent and flags (56 bytes).
            const auto at = chunk.children.front().payload_offset;
            const auto count = read_u32(out, at);
            for (std::uint32_t i = 0; i < count; ++i) scale_at(at + 4 + i * 56 + 36, 3);
            frames += count;
        }
        if (chunk.type == 0x0FU) {
            const auto decoded = decode_geometry(chunk, document.bytes());
            if (!decoded) return;
            for (const auto& target : decoded.value->morph_targets) {
                // The sphere (centre, radius) and two flags precede the positions.
                if (!target.has_vertices) continue;
                scale_at(target.vertices_offset - 24, 4);
                scale_at(target.vertices_offset, static_cast<std::size_t>(decoded.value->vertex_count) * 3);
            }
            ++geometries;
        }
        for (const auto& child : chunk.children) visit(child);
    };
    for (const auto& chunk : document.chunks()) visit(chunk);
    if (geometries == 0) {
        result.error = "no geometry to scale";
        return result;
    }
    result.value = std::move(out);
    return result;
}

} // namespace rws
