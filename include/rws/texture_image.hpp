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

// A DXT1 (BC1) DDS with a complete mip chain, as CSF's lightmaps are stored
// (flags 0xA1007, caps 0x401008, the top level's size as the linear size).
// Width and height are powers of two; alpha is ignored (opaque).
[[nodiscard]] std::vector<std::byte> encode_dds_dxt1(int width, int height, std::span<const std::uint8_t> rgba);

} // namespace rws
