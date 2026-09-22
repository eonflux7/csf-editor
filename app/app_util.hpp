#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace rwsman {

[[nodiscard]] inline std::string path_utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

// `path` renamed to <stem><suffix>[<extension>]. It joins native path pieces, so unlike
// stem().string() it cannot throw on Windows for names outside the ANSI code page.
[[nodiscard]] inline std::filesystem::path with_stem_suffix(const std::filesystem::path& path,
                                                            const std::string_view suffix,
                                                            const bool keep_extension) {
    auto name = path.stem();
    name += suffix;
    if (keep_extension) name += path.extension();
    auto result = path;
    result.replace_filename(name);
    return result;
}

[[nodiscard]] inline std::string lower_ascii(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

[[nodiscard]] inline std::string upper_ascii(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char value) {
        return static_cast<char>(std::toupper(value));
    });
    return result;
}

// FNV-1a over the whole byte range: a content signature that survives renames
// and moves, used to key per-mission settings such as camera bookmarks.
[[nodiscard]] inline std::string content_signature(const std::string_view prefix,
                                                   const std::span<const std::byte> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto byte : bytes) {
        hash ^= std::to_integer<std::uint64_t>(byte);
        hash *= 0x100000001b3ULL;
    }
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(prefix) + ":" + text;
}

} // namespace rwsman
