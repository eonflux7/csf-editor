#include "csf/godot_export.hpp"

#include "csf/authoring_project.hpp"
#include "csf/mission_edit.hpp"
#include "csf/object_database.hpp"
#include "csf/project_pipeline.hpp"
#include "rws/document.hpp"
#include "rws/scene_export.hpp"
#include "rws/texture_image.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace csf {
namespace {

constexpr std::string_view manifest_format = "opencsf-godot-manifest";

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string json_string(const std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') out += {'\\', c};
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        } else out += c;
    }
    return out + '"';
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << text;
    if (!file) throw std::runtime_error("Cannot write " + path.generic_string());
}

// The out folder must be new, empty, or an earlier export: never someone's files.
void check_out_folder(const std::filesystem::path& out) {
    std::error_code error;
    if (!std::filesystem::exists(out, error) || std::filesystem::is_empty(out, error)) return;
    std::ifstream manifest(out / "manifest.json", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
    if (text.find(json_string(manifest_format)) == std::string::npos)
        throw std::runtime_error(out.generic_string() + " is not empty and holds no earlier Godot export");
}

// How a texture's alpha is used: none, cut-out (almost every transparent pixel
// fully so, as DXT1's one-bit alpha), or blended (glass, smoke).
rws::SceneTexture::Alpha alpha_of(const std::vector<std::uint8_t>& rgba) {
    std::size_t transparent{}, partial{};
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] < 250) ++transparent;
        if (rgba[i] >= 8 && rgba[i] < 250) ++partial;
    }
    if (transparent == 0) return rws::SceneTexture::Alpha::opaque;
    return partial * 4 > transparent ? rws::SceneTexture::Alpha::blend : rws::SceneTexture::Alpha::mask;
}

// Converts the DDS/PNG textures glTF files name into PNGs under textures/,
// mirroring their corpus folders (lower case) so equal names never collide.
// A texture is looked up as the editor does: beside the file that names it,
// then in each Textures folder from there up to the mission package.
class TextureConverter {
public:
    TextureConverter(std::filesystem::path out, std::filesystem::path corpus, std::filesystem::path package_root)
        : out_(std::move(out)), corpus_(std::move(corpus)), package_root_(std::move(package_root)) {}

    // A resolver for a glTF written to `gltf` from the source file `source`.
    rws::SceneTextureResolver resolver(const std::filesystem::path& source, const std::filesystem::path& gltf) {
        return [this, source, gltf](const std::string_view name) -> std::optional<rws::SceneTexture> {
            const auto found = find(source, lower(std::string(name)));
            if (!found) {
                missing_.insert(std::string(name));
                return std::nullopt;
            }
            const auto png = convert(*found);
            if (!png) return std::nullopt;
            return rws::SceneTexture{(out_ / png->first).lexically_relative(gltf.parent_path()).generic_string(), png->second};
        };
    }
    [[nodiscard]] std::size_t converted() const noexcept { return converted_.size(); }
    [[nodiscard]] const std::set<std::string>& missing() const noexcept { return missing_; }
    [[nodiscard]] const std::map<std::string, std::string>& failed() const noexcept { return failed_; }

private:
    // Lower-case entry name (files and folders) -> path, per folder, read once.
    const std::map<std::string, std::filesystem::path>& listing(const std::filesystem::path& folder) {
        auto [it, added] = listings_.try_emplace(folder);
        if (added) {
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error))
                it->second.emplace(lower(entry.path().filename().string()), entry.path());
        }
        return it->second;
    }
    std::optional<std::filesystem::path> find(const std::filesystem::path& source, const std::string& name) {
        const auto has_extension = std::filesystem::path(name).has_extension();
        const auto in = [&](const std::filesystem::path& folder) -> std::optional<std::filesystem::path> {
            const auto& files = listing(folder);
            for (const auto& candidate : has_extension ? std::vector{name} : std::vector{name + ".dds", name + ".png"})
                if (const auto it = files.find(candidate); it != files.end()) return it->second;
            return std::nullopt;
        };
        auto folder = source.parent_path();
        if (auto found = in(folder)) return found;
        for (;;) {
            if (const auto textures = listing(folder).find("textures"); textures != listing(folder).end())
                if (auto found = in(textures->second)) return found;
            if (folder == package_root_ || !folder.has_parent_path() || folder.parent_path() == folder) return std::nullopt;
            folder = folder.parent_path();
        }
    }
    std::optional<std::pair<std::filesystem::path, rws::SceneTexture::Alpha>> convert(const std::filesystem::path& image) {
        if (const auto it = converted_.find(image); it != converted_.end()) return it->second;
        if (failed_.contains(image.generic_string())) return std::nullopt;
        int width{}, height{};
        std::vector<std::uint8_t> rgba;
        std::string error;
        if (!rws::decode_texture_image(image, width, height, rgba, error)) {
            failed_[image.generic_string()] = error;
            return std::nullopt;
        }
        auto relative = std::filesystem::path(lower(image.lexically_relative(corpus_).generic_string()));
        relative = std::filesystem::path("textures") / relative.replace_extension(".png");
        std::filesystem::create_directories((out_ / relative).parent_path());
        std::filesystem::remove(out_ / relative);
        if (!rws::write_png_rgba(out_ / relative, width, height, rgba, error)) {
            failed_[image.generic_string()] = error;
            return std::nullopt;
        }
        return converted_[image] = {relative, alpha_of(rgba)};
    }

    std::filesystem::path out_, corpus_, package_root_;
    std::map<std::filesystem::path, std::map<std::string, std::filesystem::path>> listings_;
    std::map<std::filesystem::path, std::pair<std::filesystem::path, rws::SceneTexture::Alpha>> converted_;
    std::map<std::string, std::string> failed_;  // path -> why
    std::set<std::string> missing_;              // names found nowhere
};

