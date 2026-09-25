#include "rws/world_model.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>

namespace rws {
namespace {

constexpr std::uint32_t struct_chunk = 0x01U;
constexpr std::uint32_t extension_chunk = 0x03U;
constexpr std::uint32_t material_list_chunk = 0x08U;
constexpr std::uint32_t atomic_section_chunk = 0x09U;
constexpr std::uint32_t plane_section_chunk = 0x0AU;
constexpr std::uint32_t world_chunk = 0x0BU;
constexpr std::uint64_t world_struct_size = 64U;
constexpr std::uint64_t plane_struct_size = 24U;
constexpr std::uint64_t sector_header_size = 44U;
constexpr std::size_t max_depth = 256U;  // guards recursion on damaged input
// Written bytes of a World Sector Pyro plug-in beyond its per-triangle array:
// version and presence words (0x006BEEB0); the size callback adds 4 more.
constexpr std::uint64_t pyro_sector_fixed = 8U;
constexpr std::uint32_t pyro_sector_overstatement = 4U;

std::string at(const char* message, const std::uint64_t offset) {
    return std::string(message) + " at 0x" + [&] {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string hex;
        auto value = offset;
        do {
            hex.insert(hex.begin(), digits[value & 0xFU]);
            value >>= 4U;
        } while (value != 0U);
        return hex;
    }();
}

class Parser {
public:
    Parser(const std::span<const std::byte> bytes, const std::uint32_t library)
        : bytes_(bytes), library_(library) {}

    std::string error;

    bool u32(const std::uint64_t offset, std::uint32_t& value) {
        if (offset > bytes_.size() || bytes_.size() - offset < 4U) return fail("Truncated field", offset);
        const auto i = static_cast<std::size_t>(offset);
        value = std::to_integer<std::uint32_t>(bytes_[i]) |
                (std::to_integer<std::uint32_t>(bytes_[i + 1]) << 8U) |
                (std::to_integer<std::uint32_t>(bytes_[i + 2]) << 16U) |
                (std::to_integer<std::uint32_t>(bytes_[i + 3]) << 24U);
        return true;
    }
    std::uint16_t u16_unchecked(const std::uint64_t offset) const {
        const auto i = static_cast<std::size_t>(offset);
        return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes_[i]) |
                                          (std::to_integer<std::uint16_t>(bytes_[i + 1]) << 8U));
    }
    bool f32(const std::uint64_t offset, float& value) {
        std::uint32_t raw{};
        if (!u32(offset, raw)) return false;
        value = std::bit_cast<float>(raw);
        return true;
    }
    bool vec3(const std::uint64_t offset, Vec3& value) {
        return f32(offset, value.x) && f32(offset + 4U, value.y) && f32(offset + 8U, value.z);
    }
    bool fail(const char* message, const std::uint64_t offset) {
        if (error.empty()) error = at(message, offset);
        return false;
    }

    // Reads a chunk header of the expected type and returns its declared size.
    bool header(const std::uint64_t offset, const std::uint32_t type, std::uint32_t& size) {
        std::uint32_t found{}, library{};
        if (!u32(offset, found) || !u32(offset + 4U, size) || !u32(offset + 8U, library)) return false;
        if (found != type) return fail("Unexpected chunk type", offset);
        if (library != library_) return fail("Chunk library stamp differs from the World", offset);
        return true;
    }
    bool payload(const std::uint64_t offset, const std::uint64_t size, std::vector<std::byte>& out) {
        if (offset > bytes_.size() || bytes_.size() - offset < size)
            return fail("Chunk payload runs past the end of the file", offset);
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
        out.assign(begin, begin + static_cast<std::ptrdiff_t>(size));
        return true;
    }

    // Extension children. `triangles` is set for a World Sector, whose Pyro
    // plug-in is read by its physical layout rather than its declared size.
    bool extension(std::uint64_t& offset, std::vector<WorldPlugin>& plugins,
                   const std::size_t* triangles) {
        std::uint32_t size{};
        if (!header(offset, extension_chunk, size)) return false;
        offset += 12U;
        std::int64_t remaining = size;
        while (remaining > 0) {
            std::uint32_t type{}, declared{}, library{};
            if (!u32(offset, type) || !u32(offset + 4U, declared) || !u32(offset + 8U, library))
                return false;
            WorldPlugin plugin{type, {}};
            std::uint64_t physical = declared;
            if (type == pyro_metadata_chunk && triangles) {
                std::uint32_t presence{};
                if (!u32(offset + 16U, presence)) return false;
                physical = pyro_sector_fixed + (presence != 0U ? *triangles : 0U);
                if (declared != physical && declared != physical + pyro_sector_overstatement)
                    return fail("World Sector Pyro plug-in size matches neither layout", offset);
            }
            if (!payload(offset + 12U, physical, plugin.payload)) return false;
            plugins.push_back(std::move(plugin));
            offset += 12U + physical;
            remaining -= static_cast<std::int64_t>(declared) + 12;
        }
        if (remaining != 0) return fail("Extension children overrun its declared size", offset);
        return true;
    }

