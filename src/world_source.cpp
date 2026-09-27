#include "rws/world_source.hpp"

#include "rws/document.hpp"
#include "rws/world_recovery.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace rws {
namespace {

std::string lower(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Distance from `p` to the triangle abc (closest point, Ericson 5.1.5).
float distance_to_triangle(const Vec3 p, const Vec3 a, const Vec3 b, const Vec3 c) {
    const auto sub = [](const Vec3 u, const Vec3 v) { return Vec3{u.x - v.x, u.y - v.y, u.z - v.z}; };
    const auto dot = [](const Vec3 u, const Vec3 v) { return u.x * v.x + u.y * v.y + u.z * v.z; };
    const auto at = [](const Vec3 o, const Vec3 u, const float t) { return Vec3{o.x + u.x * t, o.y + u.y * t, o.z + u.z * t}; };
    const auto ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    Vec3 q;
    const auto d1 = dot(ab, ap), d2 = dot(ac, ap);
    const auto bp = sub(p, b);
    const auto d3 = dot(ab, bp), d4 = dot(ac, bp);
    const auto cp = sub(p, c);
    const auto d5 = dot(ab, cp), d6 = dot(ac, cp);
    const auto vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
    if (d1 <= 0 && d2 <= 0) q = a;
    else if (d3 >= 0 && d4 <= d3) q = b;
    else if (vc <= 0 && d1 >= 0 && d3 <= 0) q = at(a, ab, d1 / (d1 - d3));
    else if (d6 >= 0 && d5 <= d6) q = c;
    else if (vb <= 0 && d2 >= 0 && d6 <= 0) q = at(a, ac, d2 / (d2 - d6));
    else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) q = at(b, sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
    else {
        const auto denominator = va + vb + vc;
        if (denominator == 0.0F) q = a;  // degenerate
        else q = at(at(a, ab, vb / denominator), ac, vc / denominator);
    }
    const auto d = sub(p, q);
    return std::sqrt(dot(d, d));
}

std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;
    while (!line.empty()) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos) break;
        line.remove_prefix(start);
        if (line.front() == '"') {
            // A quoted name may hold spaces (shipped lightmaps do).
            const auto close = line.find('"', 1);
            result.push_back(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
            if (close == std::string_view::npos) break;
            line.remove_prefix(close + 1);
            continue;
        }
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

namespace {

std::uint32_t u32_at(const std::span<const std::byte> bytes, const std::size_t at) {
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) value = value << 8U | std::to_integer<std::uint32_t>(bytes[at + static_cast<std::size_t>(i)]);
    return value;
}

// The embedded Texture (0x06) chunk inside a Material Effects payload, found by
// its header: type 6 with a Struct (0x01) first child of the same stamp.
std::optional<std::size_t> lightmap_texture_at(const std::span<const std::byte> bytes, const Chunk& material) {
    const auto* extension = find_child(material, 0x03U);
    const auto* effects = extension ? find_child(*extension, 0x120U) : nullptr;
    if (!effects) return std::nullopt;
    const auto begin = effects->payload_offset, end = effects->payload_offset + effects->available_size;
    for (auto at = begin; at + 36 <= end; at += 4)
        if (u32_at(bytes, at) == 0x06U && u32_at(bytes, at + 8) == effects->library_id &&
            u32_at(bytes, at + 12) == 0x01U && u32_at(bytes, at + 20) == effects->library_id)
            return at;
    return std::nullopt;
}

} // namespace

std::string material_lightmap_name(const std::span<const std::byte> material) {
    Document holder;
    const auto chunk = material_chunk(material, holder);
    if (!chunk) return {};
    const auto bytes = holder.bytes();
    const auto texture = lightmap_texture_at(bytes, *chunk);
    if (!texture) return {};
    const auto name = *texture + 12 + 12 + u32_at(bytes, *texture + 16);  // after the Struct
    if (name + 12 > bytes.size() || u32_at(bytes, name) != 0x02U) return {};
    std::string text;
    for (auto at = name + 12; at < name + 12 + u32_at(bytes, name + 4) && at < bytes.size(); ++at) {
        const auto c = static_cast<char>(bytes[at]);
        if (c == '\0') break;
        text.push_back(c);
    }
    return text;
}

namespace {

// Renames the Texture (0x06) chunk at `texture`: its name string (after its
// Struct) is rewritten and every chunk in `enclosing` (headers that contain
// the string, the texture included) grows or shrinks with it.
DecodeResult<std::vector<std::byte>> rename_texture(const std::span<const std::byte> bytes, const std::size_t texture,
                                                    std::vector<std::size_t> enclosing, const std::string_view name) {
    DecodeResult<std::vector<std::byte>> result;
    const auto string = texture + 12 + 12 + u32_at(bytes, texture + 16);  // after the Struct
    if (string + 12 > bytes.size() || u32_at(bytes, string) != 0x02U) {
        result.error = "the texture has no name string";
        return result;
    }
    const auto old_size = u32_at(bytes, string + 4);
    // RenderWare strings: the text, a terminating zero, padded to four bytes.
    const auto new_size = static_cast<std::uint32_t>((name.size() + 4) & ~std::size_t{3});
    std::vector<std::byte> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(string + 12));
    for (std::size_t i = 0; i < new_size; ++i)
        out.push_back(static_cast<std::byte>(i < name.size() ? name[i] : '\0'));
    out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(string + 12 + old_size), bytes.end());
    const auto delta = static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size);
    enclosing.push_back(string);
    for (const auto header : enclosing) {
        const auto size = static_cast<std::uint32_t>(static_cast<std::int64_t>(u32_at(out, header + 4)) + delta);
        for (int i = 0; i < 4; ++i) out[header + 4 + static_cast<std::size_t>(i)] = static_cast<std::byte>(size >> (8 * i) & 0xFFU);
    }
    result.value = std::move(out);
    return result;
}

} // namespace

