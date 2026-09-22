#pragma once

#include "rws/chunk.hpp"
#include "rws/decoded.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rws {

struct Diagnostic {
    enum class Severity { warning, error };
    Severity severity{Severity::warning};
    std::uint64_t offset{};
    std::string message;
};

enum class SceneInstanceFlag : std::uint32_t {
    enabled = 0x001,
    water = 0x002,
    mipmapped = 0x040,
    breakable_glass = 0x080,
    back_plane = 0x100,
    animated = 0x200,
    scene_registered = 0x400,
};

[[nodiscard]] constexpr bool has_scene_instance_flag(const std::uint32_t flags,
                                                     const SceneInstanceFlag flag) noexcept {
    return (flags & static_cast<std::uint32_t>(flag)) != 0;
}

[[nodiscard]] std::string scene_instance_flag_names(std::uint32_t flags);

// Commandos: Strike Force inserts these records between the streamed Clump
// prototypes and the map World. They are not ordinary RenderWare chunks even
// though each record embeds a standard Matrix/Struct pair.
struct SceneInstance {
    std::uint64_t offset{};
    std::uint32_t declared_size{};
    std::uint32_t prototype_id{};
    std::uint32_t instance_id{};
    float maximum_visibility_distance{};
    float minimum_visibility_distance{};
    float visibility_fade_range{};
    std::uint32_t flags{};
    std::array<float, 9> rotation{};
    Vec3 position;
    std::uint32_t matrix_flags{};
    std::string prototype_name;
    std::uint64_t physical_size{};
};

class Document {
public:
    [[nodiscard]] static Document load(const std::filesystem::path& path);
    [[nodiscard]] static Document from_bytes(std::vector<std::byte> bytes);

    void save_as(const std::filesystem::path& path) const;
    void set_byte(std::uint64_t offset, std::byte value);
    // Replaces the content with a newer version of the same file (for example
    // after a mission edit) and reparses it; the source path is kept and the
    // document is not marked dirty.
    void replace_bytes(std::vector<std::byte> bytes);

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept { return source_path_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] const std::vector<Chunk>& chunks() const noexcept { return chunks_; }
    [[nodiscard]] const std::vector<SceneInstance>& scene_instances() const noexcept {
        return scene_instances_;
    }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

private:
    void parse();
    [[nodiscard]] std::uint64_t parse_scene_instances(std::uint64_t begin);
    void parse_range(std::uint64_t begin, std::uint64_t end, std::vector<Chunk>& output,
                     unsigned depth, bool require_complete);

    std::filesystem::path source_path_;
    std::vector<std::byte> bytes_;
    std::vector<Chunk> chunks_;
    std::vector<SceneInstance> scene_instances_;
    std::vector<Diagnostic> diagnostics_;
    std::optional<std::uint32_t> stream_library_id_;
    bool dirty_{};
};

} // namespace rws
