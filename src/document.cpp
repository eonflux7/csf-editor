#include "rws/document.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace rws {
namespace {

constexpr std::uint64_t header_size = 12;
constexpr unsigned max_depth = 64;

std::uint32_t read_u32(const std::span<const std::byte> data, const std::uint64_t offset) {
    const auto i = static_cast<std::size_t>(offset);
    return std::to_integer<std::uint32_t>(data[i]) |
           (std::to_integer<std::uint32_t>(data[i + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(data[i + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(data[i + 3]) << 24U);
}

float read_f32(const std::span<const std::byte> data, const std::uint64_t offset) {
    return std::bit_cast<float>(read_u32(data, offset));
}

std::string at_offset(const std::string_view text, const std::uint64_t offset) {
    std::ostringstream out;
    out << text << " at 0x" << std::hex << offset;
    return out.str();
}

} // namespace

std::string scene_instance_flag_names(const std::uint32_t flags) {
    struct NamedFlag {
        SceneInstanceFlag flag;
        const char* name;
    };
    constexpr std::array named_flags{
        NamedFlag{SceneInstanceFlag::enabled, "enabled"},
        NamedFlag{SceneInstanceFlag::water, "water"},
        NamedFlag{SceneInstanceFlag::mipmapped, "mipmapped"},
        NamedFlag{SceneInstanceFlag::breakable_glass, "breakable-glass"},
        NamedFlag{SceneInstanceFlag::back_plane, "back-plane"},
        NamedFlag{SceneInstanceFlag::animated, "animated"},
        NamedFlag{SceneInstanceFlag::scene_registered, "scene-registered"},
    };
    constexpr auto known_mask = 0x7C3U;
    std::ostringstream names;
    bool first = true;
    for (const auto& item : named_flags) {
        if (!has_scene_instance_flag(flags, item.flag)) continue;
        if (!first) names << ", ";
        names << item.name;
        first = false;
    }
    if (const auto unknown = flags & ~known_mask; unknown != 0) {
        if (!first) names << ", ";
        names << "unknown 0x" << std::hex << unknown;
    }
    return names.str();
}

Document Document::load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Cannot open RWS file: " + path.string());
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error("Cannot determine RWS file size: " + path.string());
    }
    const auto size = static_cast<std::uint64_t>(end);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("RWS file is too large for this process");
    }
    Document result;
    result.source_path_ = path;
    result.bytes_.resize(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.bytes_.data()), static_cast<std::streamsize>(size));
    if (!input && size != 0) {
        throw std::runtime_error("Cannot read complete RWS file: " + path.string());
    }
    result.parse();
    return result;
}

Document Document::from_bytes(std::vector<std::byte> bytes) {
    Document result;
    result.bytes_ = std::move(bytes);
    result.parse();
    return result;
}

void Document::save_as(const std::filesystem::path& path) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Cannot create output file: " + path.string());
    }
    output.write(reinterpret_cast<const char*>(bytes_.data()),
                 static_cast<std::streamsize>(bytes_.size()));
    if (!output) {
        throw std::runtime_error("Cannot write complete output file: " + path.string());
    }
}

void Document::set_byte(const std::uint64_t offset, const std::byte value) {
    if (offset >= bytes_.size()) {
        throw std::out_of_range("Byte offset is outside the document");
    }
    auto& current = bytes_[static_cast<std::size_t>(offset)];
    if (current != value) {
        current = value;
        dirty_ = true;
        parse();
    }
}