    bool node(std::uint64_t& offset, const bool is_sector, WorldModel& world,
              const std::size_t depth) {
        if (depth > max_depth) return fail("World BSP is deeper than supported", offset);
        const auto node_index = world.nodes.size();
        world.nodes.push_back({is_sector, is_sector ? world.sectors.size() : world.planes.size()});
        std::uint32_t size{};
        if (is_sector) {
            if (!header(offset, atomic_section_chunk, size)) return false;
            WorldModelSector sector;
            if (!this->sector(offset + 12U, world.format, sector, offset)) return false;
            const auto triangles = sector.triangles.size();
            if (!extension(offset, sector.plugins, &triangles)) return false;
            world.sectors.push_back(std::move(sector));
            return true;
        }
        if (!header(offset, plane_section_chunk, size)) return false;
        std::uint32_t struct_size{};
        const auto data = offset + 24U;
        if (!header(offset + 12U, struct_chunk, struct_size)) return false;
        if (struct_size != plane_struct_size) return fail("Plane Section Struct is not 24 bytes", offset);
        WorldModelPlane plane;
        std::uint32_t left_kind{}, right_kind{};
        if (!u32(data, reinterpret_cast<std::uint32_t&>(plane.axis)) || !f32(data + 4U, plane.split) ||
            !u32(data + 8U, left_kind) || !u32(data + 12U, right_kind) ||
            !f32(data + 16U, plane.left_value) || !f32(data + 20U, plane.right_value))
            return false;
        const auto plane_index = world.planes.size();
        world.planes.push_back(plane);
        offset = data + plane_struct_size;
        world.planes[plane_index].left = world.nodes.size();
        if (!node(offset, left_kind != 0U, world, depth + 1U)) return false;
        world.planes[plane_index].right = world.nodes.size();
        if (!node(offset, right_kind != 0U, world, depth + 1U)) return false;
        (void)node_index;
        return true;
    }

    bool sector(const std::uint64_t struct_offset, const std::uint32_t format,
                WorldModelSector& sector, std::uint64_t& end) {
        std::uint32_t size{};
        if (!header(struct_offset, struct_chunk, size)) return false;
        const auto data = struct_offset + 12U;
        std::uint32_t triangles{}, vertices{};
        if (!u32(data, reinterpret_cast<std::uint32_t&>(sector.material_window_base)) ||
            !u32(data + 4U, triangles) || !u32(data + 8U, vertices) ||
            !vec3(data + 12U, sector.bounding_box_inf) || !vec3(data + 24U, sector.bounding_box_sup) ||
            !u32(data + 36U, sector.collision_sector_present) || !u32(data + 40U, sector.unused))
            return false;
        if (triangles > std::numeric_limits<std::int32_t>::max() ||
            vertices > std::numeric_limits<std::int32_t>::max())
            return fail("World Sector counts are negative", struct_offset);
        const std::uint64_t sets = world_texcoord_sets(format);
        const std::uint64_t per_vertex = 12U + ((format & 0x10U) ? 4U : 0U) +
                                         ((format & 0x08U) ? 4U : 0U) + sets * 8U;
        const auto expected = sector_header_size + per_vertex * vertices + 8ULL * triangles;
        if (expected != size) return fail("World Sector Struct size does not match its counts", struct_offset);
        if (data > bytes_.size() || bytes_.size() - data < size)
            return fail("World Sector Struct runs past the end of the file", struct_offset);
        auto cursor = data + sector_header_size;
        sector.positions.resize(vertices);
        for (auto& position : sector.positions) {
            (void)vec3(cursor, position);
            cursor += 12U;
        }
        const auto words = [&](std::vector<std::uint32_t>& out) {
            out.resize(vertices);
            for (auto& value : out) {
                (void)u32(cursor, value);
                cursor += 4U;
            }
        };
        if (format & 0x10U) words(sector.normals);
        if (format & 0x08U) words(sector.prelight);
        sector.texcoords.resize(sets);
        for (auto& set : sector.texcoords) {
            set.resize(vertices);
            for (auto& uv : set) {
                (void)f32(cursor, uv[0]);
                (void)f32(cursor + 4U, uv[1]);
                cursor += 8U;
            }
        }
        sector.triangles.resize(triangles);
        for (auto& triangle : sector.triangles) {
            for (std::size_t i = 0; i < 3; ++i) triangle.vertices[i] = u16_unchecked(cursor + 2U * i);
            triangle.material = u16_unchecked(cursor + 6U);
            cursor += 8U;
        }
        end = cursor;
        return true;
    }

private:
    std::span<const std::byte> bytes_;
    std::uint32_t library_;
};

class Writer {
public:
    explicit Writer(const WorldWriteOptions& options) : options_(options) {}
    std::vector<std::byte> out;

