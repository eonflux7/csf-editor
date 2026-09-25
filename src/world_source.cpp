#include "rws/world_source.hpp"

#include "rws/document.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace rws {
namespace {

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;
    while (!line.empty()) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos) break;
        line.remove_prefix(start);
        const auto end = line.find_first_of(" \t\r");
        result.push_back(line.substr(0, end));
        if (end == std::string_view::npos) break;
        line.remove_prefix(end);
    }
    return result;
}

template <typename T>
bool number(const std::string_view text, T& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

std::optional<Chunk> material_chunk(const std::span<const std::byte> material, Document& holder) {
    holder = Document::from_bytes(std::vector<std::byte>(material.begin(), material.end()));
    if (holder.chunks().empty() || holder.chunks().front().type != 0x07U) return std::nullopt;
    return holder.chunks().front();
}

} // namespace

std::string material_texture_name(const std::span<const std::byte> material) {
    Document holder;
    const auto chunk = material_chunk(material, holder);
    if (!chunk) return {};
    const auto* texture = find_child(*chunk, 0x06U);
    if (!texture) return {};
    const auto decoded = decode_texture(*texture, holder.bytes());
    return decoded ? decoded.value->name : std::string{};
}

std::string material_surface_name(const std::span<const std::byte> material) {
    Document holder;
    const auto chunk = material_chunk(material, holder);
    if (!chunk) return {};
    const auto* extension = find_child(*chunk, 0x03U);
    const auto* pyro = extension ? find_child(*extension, pyro_metadata_chunk) : nullptr;
    if (!pyro) return {};
    const auto decoded = decode_pyro_extension(*pyro, 0x07U, holder.bytes());
    return decoded ? std::string(decoded.value->object_name()) : std::string{};
}

DecodeResult<WorldSource> parse_world_source(const std::string_view text) {
    DecodeResult<WorldSource> result;
    WorldSource source;
    std::istringstream stream{std::string(text)};
    std::string line;
    std::size_t number_of_line = 0;
    bool header = false;
    const auto fail = [&](const std::string& message) {
        result.error = "line " + std::to_string(number_of_line) + ": " + message;
        return result;
    };
    while (std::getline(stream, line)) {
        ++number_of_line;
        const auto parts = fields(line);
        if (parts.empty() || parts[0].starts_with('#')) continue;
        if (!header) {
            if (parts.size() != 2 || parts[0] != "csfworld" || parts[1] != "1")
                return fail("expected 'csfworld 1'");
            header = true;
            continue;
        }
        if (parts[0] == "material") {
            if (parts.size() != 3 && parts.size() != 4) return fail("material <texture> <surface> [<shade>]");
            WorldSourceMaterial material{std::string(parts[1]), std::string(parts[2])};
            if (parts.size() == 4) {
                unsigned shade{};
                if (!number(parts[3], shade) || shade > 255U) return fail("shade must be 0-255");
                material.shade = static_cast<std::uint8_t>(shade);
            }
            source.materials.push_back(std::move(material));
        } else if (parts[0] == "v") {
            if (parts.size() != 9 && parts.size() != 11) return fail("v needs 8 or 10 numbers");
            std::array<float, 10> values{};
            for (std::size_t i = 1; i < parts.size(); ++i)
                if (!number(parts[i], values[i - 1])) return fail("invalid number '" + std::string(parts[i]) + "'");
            WorldBuildVertex vertex;
            vertex.position = {values[0], values[1], values[2]};
            vertex.normal = {values[3], values[4], values[5]};
            vertex.texcoords[0] = {values[6], values[7]};
            vertex.texcoords[1] = {values[8], values[9]};
            std::array<double, 3> exact{};
            for (std::size_t i = 0; i < 3; ++i) number(parts[1 + i], exact[i]);
            source.vertices.push_back(vertex);
            source.exact_positions.push_back(exact);
            source.has_second_uv.push_back(parts.size() == 11);
        } else if (parts[0] == "f") {
            if (parts.size() != 6) return fail("f <a> <b> <c> <material> <visual|collision|both>");
            WorldSourceFace face;
            for (std::size_t i = 0; i < 3; ++i)
                if (!number(parts[1 + i], face.vertices[i]) || face.vertices[i] >= source.vertices.size())
                    return fail("face vertex index out of range");
            if (!number(parts[4], face.material) || face.material >= source.materials.size())
                return fail("face material index out of range");
            if (parts[5] == "visual") face.collision = false;
            else if (parts[5] == "collision") face.visual = false;
            else if (parts[5] != "both") return fail("face role must be visual, collision or both");
            source.faces.push_back(face);
        } else if (parts[0] == "piece") {
            if (parts.size() != 10 && parts.size() != 11)
                return fail("piece <x0> <y0> <z0> <x1> <y1> <z1> <x> <y> <z> [<yaw>]");
            std::array<float, 10> values{};
            for (std::size_t i = 1; i < parts.size(); ++i)
                if (!number(parts[i], values[i - 1])) return fail("invalid piece numbers");
            WorldPiece piece;
            piece.inf = {std::min(values[0], values[3]), std::min(values[1], values[4]), std::min(values[2], values[5])};
            piece.sup = {std::max(values[0], values[3]), std::max(values[1], values[4]), std::max(values[2], values[5])};
            piece.position = {values[6], values[7], values[8]};
            piece.yaw_degrees = values[9];
            source.pieces.push_back(piece);
        } else if (parts[0] == "prop") {
            if (parts.size() != 5 && parts.size() != 6)
                return fail("prop <donor-instance-id>[,<id>...] <x> <y> <z> [<yaw>]");
            PlacedProp prop;
            for (auto ids = parts[1]; !ids.empty();) {
                const auto comma = ids.find(',');
                std::uint32_t id{};
                if (!number(ids.substr(0, comma), id)) return fail("invalid prop instance id");
                prop.donor_instance_ids.push_back(id);
                ids = comma == std::string_view::npos ? std::string_view{} : ids.substr(comma + 1);
            }
            if (!number(parts[2], prop.position.x) ||
                !number(parts[3], prop.position.y) || !number(parts[4], prop.position.z) ||
                (parts.size() == 6 && !number(parts[5], prop.yaw_degrees)))
                return fail("invalid prop numbers");
            source.props.push_back(prop);
        } else {
            return fail("unknown record '" + std::string(parts[0]) + "'");
        }
    }
    if (!header) {
        number_of_line = 0;
        return fail("empty world source");
    }
    result.value = std::move(source);
    return result;
}

