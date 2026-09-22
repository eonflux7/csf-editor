#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace rwsman {

// A tagged reference to "the thing the user is looking at". The explorer,
// viewport, inspector, and status bar all describe selection with this type, and
// the navigation history stores it. It carries identity only, never pointers, so
// it stays valid across reloads of the same document.
struct SelectionRef {
    enum class Kind : std::uint8_t {
        none,
        chunk,               // a = chunk header offset
        scene_instance,      // a = instance offset
        mission_entry,       // a = CSFFBS entry index in the mission scene
        program_script,      // a = program document index, b = script index
        program_instruction, // a = program document index, b = script index, c = entry index
        database_record,     // path = database file, a = entry index
        resource_path,       // path = resource path
    };

    Kind kind{Kind::none};
    std::uint64_t a{}, b{}, c{};
    std::string path;

    [[nodiscard]] bool empty() const noexcept { return kind == Kind::none; }
    [[nodiscard]] friend bool operator==(const SelectionRef&, const SelectionRef&) = default;

    [[nodiscard]] static SelectionRef chunk(std::uint64_t offset) {
        return {Kind::chunk, offset, 0, 0, {}};
    }
    [[nodiscard]] static SelectionRef scene_instance(std::uint64_t offset) {
        return {Kind::scene_instance, offset, 0, 0, {}};
    }
    [[nodiscard]] static SelectionRef mission_entry(std::uint32_t entry) {
        return {Kind::mission_entry, entry, 0, 0, {}};
    }
    [[nodiscard]] static SelectionRef program_script(std::uint64_t document, std::uint64_t script) {
        return {Kind::program_script, document, script, 0, {}};
    }
    [[nodiscard]] static SelectionRef program_instruction(std::uint64_t document,
                                                          std::uint64_t script,
                                                          std::uint32_t entry) {
        return {Kind::program_instruction, document, script, entry, {}};
    }
    [[nodiscard]] static SelectionRef database_record(std::string file, std::uint32_t entry) {
        return {Kind::database_record, entry, 0, 0, std::move(file)};
    }
    [[nodiscard]] static SelectionRef resource_path(std::string path) {
        return {Kind::resource_path, 0, 0, 0, std::move(path)};
    }
};

[[nodiscard]] const char* selection_kind_name(SelectionRef::Kind kind) noexcept;

struct SelectionRefHash {
    [[nodiscard]] std::size_t operator()(const SelectionRef& ref) const noexcept {
        std::size_t seed = static_cast<std::size_t>(ref.kind);
        const auto mix = [&seed](const std::size_t value) {
            seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        };
        mix(static_cast<std::size_t>(ref.a));
        mix(static_cast<std::size_t>(ref.b));
        mix(static_cast<std::size_t>(ref.c));
        mix(std::hash<std::string>{}(ref.path));
        return seed;
    }
};

} // namespace rwsman