    void u32(const std::uint32_t value) {
        for (unsigned shift = 0; shift < 32U; shift += 8U)
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
    void u16(const std::uint16_t value) {
        out.push_back(static_cast<std::byte>(value & 0xFFU));
        out.push_back(static_cast<std::byte>(value >> 8U));
    }
    void f32(const float value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void vec3(const Vec3 value) {
        f32(value.x);
        f32(value.y);
        f32(value.z);
    }
    void bytes(const std::vector<std::byte>& value) { out.insert(out.end(), value.begin(), value.end()); }

    // Opens a chunk; close() patches its size to the written payload plus any
    // declared-but-unwritten bytes of the children.
    std::size_t open(const std::uint32_t type) {
        const auto start = out.size();
        u32(type);
        u32(0);
        u32(library);
        return start;
    }
    void close(const std::size_t start, const std::uint64_t overstatement) {
        const auto size = out.size() - start - 12U + overstatement;
        for (unsigned i = 0; i < 4U; ++i)
            out[start + 4U + i] = static_cast<std::byte>((size >> (8U * i)) & 0xFFU);
    }

    std::uint64_t extension(const std::vector<WorldPlugin>& plugins, const bool sector) {
        const auto start = open(extension_chunk);
        std::uint64_t overstatement = 0;
        for (const auto& plugin : plugins) {
            const auto child = open(plugin.type);
            bytes(plugin.payload);
            const std::uint64_t extra = sector && plugin.type == pyro_metadata_chunk &&
                                                options_.reproduce_pyro_size_overstatement
                                            ? pyro_sector_overstatement
                                            : 0U;
            close(child, extra);
            overstatement += extra;
        }
        close(start, overstatement);
        return overstatement;
    }

    std::uint64_t node(const WorldModel& world, const std::size_t index) {
        const auto& entry = world.nodes[index];
        if (entry.is_sector) {
            const auto& sector = world.sectors[entry.index];
            const auto start = open(atomic_section_chunk);
            const auto data = open(struct_chunk);
            u32(static_cast<std::uint32_t>(sector.material_window_base));
            u32(static_cast<std::uint32_t>(sector.triangles.size()));
            u32(static_cast<std::uint32_t>(sector.positions.size()));
            vec3(sector.bounding_box_inf);
            vec3(sector.bounding_box_sup);
            u32(sector.collision_sector_present);
            u32(sector.unused);
            for (const auto& position : sector.positions) vec3(position);
            for (const auto value : sector.normals) u32(value);
            for (const auto value : sector.prelight) u32(value);
            for (const auto& set : sector.texcoords)
                for (const auto& uv : set) {
                    f32(uv[0]);
                    f32(uv[1]);
                }
            for (const auto& triangle : sector.triangles) {
                for (const auto vertex : triangle.vertices) u16(vertex);
                u16(triangle.material);
            }
            close(data, 0);
            const auto overstatement = extension(sector.plugins, true);
            close(start, overstatement);
            return overstatement;
        }
        const auto& plane = world.planes[entry.index];
        const auto start = open(plane_section_chunk);
        const auto data = open(struct_chunk);
        u32(static_cast<std::uint32_t>(plane.axis));
        f32(plane.split);
        u32(world.nodes[plane.left].is_sector ? 1U : 0U);
        u32(world.nodes[plane.right].is_sector ? 1U : 0U);
        f32(plane.left_value);
        f32(plane.right_value);
        close(data, 0);
        const auto overstatement = node(world, plane.left) + node(world, plane.right);
        close(start, overstatement);
        return overstatement;
    }

    std::uint32_t library{};

private:
    WorldWriteOptions options_;
};

} // namespace

std::uint32_t world_texcoord_sets(const std::uint32_t format) noexcept {
    const auto sets = (format >> 16U) & 0xFFU;
    if (sets != 0U) return sets;
    return (format & 0x80U) ? 2U : ((format & 0x04U) ? 1U : 0U);
}

DecodeResult<WorldModel> parse_world_model(const std::span<const std::byte> bytes,
                                           const std::uint64_t offset, std::uint64_t* end) {
    DecodeResult<WorldModel> result;
    WorldModel world;
    Parser probe(bytes, 0);
    if (!probe.u32(offset + 8U, world.library_id)) {
        result.error = probe.error;
        return result;
    }
    Parser parser(bytes, world.library_id);
    const auto failed = [&]() {
        result.error = parser.error.empty() ? "World parse failed" : parser.error;
        return result;
    };
    std::uint32_t size{}, struct_size{}, root_kind{};
    if (!parser.header(offset, world_chunk, size) || !parser.header(offset + 12U, struct_chunk, struct_size))
        return failed();
    if (struct_size != world_struct_size) {
        parser.fail("World Struct is not the 64-byte 3.7 layout", offset);
        return failed();
    }
    const auto data = offset + 24U;
    if (!parser.u32(data, root_kind) || !parser.vec3(data + 4U, world.inverse_origin) ||
        !parser.u32(data + 16U, reinterpret_cast<std::uint32_t&>(world.triangle_count)) ||
        !parser.u32(data + 20U, reinterpret_cast<std::uint32_t&>(world.vertex_count)) ||
        !parser.u32(data + 24U, reinterpret_cast<std::uint32_t&>(world.plane_sector_count)) ||
        !parser.u32(data + 28U, reinterpret_cast<std::uint32_t&>(world.world_sector_count)) ||
        !parser.u32(data + 32U, reinterpret_cast<std::uint32_t&>(world.collision_sector_size)) ||
        !parser.u32(data + 36U, world.format) || !parser.vec3(data + 40U, world.bounding_box_sup) ||
        !parser.vec3(data + 52U, world.bounding_box_inf))
        return failed();
    auto cursor = data + world_struct_size;
    std::uint32_t list_size{};
    if (!parser.header(cursor, material_list_chunk, list_size) ||
        !parser.payload(cursor + 12U, list_size, world.material_list))
        return failed();
    cursor += 12U + list_size;
    if (!parser.node(cursor, root_kind != 0U, world, 0) ||
        !parser.extension(cursor, world.plugins, nullptr))
        return failed();
    if (end) *end = cursor;
    result.value = std::move(world);
    return result;
}

std::vector<std::byte> write_world_model(const WorldModel& world, const WorldWriteOptions& options) {
    Writer writer(options);
    writer.library = world.library_id;
    const auto start = writer.open(world_chunk);
    const auto data = writer.open(struct_chunk);
    writer.u32(!world.nodes.empty() && world.nodes.front().is_sector ? 1U : 0U);
    writer.vec3(world.inverse_origin);
    writer.u32(static_cast<std::uint32_t>(world.triangle_count));
    writer.u32(static_cast<std::uint32_t>(world.vertex_count));
    writer.u32(static_cast<std::uint32_t>(world.plane_sector_count));
    writer.u32(static_cast<std::uint32_t>(world.world_sector_count));
    writer.u32(static_cast<std::uint32_t>(world.collision_sector_size));
    writer.u32(world.format);
    writer.vec3(world.bounding_box_sup);
    writer.vec3(world.bounding_box_inf);
    writer.close(data, 0);
    const auto list = writer.open(material_list_chunk);
    writer.bytes(world.material_list);
    writer.close(list, 0);
    std::uint64_t overstatement = 0;
    if (!world.nodes.empty()) overstatement += writer.node(world, 0);
    overstatement += writer.extension(world.plugins, false);
    writer.close(start, overstatement);
    return std::move(writer.out);
}

DecodeResult<PyroSectorMetadata> decode_pyro_sector_metadata(const WorldPlugin& plugin,
                                                            const std::size_t triangle_count) {
    DecodeResult<PyroSectorMetadata> result;
    if (plugin.type != pyro_metadata_chunk) {
        result.error = "Plug-in is not the Pyro 0xFFFFFF00 chunk";
        return result;
    }
    Parser parser(plugin.payload, 0);
    PyroSectorMetadata metadata;
    std::uint32_t presence{};
    if (!parser.u32(0, metadata.version) || !parser.u32(4, presence)) {
        result.error = "Pyro World Sector plug-in is shorter than 8 bytes";
        return result;
    }
    metadata.present = presence != 0U;
    const auto expected = pyro_sector_fixed + (metadata.present ? triangle_count : 0U);
    if (plugin.payload.size() != expected) {
        result.error = "Pyro World Sector plug-in size does not match the triangle count";
        return result;
    }
    for (auto i = pyro_sector_fixed; i < expected; ++i)
        metadata.triangle_bytes.push_back(std::to_integer<std::uint8_t>(plugin.payload[i]));
    result.value = std::move(metadata);
    return result;
}

WorldPlugin encode_pyro_sector_metadata(const PyroSectorMetadata& metadata) {
    Writer writer({});
    writer.u32(metadata.version);
    writer.u32(metadata.present ? 1U : 0U);
    if (metadata.present)
        for (const auto value : metadata.triangle_bytes) writer.out.push_back(static_cast<std::byte>(value));
    return {pyro_metadata_chunk, std::move(writer.out)};
}

std::optional<std::uint64_t> find_map_world(const std::span<const std::byte> bytes) {
    constexpr std::uint32_t scene_instance_chunk = 0x16FC0U;
    // Physical length is 116 + name length against a declared 92 + name length,
    // so 12 bytes follow the declared payload (FUN_006C47C0).
    constexpr std::uint64_t scene_instance_undeclared = 12U;
    Parser parser(bytes, 0);
    std::uint64_t offset = 0;
    std::uint32_t type{}, size{};
    while (parser.u32(offset, type) && parser.u32(offset + 4U, size) && offset + 12U <= bytes.size()) {
        if (type == world_chunk) return offset;
        offset += 12U + size + (type == scene_instance_chunk ? scene_instance_undeclared : 0U);
    }
    return std::nullopt;
}

std::vector<std::string> check_world_model(const WorldModel& world) {
    std::vector<std::string> problems;
    if (world.nodes.empty()) {
        problems.emplace_back("World has no BSP nodes");
        return problems;
    }
    std::vector<int> parents(world.nodes.size(), 0);
    std::size_t sector_nodes = 0, plane_nodes = 0;
    for (const auto& node : world.nodes) {
        if (node.is_sector) {
            ++sector_nodes;
            if (node.index >= world.sectors.size()) problems.emplace_back("Node names a missing sector");
        } else {
            ++plane_nodes;
            if (node.index >= world.planes.size()) {
                problems.emplace_back("Node names a missing plane");
                continue;
            }
            const auto& plane = world.planes[node.index];
            if (plane.left >= world.nodes.size() || plane.right >= world.nodes.size()) {
                problems.emplace_back("Plane child is out of range");
                continue;
            }
            ++parents[plane.left];
            ++parents[plane.right];
            if (plane.axis != 0 && plane.axis != 4 && plane.axis != 8)
                problems.emplace_back("Plane axis is not 0, 4 or 8");
        }
    }
    if (parents[0] != 0) problems.emplace_back("Root node has a parent");
    for (std::size_t i = 1; i < parents.size(); ++i)
        if (parents[i] != 1) problems.emplace_back("Node " + std::to_string(i) + " does not have exactly one parent");
    if (sector_nodes != world.sectors.size() || plane_nodes != world.planes.size())
        problems.emplace_back("Node list does not reference every plane and sector once");
    const auto sets = world_texcoord_sets(world.format);
    for (std::size_t i = 0; i < world.sectors.size(); ++i) {
        const auto& sector = world.sectors[i];
        const auto name = "Sector " + std::to_string(i) + ": ";
        const auto vertices = sector.positions.size();
        // The reader keeps both counts as u16 (sector +0x82/+0x84, KB-world-geometry-5).
        if (vertices > 0xFFFFU) problems.push_back(name + "more than 65535 vertices");
        if (sector.triangles.size() > 0xFFFFU) problems.push_back(name + "more than 65535 triangles");
        if ((world.format & 0x10U) ? sector.normals.size() != vertices : !sector.normals.empty())
            problems.push_back(name + "normal array does not match the World format");
        if ((world.format & 0x08U) ? sector.prelight.size() != vertices : !sector.prelight.empty())
            problems.push_back(name + "prelight array does not match the World format");
        if (sector.texcoords.size() != sets)
            problems.push_back(name + "texture coordinate set count does not match the World format");
        for (const auto& set : sector.texcoords)
            if (set.size() != vertices) problems.push_back(name + "texture coordinate array size differs");
        for (const auto& triangle : sector.triangles)
            for (const auto vertex : triangle.vertices)
                if (vertex >= vertices) {
                    problems.push_back(name + "triangle vertex index out of range");
                    break;
                }
    }
    return problems;
}

void update_world_counts(WorldModel& world) {
    std::int64_t triangles = 0, vertices = 0;
    for (const auto& sector : world.sectors) {
        triangles += static_cast<std::int64_t>(sector.triangles.size());
        vertices += static_cast<std::int64_t>(sector.positions.size());
    }
    world.triangle_count = static_cast<std::int32_t>(triangles);
    world.vertex_count = static_cast<std::int32_t>(vertices);
    world.plane_sector_count = static_cast<std::int32_t>(world.planes.size());
    world.world_sector_count = static_cast<std::int32_t>(world.sectors.size());
}

namespace {

constexpr std::uint32_t material_chunk = 0x07U;
constexpr std::uint32_t bin_mesh_chunk = 0x50EU;
constexpr std::uint32_t right_to_render_chunk = 0x1FU;
constexpr std::uint32_t material_effects_chunk = 0x120U;
constexpr std::uint32_t user_data_chunk = 0x11FU;
// Three u16 vertex indices per triangle and u16 sector counts.
constexpr std::size_t max_sector_triangles = 0xFFFFU / 3U;

float axis_value(const Vec3& value, const std::size_t axis) noexcept {
    return axis == 0 ? value.x : (axis == 1 ? value.y : value.z);
}

struct Bounds {
    Vec3 inf{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    Vec3 sup{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
             std::numeric_limits<float>::lowest()};
    void add(const Vec3& p) {
        inf = {std::min(inf.x, p.x), std::min(inf.y, p.y), std::min(inf.z, p.z)};
        sup = {std::max(sup.x, p.x), std::max(sup.y, p.y), std::max(sup.z, p.z)};
    }
};

Bounds triangle_bounds(const WorldBuildTriangle& triangle) {
    Bounds bounds;
    for (const auto& vertex : triangle.vertices) bounds.add(vertex.position);
    return bounds;
}

std::uint32_t pack_normal(const Vec3& normal) {
    const auto length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    const auto component = [&](const float value) {
        const auto scaled = length > 0.0F ? std::lround(value / length * 127.0F) : 0L;
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(static_cast<std::int8_t>(scaled)));
    };
    return component(normal.x) | (component(normal.y) << 8U) | (component(normal.z) << 16U);
}

std::vector<std::byte> little_endian(std::initializer_list<std::uint32_t> words) {
    Writer writer({});
    for (const auto word : words) writer.u32(word);
    return std::move(writer.out);
}

class Builder {
public:
    Builder(const std::span<const WorldBuildTriangle> triangles, const WorldBuildOptions& options,
            WorldModel& world)
        : triangles_(triangles), options_(options), world_(world),
          budget_(std::clamp<std::size_t>(options.max_sector_triangles, 1U, max_sector_triangles)) {}

