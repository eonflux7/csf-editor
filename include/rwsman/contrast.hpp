#pragma once

#include <cstdint>

namespace rwsman {

// WCAG 2 contrast ratio (1 to 21) of two opaque 0xRRGGBB colours. Body text
// needs 4.5 (AA); large text and UI glyphs 3.
[[nodiscard]] double contrast_ratio(std::uint32_t first_rgb, std::uint32_t second_rgb);

} // namespace rwsman
