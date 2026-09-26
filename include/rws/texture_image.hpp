#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace rws {

// Decode a texture image referenced by the preview (DDS or PNG) into tightly
// packed, top-down RGBA8 pixels. The dimensions and 'rgba' buffer are only
// modified on success; 'error' describes the failure otherwise.
bool decode_texture_image(const std::filesystem::path& path, int& width, int& height,
                          std::vector<std::uint8_t>& rgba, std::string& error);

// Decode an in-memory PNG stream into tightly packed, top-down RGBA8 pixels.
bool decode_png(std::span<const std::byte> bytes, int& width, int& height,
                std::vector<std::uint8_t>& rgba, std::string& error);

// Encode tightly packed, top-down RGBA8 pixels as a PNG file. Existing files are
// never replaced: the caller chooses a new path. 'error' describes a failure.
bool write_png_rgba(const std::filesystem::path& path, int width, int height,
                    std::span<const std::uint8_t> rgba, std::string& error);

// How two RGBA8 images differ: pixels whose largest channel difference
// (alpha ignored) exceeds `tolerance`, and a diff image (the second image
// dimmed, changed pixels in magenta) of the second image's size. Images of
// different sizes differ in every pixel.
struct ImageDifference {
    bool same_size{};
    std::size_t changed_pixels{}, total_pixels{};
    int max_channel_delta{};
    std::vector<std::uint8_t> diff_rgba;
    [[nodiscard]] double changed_percent() const {
        return total_pixels ? 100.0 * static_cast<double>(changed_pixels) / static_cast<double>(total_pixels) : 0.0;
    }
};
[[nodiscard]] ImageDifference compare_rgba_images(int width_a, int height_a, std::span<const std::uint8_t> a,
                                                  int width_b, int height_b, std::span<const std::uint8_t> b,
                                                  int tolerance);

// A DXT1 (BC1) DDS with a complete mip chain, as CSF's lightmaps are stored
// (flags 0xA1007, caps 0x401008, the top level's size as the linear size).
// Width and height are powers of two; alpha is ignored (opaque).
[[nodiscard]] std::vector<std::byte> encode_dds_dxt1(int width, int height, std::span<const std::uint8_t> rgba);

} // namespace rws