    std::size_t node(std::vector<std::size_t> indices) {
        const auto index = world_.nodes.size();
        if (indices.size() <= budget_) {
            world_.nodes.push_back({true, world_.sectors.size()});
            world_.sectors.push_back(sector(indices));
            return index;
        }
        Bounds bounds;
        for (const auto i : indices) {
            const auto triangle = triangle_bounds(triangles_[i]);
            bounds.add(triangle.inf);
            bounds.add(triangle.sup);
        }
        const Vec3 extent{bounds.sup.x - bounds.inf.x, bounds.sup.y - bounds.inf.y,
                          bounds.sup.z - bounds.inf.z};
        std::size_t axis = 0;
        if (extent.y > axis_value(extent, axis)) axis = 1;
        if (extent.z > axis_value(extent, axis)) axis = 2;
        const auto centroid = [&](const std::size_t i) {
            const auto& v = triangles_[i].vertices;
            return axis_value(v[0].position, axis) + axis_value(v[1].position, axis) +
                   axis_value(v[2].position, axis);
        };
        // Median split on centroids; a triangle is never cut, so the two
        // children may overlap by up to one triangle's extent.
        const auto middle = indices.begin() + static_cast<std::ptrdiff_t>(indices.size() / 2U);
        std::nth_element(indices.begin(), middle, indices.end(),
                         [&](const std::size_t a, const std::size_t b) { return centroid(a) < centroid(b); });
        std::vector<std::size_t> left(indices.begin(), middle), right(middle, indices.end());
        WorldModelPlane plane;
        plane.axis = static_cast<std::int32_t>(axis * 4U);
        plane.split = centroid(*middle) / 3.0F;
        plane.left_value = std::numeric_limits<float>::lowest();
        plane.right_value = std::numeric_limits<float>::max();
        for (const auto i : left) plane.left_value = std::max(plane.left_value, axis_value(triangle_bounds(triangles_[i]).sup, axis));
        for (const auto i : right) plane.right_value = std::min(plane.right_value, axis_value(triangle_bounds(triangles_[i]).inf, axis));
        world_.nodes.push_back({false, world_.planes.size()});
        const auto plane_index = world_.planes.size();
        world_.planes.push_back(plane);
        const auto left_node = node(std::move(left));
        const auto right_node = node(std::move(right));
        world_.planes[plane_index].left = left_node;
        world_.planes[plane_index].right = right_node;
        return index;
    }

private:
    WorldModelSector sector(std::vector<std::size_t> indices) {
        std::ranges::stable_sort(indices, {}, [&](const std::size_t i) { return triangles_[i].material; });
        WorldModelSector result;
        const auto sets = std::min<std::uint32_t>(world_texcoord_sets(options_.format), 2U);
        result.texcoords.resize(sets);
        std::map<std::array<std::uint32_t, 9>, std::uint16_t> unique;
        Bounds bounds;
        // Every shipped sector has material window base 0, and the runtime
        // reads a triangle's material as an ordinal into the World list
        // (KB-world-geometry-4/R3), so materials stay absolute.
        constexpr std::uint16_t base = 0;
        result.material_window_base = base;
        std::vector<std::uint8_t> pyro;
        for (const auto i : indices) {
            const auto& source = triangles_[i];
            WorldTriangle triangle;
            for (std::size_t corner = 0; corner < 3U; ++corner) {
                const auto& vertex = source.vertices[corner];
                const auto normal = (options_.format & 0x10U) ? pack_normal(vertex.normal) : 0U;
                const auto prelight = (options_.format & 0x08U) ? vertex.prelight : 0U;
                std::array<std::uint32_t, 9> key{
                    std::bit_cast<std::uint32_t>(vertex.position.x),
                    std::bit_cast<std::uint32_t>(vertex.position.y),
                    std::bit_cast<std::uint32_t>(vertex.position.z), normal, prelight, 0, 0, 0, 0};
                for (std::uint32_t set = 0; set < sets; ++set) {
                    key[5 + 2 * set] = std::bit_cast<std::uint32_t>(vertex.texcoords[set][0]);
                    key[6 + 2 * set] = std::bit_cast<std::uint32_t>(vertex.texcoords[set][1]);
                }
                const auto [it, added] = unique.try_emplace(key, static_cast<std::uint16_t>(result.positions.size()));
                if (added) {
                    result.positions.push_back(vertex.position);
                    bounds.add(vertex.position);
                    if (options_.format & 0x10U) result.normals.push_back(normal);
                    if (options_.format & 0x08U) result.prelight.push_back(prelight);
                    for (std::uint32_t set = 0; set < sets; ++set) result.texcoords[set].push_back(vertex.texcoords[set]);
                }
                triangle.vertices[corner] = it->second;
            }
            triangle.material = static_cast<std::uint16_t>(source.material - base);
            result.triangles.push_back(triangle);
            pyro.push_back(source.pyro);
        }
        if (!indices.empty()) {
            result.bounding_box_inf = bounds.inf;
            result.bounding_box_sup = bounds.sup;
        }
        result.plugins.push_back(bin_mesh(result));
        if (!options_.visual_plugins && !result.triangles.empty())
            result.plugins.push_back(build_collision_tree(result, options_.library_id));
        if (options_.visual_plugins) {
            result.plugins.push_back({right_to_render_chunk, little_endian({material_effects_chunk, 0U})});
            result.plugins.push_back({material_effects_chunk, little_endian({1U})});
        }
        result.plugins.push_back(fvf_user_data());
        const bool present = !options_.visual_plugins && !pyro.empty();
        result.plugins.push_back(encode_pyro_sector_metadata({1U, present, present ? pyro : std::vector<std::uint8_t>{}}));
        return result;
    }

