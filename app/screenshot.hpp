#pragma once

#include <filesystem>
#include <string>

struct GLFWwindow;

namespace rwsman {

// Reads the back buffer and writes it to a new PNG file. Existing files are not
// replaced. Call after rendering and before swapping buffers.
bool save_screenshot(GLFWwindow* window, const std::filesystem::path& path, std::string& error);

} // namespace rwsman
