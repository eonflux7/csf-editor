#include "screenshot.hpp"

#include "rws/texture_image.hpp"

#include <GLFW/glfw3.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace rwsman {

bool capture_back_buffer(GLFWwindow* window, int& width, int& height, std::vector<std::uint8_t>& rgba,
                         std::string& error) {
    glfwGetFramebufferSize(window, &width, &height);
    if (width <= 0 || height <= 0) {
        error = "The framebuffer is empty";
        return false;
    }
    const auto row = static_cast<std::size_t>(width) * 4U;
    std::vector<std::uint8_t> pixels(row * static_cast<std::size_t>(height));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // OpenGL returns rows bottom-up; PNG is top-down. Alpha is not meaningful.
    std::vector<std::uint8_t> flipped(pixels.size());
    for (int y = 0; y < height; ++y) {
        const auto* source = pixels.data() + row * static_cast<std::size_t>(height - 1 - y);
        auto* destination = flipped.data() + row * static_cast<std::size_t>(y);
        for (std::size_t i = 0; i < row; i += 4) {
            destination[i] = source[i];
            destination[i + 1] = source[i + 1];
            destination[i + 2] = source[i + 2];
            destination[i + 3] = 255;
        }
    }
    rgba = std::move(flipped);
    return true;
}

bool save_screenshot(GLFWwindow* window, const std::filesystem::path& path, std::string& error) {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    return capture_back_buffer(window, width, height, rgba, error) &&
           rws::write_png_rgba(path, width, height, rgba, error);
}

} // namespace rwsman