    static WorldPlugin bin_mesh(const WorldModelSector& sector) {
        // Triangle list (flags 0), one mesh per material in triangle order,
        // absolute material indices: the only form in the shipped Worlds.
        std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> meshes;
        for (const auto& triangle : sector.triangles) {
            const auto material = static_cast<std::uint32_t>(sector.material_window_base) + triangle.material;
            if (meshes.empty() || meshes.back().first != material) meshes.push_back({material, {}});
            for (const auto vertex : triangle.vertices) meshes.back().second.push_back(vertex);
        }
        Writer writer({});
        writer.u32(0U);
        writer.u32(static_cast<std::uint32_t>(meshes.size()));
        writer.u32(static_cast<std::uint32_t>(sector.triangles.size() * 3U));
        for (const auto& [material, indices] : meshes) {
            writer.u32(static_cast<std::uint32_t>(indices.size()));
            writer.u32(material);
            for (const auto index : indices) writer.u32(index);
        }
        return {bin_mesh_chunk, std::move(writer.out)};
    }

    static WorldPlugin fvf_user_data() {
        static constexpr char name[] = "FVF.UserData";
        Writer writer({});
        writer.u32(1U);            // one array
        writer.u32(sizeof name);   // name length including the terminator
        for (const char c : name) writer.out.push_back(static_cast<std::byte>(c));
        writer.u32(1U);            // format: 32-bit integer
        writer.u32(1U);            // one element
        writer.u32(0x3003U);
        return {user_data_chunk, std::move(writer.out)};
    }