DecodeResult<std::vector<std::byte>> replace_material_lightmap(const std::span<const std::byte> material,
                                                               const std::string_view lightmap) {
    DecodeResult<std::vector<std::byte>> result;
    Document holder;
    const auto chunk = material_chunk(material, holder);
    if (!chunk) {
        result.error = "not a Material chunk";
        return result;
    }
    const auto bytes = holder.bytes();
    const auto texture = lightmap_texture_at(bytes, *chunk);
    if (!texture) {
        result.error = "the material has no lightmap (Material Effects dual pass)";
        return result;
    }
    const auto* extension = find_child(*chunk, 0x03U);
    const auto* effects = find_child(*extension, 0x120U);
    // Every chunk that encloses the name: the texture, the effects plug-in,
    // the extension and the material.
    auto renamed = rename_texture(bytes, *texture,
                                  {*texture, static_cast<std::size_t>(effects->offset),
                                   static_cast<std::size_t>(extension->offset), static_cast<std::size_t>(chunk->offset)},
                                  lightmap);
    if (!renamed) result.error = "the material's lightmap texture has no name string";
    else result.value = std::move(renamed.value);
    return result;
}

DecodeResult<std::vector<std::byte>> replace_material_texture(const std::span<const std::byte> material,
                                                              const std::string_view texture) {
    DecodeResult<std::vector<std::byte>> result;
    Document holder;
    const auto chunk = material_chunk(material, holder);
    const auto* found = chunk ? find_child(*chunk, 0x06U) : nullptr;
    if (!found) {
        result.error = chunk ? "the material has no texture" : "not a Material chunk";
        return result;
    }
    return rename_texture(holder.bytes(), static_cast<std::size_t>(found->offset),
                          {static_cast<std::size_t>(found->offset), static_cast<std::size_t>(chunk->offset)}, texture);
}

