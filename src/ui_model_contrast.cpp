#include "rwsman/contrast.hpp"

#include <algorithm>
#include <cmath>

namespace rwsman {
namespace {

double luminance(const std::uint32_t rgb) {
    const auto channel = [](const std::uint32_t value) {
        const double c = static_cast<double>(value & 0xFFU) / 255.0;
        return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(rgb >> 16U) + 0.7152 * channel(rgb >> 8U) + 0.0722 * channel(rgb);
}

} // namespace

double contrast_ratio(const std::uint32_t first_rgb, const std::uint32_t second_rgb) {
    const double a = luminance(first_rgb), b = luminance(second_rgb);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

} // namespace rwsman