    std::span<const WorldBuildTriangle> triangles_;
    const WorldBuildOptions& options_;
    WorldModel& world_;
    std::size_t budget_;
};

} // namespace

DecodeResult<std::vector<std::vector<std::byte>>>
split_material_list(const std::span<const std::byte> payload, const std::uint32_t library_id) {
    DecodeResult<std::vector<std::vector<std::byte>>> result;
    Parser parser(payload, library_id);
    std::uint32_t size{}, count{};
    if (!parser.header(0, struct_chunk, size) || !parser.u32(12, count) || size != 4U + 4ULL * count) {
        result.error = parser.error.empty() ? "Material List Struct does not match its count" : parser.error;
        return result;
    }
    std::vector<std::vector<std::byte>> materials;
    std::uint64_t cursor = 12U + size;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t index{};
        (void)parser.u32(16U + 4ULL * i, index);
        const auto reference = std::bit_cast<std::int32_t>(index);
        if (reference >= 0) {
            if (static_cast<std::uint32_t>(reference) >= i) {
                result.error = "Material List entry references a later material";
                return result;
            }
            materials.push_back(materials[static_cast<std::size_t>(reference)]);
            continue;
        }
        std::uint32_t material_size{};
        std::vector<std::byte> material;
        if (!parser.header(cursor, material_chunk, material_size) ||
            !parser.payload(cursor, 12ULL + material_size, material)) {
            result.error = parser.error;
            return result;
        }
        materials.push_back(std::move(material));
        cursor += 12ULL + material_size;
    }
    if (cursor != payload.size()) {
        result.error = "Material List has bytes after its last material";
        return result;
    }
    result.value = std::move(materials);
    return result;
}

