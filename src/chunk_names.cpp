#include "rws/chunk.hpp"

namespace rws {

std::string_view chunk_name(const std::uint32_t type) noexcept {
    switch (type) {
    case 0x00:
        return "NA Object";
    case 0x01:
        return "Struct";
    case 0x02:
        return "String";
    case 0x03:
        return "Extension";
    case 0x05:
        return "Camera";
    case 0x06:
        return "Texture";
    case 0x07:
        return "Material";
    case 0x08:
        return "Material List";
    case 0x09:
        return "Atomic Section";
    case 0x0A:
        return "Plane Section";
    case 0x0B:
        return "World";
    case 0x0C:
        return "Spline";
    case 0x0D:
        return "Matrix";
    case 0x0E:
        return "Frame List";
    case 0x0F:
        return "Geometry";
    case 0x10:
        return "Clump";
    case 0x12:
        return "Light";
    case 0x13:
        return "Unicode String";
    case 0x14:
        return "Atomic";
    case 0x15:
        return "Texture Native";
    case 0x16:
        return "Texture Dictionary";
    case 0x1A:
        return "Geometry List";
    case 0x1B:
        return "Animation Animation";
    case 0x1C:
        return "Team";
    case 0x1D:
        return "Crowd";
    case 0x1E:
        return "Delta Morph Animation";
    case 0x1F:
        return "Right To Render";
    case 0x20:
        return "Multi Texture Effect Native";
    case 0x21:
        return "Multi Texture Effect Dictionary";
    case 0x22:
        return "Team Dictionary";
    case 0x23:
        return "Platform Independent Texture Dictionary";
    case 0x24:
        return "Table of Contents";
    case 0x25:
        return "Particle Standard Global Data";
    case 0x26:
        return "AltPipe";
    case 0x27:
        return "Platform Independent Peds";
    case 0x28:
        return "Patch Mesh";
    case 0x29:
        return "Chunk Group Start";
    case 0x2A:
        return "Chunk Group End";
    case 0x2B:
        return "UV Animation Dictionary";
    case 0x2C:
        return "Coll Tree";
    case 0x0105:
        return "Morph Plugin";
    case 0x0110:
        return "Sky Mipmap Value";
    case 0x0116:
        return "Skin Plugin";
    case 0x011D:
        return "Collision Plugin";
    case 0x011E:
        return "HAnim Plugin";
    case 0x011F:
        return "User Data Plugin";
    case 0x0120:
        return "Material Effects Plugin";
    case 0x0127:
        return "Anisotropy Plugin";
    case 0x050E:
        return "Bin Mesh Plugin";
    case 0x0510:
        return "Native Data Plugin";
    case 0x0907:
        return "RenderWare Physics Body Definition";
    case 0x0909:
        return "RenderWare Physics Ragdoll Definition";
    case 0x090B:
        return "RenderWare Physics Generic Definition";
    case 0xFFFFFF00U:
        return "Pyro Studios Object Metadata";
    default:
        return "Unknown";
    }
}

std::string_view chunk_vendor_name(const std::uint32_t vendor) noexcept {
    switch (vendor) {
    case 0x000000:
        return "RenderWare Core";
    case 0x000001:
        return "Criterion Toolkit";
    case 0x000002:
        return "Redline Racer";
    case 0x000003:
        return "Criterion CSL/RD";
    case 0x000004:
        return "Criterion Internal";
    case 0x000005:
        return "Criterion World";
    case 0x000006:
        return "Criterion Beta";
    case 0x000007:
        return "Criterion RenderWare Studio";
    case 0x000008:
        return "RenderWare Audio";
    case 0x000009:
        return "RenderWare Physics";
    default:
        return "Unknown vendor";
    }
}

bool is_container_chunk(const std::uint32_t type) noexcept {
    switch (type) {
    case 0x03: // extension: zero or more plugin chunks
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0E:
    case 0x0F:
    case 0x10:
    case 0x12:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x1A:
    case 0x0907:
    case 0x0909:
        return true;
    default:
        return false;
    }
}

} // namespace rws
