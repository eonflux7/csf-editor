#pragma once

#include "rws/decoded.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rws {

// A model (an .rpc Clump stream) uniformly scaled about its origin: the frame
// translations and every Geometry's morph-target positions and bounding
// spheres. Normals, UVs, materials and every other chunk are kept byte for byte.
[[nodiscard]] DecodeResult<std::vector<std::byte>> scale_clump(std::span<const std::byte> bytes, float scale);

} // namespace rws