std::vector<std::byte> compose_material_list(const std::span<const std::vector<std::byte>> materials,
                                             const std::uint32_t library_id) {
    Writer writer({});
    writer.library = library_id;
    const auto data = writer.open(struct_chunk);
    writer.u32(static_cast<std::uint32_t>(materials.size()));
    for (std::size_t i = 0; i < materials.size(); ++i) writer.u32(0xFFFFFFFFU);
    writer.close(data, 0);
    for (const auto& material : materials) writer.bytes(material);
    return std::move(writer.out);
}

WorldPlugin build_collision_tree(const WorldModelSector& sector, const std::uint32_t library_id) {
    constexpr std::uint32_t collision_plugin = 0x11DU, coll_tree = 0x2CU;
    constexpr std::size_t leaf_triangles = 3;  // shipped leaves: mostly 1-3, rarely more
    struct Split {
        std::array<std::uint8_t, 2> type{}, contents{};
        std::array<std::uint16_t, 2> index{};
        std::array<float, 2> value{};
    };
    std::vector<Split> splits;
    std::vector<std::uint16_t> remap;
    const auto bounds = [&](const std::size_t t) {
        Bounds b;
        for (const auto v : sector.triangles[t].vertices) b.add(sector.positions[v]);
        return b;
    };
    // Returns the child descriptor {contents, index} for `indices`.
    const auto build = [&](auto&& self, std::vector<std::size_t> indices, const bool force_split)
        -> std::pair<std::uint8_t, std::uint16_t> {
        const auto leaf = [&] {
            const auto first = static_cast<std::uint16_t>(remap.size());
            for (const auto t : indices) remap.push_back(static_cast<std::uint16_t>(t));
            return std::pair{static_cast<std::uint8_t>(indices.size()), first};
        };
        if (indices.size() <= leaf_triangles && !force_split) return leaf();
        Bounds all;
        for (const auto t : indices) {
            const auto b = bounds(t);
            all.add(b.inf);
            all.add(b.sup);
        }
        std::size_t axis = 0;
        const Vec3 extent{all.sup.x - all.inf.x, all.sup.y - all.inf.y, all.sup.z - all.inf.z};
        if (extent.y > axis_value(extent, axis)) axis = 1;
        if (extent.z > axis_value(extent, axis)) axis = 2;
        const auto centre = [&](const std::size_t t) {
            const auto b = bounds(t);
            return axis_value(b.inf, axis) + axis_value(b.sup, axis);
        };
        std::ranges::stable_sort(indices, {}, centre);
        const auto half = std::max<std::size_t>(1, indices.size() / 2);
        // Leaves hold at most 254 triangles; beyond that a split is forced.
        if (indices.size() <= 254 && !force_split && centre(indices.front()) == centre(indices.back()))
            return leaf();
        std::vector<std::size_t> left(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(half));
        std::vector<std::size_t> right(indices.begin() + static_cast<std::ptrdiff_t>(half), indices.end());
        const auto index = splits.size();
        splits.emplace_back();
        Split split;
        split.type = {static_cast<std::uint8_t>(axis * 4U + 1U), static_cast<std::uint8_t>(axis * 4U)};
        split.value = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max()};
        for (const auto t : left) split.value[0] = std::max(split.value[0], axis_value(bounds(t).sup, axis));
        for (const auto t : right) split.value[1] = std::min(split.value[1], axis_value(bounds(t).inf, axis));
        if (right.empty()) split.value[1] = split.value[0];
        for (std::size_t side = 0; side < 2; ++side) {
            auto& part = side == 0 ? left : right;
            if (part.size() > leaf_triangles) {
                split.contents[side] = 0xFFU;
                split.index[side] = static_cast<std::uint16_t>(splits.size());
                (void)self(self, std::move(part), false);
            } else {
                const auto [count, first] = self(self, std::move(part), false);
                split.contents[side] = count;
                split.index[side] = first;
            }
        }
        splits[index] = split;
        return {0xFFU, static_cast<std::uint16_t>(index)};
    };
    std::vector<std::size_t> all(sector.triangles.size());
    std::iota(all.begin(), all.end(), std::size_t{0});
    (void)build(build, std::move(all), true);  // every shipped tree has a split
    Writer tree({});
    tree.u32(1U);
    tree.vec3(sector.bounding_box_inf);
    tree.vec3(sector.bounding_box_sup);
    tree.u32(static_cast<std::uint32_t>(sector.triangles.size()));
    tree.u32(static_cast<std::uint32_t>(splits.size()));
    for (const auto& split : splits)
        for (std::size_t side = 0; side < 2; ++side) {
            tree.out.push_back(static_cast<std::byte>(split.type[side]));
            tree.out.push_back(static_cast<std::byte>(split.contents[side]));
            tree.u16(split.index[side]);
            tree.f32(split.value[side]);
        }
    for (const auto t : remap) tree.u16(t);
    Writer plugin({});
    plugin.library = library_id;
    plugin.u32(0x37002U);
    const auto outer = plugin.open(coll_tree);
    const auto data = plugin.open(struct_chunk);
    plugin.bytes(tree.out);
    plugin.close(data, 0);
    plugin.close(outer, 0);
    return {collision_plugin, std::move(plugin.out)};
}

