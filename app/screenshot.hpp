#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct GLFWwindow;

namespace rwsman {

// Reads the back buffer and writes it to a new PNG file. Existing files are not
// replaced. Call after rendering and before swapping buffers.
bool save_screenshot(GLFWwindow* window, const std::filesystem::path& path, std::string& error);
// Reads the back buffer as top-down RGBA8 (opaque).
bool capture_back_buffer(GLFWwindow* window, int& width, int& height, std::vector<std::uint8_t>& rgba,
                         std::string& error);

} // namespace rwsman
