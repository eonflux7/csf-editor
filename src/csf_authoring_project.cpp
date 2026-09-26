#include "csf/authoring_project.hpp"

#include "csf/authoring.hpp"
#include "csf/mission_scene.hpp"
#include "csf/mod_project.hpp"
#include "rws/texture_image.hpp"
#include "rws/world_queries.hpp"
#include "rws/world_source.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cmath>
#include <functional>
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

// Rewrites the file records of a csf-mod workspace that package `outputs`
// with their new hashes, so validation and export see them as current.
void refresh_workspace(const std::filesystem::path& workspace, const std::filesystem::path& project,
                       const std::vector<ProjectOutput>& outputs) {
    if (!std::filesystem::is_regular_file(workspace / ".csf-mod-state")) return;
    auto packaged = ModProject::load(workspace);
    bool changed = false;
    for (auto& file : packaged.files)
        for (const auto& output : outputs) {
            std::error_code error;
            if (std::filesystem::equivalent(file.authored_path, project / output.path, error) &&
                file.output_sha256 != output.hash.substr(7)) {
                file.output_sha256 = output.hash.substr(7);  // without "sha256:"
                changed = true;
            }
        }
    if (changed) packaged.save();
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
        } else if (record == "lightmap") {
            expect(f, 3, "lightmap <name> <source.png>");
            project.lightmaps.push_back({f[1], path_of(f[2])});
        } else if (record == "text") {
            expect(f, 3, "text <id> <string>");
            project.strings.push_back({f[1], f[2]});
        } else if (record == "playtest") {
            expect(f, 4, "playtest <build-id> <worked|failed> <note>");
            if (f[2] != "worked" && f[2] != "failed") throw std::runtime_error("playtest result must be worked or failed");
            project.playtests.push_back({f[1], f[2] == "worked", f[3]});
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
        if (record == "original") {
            expect(f, 3, "original <archive> <path>");
            project.local.originals[path_of(f[1])] = path_of(f[2]);
            return;
        }
        if (record == "deployment") {
            expect(f, 4, "deployment <build-id> <archive> <state file>");
            project.local.deployments.push_back({f[1], path_of(f[2]), path_of(f[3])});
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
    if (!lightmaps.empty()) out += "\n# Baked lightmaps\n";
    for (const auto& lightmap : lightmaps) out += "lightmap " + field(lightmap.name) + ' ' + field(lightmap.source) + '\n';
    if (!strings.empty()) out += "\n# Mission text (GlobalEK)\n";
    for (const auto& string : strings) out += "text " + field(string.id) + ' ' + field(string.text) + '\n';
    if (!playtests.empty()) out += "\n# Playtests of built archives\n";
    for (const auto& playtest : playtests)
        out += "playtest " + field(playtest.build) + (playtest.worked ? " worked " : " failed ") + field(playtest.note) + '\n';
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
    for (const auto& [archive, path] : local.originals) out += "original " + field(archive) + ' ' + field(path) + '\n';
    for (const auto& deployment : local.deployments)
        out += "deployment " + field(deployment.build) + ' ' + field(deployment.archive) + ' ' + field(deployment.manifest) +
               '\n';
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
    std::set<std::string> lightmap_names;
    for (const auto& lightmap : lightmaps) {
        if (!lightmap_names.insert(lightmap.name).second) problems.push_back("Duplicate lightmap " + lightmap.name);
        if (lightmap.name.empty() || lightmap.name.find_first_of(" \\/.") != std::string::npos)
            problems.push_back("Lightmap name '" + lightmap.name + "' must be a plain texture name");
    }
    std::set<std::string> text_ids;
    for (const auto& string : strings) {
        const bool digits = !string.id.empty() &&
                            std::ranges::all_of(string.id, [](const char c) { return c >= '0' && c <= '9'; });
        if (!digits) problems.push_back("Text ID '" + string.id + "' is not a number");
        else if (!texts) problems.push_back("Text " + string.id + " needs a texts record (file and ID range)");
        else if (const auto value = std::stoi(string.id); value < texts->first || value > texts->last)
            problems.push_back("Text " + string.id + " is outside the project's range " + std::to_string(texts->first) +
                               ".." + std::to_string(texts->last));
        if (!text_ids.insert(string.id).second) problems.push_back("Duplicate text ID " + string.id);
    }
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
    // Nothing of its own yet: the slot's shipped map stays.
    if (assets.empty() && placements.empty()) {
        report.lines.push_back("world\tno assets or placements: the slot's own map is used");
        return report;
    }
    const auto package = package_root();
    const auto donor_visual = read_bytes(package / donor_map.visual);
    const auto donor_collision = read_bytes(package / donor_map.collision);

    // Everything the World depends on, in one canonical text.
    std::string inputs = "world 1\ndonor " + hash_of(donor_visual) + ' ' + hash_of(donor_collision) + '\n';
    for (auto& asset : assets) {
        const auto bytes = read_bytes(directory / asset.export_path);
        const auto hash = hash_of(bytes);
        if (!asset.export_hash.empty() && asset.export_hash != hash)
            report.lines.push_back("asset\t" + asset.id + "\texport changed since it was recorded");
        asset.export_hash = hash;
        inputs += "asset " + asset.id + ' ' + std::to_string(static_cast<int>(asset.kind)) + ' ' + hash + '\n';
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

    const auto merged = merged_source(true);
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
    // The mission workspace packages these files from build/.
    refresh_workspace(directory / "mission", directory, outputs);
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

namespace {

// UTF-8 to UTF-16 code units (surrogate pairs above the BMP).
void append_utf16(std::vector<char16_t>& out, const std::string_view utf8) {
    for (std::size_t i = 0; i < utf8.size();) {
        const auto byte = static_cast<unsigned char>(utf8[i]);
        std::uint32_t code{};
        std::size_t length = 1;
        if (byte < 0x80) code = byte;
        else if ((byte >> 5) == 0x6) code = byte & 0x1F, length = 2;
        else if ((byte >> 4) == 0xE) code = byte & 0x0F, length = 3;
        else if ((byte >> 3) == 0x1E) code = byte & 0x07, length = 4;
        else throw std::runtime_error("Text is not UTF-8");
        if (i + length > utf8.size()) throw std::runtime_error("Text is not UTF-8");
        for (std::size_t k = 1; k < length; ++k) code = code << 6 | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        i += length;
        if (code >= 0x10000) {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
}

} // namespace

namespace {

// A .fli's text as UTF-16 code units, without its BOM, lines ending in LF.
std::vector<char16_t> fli_text(const std::span<const std::byte> fli) {
    if (fli.size() % 2 != 0) throw std::runtime_error("The text file is not UTF-16");
    std::vector<char16_t> text;
    for (std::size_t i = 0; i + 1 < fli.size(); i += 2)
        text.push_back(static_cast<char16_t>(std::to_integer<unsigned>(fli[i]) | std::to_integer<unsigned>(fli[i + 1]) << 8));
    if (!text.empty() && text.front() == 0xFEFF) text.erase(text.begin());
    std::vector<char16_t> lines;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (!(text[i] == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n')) lines.push_back(text[i]);
    return lines;
}

std::set<std::string> fli_ids(const std::vector<char16_t>& text) {
    std::set<std::string> ids;
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = begin;
        while (end < text.size() && text[end] != u'\n') ++end;
        std::string line;
        for (auto k = begin; k < end; ++k) line.push_back(text[k] < 0x80 ? static_cast<char>(text[k]) : '?');
        const auto first = line.find_first_not_of(" \t"), last = line.find_last_not_of(" \t");
        if (first != std::string::npos) {
            line = line.substr(first, last - first + 1);
            if (std::ranges::all_of(line, [](const char c) { return c >= '0' && c <= '9'; })) ids.insert(line);
        }
        begin = end + 1;
    }
    return ids;
}

} // namespace

std::set<std::string> fli_string_ids(const std::span<const std::byte> fli) { return fli_ids(fli_text(fli)); }

std::vector<std::byte> append_fli_strings(const std::span<const std::byte> donor, const std::vector<ProjectText>& strings) {
    auto text = fli_text(donor);
    // The IDs the donor already has: lines of digits.
    const auto ids = fli_ids(text);
    for (const auto& string : strings)
        if (ids.contains(string.id)) throw std::runtime_error("The donor text file already has string " + string.id);
    while (!text.empty() && text.back() == u'\n') text.pop_back();
    append_utf16(text, "\n\n");
    for (const auto& string : strings) append_utf16(text, string.id + "\n\"" + string.text + "\"\n\n");
    std::vector<std::byte> out{std::byte{0xFF}, std::byte{0xFE}};
    const auto put = [&](const char16_t c) {
        out.push_back(static_cast<std::byte>(c & 0xFF));
        out.push_back(static_cast<std::byte>(c >> 8));
    };
    for (const auto c : text) {
        if (c == u'\n') put(u'\r');
        put(c);
    }
    return out;
}

std::filesystem::path AuthoringProject::lightmap_package_path(const ProjectLightmap& lightmap) const {
    return donor_map.visual.parent_path() / "Textures" / (lightmap.name + ".dds");
}

ProjectBuildReport AuthoringProject::build_lightmaps(const bool force) {
    ProjectBuildReport report;
    if (lightmaps.empty()) return report;
    if (const auto problems = check(); !problems.empty()) throw std::runtime_error(problems.front());
    for (const auto& lightmap : lightmaps) {
        const auto source = read_bytes(directory / lightmap.source);
        const auto inputs_hash = hash_of("lightmap 1\n" + hash_of(source));
        const auto path = std::filesystem::path("build") / lightmap_package_path(lightmap);
        const auto record = std::ranges::find(outputs, path, &ProjectOutput::path);
        std::error_code error;
        if (!force && record != outputs.end() && record->inputs_hash == inputs_hash &&
            std::filesystem::is_regular_file(directory / path, error) && hash_of(read_bytes(directory / path)) == record->hash) {
            report.lines.push_back("lightmap\t" + lightmap.name + "\tup to date");
            continue;
        }
        int width{}, height{};
        std::vector<std::uint8_t> rgba;
        std::string problem;
        if (!rws::decode_png(source, width, height, rgba, problem))
            throw std::runtime_error(lightmap.source.generic_string() + ": " + problem);
        const auto bytes = rws::encode_dds_dxt1(width, height, rgba);
        write_atomically(directory / path, bytes);
        const ProjectOutput output{path, "lightmap", hash_of(bytes), inputs_hash};
        if (record == outputs.end()) outputs.push_back(output);
        else *record = output;
        report.rebuilt = true;
        report.lines.push_back("lightmap\t" + path.generic_string() + '\t' + std::to_string(width) + 'x' +
                               std::to_string(height));
    }
    refresh_workspace(directory / "mission", directory, outputs);
    return report;
}

std::optional<std::string> AuthoringProject::next_text_id() const {
    if (!texts) return std::nullopt;
    std::set<std::int32_t> used;
    for (const auto& string : strings)
        if (!string.id.empty() && std::ranges::all_of(string.id, [](const char c) { return c >= '0' && c <= '9'; }))
            used.insert(std::stoi(string.id));
    for (auto id = texts->first; id <= texts->last; ++id)
        if (!used.contains(id)) {
            auto text = std::to_string(id);
            return std::string(text.size() < 4 ? 4 - text.size() : 0, '0') + text;
        }
    return std::nullopt;
}

ProjectBuildReport AuthoringProject::build_texts(const bool force) {
    ProjectBuildReport report;
    if (!texts || strings.empty()) {
        report.lines.push_back("texts\tno project strings");
        return report;
    }
    if (const auto problems = check(); !problems.empty()) throw std::runtime_error(problems.front());
    if (local.corpus.empty()) throw std::runtime_error("local.csfproj sets no corpus");
    const auto archive = texts->archive.stem();
    const auto donor = read_bytes(local.corpus / archive / texts->file);
    std::string inputs = "texts 1\ndonor " + hash_of(donor) + '\n';
    for (const auto& string : strings) inputs += string.id + ' ' + string.text + '\n';
    const auto inputs_hash = hash_of(inputs);
    const auto path = std::filesystem::path("build") / archive / texts->file;
    const auto record = std::ranges::find(outputs, path, &ProjectOutput::path);
    std::error_code error;
    if (!force && record != outputs.end() && record->inputs_hash == inputs_hash &&
        std::filesystem::is_regular_file(directory / path, error) && hash_of(read_bytes(directory / path)) == record->hash) {
        report.lines.push_back("texts\tup to date");
        return report;
    }
    const auto bytes = append_fli_strings(donor, strings);
    write_atomically(directory / path, bytes);
    const ProjectOutput output{path, "texts", hash_of(bytes), inputs_hash};
    if (record == outputs.end()) outputs.push_back(output);
    else *record = output;
    refresh_workspace(directory / "texts", directory, outputs);
    report.rebuilt = true;
    report.lines.push_back("texts\t" + path.generic_string() + '\t' + std::to_string(strings.size()) + " strings");
    return report;
}

rws::WorldSource AuthoringProject::merged_source(const bool with_donor_placements) const {
    std::map<std::string, rws::WorldSource> exports;
    for (const auto& asset : assets) {
        const auto text = read_text(directory / asset.export_path);
        auto parsed = rws::parse_world_source(text);
        if (!parsed) throw std::runtime_error(asset.export_path.generic_string() + ": " + parsed.error);
        exports.emplace(asset.id, std::move(*parsed.value));
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
            if (with_donor_placements)
                merged.pieces.push_back({placement.box_min, placement.box_max, placement.position, placement.yaw_degrees});
            break;
        case ProjectPlacement::Kind::prop:
            if (with_donor_placements)
                merged.props.push_back({placement.donor_instances, placement.position, placement.yaw_degrees});
            break;
        }
    }
    return merged;
}

std::vector<HeightFinding> AuthoringProject::height_report(const ActorPositions& actors) const {
    if (const auto problems = check(); !problems.empty()) throw std::runtime_error(problems.front());
    // Buildings stand on the terrain (not on themselves); props and actors on
    // the terrain and the buildings.
    const rws::GroundQuery ground(merged_source(false));
    rws::WorldSource terrain_only;
    for (const auto& asset : assets)
        if (asset.kind == ProjectAsset::Kind::terrain) {
            auto parsed = rws::parse_world_source(read_text(directory / asset.export_path));
            if (!parsed) throw std::runtime_error(asset.export_path.generic_string() + ": " + parsed.error);
            rws::append_world_source(terrain_only, *parsed.value);
        }
    const rws::GroundQuery terrain(terrain_only);
    std::map<std::string, const ProjectPlacement*> by_id;
    for (const auto& placement : placements) by_id[placement.id] = &placement;
    std::map<std::int32_t, const ProjectAnchor*> anchored;
    for (const auto& anchor : anchors) anchored[anchor.actor_id] = &anchor;

    // Resolved heights, memoized; an error string when a rule does not resolve.
    struct Resolved {
        std::optional<float> height;
        std::string problem;
    };
    std::map<std::string, Resolved> placement_heights;
    std::map<std::int32_t, Resolved> actor_heights;
    std::set<std::string> resolving;  // guards `on actor` chains back into placements
    std::function<Resolved(const HeightRule&, const rws::Vec3&, const std::string&)> resolve_rule;
    std::function<Resolved(const std::string&)> placement_height;
    std::function<Resolved(std::int32_t)> actor_height;
    resolve_rule = [&](const HeightRule& rule, const rws::Vec3& at, const std::string& key) -> Resolved {
        if (!resolving.insert(key).second) return {std::nullopt, "is part of a cycle of supports"};
        Resolved result;
        switch (rule.mode) {
        case HeightRule::Mode::absolute: result.height = at.y; break;
        case HeightRule::Mode::ground:
            if (const auto hit = (key.starts_with("building ") ? terrain : ground).highest(at.x, at.z))
                result.height = hit->height + rule.offset;
            else result.problem = "has no ground under it";
            break;
        case HeightRule::Mode::on: {
            Resolved support;
            if (rule.support_kind == "actor") {
                std::int32_t id{};
                const auto [end, error] = std::from_chars(rule.support_id.data(),
                                                          rule.support_id.data() + rule.support_id.size(), id);
                support = error == std::errc{} && end == rule.support_id.data() + rule.support_id.size()
                              ? actor_height(id)
                              : Resolved{std::nullopt, "stands on an invalid actor ID"};
            } else {
                support = placement_height(rule.support_id);
            }
            if (support.height) result.height = *support.height + rule.offset;
            else result.problem = "stands on " + rule.support_kind + ' ' + rule.support_id + ", which " + support.problem;
            break;
        }
        }
        resolving.erase(key);
        return result;
    };
    placement_height = [&](const std::string& id) -> Resolved {
        if (const auto found = placement_heights.find(id); found != placement_heights.end()) return found->second;
        const auto placement = by_id.find(id);
        if (placement == by_id.end()) return {std::nullopt, "does not exist"};
        const auto kind = placement->second->kind == ProjectPlacement::Kind::building ? "building " : "placement ";
        auto result = resolve_rule(placement->second->height, placement->second->position, kind + id);
        return placement_heights[id] = result;
    };
    actor_height = [&](const std::int32_t id) -> Resolved {
        if (const auto found = actor_heights.find(id); found != actor_heights.end()) return found->second;
        const auto position = actors.find(id);
        if (position == actors.end()) return {std::nullopt, "is not in the mission"};
        const auto anchor = anchored.find(id);
        auto result = anchor == anchored.end()
                          ? Resolved{position->second.y, {}}
                          : resolve_rule(anchor->second->height, position->second, "actor " + std::to_string(id));
        return actor_heights[id] = result;
    };

    std::vector<HeightFinding> findings;
    const auto report = [&](HeightFinding finding, const Resolved& resolved) {
        if (resolved.height && std::abs(*resolved.height - finding.position.y) <= 1.0F) return;
        finding.resolved = resolved.height;
        finding.problem = resolved.problem;
        findings.push_back(std::move(finding));
    };
    for (const auto& placement : placements)
        if (placement.height.mode != HeightRule::Mode::absolute)
            report({HeightFinding::Subject::placement, placement.id, 0, placement.position, {}, {}},
                   placement_height(placement.id));
    for (const auto& anchor : anchors) {
        if (anchor.height.mode == HeightRule::Mode::absolute) continue;
        const auto position = actors.find(anchor.actor_id);
        if (position == actors.end()) {
            findings.push_back({HeightFinding::Subject::actor, std::to_string(anchor.actor_id), anchor.actor_id, {},
                                std::nullopt, "is not in the mission"});
            continue;
        }
        report({HeightFinding::Subject::actor, std::to_string(anchor.actor_id), anchor.actor_id, position->second, {}, {}},
               actor_height(anchor.actor_id));
    }
    return findings;
}

void AuthoringProject::resnap(const std::vector<HeightFinding>& findings) {
    for (const auto& finding : findings) {
        if (finding.subject != HeightFinding::Subject::placement || !finding.resolved) continue;
        const auto placement = std::ranges::find(placements, finding.id, &ProjectPlacement::id);
        if (placement != placements.end()) placement->position.y = *finding.resolved;
    }
}

namespace {

std::string json_string(const std::string_view text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c);
        else if (c < 0x20) {
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
        } else out += static_cast<char>(c);
    }
    return out + '"';
}

std::string json_vec(const float x, const float y, const float z) {
    return "[" + text_of(x) + "," + text_of(y) + "," + text_of(z) + "]";
}

std::string json_vec(const csf::Vec3& v) { return json_vec(v.x, v.y, v.z); }

} // namespace

std::string reference_markers_json(const AuthoringProject& project, const MissionScene* scene,
                                   const std::map<std::int32_t, std::string>& actor_models) {
    std::string out = "{\n\"format\":\"csf-reference-1\",\n\"units\":\"game centimetres, Y up\",\n\"placements\":[";
    const auto kind_name = [](const ProjectPlacement::Kind kind) {
        return kind == ProjectPlacement::Kind::building ? "building" : kind == ProjectPlacement::Kind::piece ? "piece" : "prop";
    };
    for (std::size_t i = 0; i < project.placements.size(); ++i) {
        const auto& p = project.placements[i];
        out += std::string(i ? ",\n" : "\n") + "{\"id\":" + json_string(p.id) + ",\"kind\":\"" + kind_name(p.kind) +
               "\",\"position\":" + json_vec(p.position.x, p.position.y, p.position.z) + ",\"yaw\":" + text_of(p.yaw_degrees);
        if (!p.asset.empty()) out += ",\"asset\":" + json_string(p.asset);
        if (p.kind == ProjectPlacement::Kind::piece)
            out += ",\"box\":[" + json_vec(p.box_min.x, p.box_min.y, p.box_min.z) + "," +
                   json_vec(p.box_max.x, p.box_max.y, p.box_max.z) + "]";
        out += "}";
    }
    out += "\n],\n\"actors\":[";
    if (scene) {
        bool first = true;
        for (const auto& actor : scene->actors()) {
            const auto position = scene->actor_spawn_position(actor);
            if (!actor.id || !position) continue;
            out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*actor.id) +
                   ",\"name\":" + json_string(actor.name.value_or("")) +
                   ",\"class\":" + std::to_string(actor.class_id.value_or(-1)) + ",\"position\":" + json_vec(*position) +
                   ",\"heading\":" + text_of(actor.heading.value_or(0));
            if (actor.class_id)
                if (const auto model = actor_models.find(*actor.class_id); model != actor_models.end())
                    out += ",\"model\":" + json_string(model->second);
            out += "}";
            first = false;
        }
    }
    out += "\n],\n\"navigation\":[";
    if (scene) {
        bool first_group = true;
        for (const auto& group : scene->navigation()) {
            if (!group.id) continue;
            out += std::string(first_group ? "\n" : ",\n") + "{\"id\":" + std::to_string(*group.id) +
                   ",\"name\":" + json_string(group.name.value_or("")) + ",\"type\":" + std::to_string(group.type.value_or(0)) +
                   ",\"points\":[";
            bool first = true;
            for (const auto& point : group.points) {
                if (!point.id || !point.position) continue;
                out += std::string(first ? "" : ",") + "{\"id\":" + std::to_string(*point.id) +
                       ",\"position\":" + json_vec(*point.position) + "}";
                first = false;
            }
            out += "],\"links\":[";
            first = true;
            for (const auto& link : group.connections) {
                if (!link.origin_point || !link.destination_point) continue;
                out += std::string(first ? "" : ",") + "[" + std::to_string(*link.origin_point) + "," +
                       std::to_string(*link.destination_point) + "]";
                first = false;
            }
            out += "]}";
            first_group = false;
        }
    }
    out += "\n],\n\"links\":[";
    if (scene) {
        bool first = true;
        for (const auto& link : scene->cross_group_connections()) {
            if (!link.origin_group || !link.origin_point || !link.destination_group || !link.destination_point) continue;
            out += std::string(first ? "\n" : ",\n") + "[" + std::to_string(*link.origin_group) + "," +
                   std::to_string(*link.origin_point) + "," + std::to_string(*link.destination_group) + "," +
                   std::to_string(*link.destination_point) + "]";
            first = false;
        }
    }
    out += "\n],\n\"areas\":[";
    if (scene) {
        bool first = true;
        for (const auto& area : scene->areas()) {
            if (!area.id) continue;
            out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*area.id) +
                   ",\"name\":" + json_string(area.name.value_or("")) + ",\"height\":" + text_of(area.height.value_or(0)) +
                   ",\"points\":[";
            for (std::size_t i = 0; i < area.points.size(); ++i) out += (i ? "," : "") + json_vec(area.points[i]);
            out += "]}";
            first = false;
        }
    }
    out += "\n],\n\"dummies\":[";
    if (scene) {
        bool first = true;
        for (const auto& dummy : scene->dummies()) {
            if (!dummy.id || !dummy.position) continue;
            out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*dummy.id) +
                   ",\"name\":" + json_string(dummy.name.value_or("")) + ",\"position\":" + json_vec(*dummy.position) +
                   ",\"heading\":" + text_of(dummy.heading.value_or(0)) + "}";
            first = false;
        }
    }
    return out + "\n]\n}\n";
}

} // namespace csf