// One manifest entry: its logical ID and its fields, already JSON.
using ManifestEntry = std::vector<std::pair<std::string, std::string>>;

} // namespace

GodotExportResult export_godot(const GodotExportOptions& options) {
    GodotExportResult result;
    const auto slots = mission_slots(options.corpus);
    const auto slot = std::ranges::find_if(slots, [&](const MissionSlot& value) { return lower(value.mission) == lower(options.mission); });
    if (slot == slots.end()) throw std::runtime_error("No mission '" + options.mission + "' in " + options.corpus.generic_string());
    check_out_folder(options.out);
    const auto corpus = std::filesystem::weakly_canonical(std::filesystem::absolute(options.corpus));
    const auto package_root = corpus / slot->mission;
    // Provenance names files as corpus-relative paths, the same on every machine.
    const auto source = [&](const std::filesystem::path& path) {
        return std::filesystem::weakly_canonical(std::filesystem::absolute(path)).lexically_relative(corpus).generic_string();
    };
    std::map<std::string, ManifestEntry> assets;
    TextureConverter textures(options.out, corpus, package_root);

    // The map: visual and collision glTF, beside each other.
    const auto map_folder = std::filesystem::path("maps") / slot->mission;
    std::filesystem::create_directories(options.out / map_folder);
    const auto visual = rws::Document::load(package_root / slot->visual_map);
    const auto visual_stats =
        rws::export_scene_gltf(visual.chunks(), visual.scene_instances(), visual.bytes(), options.out / map_folder / "visual.gltf",
                               textures.resolver(package_root / slot->visual_map, options.out / map_folder / "visual.gltf"));
    result.lines.push_back("visual\t" + (map_folder / "visual.gltf").generic_string() + '\t' +
                           std::to_string(visual_stats.triangles) + " triangles");
    const auto collision = rws::Document::load(package_root / slot->collision_map);
    const auto collision_stats =
        rws::export_collision_gltf(collision.chunks(), collision.bytes(), options.out / map_folder / "collision.gltf");
    result.lines.push_back("collision\t" + (map_folder / "collision.gltf").generic_string() + '\t' +
                           std::to_string(collision_stats.triangles) + " triangles");

    ResourceIndex package;
    package.add_root(package_root);
    package.build();

    // The sky dome the mission's .vis names, when it has one.
    std::optional<std::filesystem::path> sky;
    auto vis = package_root / slot->scene;
    vis.replace_extension(".vis");
    if (std::filesystem::is_regular_file(vis))
        for (const auto& reference : read_vis(vis).references) {
            if (reference.kind != DependencyKind::sky_model) continue;
            const auto resolution = package.resolve(reference.path);
            if (resolution.candidate_indices.size() != 1) {
                result.problems.push_back("sky " + reference.path + ": not found");
                continue;
            }
            const auto& model_path = package.resources()[resolution.candidate_indices.front()].path;
            try {
                const auto model = rws::Document::load(model_path);
                (void)rws::export_scene_gltf(model.chunks(), model.scene_instances(), model.bytes(),
                                             options.out / map_folder / "sky.gltf",
                                             textures.resolver(model_path, options.out / map_folder / "sky.gltf"));
                sky = map_folder / "sky.gltf";
                result.lines.push_back("sky\t" + sky->generic_string());
            } catch (const std::exception& error) {
                result.problems.push_back("sky " + source(model_path) + ": " + error.what());
            }
        }

    // One model per actor class: characters from Models/Char, props from the rest.
    // A scene that cannot be opened still leaves the map usable.
    std::optional<MissionEditor> editor;
    try {
        editor.emplace(MissionEditor::open(package_root / slot->scene, package_root));
    } catch (const std::exception& error) {
        result.problems.push_back("scene " + source(package_root / slot->scene) + ": " + error.what());
    }
    std::map<std::int32_t, std::string> class_ids;  // class -> logical ID
    if (editor) {
        const auto resources = editor->resource_index(package);
        for (const auto& association : associate_actors(editor->scene(), editor->objects(), resources)) {
            if (!association.class_id || class_ids.contains(*association.class_id)) continue;
            const auto class_name = "class " + std::to_string(*association.class_id);
            if (association.visual_models.size() != 1 || !association.visual_models.front().resolved_path) {
                result.problems.push_back(class_name + ": no single resolved visual model (" +
                                          std::to_string(association.visual_models.size()) + " named)");
                continue;
            }
            const auto& model_path = *association.visual_models.front().resolved_path;
            const auto relative = model_path.lexically_relative(package_root);
            const bool character = std::ranges::any_of(relative, [](const auto& part) { return lower(part.string()) == "char"; });
            const auto stem = lower(model_path.stem().string());
            const auto id = std::string(character ? "character/" : "prop/") + stem;
            const auto file = std::filesystem::path(character ? "characters" : "props") / (stem + ".gltf");
            if (!assets.contains(id)) {
                try {
                    const auto model = rws::Document::load(model_path);
                    (void)rws::export_scene_gltf(model.chunks(), model.scene_instances(), model.bytes(), options.out / file,
                                                 textures.resolver(model_path, options.out / file));
                } catch (const std::exception& error) {
                    result.problems.push_back(class_name + " (" + source(model_path) + "): " + error.what());
                    continue;
                }
                assets[id] = {{"file", json_string(file.generic_string())}, {"source", json_string(source(model_path))}};
                result.lines.push_back(std::string(character ? "character\t" : "prop\t") + file.generic_string());
            }
            class_ids[*association.class_id] = id;
        }
    }

    // Actors, routes, areas and dummies, in game centimetres as the reference export writes them.
    const auto markers = map_folder / "markers.json";
    write_file(options.out / markers, reference_markers_json(AuthoringProject{}, editor ? &editor->scene() : nullptr, class_ids));
    result.lines.push_back("markers\t" + markers.generic_string());
    assets["map/" + lower(slot->mission)] = {
        {"visual", json_string((map_folder / "visual.gltf").generic_string())},
        {"collision", json_string((map_folder / "collision.gltf").generic_string())},
        {"markers", json_string(markers.generic_string())},
        {"source", json_string(source(package_root / slot->scene))},
    };
    if (sky) assets["map/" + lower(slot->mission)].emplace_back("sky", json_string(sky->generic_string()));

    result.lines.push_back("textures\t" + std::to_string(textures.converted()) + " PNG");
    for (const auto& name : textures.missing()) result.problems.push_back("texture " + name + ": not found");
    for (const auto& [path, why] : textures.failed())
        result.problems.push_back("texture " + source(path) + ": " + why);

    std::string manifest = "{\n  \"format\": " + json_string(manifest_format) +
                           ",\n  \"version\": 1,\n  \"units\": \"metres\",\n  \"up\": \"Y\",\n  \"missions\": [" +
                           json_string(slot->mission) + "],\n  \"assets\": {";
    bool first = true;
    for (const auto& [id, fields] : assets) {
        manifest += std::string(first ? "\n" : ",\n") + "    " + json_string(id) + ": {";
        for (std::size_t i = 0; i < fields.size(); ++i)
            manifest += (i ? ", " : "") + json_string(fields[i].first) + ": " + fields[i].second;
        manifest += "}";
        first = false;
    }
    write_file(options.out / "manifest.json", manifest + "\n  }\n}\n");
    result.lines.push_back("manifest\tmanifest.json\t" + std::to_string(assets.size()) + " assets");
    return result;
}

} // namespace csf