std::vector<WorldBuildTriangle> world_build_triangles(const WorldModel& world) {
    std::vector<WorldBuildTriangle> result;
    const auto unpack = [](const std::uint32_t packed) {
        const auto component = [&](const unsigned shift) {
            return static_cast<float>(static_cast<std::int8_t>((packed >> shift) & 0xFFU)) / 127.0F;
        };
        return Vec3{component(0), component(8), component(16)};
    };
    for (const auto& sector : world.sectors) {
        std::vector<std::uint8_t> pyro;
        for (const auto& plugin : sector.plugins)
            if (plugin.type == pyro_metadata_chunk)
                if (const auto decoded = decode_pyro_sector_metadata(plugin, sector.triangles.size()))
                    pyro = decoded.value->triangle_bytes;
        for (std::size_t t = 0; t < sector.triangles.size(); ++t) {
            const auto& source = sector.triangles[t];
            WorldBuildTriangle triangle;
            triangle.material = static_cast<std::uint16_t>(sector.material_window_base + source.material);
            triangle.pyro = t < pyro.size() ? pyro[t] : 0U;
            for (std::size_t corner = 0; corner < 3U; ++corner) {
                const auto v = source.vertices[corner];
                auto& vertex = triangle.vertices[corner];
                vertex.position = sector.positions[v];
                if (v < sector.normals.size()) vertex.normal = unpack(sector.normals[v]);
                if (v < sector.prelight.size()) vertex.prelight = sector.prelight[v];
                for (std::size_t set = 0; set < std::min<std::size_t>(2U, sector.texcoords.size()); ++set)
                    vertex.texcoords[set] = sector.texcoords[set][v];
            }
            result.push_back(triangle);
        }
    }
    return result;
}

DecodeResult<WorldModel> build_world(const std::span<const WorldBuildTriangle> triangles,
                                     const WorldBuildOptions& options) {
    DecodeResult<WorldModel> result;
    if (triangles.empty()) {
        result.error = "A World needs at least one triangle";
        return result;
    }
    // Collision triangles carry the Pyro shade in the material's high byte at
    // runtime (FUN_006BD470), leaving 256 material ordinals.
    if (!options.visual_plugins &&
        std::ranges::any_of(triangles, [](const auto& t) { return t.material > 0xFFU; })) {
        result.error = "A collision World can use at most 256 materials";
        return result;
    }
    WorldModel world;
    world.library_id = options.library_id;
    world.format = options.format;
    world.material_list = options.material_list;
    Bounds bounds;
    for (const auto& triangle : triangles)
        for (const auto& vertex : triangle.vertices) bounds.add(vertex.position);
    world.bounding_box_inf = bounds.inf;
    world.bounding_box_sup = bounds.sup;
    std::vector<std::size_t> indices(triangles.size());
    std::iota(indices.begin(), indices.end(), std::size_t{0});
    Builder builder(triangles, options, world);
    (void)builder.node(std::move(indices));
    update_world_counts(world);
    world.plugins = {};
    result.value = std::move(world);
    return result;
}

} // namespace rws
