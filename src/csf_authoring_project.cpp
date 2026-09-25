#include "csf/authoring_project.hpp"

#include "csf/authoring.hpp"
#include "rws/world_queries.hpp"
#include "rws/world_source.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <stdexcept>

namespace csf {
namespace {

constexpr const char* project_file = "project.csfproj";
constexpr const char* local_file = "local.csfproj";

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.generic_string());
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(text.size());
    std::ranges::transform(text, bytes.begin(), [](const char c) { return static_cast<std::byte>(c); });
    return bytes;
}

std::string read_text(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Writes through a temporary file in the same directory, then renames it.
void write_atomically(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("Cannot write " + temporary.generic_string());
    }
    std::filesystem::rename(temporary, path);
}

void write_atomically(const std::filesystem::path& path, const std::string_view text) {
    write_atomically(path, std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::string hash_of(const std::span<const std::byte> bytes) { return "sha256:" + sha256(bytes); }

std::string hash_of(const std::string_view text) {
    return hash_of(std::span(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

// ---- Tokens ---------------------------------------------------------------------

std::vector<std::string> tokens(const std::string_view line) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') {
            ++i;
            continue;
        }
        std::string token;
        if (line[i] == '"') {
            for (++i; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                token.push_back(line[i]);
            }
            if (i >= line.size()) throw std::runtime_error("unterminated string");
            ++i;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') token.push_back(line[i++]);
        }
        result.push_back(std::move(token));
    }
    return result;
}

std::string field(const std::string_view text) {
    const bool plain = !text.empty() && text.front() != '#' && std::ranges::none_of(text, [](const char c) {
        return c == ' ' || c == '\t' || c == '"' || c == '\\';
    });
    if (plain) return std::string(text);
    std::string out = "\"";
    for (const auto c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out + '"';
}

std::string field(const std::string& text) { return field(std::string_view(text)); }

std::string field(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return field(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
}

std::filesystem::path path_of(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

template <typename T>
T number(const std::string& text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("invalid number '" + text + "'");
    return value;
}

std::string text_of(const float value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), end};
}

std::string text_of(const rws::Vec3& v) { return text_of(v.x) + ' ' + text_of(v.y) + ' ' + text_of(v.z); }

// Reads a height rule starting at fields[at]; it must end the line.
HeightRule height_rule(const std::vector<std::string>& fields, const std::size_t at) {
    if (at >= fields.size()) throw std::runtime_error("missing height rule");
    HeightRule rule;
    std::size_t used = 1;
    if (fields[at] == "absolute") {
        rule.mode = HeightRule::Mode::absolute;
    } else if (fields[at] == "ground") {
        rule.mode = HeightRule::Mode::ground;
        if (at + 1 >= fields.size()) throw std::runtime_error("ground needs an offset");
        rule.offset = number<float>(fields[at + 1]);
        used = 2;
    } else if (fields[at] == "on") {
        rule.mode = HeightRule::Mode::on;
        if (at + 3 >= fields.size()) throw std::runtime_error("on needs <kind> <id> <offset>");
        rule.support_kind = fields[at + 1];
        rule.support_id = fields[at + 2];
        rule.offset = number<float>(fields[at + 3]);
        static const std::set<std::string> kinds{"actor", "building", "piece", "prop"};
        if (!kinds.contains(rule.support_kind))
            throw std::runtime_error("unknown support kind '" + rule.support_kind + "'");
        used = 4;
    } else {
        throw std::runtime_error("unknown height rule '" + fields[at] + "'");
    }
    if (at + used != fields.size()) throw std::runtime_error("unexpected fields after the height rule");
    return rule;
}

std::string text_of(const HeightRule& rule) {
    switch (rule.mode) {
    case HeightRule::Mode::absolute: return "absolute";
    case HeightRule::Mode::ground: return "ground " + text_of(rule.offset);
    case HeightRule::Mode::on: return "on " + rule.support_kind + ' ' + field(rule.support_id) + ' ' + text_of(rule.offset);
    }
    return "absolute";
}

void expect(const std::vector<std::string>& fields, const std::size_t count, const char* usage) {
    if (fields.size() != count) throw std::runtime_error(std::string("expected ") + usage);
}

std::string placement_text(const ProjectPlacement& placement) {
    std::string line;
    switch (placement.kind) {
    case ProjectPlacement::Kind::building:
        line = "building " + field(placement.id) + ' ' + field(placement.asset);
        break;
    case ProjectPlacement::Kind::piece:
        line = "piece " + field(placement.id) + ' ' + text_of(placement.box_min) + ' ' + text_of(placement.box_max);
        break;
    case ProjectPlacement::Kind::prop: {
        line = "prop " + field(placement.id) + ' ';
        for (std::size_t i = 0; i < placement.donor_instances.size(); ++i)
            line += (i ? "," : "") + std::to_string(placement.donor_instances[i]);
        break;
    }
    }
    return line + ' ' + text_of(placement.position) + ' ' + text_of(placement.yaw_degrees) + ' ' +
           text_of(placement.height);
}

} // namespace

const char* height_mode_name(const HeightRule::Mode mode) noexcept {
    switch (mode) {
    case HeightRule::Mode::absolute: return "absolute";
    case HeightRule::Mode::ground: return "ground";
    case HeightRule::Mode::on: return "on";
    }
    return "absolute";
}

AuthoringProject AuthoringProject::parse(const std::string_view project_text, const std::string_view local_text) {
    AuthoringProject project;
    const auto each_line = [](const std::string_view text, const char* file, const auto& handle) {
        std::size_t number_of_line = 0;
        for (std::size_t begin = 0; begin < text.size();) {
            const auto end = std::min(text.find('\n', begin), text.size());
            const auto line = text.substr(begin, end - begin);
            begin = end + 1;
            ++number_of_line;
            try {
                const auto fields = tokens(line);
                if (fields.empty() || fields.front().starts_with('#')) continue;
                handle(fields, number_of_line);
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(file) + " line " + std::to_string(number_of_line) + ": " +
                                         error.what());
            }
        }
    };
    bool header = false;
    each_line(project_text, project_file, [&](const std::vector<std::string>& f, std::size_t) {
        const auto& record = f.front();
        if (!header) {
            if (record != "csfproj" || f.size() != 2) throw std::runtime_error("expected 'csfproj <version>'");
            if (number<unsigned>(f[1]) != format_version)
                throw std::runtime_error("unsupported project version " + f[1]);
            header = true;
        } else if (record == "name") {
            expect(f, 2, "name <string>");
            project.name = f[1];
        } else if (record == "slot") {
            expect(f, 4, "slot <mission> <scene> <archive>");
            project.slot = {f[1], path_of(f[2]), path_of(f[3])};
        } else if (record == "texts") {
            expect(f, 5, "texts <archive> <file> <first> <last>");
            project.texts = Texts{path_of(f[1]), path_of(f[2]), number<std::int32_t>(f[3]), number<std::int32_t>(f[4])};
        } else if (record == "donor-map") {
            expect(f, 3, "donor-map <visual> <collision>");
            project.donor_map = {path_of(f[1]), path_of(f[2])};
        } else if (record == "asset") {
            expect(f, 5, "asset <id> <terrain|building> <blend> <export>");
            ProjectAsset asset;
            asset.id = f[1];
            if (f[2] == "terrain") asset.kind = ProjectAsset::Kind::terrain;
            else if (f[2] == "building") asset.kind = ProjectAsset::Kind::building;
            else throw std::runtime_error("asset kind must be terrain or building");
            asset.blend = path_of(f[3]);
            asset.export_path = path_of(f[4]);
            project.assets.push_back(std::move(asset));
        } else if (record == "asset-export") {
            expect(f, 5, "asset-export <id> <exporter> <version> <hash>");
            const auto asset = std::ranges::find(project.assets, f[1], &ProjectAsset::id);
            if (asset == project.assets.end()) throw std::runtime_error("asset-export names no asset '" + f[1] + "'");
            asset->exporter = f[2];
            asset->exporter_version = number<unsigned>(f[3]);
            asset->export_hash = f[4];
        } else if (record == "building" || record == "piece" || record == "prop") {
            ProjectPlacement placement;
            if (f.size() < 2) throw std::runtime_error("missing placement id");
            placement.id = f[1];
            std::size_t at = 2;
            if (record == "building") {
                placement.kind = ProjectPlacement::Kind::building;
                if (f.size() < 3) throw std::runtime_error("building <id> <asset> <x y z> <yaw> <height>");
                placement.asset = f[at++];
            } else if (record == "piece") {
                placement.kind = ProjectPlacement::Kind::piece;
                if (f.size() < 8) throw std::runtime_error("piece <id> <box> <x y z> <yaw> <height>");
                placement.box_min = {number<float>(f[2]), number<float>(f[3]), number<float>(f[4])};
                placement.box_max = {number<float>(f[5]), number<float>(f[6]), number<float>(f[7])};
                at = 8;
            } else {
                placement.kind = ProjectPlacement::Kind::prop;
                if (f.size() < 3) throw std::runtime_error("prop <id> <donor-instances> <x y z> <yaw> <height>");
                for (std::string_view ids = f[at++]; !ids.empty();) {
                    const auto comma = ids.find(',');
                    placement.donor_instances.push_back(number<std::uint32_t>(std::string(ids.substr(0, comma))));
                    ids = comma == std::string_view::npos ? std::string_view{} : ids.substr(comma + 1);
                }
            }
            if (f.size() < at + 4) throw std::runtime_error("missing position or yaw");
            placement.position = {number<float>(f[at]), number<float>(f[at + 1]), number<float>(f[at + 2])};
            placement.yaw_degrees = number<float>(f[at + 3]);
            placement.height = height_rule(f, at + 4);
            project.placements.push_back(std::move(placement));
        } else if (record == "anchor") {
            if (f.size() < 3 || f[1] != "actor") throw std::runtime_error("anchor actor <id> <height>");
            project.anchors.push_back({number<std::int32_t>(f[2]), height_rule(f, 3)});
        } else if (record == "output") {
            expect(f, 6, "output <path> <kind> <hash> inputs <hash>");
            if (f[4] != "inputs") throw std::runtime_error("expected 'inputs' before the input hash");
            project.outputs.push_back({path_of(f[1]), f[2], f[3], f[5]});
        } else {
            throw std::runtime_error("unknown record '" + record + "'");
        }
    });
    if (!header) throw std::runtime_error(std::string(project_file) + ": empty project");
    bool local_header = false;
    each_line(local_text, local_file, [&](const std::vector<std::string>& f, std::size_t) {
        const auto& record = f.front();
        if (!local_header) {
            if (record != "csfproj-local" || f.size() != 2 || number<unsigned>(f[1]) != format_version)
                throw std::runtime_error("expected 'csfproj-local 1'");
            local_header = true;
            return;
        }
        expect(f, 2, "<setting> <path>");
        if (record == "corpus") project.local.corpus = path_of(f[1]);
        else if (record == "blender") project.local.blender = path_of(f[1]);
        else if (record == "test-install") project.local.test_install = path_of(f[1]);
        else throw std::runtime_error("unknown setting '" + record + "'");
    });
    return project;
}

AuthoringProject AuthoringProject::load(const std::filesystem::path& directory) {
    const auto local = directory / local_file;
    auto project = parse(read_text(directory / project_file),
                         std::filesystem::exists(local) ? read_text(local) : std::string{});
    project.directory = directory;
    return project;
}

std::string AuthoringProject::project_text() const {
    std::string out = "csfproj " + std::to_string(format_version) + '\n';
    out += "name " + field(name) + '\n';
    out += "slot " + field(slot.mission) + ' ' + field(slot.scene) + ' ' + field(slot.archive) + '\n';
    if (texts)
        out += "texts " + field(texts->archive) + ' ' + field(texts->file) + ' ' + std::to_string(texts->first) +
               ' ' + std::to_string(texts->last) + '\n';
    out += "donor-map " + field(donor_map.visual) + ' ' + field(donor_map.collision) + '\n';
    if (!assets.empty()) out += "\n# World geometry from Blender\n";
    for (const auto& asset : assets) {
        out += "asset " + field(asset.id) + (asset.kind == ProjectAsset::Kind::terrain ? " terrain " : " building ") +
               field(asset.blend) + ' ' + field(asset.export_path) + '\n';
        if (!asset.exporter.empty())
            out += "asset-export " + field(asset.id) + ' ' + field(asset.exporter) + ' ' +
                   std::to_string(asset.exporter_version) + ' ' + asset.export_hash + '\n';
    }
    if (!placements.empty()) out += "\n# Buildings, donor pieces and props placed by the editor\n";
    for (const auto& placement : placements) out += placement_text(placement) + '\n';
    if (!anchors.empty()) out += "\n# Height relations of mission actors\n";
    for (const auto& anchor : anchors)
        out += "anchor actor " + std::to_string(anchor.actor_id) + ' ' + text_of(anchor.height) + '\n';
    if (!outputs.empty()) out += "\n# Generated outputs and what they were made from\n";
    for (const auto& output : outputs)
        out += "output " + field(output.path) + ' ' + output.kind + ' ' + output.hash + " inputs " +
               output.inputs_hash + '\n';
    return out;
}

std::string AuthoringProject::local_text() const {
    std::string out = "csfproj-local " + std::to_string(format_version) + '\n';
    if (!local.corpus.empty()) out += "corpus " + field(local.corpus) + '\n';
    if (!local.blender.empty()) out += "blender " + field(local.blender) + '\n';
    if (!local.test_install.empty()) out += "test-install " + field(local.test_install) + '\n';
    return out;
}

void AuthoringProject::save() const {
    write_atomically(directory / project_file, project_text());
    write_atomically(directory / local_file, local_text());
}

std::vector<std::string> AuthoringProject::check() const {
    std::vector<std::string> problems;
    std::set<std::string> asset_ids, placement_ids;
    for (const auto& asset : assets)
        if (!asset_ids.insert(asset.id).second) problems.push_back("Duplicate asset ID '" + asset.id + "'");
    std::map<std::string, const ProjectPlacement*> by_id;
    for (const auto& placement : placements) {
        if (!placement_ids.insert(placement.id).second)
            problems.push_back("Duplicate placement ID '" + placement.id + "'");
        by_id[placement.id] = &placement;
        if (placement.kind == ProjectPlacement::Kind::building) {
            const auto asset = std::ranges::find(assets, placement.asset, &ProjectAsset::id);
            if (asset == assets.end() || asset->kind != ProjectAsset::Kind::building)
                problems.push_back("Building '" + placement.id + "' names no building asset '" + placement.asset + "'");
        }
    }
    const auto kind_name = [](const ProjectPlacement::Kind kind) {
        return kind == ProjectPlacement::Kind::building ? "building" : kind == ProjectPlacement::Kind::piece ? "piece" : "prop";
    };
    for (const auto& placement : placements) {
        // Follow the chain of supports; revisiting a placement is a cycle.
        std::set<std::string> seen{placement.id};
        for (const auto* current = &placement; current->height.mode == HeightRule::Mode::on;) {
            if (current->height.support_kind == "actor") break;
            const auto next = by_id.find(current->height.support_id);
            if (next == by_id.end() || kind_name(next->second->kind) != current->height.support_kind) {
                problems.push_back("'" + current->id + "' stands on an unknown " + current->height.support_kind + " '" +
                                   current->height.support_id + "'");
                break;
            }
            if (!seen.insert(next->second->id).second) {
                problems.push_back("'" + placement.id + "' is part of a cycle of supports");
                break;
            }
            current = next->second;
        }
    }
    return problems;
}

std::filesystem::path AuthoringProject::package_root() const {
    if (local.corpus.empty()) throw std::runtime_error("local.csfproj sets no corpus");
    return local.corpus / slot.mission;
}

ProjectBuildReport AuthoringProject::build_world(const bool force) {
    if (const auto problems = check(); !problems.empty()) throw std::runtime_error(problems.front());
    ProjectBuildReport report;
    const auto package = package_root();
    const auto donor_visual = read_bytes(package / donor_map.visual);
    const auto donor_collision = read_bytes(package / donor_map.collision);

    // Everything the World depends on, in one canonical text.
    std::map<std::string, rws::WorldSource> exports;
    std::string inputs = "world 1\ndonor " + hash_of(donor_visual) + ' ' + hash_of(donor_collision) + '\n';
    for (auto& asset : assets) {
        const auto bytes = read_bytes(directory / asset.export_path);
        const auto hash = hash_of(bytes);
        if (!asset.export_hash.empty() && asset.export_hash != hash)
            report.lines.push_back("asset\t" + asset.id + "\texport changed since it was recorded");
        asset.export_hash = hash;
        inputs += "asset " + asset.id + ' ' + std::to_string(static_cast<int>(asset.kind)) + ' ' + hash + '\n';
        auto parsed = rws::parse_world_source({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        if (!parsed) throw std::runtime_error(asset.export_path.generic_string() + ": " + parsed.error);
        exports.emplace(asset.id, std::move(*parsed.value));
    }
    for (const auto& placement : placements) inputs += placement_text(placement) + '\n';
    const auto inputs_hash = hash_of(inputs);

    const auto map_path = std::filesystem::path("build") / donor_map.visual;
    const auto collision_path = std::filesystem::path("build") / donor_map.collision;
    const auto sectors_path = std::filesystem::path("build") / "Maps" / "Secs" / (slot.mission + ".sec");
    const auto source_path = std::filesystem::path("build") / "world.csfworld";
    const std::array<std::pair<std::filesystem::path, const char*>, 4> wanted{
        {{map_path, "world"}, {collision_path, "world"}, {sectors_path, "sectors"}, {source_path, "source"}}};
    const auto current = [&](const std::filesystem::path& path) {
        const auto record = std::ranges::find(outputs, path, &ProjectOutput::path);
        if (record == outputs.end() || record->inputs_hash != inputs_hash) return false;
        std::error_code error;
        if (!std::filesystem::is_regular_file(directory / path, error)) return false;
        return hash_of(read_bytes(directory / path)) == record->hash;
    };
    if (!force && std::ranges::all_of(wanted, [&](const auto& output) { return current(output.first); })) {
        report.lines.push_back("world\tup to date");
        return report;
    }

    // Terrain where it was modelled, buildings at their placements, then the
    // donor pieces and props in project order.
    rws::WorldSource merged;
    for (const auto& asset : assets)
        if (asset.kind == ProjectAsset::Kind::terrain) rws::append_world_source(merged, exports.at(asset.id));
    for (const auto& placement : placements) {
        switch (placement.kind) {
        case ProjectPlacement::Kind::building:
            rws::append_world_source(merged, exports.at(placement.asset),
                                     rws::WorldSourcePlacement{placement.position, placement.yaw_degrees});
            break;
        case ProjectPlacement::Kind::piece:
            merged.pieces.push_back({placement.box_min, placement.box_max, placement.position, placement.yaw_degrees});
            break;
        case ProjectPlacement::Kind::prop:
            merged.props.push_back({placement.donor_instances, placement.position, placement.yaw_degrees});
            break;
        }
    }
    const auto built = rws::build_map_files(merged, donor_visual, donor_collision);
    if (!built) throw std::runtime_error(built.error);
    const auto sectors = rws::build_sector_map(merged);
    const auto source_text = rws::write_world_source(merged);
    const auto store = [&](const std::filesystem::path& path, const char* kind, const std::span<const std::byte> bytes) {
        write_atomically(directory / path, bytes);
        const auto hash = hash_of(bytes);
        const auto record = std::ranges::find(outputs, path, &ProjectOutput::path);
        if (record == outputs.end()) outputs.push_back({path, kind, hash, inputs_hash});
        else *record = {path, kind, hash, inputs_hash};
    };
    store(map_path, "world", built.value->map);
    store(collision_path, "world", built.value->collision);
    store(sectors_path, "sectors", sectors.bytes);
    store(source_path, "source", std::span(reinterpret_cast<const std::byte*>(source_text.data()), source_text.size()));
    report.rebuilt = true;
    report.lines.push_back("visual\t" + map_path.generic_string() + '\t' + std::to_string(built.value->visual_triangles) +
                           " triangles\t" + std::to_string(built.value->visual_sectors) + " sectors");
    report.lines.push_back("collision\t" + collision_path.generic_string() + '\t' +
                           std::to_string(built.value->collision_triangles) + " triangles\t" +
                           std::to_string(built.value->collision_sectors) + " sectors");
    report.lines.push_back("sectors\t" + sectors_path.generic_string() + '\t' + std::to_string(sectors.vertex_count) +
                           " vertices\t" + std::to_string(sectors.sector_count) + " sectors");
    for (const auto& note : built.value->notes) report.lines.push_back("note\t" + note);
    return report;
}

} // namespace csf
