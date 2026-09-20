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

} // namespace rws