void Document::parse() {
    chunks_.clear();
    scene_instances_.clear();
    diagnostics_.clear();
    stream_library_id_.reset();
    if (bytes_.size() >= header_size) {
        stream_library_id_ = read_u32(bytes_, 8);
    }
    parse_range(0, bytes_.size(), chunks_, 0, true);

    // A CSF instance record can masquerade as one final top-level chunk. Its
    // wrapper size does not describe the complete physical record, so remove
    // that tentative chunk and consume the records with their own grammar.
    std::optional<std::uint64_t> instance_begin;
    const auto custom = std::find_if(chunks_.begin(), chunks_.end(),
                                     [](const Chunk& chunk) { return chunk.type == 0x00016FC0U; });
    if (custom != chunks_.end()) {
        instance_begin = custom->offset;
        chunks_.erase(custom, chunks_.end());
    }
    const auto instance_end = instance_begin ? parse_scene_instances(*instance_begin) : 0U;
    if (instance_begin && instance_end != 0) {
        diagnostics_.erase(std::remove_if(diagnostics_.begin(), diagnostics_.end(),
                                          [&](const Diagnostic& diagnostic) {
                                              return diagnostic.offset >= *instance_begin &&
                                                     diagnostic.offset < instance_end &&
                                                     diagnostic.message ==
                                                         "Remaining bytes are not a chunk sequence "
                                                         "with the stream library ID";
                                          }),
                           diagnostics_.end());
    }

    // CSF map streams may place a short game-specific instance table between
    // the ordinary top-level Clumps and an embedded RenderWare World. Recover
    // only a strongly identifiable World suffix: matching library stamp,
    // leading Struct child, and a declared end at (or just beyond) physical EOF.
    // The small overrun is retained as truncation instead of rewriting the file.
    if (stream_library_id_ && bytes_.size() >= header_size * 2) {
        std::uint64_t scan_begin = instance_end;
        if (scan_begin == 0 && !chunks_.empty())
            scan_begin = chunks_.back().payload_offset + chunks_.back().available_size;
        for (std::uint64_t candidate = scan_begin; candidate + header_size * 2 <= bytes_.size();
             ++candidate) {
            if (read_u32(bytes_, candidate) != 0x0B ||
                read_u32(bytes_, candidate + 8) != *stream_library_id_ ||
                read_u32(bytes_, candidate + header_size) != 0x01 ||
                read_u32(bytes_, candidate + header_size + 8) != *stream_library_id_)
                continue;
            const auto declared = static_cast<std::uint64_t>(read_u32(bytes_, candidate + 4));
            const auto physical = bytes_.size() - candidate - header_size;
            if (declared < physical || declared - physical > 4096) continue;
            std::vector<Chunk> recovered;
            parse_range(candidate, bytes_.size(), recovered, 0, false);
            if (recovered.size() == 1 && recovered.front().type == 0x0B) {
                chunks_.push_back(std::move(recovered.front()));
                diagnostics_.push_back(
                    {Diagnostic::Severity::warning, candidate,
                     scene_instances_.empty()
                         ? "Recovered RenderWare World after opaque map data"
                         : "Recovered RenderWare World after decoded CSF scene instances"});
            }
            break;
        }
    }
}