DecodeResult<CompiledWorlds> compile_world_source(const WorldSource& source,
                                                  const WorldModel& donor_visual,
                                                  const WorldModel& donor_collision,
                                                  const WorldCompileOptions& options,
                                                  const std::span<const WorldBuildTriangle> extra_collision) {
    DecodeResult<CompiledWorlds> result;
    const auto visual_materials = split_material_list(donor_visual.material_list, donor_visual.library_id);
    const auto collision_materials =
        split_material_list(donor_collision.material_list, donor_collision.library_id);
    if (!visual_materials || !collision_materials) {
        result.error = "donor Material List: " +
                       (visual_materials ? collision_materials.error : visual_materials.error);
        return result;
    }
    CompiledWorlds compiled;
    // Visual: one copied donor material per texture name, in first-use order.
    std::map<std::string, std::uint16_t> visual_slot;
    std::vector<std::vector<std::byte>> visual_list;
    std::map<std::string, std::uint16_t> surface_slot;
    for (std::size_t i = 0; i < collision_materials.value->size(); ++i) {
        const auto name = lower(material_surface_name((*collision_materials.value)[i]));
        if (!name.empty()) surface_slot.try_emplace(name, static_cast<std::uint16_t>(i));
    }
    std::vector<WorldBuildTriangle> visual, collision;
    for (const auto& face : source.faces) {
        const auto& material = source.materials[face.material];
        WorldBuildTriangle triangle;
        for (std::size_t c = 0; c < 3; ++c) {
            triangle.vertices[c] = source.vertices[face.vertices[c]];
            if (options.constant_lightmap_uv && !source.has_second_uv[face.vertices[c]])
                triangle.vertices[c].texcoords[1] = options.lightmap_uv;
        }
        if (face.visual) {
            const auto key = lower(material.texture);
            auto slot = visual_slot.find(key);
            if (slot == visual_slot.end()) {
                const auto found = std::ranges::find_if(*visual_materials.value, [&](const auto& chunk) {
                    return lower(material_texture_name(chunk)) == key;
                });
                if (found == visual_materials.value->end()) {
                    result.error = "no donor visual material uses texture '" + material.texture + "'";
                    return result;
                }
                compiled.notes.push_back("visual material " + std::to_string(visual_list.size()) + " = donor " +
                                         std::to_string(found - visual_materials.value->begin()) +
                                         " (texture " + material.texture + ")");
                slot = visual_slot.emplace(key, static_cast<std::uint16_t>(visual_list.size())).first;
                visual_list.push_back(*found);
            }
            triangle.material = slot->second;
            visual.push_back(triangle);
        }
        if (face.collision) {
            const auto slot = surface_slot.find(lower(material.surface));
            if (slot == surface_slot.end()) {
                result.error = "no donor collision material is named '" + material.surface + "'";
                return result;
            }
            triangle.material = slot->second;
            triangle.pyro = material.shade;
            collision.push_back(triangle);
        }
    }
    // Pieces: donor World triangles inside a box, moved like props.
    std::map<std::uint16_t, std::uint16_t> donor_visual_slot;
    const auto donor_visual_triangles = source.pieces.empty() ? std::vector<WorldBuildTriangle>{}
                                                              : world_build_triangles(donor_visual);
    const auto donor_collision_triangles = source.pieces.empty() ? std::vector<WorldBuildTriangle>{}
                                                                 : world_build_triangles(donor_collision);
    for (const auto& piece : source.pieces) {
        const auto angle = piece.yaw_degrees * 3.14159265358979F / 180.0F;
        const auto c = std::cos(angle), s = std::sin(angle);
        const auto yaw = [&](const Vec3 v) { return Vec3{c * v.x + s * v.z, v.y, -s * v.x + c * v.z}; };
        const Vec3 anchor{(piece.inf.x + piece.sup.x) / 2.0F, piece.inf.y, (piece.inf.z + piece.sup.z) / 2.0F};
        const auto inside = [&](const WorldBuildTriangle& t) {
            return std::ranges::all_of(t.vertices, [&](const WorldBuildVertex& v) {
                const auto& p = v.position;
                return p.x >= piece.inf.x && p.x <= piece.sup.x && p.y >= piece.inf.y && p.y <= piece.sup.y &&
                       p.z >= piece.inf.z && p.z <= piece.sup.z;
            });
        };
        const auto move = [&](WorldBuildTriangle t) {
            for (auto& v : t.vertices) {
                const auto turned = yaw({v.position.x - anchor.x, v.position.y - anchor.y, v.position.z - anchor.z});
                v.position = {turned.x + piece.position.x, turned.y + piece.position.y, turned.z + piece.position.z};
                v.normal = yaw(v.normal);
            }
            return t;
        };
        std::size_t visual_count = 0, collision_count = 0;
        for (const auto& triangle : donor_visual_triangles) {
            if (!inside(triangle)) continue;
            auto moved = move(triangle);
            auto slot = donor_visual_slot.find(triangle.material);
            if (slot == donor_visual_slot.end()) {
                if (triangle.material >= visual_materials.value->size()) {
                    result.error = "donor visual triangle names a missing material";
                    return result;
                }
                slot = donor_visual_slot.emplace(triangle.material, static_cast<std::uint16_t>(visual_list.size())).first;
                visual_list.push_back((*visual_materials.value)[triangle.material]);
            }
            moved.material = slot->second;
            visual.push_back(moved);
            ++visual_count;
        }
        for (const auto& triangle : donor_collision_triangles)
            if (inside(triangle)) {
                collision.push_back(move(triangle));
                ++collision_count;
            }
        compiled.notes.push_back("piece: " + std::to_string(visual_count) + " visual and " +
                                 std::to_string(collision_count) + " collision triangles");
    }
    collision.insert(collision.end(), extra_collision.begin(), extra_collision.end());
    if (visual.empty() || collision.empty()) {
        result.error = "the source needs visual and collision faces";
        return result;
    }
    for (const auto& [name, slot] : surface_slot)
        if (std::ranges::any_of(source.materials, [&](const auto& m) { return lower(m.surface) == name; }))
            compiled.notes.push_back("collision surface " + name + " = donor material " + std::to_string(slot));
    WorldBuildOptions visual_options;
    visual_options.library_id = donor_visual.library_id;
    visual_options.format = donor_visual.format;
    visual_options.material_list = compose_material_list(visual_list, donor_visual.library_id);
    visual_options.max_sector_triangles = options.max_sector_triangles;
    visual_options.visual_plugins = true;
    auto built_visual = build_world(visual, visual_options);
    WorldBuildOptions collision_options;
    collision_options.library_id = donor_collision.library_id;
    collision_options.format = donor_collision.format;
    collision_options.material_list = donor_collision.material_list;
    collision_options.max_sector_triangles = options.max_sector_triangles;
    auto built_collision = build_world(collision, collision_options);
    if (!built_visual || !built_collision) {
        result.error = built_visual ? built_collision.error : built_visual.error;
        return result;
    }
    compiled.visual = std::move(*built_visual.value);
    compiled.collision = std::move(*built_collision.value);
    result.value = std::move(compiled);
    return result;
}

} // namespace rws