std::string material_color_name(const std::span<const std::byte> material) {
    Document holder;
    const auto chunk = material_chunk(material, holder);
    if (!chunk) return {};
    const auto decoded = decode_material(*chunk, holder.bytes());
    if (!decoded) return {};
    std::string out;
    for (const auto component : decoded.value->color) {
        constexpr char digits[] = "0123456789ABCDEF";
        out += digits[component >> 4U];
        out += digits[component & 15U];
    }
    return out;
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
            if (parts.size() < 3 || parts.size() > 5) return fail("material <texture> <surface> [<shade> [<lightmap>]]");
            WorldSourceMaterial material;
            material.texture = std::string(parts[1]);
            material.surface = std::string(parts[2]);
            if (parts.size() >= 4) {
                unsigned shade{};
                if (!number(parts[3], shade) || shade > 255U) return fail("shade must be 0-255");
                material.shade = static_cast<std::uint8_t>(shade);
            }
            if (parts.size() == 5) material.lightmap = std::string(parts[4]);
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
            // Numbers, then donor= and lightmaps= in any order.
            auto numbers = parts.size();
            while (numbers > 1 && parts[numbers - 1].find('=') != std::string_view::npos) --numbers;
            if (numbers != 10 && numbers != 11)
                return fail("piece <x0> <y0> <z0> <x1> <y1> <z1> <x> <y> <z> [<yaw>] [donor=<key>] [lightmaps=<name>,...]");
            std::array<float, 10> values{};
            for (std::size_t i = 1; i < numbers; ++i)
                if (!number(parts[i], values[i - 1])) return fail("invalid piece numbers");
            WorldPiece piece;
            piece.inf = {std::min(values[0], values[3]), std::min(values[1], values[4]), std::min(values[2], values[5])};
            piece.sup = {std::max(values[0], values[3]), std::max(values[1], values[4]), std::max(values[2], values[5])};
            piece.position = {values[6], values[7], values[8]};
            piece.yaw_degrees = values[9];
            for (auto i = numbers; i < parts.size(); ++i) {
                const auto equals = parts[i].find('=');
                const auto key = parts[i].substr(0, equals), value = parts[i].substr(equals + 1);
                if (key == "donor" && !value.empty()) {
                    piece.donor = std::string(value);
                } else if (key == "lightmaps" && !value.empty()) {
                    for (auto names = value; !names.empty();) {
                        const auto comma = names.find(',');
                        if (const auto name = names.substr(0, comma); !name.empty()) piece.lightmaps.emplace_back(name);
                        names = comma == std::string_view::npos ? std::string_view{} : names.substr(comma + 1);
                    }
                } else {
                    return fail("unknown piece option '" + std::string(parts[i]) + "'");
                }
            }
            source.pieces.push_back(std::move(piece));
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
                                                  const std::span<const WorldBuildTriangle> extra_collision,
                                                  const std::span<const DonorWorlds> donors) {
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
            const auto key = lower(material.texture) + '|' + lower(material.lightmap);
            auto slot = visual_slot.find(key);
            if (slot == visual_slot.end()) {
                // A texture of the World's own copies its template's material.
                const auto own = options.new_textures.find(lower(material.texture));
                const auto donor_texture = own == options.new_textures.end() ? material.texture : own->second;
                // `-` is an untextured donor material, `-#RRGGBBAA` the one of that colour.
                const auto untextured = donor_texture.starts_with('-');
                const auto uses_texture = [&](const auto& chunk) {
                    if (untextured)
                        return material_texture_name(chunk).empty() &&
                               (donor_texture.size() < 2 || lower("-#" + material_color_name(chunk)) == lower(donor_texture));
                    return lower(material_texture_name(chunk)) == lower(donor_texture);
                };
                // Prefer the donor material already lit by that lightmap (a
                // decompiled map names its own), else the first with the texture.
                auto found = material.lightmap.empty()
                                 ? visual_materials.value->end()
                                 : std::ranges::find_if(*visual_materials.value, [&](const auto& chunk) {
                                       return uses_texture(chunk) &&
                                              lower(material_lightmap_name(chunk)) == lower(material.lightmap);
                                   });
                if (found == visual_materials.value->end())
                    found = std::ranges::find_if(*visual_materials.value, uses_texture);
                if (found == visual_materials.value->end()) {
                    result.error = "no donor visual material uses texture '" + donor_texture + "'";
                    return result;
                }
                compiled.notes.push_back("visual material " + std::to_string(visual_list.size()) + " = donor " +
                                         std::to_string(found - visual_materials.value->begin()) +
                                         " (texture " + material.texture + ")");
                slot = visual_slot.emplace(key, static_cast<std::uint16_t>(visual_list.size())).first;
                auto chosen = *found;
                if (own != options.new_textures.end()) {
                    auto renamed = replace_material_texture(chosen, material.texture);
                    if (!renamed) {
                        result.error = "texture '" + material.texture + "': " + renamed.error;
                        return result;
                    }
                    chosen = std::move(*renamed.value);
                    compiled.notes.back() += ", renamed";
                }
                if (material.lightmap.empty()) {
                    visual_list.push_back(std::move(chosen));
                } else {
                    auto lit = replace_material_lightmap(chosen, material.lightmap);
                    if (!lit) {
                        result.error = "texture '" + material.texture + "': " + lit.error;
                        return result;
                    }
                    compiled.notes.back() += ", lightmap " + material.lightmap;
                    visual_list.push_back(std::move(*lit.value));
                }
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
    // Pieces: donor World triangles inside a box, moved like props. Each
    // donor's triangles and material slots are set up on first use.
    struct PieceDonor {
        const DonorWorlds* other{};  // null: the map's own donor
        std::vector<std::vector<std::byte>> visual_materials, collision_materials;
        std::vector<std::string> lightmap_names;  // per visual material, lower case
        std::vector<WorldBuildTriangle> visual, collision;
        std::map<std::uint16_t, std::uint16_t> visual_slot, collision_slot;
    };
    std::map<std::string, PieceDonor> piece_donors;
    auto collision_list = *collision_materials.value;
    bool collision_list_grew = false;
    std::set<DonorTexture> donor_textures;
    const auto piece_donor = [&](const std::string& key) -> PieceDonor* {
        if (const auto found = piece_donors.find(key); found != piece_donors.end()) return &found->second;
        PieceDonor donor;
        const WorldModel* visual_world = &donor_visual;
        const WorldModel* collision_world = &donor_collision;
        if (!key.empty()) {
            const auto other = std::ranges::find(donors, key, &DonorWorlds::key);
            if (other == donors.end()) {
                result.error = "a piece names an unknown donor '" + key + "'";
                return nullptr;
            }
            donor.other = &*other;
            visual_world = &other->visual;
            collision_world = &other->collision;
            auto visual_list = split_material_list(visual_world->material_list, visual_world->library_id);
            auto collision_list_of = split_material_list(collision_world->material_list, collision_world->library_id);
            if (!visual_list || !collision_list_of) {
                result.error = "donor " + key + " Material List: " + (visual_list ? collision_list_of.error : visual_list.error);
                return nullptr;
            }
            donor.visual_materials = std::move(*visual_list.value);
            donor.collision_materials = std::move(*collision_list_of.value);
        } else {
            donor.visual_materials = *visual_materials.value;
            donor.collision_materials = *collision_materials.value;
        }
        for (const auto& material : donor.visual_materials) donor.lightmap_names.push_back(lower(material_lightmap_name(material)));
        donor.visual = world_build_triangles(*visual_world);
        donor.collision = world_build_triangles(*collision_world);
        return &piece_donors.emplace(key, std::move(donor)).first->second;
    };
    // A copied material of another donor, its textures renamed as the donor says.
    const auto foreign_material = [&](const PieceDonor& donor, const std::uint16_t index) -> std::optional<std::vector<std::byte>> {
        auto material = donor.visual_materials[index];
        const auto rename = [&](const std::string& name) {
            const auto found = donor.other->renamed.find(lower(name));
            return found == donor.other->renamed.end() ? name : found->second;
        };
        if (const auto texture = material_texture_name(material); !texture.empty()) {
            const auto name = rename(texture);
            if (name != texture) {
                auto renamed = replace_material_texture(material, name);
                if (!renamed) {
                    result.error = "donor " + donor.other->key + " texture " + texture + ": " + renamed.error;
                    return std::nullopt;
                }
                material = std::move(*renamed.value);
            }
            donor_textures.insert({donor.other->key, texture, name});
        }
        if (const auto lightmap = material_lightmap_name(material); !lightmap.empty()) {
            const auto name = rename(lightmap);
            if (name != lightmap) {
                auto renamed = replace_material_lightmap(material, name);
                if (!renamed) {
                    result.error = "donor " + donor.other->key + " lightmap " + lightmap + ": " + renamed.error;
                    return std::nullopt;
                }
                material = std::move(*renamed.value);
            }
            donor_textures.insert({donor.other->key, lightmap, name});
        }
        return material;
    };
    for (const auto& piece : source.pieces) {
        auto* donor = piece_donor(piece.donor);
        if (!donor) return result;
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
        std::set<std::string> groups;
        for (const auto& name : piece.lightmaps) groups.insert(lower(name) + "_lm");
        const auto kept = [&](const WorldBuildTriangle& t) {
            return groups.empty() || (t.material < donor->lightmap_names.size() && groups.contains(donor->lightmap_names[t.material]));
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
        std::vector<const WorldBuildTriangle*> kept_visual;
        for (const auto& triangle : donor->visual) {
            if (!inside(triangle) || !kept(triangle)) continue;
            kept_visual.push_back(&triangle);
            auto moved = move(triangle);
            auto slot = donor->visual_slot.find(triangle.material);
            if (slot == donor->visual_slot.end()) {
                if (triangle.material >= donor->visual_materials.size()) {
                    result.error = "donor visual triangle names a missing material";
                    return result;
                }
                if (donor->other) {
                    auto material = foreign_material(*donor, triangle.material);
                    if (!material) return result;
                    visual_list.push_back(std::move(*material));
                } else {
                    visual_list.push_back(donor->visual_materials[triangle.material]);
                }
                slot = donor->visual_slot.emplace(triangle.material, static_cast<std::uint16_t>(visual_list.size() - 1)).first;
            }
            moved.material = slot->second;
            visual.push_back(moved);
            ++visual_count;
        }
        // With lightmap groups, the collision triangles lying on the kept
        // visual ones (their centre within 30 cm), found through a 2 m grid.
        constexpr float near = 30.0F, cell = 200.0F;
        std::map<std::array<int, 3>, std::vector<const WorldBuildTriangle*>> grid;
        const auto cell_of = [&](const float x, const float y, const float z) {
            return std::array{static_cast<int>(std::floor(x / cell)), static_cast<int>(std::floor(y / cell)),
                              static_cast<int>(std::floor(z / cell))};
        };
        if (!groups.empty())
            for (const auto* triangle : kept_visual) {
                Vec3 lo = triangle->vertices[0].position, hi = lo;
                for (const auto& v : triangle->vertices) {
                    lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
                    hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
                }
                const auto a = cell_of(lo.x - near, lo.y - near, lo.z - near), b = cell_of(hi.x + near, hi.y + near, hi.z + near);
                for (int x = a[0]; x <= b[0]; ++x)
                    for (int y = a[1]; y <= b[1]; ++y)
                        for (int z = a[2]; z <= b[2]; ++z) grid[{x, y, z}].push_back(triangle);
            }
        const auto on_kept = [&](const WorldBuildTriangle& t) {
            if (groups.empty()) return true;
            Vec3 centre{};
            for (const auto& v : t.vertices)
                centre = {centre.x + v.position.x / 3.0F, centre.y + v.position.y / 3.0F, centre.z + v.position.z / 3.0F};
            const auto found = grid.find(cell_of(centre.x, centre.y, centre.z));
            if (found == grid.end()) return false;
            return std::ranges::any_of(found->second, [&](const WorldBuildTriangle* visual_triangle) {
                return distance_to_triangle(centre, visual_triangle->vertices[0].position, visual_triangle->vertices[1].position,
                                            visual_triangle->vertices[2].position) <= near;
            });
        };
        for (const auto& triangle : donor->collision) {
            if (!inside(triangle) || !on_kept(triangle)) continue;
            auto moved = move(triangle);
            if (donor->other) {
                // The map's collision material of the same surface, else the donor's own, appended.
                auto slot = donor->collision_slot.find(triangle.material);
                if (slot == donor->collision_slot.end()) {
                    if (triangle.material >= donor->collision_materials.size()) {
                        result.error = "donor collision triangle names a missing material";
                        return result;
                    }
                    const auto& material = donor->collision_materials[triangle.material];
                    const auto surface = lower(material_surface_name(material));
                    auto target = surface_slot.find(surface);
                    if (target == surface_slot.end()) {
                        collision_list.push_back(material);
                        collision_list_grew = true;
                        target = surface_slot.emplace(surface, static_cast<std::uint16_t>(collision_list.size() - 1)).first;
                        compiled.notes.push_back("collision surface " + surface + " = donor " + donor->other->key +
                                                 " material, appended as " + std::to_string(target->second));
                    }
                    slot = donor->collision_slot.emplace(triangle.material, target->second).first;
                }
                moved.material = slot->second;
            }
            collision.push_back(moved);
            ++collision_count;
        }
        compiled.notes.push_back("piece" + (piece.donor.empty() ? std::string{} : " from " + piece.donor) + ": " +
                                 std::to_string(visual_count) + " visual and " + std::to_string(collision_count) +
                                 " collision triangles");
    }
    compiled.textures.assign(donor_textures.begin(), donor_textures.end());
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
    collision_options.material_list = collision_list_grew ? compose_material_list(collision_list, donor_collision.library_id)
                                                          : donor_collision.material_list;
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

DecodeResult<BuiltMap> build_map_files(const WorldSource& source, const std::span<const std::byte> donor_map,
                                       const std::span<const std::byte> donor_collision,
                                       const WorldCompileOptions& options, const bool keep_donor_props,
                                       const std::span<const WorldDonor> donors) {
    DecodeResult<BuiltMap> result;
    const auto load_world = [&](const std::span<const std::byte> bytes, const char* what,
                                std::uint64_t& begin) -> std::optional<WorldModel> {
        const auto offset = find_map_world(bytes);
        if (!offset) {
            result.error = std::string("No World in the donor ") + what;
            return std::nullopt;
        }
        auto parsed = parse_world_model(bytes, *offset);
        if (!parsed) {
            result.error = std::string("Donor ") + what + ": " + parsed.error;
            return std::nullopt;
        }
        begin = *offset;
        return std::move(*parsed.value);
    };
    std::uint64_t map_begin{}, collision_begin{};
    const auto donor_visual = load_world(donor_map, "map", map_begin);
    if (!donor_visual) return result;
    const auto donor_col = load_world(donor_collision, "collision map", collision_begin);
    if (!donor_col) return result;
    std::vector<DonorWorlds> other_donors;
    for (const auto& donor : donors) {
        std::uint64_t ignored{};
        const auto name = "map of donor " + donor.key;
        auto visual = load_world(donor.map, name.c_str(), ignored);
        if (!visual) return result;
        const auto collision_name = "collision map of donor " + donor.key;
        auto collision = load_world(donor.collision, collision_name.c_str(), ignored);
        if (!collision) return result;
        other_donors.push_back({donor.key, std::move(*visual), std::move(*collision), donor.renamed});
    }
    BuiltMap built;
    // Props: donor Clumps and instance records, collision cut from the donor.
    std::optional<AssembledProps> props;
    if (!source.props.empty()) {
        if (keep_donor_props) {
            result.error = "Keeping the donor's props cannot be combined with placed props";
            return result;
        }
        const auto donor_document = Document::from_bytes({donor_map.begin(), donor_map.end()});
        auto assembled = assemble_props(donor_document, *donor_col, source.props);
        if (!assembled) {
            result.error = assembled.error;
            return result;
        }
        props = std::move(*assembled.value);
        built.notes = props->notes;
    }
    const auto compiled = compile_world_source(source, *donor_visual, *donor_col, options,
                                               props ? std::span<const WorldBuildTriangle>(props->collision)
                                                     : std::span<const WorldBuildTriangle>{},
                                               other_donors);
    if (!compiled) {
        result.error = compiled.error;
        return result;
    }
    for (const auto* world : {&compiled.value->visual, &compiled.value->collision})
        if (const auto problems = check_world_model(*world); !problems.empty()) {
            result.error = "Built World is invalid: " + problems.front();
            return result;
        }
    if (keep_donor_props)
        built.map.assign(donor_map.begin(), donor_map.begin() + static_cast<std::ptrdiff_t>(map_begin));
    if (props) built.map = props->prefix;
    const auto visual_bytes = write_world_model(compiled.value->visual);
    built.map.insert(built.map.end(), visual_bytes.begin(), visual_bytes.end());
    built.collision = write_world_model(compiled.value->collision);
    for (const auto* bytes : {&built.map, &built.collision}) {
        const auto document = Document::from_bytes(*bytes);
        const auto recovered = recover_worlds(document.chunks(), document.bytes());
        if (recovered.size() != 1U || recovered[0].status != WorldRecoveryStatus::complete ||
            recovered[0].topology_status != WorldTopologyStatus::complete) {
            result.error = "Built World does not recover as complete";
            return result;
        }
    }
    built.notes.insert(built.notes.end(), compiled.value->notes.begin(), compiled.value->notes.end());
    built.textures = compiled.value->textures;
    built.visual_triangles = compiled.value->visual.triangle_count;
    built.visual_sectors = compiled.value->visual.world_sector_count;
    built.collision_triangles = compiled.value->collision.triangle_count;
    built.collision_sectors = compiled.value->collision.world_sector_count;
    result.value = std::move(built);
    return result;
}

namespace {

std::string quoted(const std::string& name) {
    return std::ranges::any_of(name, [](const unsigned char c) { return std::isspace(c); }) ? '"' + name + '"' : name;
}

template <typename T>
void put(std::string& out, const T value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.push_back(' ');
    out.append(buffer.data(), end);
}

} // namespace

std::string write_world_source(const WorldSource& source) {
    std::string out = "csfworld 1\n";
    for (const auto& material : source.materials) {
        out += "material " + quoted(material.texture) + ' ' + quoted(material.surface);
        put(out, static_cast<unsigned>(material.shade));
        if (!material.lightmap.empty()) out += ' ' + quoted(material.lightmap);
        out += '\n';
    }
    for (std::size_t i = 0; i < source.vertices.size(); ++i) {
        const auto& vertex = source.vertices[i];
        out += 'v';
        if (i < source.exact_positions.size())
            for (const auto c : source.exact_positions[i]) put(out, c);
        else
            for (const auto c : {vertex.position.x, vertex.position.y, vertex.position.z}) put(out, c);
        for (const auto c : {vertex.normal.x, vertex.normal.y, vertex.normal.z}) put(out, c);
        put(out, vertex.texcoords[0][0]);
        put(out, vertex.texcoords[0][1]);
        if (i < source.has_second_uv.size() && source.has_second_uv[i]) {
            put(out, vertex.texcoords[1][0]);
            put(out, vertex.texcoords[1][1]);
        }
        out += '\n';
    }
    for (const auto& face : source.faces) {
        out += 'f';
        for (const auto v : face.vertices) put(out, v);
        put(out, face.material);
        out += face.visual && face.collision ? " both\n" : face.visual ? " visual\n" : " collision\n";
    }
    for (const auto& piece : source.pieces) {
        out += "piece";
        for (const auto c : {piece.inf.x, piece.inf.y, piece.inf.z, piece.sup.x, piece.sup.y, piece.sup.z,
                             piece.position.x, piece.position.y, piece.position.z, piece.yaw_degrees})
            put(out, c);
        if (!piece.donor.empty()) out += " donor=" + piece.donor;
        for (std::size_t i = 0; i < piece.lightmaps.size(); ++i) out += (i ? "," : " lightmaps=") + piece.lightmaps[i];
        out += '\n';
    }
    for (const auto& prop : source.props) {
        out += "prop ";
        for (std::size_t i = 0; i < prop.donor_instance_ids.size(); ++i)
            out += (i ? "," : "") + std::to_string(prop.donor_instance_ids[i]);
        for (const auto c : {prop.position.x, prop.position.y, prop.position.z, prop.yaw_degrees}) put(out, c);
        out += '\n';
    }
    return out;
}

DecodeResult<WorldSource> world_source_from_map(const WorldModel& visual, const WorldModel& collision) {
    DecodeResult<WorldSource> result;
    const auto visual_materials = split_material_list(visual.material_list, visual.library_id);
    const auto collision_materials = split_material_list(collision.material_list, collision.library_id);
    if (!visual_materials || !collision_materials) {
        result.error = "Material List: " + (visual_materials ? collision_materials.error : visual_materials.error);
        return result;
    }
    // A .csfworld name is one token, quoted when it holds spaces.
    const auto token = [](const std::string& name) {
        return !name.empty() && std::ranges::none_of(name, [](const unsigned char c) {
                   return c == '"' || c == '\n' || c == '\t' || c == '\r';
               });
    };
    WorldSource source;
    std::map<std::tuple<std::string, std::string, std::uint8_t, std::string>, std::uint32_t> material_index;
    const auto material_of = [&](WorldSourceMaterial material) {
        const auto [it, added] = material_index.try_emplace(
            {material.texture, material.surface, material.shade, material.lightmap},
            static_cast<std::uint32_t>(source.materials.size()));
        if (added) source.materials.push_back(std::move(material));
        return it->second;
    };
    const auto add = [&](const WorldModel& world, const std::vector<std::vector<std::byte>>& list, const bool is_visual) {
        const auto sets = world_texcoord_sets(world.format);
        for (const auto& triangle : world_build_triangles(world)) {
            if (triangle.material >= list.size()) {
                result.error = std::string(is_visual ? "visual" : "collision") + " triangle names a missing material";
                return false;
            }
            const auto& chunk = list[triangle.material];
            WorldSourceMaterial material;
            if (is_visual) {
                material.texture = material_texture_name(chunk);
                material.lightmap = material_lightmap_name(chunk);
                material.surface = material_surface_name(chunk);
                if (!token(material.surface)) material.surface = "Tierra";
                if (material.texture.empty()) material.texture = "-#" + material_color_name(chunk);
                for (const auto* name : {&material.texture, &material.lightmap})
                    if (!name->empty() && !token(*name)) {
                        result.error = "visual material " + std::to_string(triangle.material) + " name '" + *name +
                                       "' cannot be written";
                        return false;
                    }
            } else {
                material.surface = material_surface_name(chunk);
                if (!token(material.surface)) {
                    result.error = "collision material " + std::to_string(triangle.material) +
                                   (material.surface.empty() ? " has no surface name"
                                                             : " surface '" + material.surface + "' cannot be written");
                    return false;
                }
                material.texture = material_texture_name(chunk);
                if (!token(material.texture)) material.texture = "-";
                material.shade = triangle.pyro;
            }
            WorldSourceFace face;
            face.material = material_of(std::move(material));
            face.visual = is_visual;
            face.collision = !is_visual;
            for (std::size_t c = 0; c < 3; ++c) {
                const auto& vertex = triangle.vertices[c];
                face.vertices[c] = static_cast<std::uint32_t>(source.vertices.size());
                WorldBuildVertex copy = vertex;
                if (!is_visual) copy.normal = {};
                source.vertices.push_back(copy);
                source.exact_positions.push_back({vertex.position.x, vertex.position.y, vertex.position.z});
                source.has_second_uv.push_back(is_visual && sets >= 2U);
            }
            source.faces.push_back(face);
        }
        return true;
    };
    if (!add(visual, *visual_materials.value, true) || !add(collision, *collision_materials.value, false)) return result;
    result.value = std::move(source);
    return result;
}

void append_world_source(WorldSource& target, const WorldSource& part,
                         const std::optional<WorldSourcePlacement>& placed) {
    // Keep the per-vertex arrays aligned with the vertices.
    while (target.exact_positions.size() < target.vertices.size()) {
        const auto& p = target.vertices[target.exact_positions.size()].position;
        target.exact_positions.push_back({p.x, p.y, p.z});
    }
    target.has_second_uv.resize(target.vertices.size());
    std::vector<std::uint32_t> materials;
    for (const auto& material : part.materials) {
        const auto found = std::ranges::find_if(target.materials, [&](const WorldSourceMaterial& m) {
            return m.texture == material.texture && m.surface == material.surface && m.shade == material.shade &&
                   m.lightmap == material.lightmap;
        });
        materials.push_back(static_cast<std::uint32_t>(found - target.materials.begin()));
        if (found == target.materials.end()) target.materials.push_back(material);
    }
    const double angle = placed ? placed->yaw_degrees * 3.14159265358979323846 / 180.0 : 0.0;
    const double c = std::cos(angle), s = std::sin(angle);
    const auto first = static_cast<std::uint32_t>(target.vertices.size());
    for (std::size_t i = 0; i < part.vertices.size(); ++i) {
        auto vertex = part.vertices[i];
        std::array<double, 3> exact = i < part.exact_positions.size()
                                          ? part.exact_positions[i]
                                          : std::array<double, 3>{vertex.position.x, vertex.position.y,
                                                                  vertex.position.z};
        if (placed) {
            exact = {c * exact[0] + s * exact[2] + placed->offset.x, exact[1] + placed->offset.y,
                     -s * exact[0] + c * exact[2] + placed->offset.z};
            const auto& n = vertex.normal;
            vertex.normal = {static_cast<float>(c * n.x + s * n.z), n.y, static_cast<float>(-s * n.x + c * n.z)};
            vertex.position = {static_cast<float>(exact[0]), static_cast<float>(exact[1]),
                               static_cast<float>(exact[2])};
        }
        target.vertices.push_back(vertex);
        target.exact_positions.push_back(exact);
        target.has_second_uv.push_back(i < part.has_second_uv.size() && part.has_second_uv[i]);
    }
    for (auto face : part.faces) {
        for (auto& v : face.vertices) v += first;
        face.material = materials.at(face.material);
        target.faces.push_back(face);
    }
    target.props.insert(target.props.end(), part.props.begin(), part.props.end());
    target.pieces.insert(target.pieces.end(), part.pieces.begin(), part.pieces.end());
}

} // namespace rws