std::uint64_t Document::parse_scene_instances(const std::uint64_t begin) {
    if (!stream_library_id_) return 0;
    std::uint64_t cursor = begin;
    while (cursor <= bytes_.size() && bytes_.size() - cursor >= 116U) {
        if (read_u32(bytes_, cursor) != 0x00016FC0U ||
            read_u32(bytes_, cursor + 8) != *stream_library_id_ ||
            read_u32(bytes_, cursor + 36) != 0x0DU || read_u32(bytes_, cursor + 40) != 64U ||
            read_u32(bytes_, cursor + 44) != *stream_library_id_ ||
            read_u32(bytes_, cursor + 48) != 0x01U || read_u32(bytes_, cursor + 52) != 52U ||
            read_u32(bytes_, cursor + 56) != *stream_library_id_)
            break;

        const auto name_size = static_cast<std::uint64_t>(read_u32(bytes_, cursor + 112));
        const auto declared_size = static_cast<std::uint64_t>(read_u32(bytes_, cursor + 4));
        if (name_size > 4096U || name_size > bytes_.size() - cursor - 116U ||
            declared_size != 92U + name_size)
            break;

        SceneInstance instance;
        instance.offset = cursor;
        instance.declared_size = read_u32(bytes_, cursor + 4);
        instance.prototype_id = read_u32(bytes_, cursor + 12);
        instance.instance_id = read_u32(bytes_, cursor + 16);
        instance.maximum_visibility_distance = read_f32(bytes_, cursor + 20);
        instance.minimum_visibility_distance = read_f32(bytes_, cursor + 24);
        instance.visibility_fade_range = read_f32(bytes_, cursor + 28);
        instance.flags = read_u32(bytes_, cursor + 32);
        for (std::size_t i = 0; i < instance.rotation.size(); ++i)
            instance.rotation[i] = read_f32(bytes_, cursor + 60U + i * 4U);
        instance.position = {read_f32(bytes_, cursor + 96), read_f32(bytes_, cursor + 100),
                             read_f32(bytes_, cursor + 104)};
        instance.matrix_flags = read_u32(bytes_, cursor + 108);
        instance.prototype_name.reserve(static_cast<std::size_t>(name_size));
        for (std::uint64_t i = 0; i < name_size; ++i)
            instance.prototype_name.push_back(static_cast<char>(std::to_integer<unsigned char>(
                bytes_[static_cast<std::size_t>(cursor + 116U + i)])));
        instance.physical_size = 116U + name_size;

        const auto finite = std::all_of(instance.rotation.begin(), instance.rotation.end(),
                                        [](const float value) { return std::isfinite(value); }) &&
                            std::isfinite(instance.position.x) &&
                            std::isfinite(instance.position.y) &&
                            std::isfinite(instance.position.z) &&
                            std::isfinite(instance.maximum_visibility_distance) &&
                            std::isfinite(instance.minimum_visibility_distance) &&
                            std::isfinite(instance.visibility_fade_range);
        if (!finite) break;
        scene_instances_.push_back(std::move(instance));
        cursor += scene_instances_.back().physical_size;
    }
    if (scene_instances_.empty()) return 0;
    return cursor;
}

void Document::parse_range(const std::uint64_t begin, const std::uint64_t end,
                           std::vector<Chunk>& output, const unsigned depth,
                           const bool require_complete) {
    if (depth > max_depth) {
        diagnostics_.push_back(
            {Diagnostic::Severity::error, begin, "Maximum chunk nesting exceeded"});
        return;
    }

    std::uint64_t cursor = begin;
    while (cursor < end) {
        const auto remaining = end - cursor;
        if (remaining < header_size) {
            if (require_complete && remaining != 0) {
                diagnostics_.push_back({Diagnostic::Severity::warning, cursor,
                                        "Trailing bytes do not form a complete chunk header"});
            }
            return;
        }

        Chunk chunk;
        chunk.offset = cursor;
        chunk.payload_offset = cursor + header_size;
        chunk.type = read_u32(bytes_, cursor);
        chunk.declared_size = read_u32(bytes_, cursor + 4);
        chunk.library_id = read_u32(bytes_, cursor + 8);

        // Chunks produced by one RenderWare stream use one library stamp. Treat a
        // mismatch as opaque payload/trailing data rather than inventing a tree
        // from coincidental integer patterns inside game-specific binary data.
        if (stream_library_id_ && chunk.library_id != *stream_library_id_) {
            if (require_complete) {
                diagnostics_.push_back(
                    {Diagnostic::Severity::warning, cursor,
                     "Remaining bytes are not a chunk sequence with the stream library ID"});
            }
            return;
        }

        const auto available = end - chunk.payload_offset;
        chunk.available_size = std::min<std::uint64_t>(chunk.declared_size, available);
        chunk.truncated = chunk.declared_size > available;
        if (chunk.truncated) {
            diagnostics_.push_back({Diagnostic::Severity::warning, cursor,
                                    at_offset("Chunk payload is truncated", cursor)});
        }

        if (is_container_chunk(chunk.type) && chunk.available_size >= header_size) {
            parse_range(chunk.payload_offset, chunk.payload_offset + chunk.available_size,
                        chunk.children, depth + 1, false);
        }
        output.push_back(std::move(chunk));

        if (output.back().truncated) {
            return;
        }
        cursor = output.back().payload_offset + output.back().declared_size;
    }
}

} // namespace rws
