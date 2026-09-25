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
    const auto name = *texture + 12 + 12 + u32_at(bytes, *texture + 16);
    if (name + 12 > bytes.size() || u32_at(bytes, name) != 0x02U) {
        result.error = "the material's lightmap texture has no name string";
        return result;
    }
    const auto old_size = u32_at(bytes, name + 4);
    // RenderWare strings: the text, a terminating zero, padded to four bytes.
    const auto new_size = static_cast<std::uint32_t>((lightmap.size() + 4) & ~std::size_t{3});
    std::vector<std::byte> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(name + 12));
    for (std::size_t i = 0; i < new_size; ++i)
        out.push_back(static_cast<std::byte>(i < lightmap.size() ? lightmap[i] : '\0'));
    out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(name + 12 + old_size), bytes.end());
    const auto delta = static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size);
    const auto grow = [&](const std::size_t header) {
        const auto size = static_cast<std::uint32_t>(static_cast<std::int64_t>(u32_at(out, header + 4)) + delta);
        for (int i = 0; i < 4; ++i) out[header + 4 + static_cast<std::size_t>(i)] = static_cast<std::byte>(size >> (8 * i) & 0xFFU);
    };
    const auto* extension = find_child(*chunk, 0x03U);
    const auto* effects = find_child(*extension, 0x120U);
    // Every chunk that encloses the name: the string, the texture, the
    // effects plug-in, the extension and the material.
    for (const auto header : {name, *texture, static_cast<std::size_t>(effects->offset),
                              static_cast<std::size_t>(extension->offset), static_cast<std::size_t>(chunk->offset)})
        grow(header);
    result.value = std::move(out);
    return result;
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
            const auto key = lower(material.texture) + '|' + lower(material.lightmap);
            auto slot = visual_slot.find(key);
            if (slot == visual_slot.end()) {
                const auto found = std::ranges::find_if(*visual_materials.value, [&](const auto& chunk) {
                    return lower(material_texture_name(chunk)) == lower(material.texture);
                });
                if (found == visual_materials.value->end()) {
                    result.error = "no donor visual material uses texture '" + material.texture + "'";
                    return result;
                }
                compiled.notes.push_back("visual material " + std::to_string(visual_list.size()) + " = donor " +
                                         std::to_string(found - visual_materials.value->begin()) +
                                         " (texture " + material.texture + ")");
                slot = visual_slot.emplace(key, static_cast<std::uint16_t>(visual_list.size())).first;
                if (material.lightmap.empty()) {
                    visual_list.push_back(*found);
                } else {
                    auto lit = replace_material_lightmap(*found, material.lightmap);
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

DecodeResult<BuiltMap> build_map_files(const WorldSource& source, const std::span<const std::byte> donor_map,
                                       const std::span<const std::byte> donor_collision,
                                       const WorldCompileOptions& options, const bool keep_donor_props) {
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
                                                     : std::span<const WorldBuildTriangle>{});
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
    built.visual_triangles = compiled.value->visual.triangle_count;
    built.visual_sectors = compiled.value->visual.world_sector_count;
    built.collision_triangles = compiled.value->collision.triangle_count;
    built.collision_sectors = compiled.value->collision.world_sector_count;
    result.value = std::move(built);
    return result;
}

namespace {

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
        out += "material " + material.texture + ' ' + material.surface;
        put(out, static_cast<unsigned>(material.shade));
        if (!material.lightmap.empty()) out += ' ' + material.lightmap;
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
