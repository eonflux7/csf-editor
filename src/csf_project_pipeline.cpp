#include "csf/project_pipeline.hpp"

#include "csf/authoring.hpp"
#include "csf/mission_edit.hpp"
#include "csf/mod_project.hpp"
#include "rws/world_model.hpp"
#include "rws/world_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>

namespace csf {
namespace {

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path.generic_string());
    std::vector<std::byte> bytes;
    for (std::istreambuf_iterator<char> it(input), end; it != end; ++it) bytes.push_back(static_cast<std::byte>(*it));
    return bytes;
}

void write_text(const std::filesystem::path& path, const std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) throw std::runtime_error("Cannot write " + path.generic_string());
    }
    std::filesystem::rename(temporary, path);
}

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A child of `directory` whose name matches `name` ignoring case.
std::optional<std::filesystem::path> child_named(const std::filesystem::path& directory, const std::string& name) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
        if (lower(entry.path().filename().string()) == lower(name)) return entry.path();
    return std::nullopt;
}

std::string file_hash(const std::filesystem::path& path) { return sha256(read_bytes(path)); }

// The donor map's most used texture (visual World) and surface (collision
// World), for a starter terrain that looks like the slot's ground.
std::pair<std::string, std::string> dominant_materials(const std::filesystem::path& visual,
                                                       const std::filesystem::path& collision) {
    const auto most_used = [](const std::filesystem::path& path, const bool surface) -> std::string {
        const auto bytes = read_bytes(path);
        const auto offset = rws::find_map_world(bytes);
        if (!offset) throw std::runtime_error(path.generic_string() + " has no World");
        const auto world = rws::parse_world_model(bytes, *offset);
        if (!world) throw std::runtime_error(path.generic_string() + ": " + world.error);
        const auto materials = rws::split_material_list(world.value->material_list, world.value->library_id);
        if (!materials) throw std::runtime_error(path.generic_string() + ": " + materials.error);
        // Ground: the area of the triangles that face up, seen from above.
        std::map<std::size_t, double> uses;
        for (const auto& sector : world.value->sectors)
            for (const auto& triangle : sector.triangles) {
                const auto& a = sector.positions.at(triangle.vertices[0]);
                const auto& b = sector.positions.at(triangle.vertices[1]);
                const auto& c = sector.positions.at(triangle.vertices[2]);
                const double ux = b.x - a.x, uz = b.z - a.z, vx = c.x - a.x, vz = c.z - a.z;
                const double uy = b.y - a.y, vy = c.y - a.y;
                const double ny = uz * vx - ux * vz;  // (u x v).y
                const double nx = uy * vz - uz * vy, nz = ux * vy - uy * vx;
                const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length <= 0 || std::abs(ny) / length < 0.8) continue;
                uses[static_cast<std::size_t>(sector.material_window_base) + triangle.material] += std::abs(ny) / 2;
            }
        std::string best;
        double count = 0;
        for (const auto& [index, n] : uses) {
            if (index >= materials.value->size() || n <= count) continue;
            const auto& material = (*materials.value)[index];
            auto name = surface ? rws::material_surface_name(material) : rws::material_texture_name(material);
            if (name.empty()) continue;
            best = std::move(name);
            count = n;
        }
        if (best.empty()) throw std::runtime_error(path.generic_string() + " has no named material");
        return best;
    };
    return {most_used(visual, false), most_used(collision, true)};
}

// A flat square of `size` centimetres a side centred on the donor map, in
// 400 cm cells.
std::string flat_terrain(const std::filesystem::path& visual, const std::filesystem::path& collision, const float size) {
    const auto [texture, surface] = dominant_materials(visual, collision);
    const auto bytes = read_bytes(visual);
    const auto world = rws::parse_world_model(bytes, *rws::find_map_world(bytes));
    const auto& box_min = world.value->bounding_box_inf;
    const auto& box_max = world.value->bounding_box_sup;
    const float cx = std::round((box_min.x + box_max.x) / 2.0F / 100.0F) * 100.0F;
    const float cz = std::round((box_min.z + box_max.z) / 2.0F / 100.0F) * 100.0F;
    constexpr float cell = 400.0F;
    const auto cells = std::max(1, static_cast<int>(std::ceil(size / cell)));
    const float half = static_cast<float>(cells) * cell / 2.0F;
    rws::WorldSource source;
    source.materials.push_back({texture, surface, 228, {}});
    for (int row = 0; row <= cells; ++row)
        for (int column = 0; column <= cells; ++column) {
            rws::WorldBuildVertex vertex;
            const float x = cx - half + static_cast<float>(column) * cell;
            const float z = cz - half + static_cast<float>(row) * cell;
            vertex.position = {x, 0.0F, z};
            vertex.normal = {0.0F, 1.0F, 0.0F};
            vertex.texcoords[0] = {static_cast<float>(column), static_cast<float>(row)};
            source.vertices.push_back(vertex);
            source.has_second_uv.push_back(false);
            source.exact_positions.push_back({x, 0.0, z});
        }
    const auto at = [&](const int column, const int row) { return static_cast<std::uint32_t>(row * (cells + 1) + column); };
    for (int row = 0; row < cells; ++row)
        for (int column = 0; column < cells; ++column) {
            // Counter-clockwise seen from above (+Y).
            source.faces.push_back({{at(column, row), at(column, row + 1), at(column + 1, row)}, 0, true, true});
            source.faces.push_back({{at(column + 1, row), at(column, row + 1), at(column + 1, row + 1)}, 0, true, true});
        }
    return "# A flat starter terrain made by `csf-mod project-new`; replace it from Blender.\n" +
           rws::write_world_source(source);
}

std::string timestamp_id() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream out;
    out << std::put_time(&local, "%Y%m%d-%H%M%S");
    return out.str();
}

// Registers `built` (a generated file) at `relative` in a csf-mod workspace.
void register_file(ModProject& workspace, const std::filesystem::path& relative, const std::filesystem::path& built) {
    ModFile file;
    file.relative_path = relative;
    file.authored_path = std::filesystem::absolute(built);
    const auto source = workspace.source_root / relative;
    if (std::filesystem::is_regular_file(source)) file.source_sha256 = file_hash(source);
    file.output_sha256 = file_hash(built);
    workspace.add_file(std::move(file));
}

} // namespace

std::vector<MissionSlot> mission_slots(const std::filesystem::path& corpus) {
    std::vector<MissionSlot> slots;
    std::error_code error;
    std::vector<std::filesystem::path> missions;
    for (const auto& entry : std::filesystem::directory_iterator(corpus, error))
        if (entry.is_directory()) missions.push_back(entry.path());
    std::ranges::sort(missions);
    for (const auto& mission : missions) {
        const auto name = mission.filename().string();
        const auto maps = child_named(mission, "Maps");
        if (!maps) continue;
        std::vector<std::filesystem::path> folders;
        for (const auto& entry : std::filesystem::directory_iterator(*maps, error))
            if (entry.is_directory()) folders.push_back(entry.path());
        std::ranges::sort(folders);
        for (const auto& folder : folders) {
            // <mission>.scn, or the folder's only scene; the map is <folder>.rws
            // with <folder>_col.rws, or the folder's only such pair.
            auto scene = child_named(folder, name + ".scn");
            std::vector<std::filesystem::path> scenes, maps;
            for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
                const auto file = lower(entry.path().filename().string());
                if (file.ends_with(".scn")) scenes.push_back(entry.path());
                if (file.ends_with("_col.rws")) maps.push_back(entry.path());
            }
            if (!scene && scenes.size() == 1) scene = scenes.front();
            const auto map_name = folder.filename().string();
            auto visual = child_named(folder, map_name + ".rws");
            auto collision = child_named(folder, map_name + "_col.rws");
            if ((!visual || !collision) && maps.size() == 1) {
                const auto stem = maps.front().filename().string();
                collision = maps.front();
                visual = child_named(folder, stem.substr(0, stem.size() - 8) + ".rws");
            }
            if (!scene || !visual || !collision) continue;
            slots.push_back({name, scene->lexically_relative(mission), std::filesystem::path("maps") / (name + ".pak"),
                             visual->lexically_relative(mission), collision->lexically_relative(mission)});
            break;
        }
    }
    return slots;
}

std::pair<std::int32_t, std::int32_t> free_text_range(const std::filesystem::path& corpus,
                                                      const std::filesystem::path& texts_archive,
                                                      const std::filesystem::path& texts_file,
                                                      const std::filesystem::path& projects_root,
                                                      const std::filesystem::path& except) {
    std::vector<std::pair<std::int32_t, std::int32_t>> taken;
    const auto donor = corpus / texts_archive.stem() / texts_file;
    if (std::filesystem::is_regular_file(donor))
        for (const auto& id : fli_string_ids(read_bytes(donor))) {
            const auto value = std::stoi(id);
            taken.emplace_back(value, value);
        }
    std::error_code error;
    if (!projects_root.empty())
        for (const auto& entry : std::filesystem::directory_iterator(projects_root, error)) {
            if (!std::filesystem::is_regular_file(entry.path() / "project.csfproj", error)) continue;
            if (!except.empty() && std::filesystem::equivalent(entry.path(), except, error)) continue;
            try {
                const auto other = AuthoringProject::load(entry.path());
                if (other.texts && lower(other.texts->file.generic_string()) == lower(texts_file.generic_string()))
                    taken.emplace_back(other.texts->first, other.texts->last);
            } catch (const std::exception&) {
                // Unreadable projects take nothing.
            }
        }
    // Hundreds from 900 (hello world's), clear of everything taken.
    for (std::int32_t first = 900;; first += 100) {
        const auto last = first + 99;
        if (std::ranges::none_of(taken, [&](const auto& range) { return range.first <= last && range.second >= first; }))
            return {first, last};
    }
}

NewProject create_authoring_project(const NewProjectOptions& options) {
    NewProject result;
    std::error_code error;
    if (std::filesystem::exists(options.directory, error) && !std::filesystem::is_empty(options.directory, error))
        throw std::runtime_error(options.directory.generic_string() + " exists and is not empty");
    if (options.name.empty()) throw std::runtime_error("The project needs a name");
    const auto slots = mission_slots(options.corpus);
    const auto slot = std::ranges::find_if(slots, [&](const MissionSlot& value) { return lower(value.mission) == lower(options.slot); });
    if (slot == slots.end()) throw std::runtime_error("No mission slot '" + options.slot + "' in " + options.corpus.generic_string());
    std::filesystem::create_directories(options.directory);
    const auto directory = std::filesystem::canonical(options.directory);
    const auto package_root = options.corpus / slot->mission;

    auto& project = result.project;
    project.directory = directory;
    project.name = options.name;
    project.slot = {slot->mission, slot->scene, slot->archive};
    project.donor_map = {slot->visual_map, slot->collision_map};
    project.local.corpus = std::filesystem::weakly_canonical(std::filesystem::absolute(options.corpus));
    project.local.blender = options.blender;
    project.local.test_install = options.test_install;
    const std::filesystem::path texts_archive = "GlobalEK.pak";
    const auto texts_file = std::filesystem::path("Texts") / (slot->mission + ".fli");
    if (std::filesystem::is_regular_file(options.corpus / texts_archive.stem() / texts_file)) {
        const auto [first, last] = free_text_range(options.corpus, texts_archive, texts_file, options.projects_root, directory);
        project.texts = AuthoringProject::Texts{texts_archive, texts_file, first, last};
        result.lines.push_back("texts\t" + texts_file.generic_string() + " IDs " + std::to_string(first) + "-" +
                               std::to_string(last));
    }
    std::string terrain_text;
    if (options.terrain_size > 0) try {
            terrain_text = flat_terrain(package_root / slot->visual_map, package_root / slot->collision_map,
                                        options.terrain_size);
        } catch (const std::runtime_error& failure) {
            // A donor without named materials (a test map): the slot's own map stays.
            result.lines.push_back(std::string("terrain\tnone: ") + failure.what());
        }
    if (!terrain_text.empty()) {
        const std::filesystem::path export_path = "sources/world/terrain.csfworld";
        const auto& text = terrain_text;
        if (const auto parsed = rws::parse_world_source(text); !parsed)
            throw std::runtime_error("The starter terrain does not parse: " + parsed.error);
        write_text(directory / export_path, text);
        ProjectAsset terrain;
        terrain.id = "terrain";
        terrain.kind = ProjectAsset::Kind::terrain;
        terrain.blend = "sources/world/terrain.blend";
        terrain.export_path = export_path;
        terrain.exporter = "project-new";
        terrain.exporter_version = 1;
        terrain.export_hash = "sha256:" + sha256(std::as_bytes(std::span(text)));
        project.assets.push_back(std::move(terrain));
        result.lines.push_back("terrain\t" + export_path.generic_string());
    }
    project.save();
    if (!project.assets.empty())
        for (const auto& line : project.build_world(true).lines) result.lines.push_back(line);
    project.save();

    // The mission workspace: the slot emptied, the built map files packaged.
    const auto workspace_dir = directory / "mission";
    auto workspace = ModProject::create(workspace_dir, package_root, options.name, options.tool_version);
    for (const auto& output : project.outputs)
        if (output.kind == "world" || output.kind == "sectors")
            register_file(workspace, output.path.lexically_relative("build"), directory / output.path);
    workspace.save();
    write_mission_project_info(workspace_dir, {slot->scene, {}});
    auto editor = MissionEditor::open(package_root / slot->scene, package_root, &workspace);
    if (const auto emptied = editor.new_mission(); !emptied.applied)
        throw std::runtime_error("Could not empty the slot: " + emptied.message);
    (void)editor.save(workspace);
    result.lines.push_back("mission\t" + workspace_dir.generic_string() + "\t" + std::to_string(workspace.files.size()) +
                           " files");
    return result;
}

std::optional<std::filesystem::path> find_original_archive(const AuthoringProject& project,
                                                           const std::filesystem::path& archive) {
    std::error_code error;
    if (const auto found = project.local.originals.find(archive); found != project.local.originals.end())
        return std::filesystem::is_regular_file(found->second, error) ? std::optional(found->second) : std::nullopt;
    const auto& install = project.local.test_install;
    if (install.empty()) return std::nullopt;
    // Deployment backups are named by time: the oldest holds what shipped.
    std::vector<std::filesystem::path> backups;
    for (const auto& entry : std::filesystem::directory_iterator(install / ".csf-mod-backups", error)) {
        const auto name = entry.path().filename().string();
        if (!name.empty() && std::ranges::all_of(name, [](const char c) { return c >= '0' && c <= '9'; }) &&
            std::filesystem::is_regular_file(entry.path() / "files" / archive, error))
            backups.push_back(entry.path());
    }
    std::ranges::sort(backups, [](const auto& a, const auto& b) {
        const auto x = a.filename().string(), y = b.filename().string();
        return x.size() != y.size() ? x.size() < y.size() : x < y;
    });
    if (!backups.empty()) return backups.front() / "files" / archive;
    if (std::filesystem::is_regular_file(install / archive, error)) return install / archive;
    return std::nullopt;
}

ArchiveBuild build_archives(AuthoringProject& project, const std::filesystem::path& original_mission,
                            const std::filesystem::path& original_texts, const std::string& tool_version) {
    ArchiveBuild build;
    for (auto report : {project.build_world(false), project.build_texts(false), project.build_lightmaps(false)})
        build.lines.insert(build.lines.end(), report.lines.begin(), report.lines.end());
    project.save();
    build.id = timestamp_id();
    build.directory = project.directory / "dist" / build.id;
    for (int n = 2; std::filesystem::exists(build.directory); ++n)
        build.directory = project.directory / "dist" / (build.id + "-" + std::to_string(n));
    build.id = build.directory.filename().string();
    std::filesystem::create_directories(build.directory);

    const auto mission = ModProject::load(project.directory / "mission");
    if (const auto report = mission.validate(); !report.passed)
        throw std::runtime_error("The mission workspace does not validate: " +
                                 (report.diagnostics.empty() ? std::string("?")
                                                             : report.diagnostics.front().relative_path.generic_string() +
                                                                   ": " + report.diagnostics.front().message));
    build.mission_archive = build.directory / project.slot.archive.filename();
    const auto mission_result = mission.export_mission_pak(original_mission, build.mission_archive);
    build.lines.push_back("mission\t" + build.mission_archive.generic_string() + "\t" +
                          std::to_string(mission_result.replaced) + " replaced, " + std::to_string(mission_result.added) +
                          " added");
    std::string texts_json;
    if (project.texts && !project.strings.empty()) {
        const auto built = project.directory / "build" / project.texts->archive.stem() / project.texts->file;
        const auto workspace_dir = project.directory / "texts";
        auto texts = std::filesystem::is_regular_file(workspace_dir / ".csf-mod-state")
                         ? ModProject::load(workspace_dir)
                         : ModProject::create(workspace_dir, project.local.corpus / project.texts->archive.stem(),
                                              project.name + " texts", tool_version);
        register_file(texts, project.texts->file, built);
        texts.save();
        build.texts_archive = build.directory / project.texts->archive.filename();
        const auto texts_result = texts.export_mission_pak(original_texts, build.texts_archive);
        build.lines.push_back("texts\t" + build.texts_archive.generic_string() + "\t" +
                              std::to_string(texts_result.replaced + texts_result.added) + " files");
        texts_json = ",\n    \"" + build.texts_archive.filename().generic_string() + "\": \"" + texts_result.archive_sha256 +
                     "\"";
        texts_json += "\n  },\n  \"originals\": {\n    \"" + project.texts->archive.generic_string() + "\": \"" +
                      texts_result.original_sha256 + "\",\n    \"" + project.slot.archive.generic_string() + "\": \"" +
                      mission_result.original_sha256 + "\"";
    } else {
        texts_json = "\n  },\n  \"originals\": {\n    \"" + project.slot.archive.generic_string() + "\": \"" +
                     mission_result.original_sha256 + "\"";
    }
    const auto json = "{\n  \"schema\": \"csf-project-build-1\",\n  \"id\": \"" + build.id + "\",\n  \"tool\": \"" +
                      tool_version + "\",\n  \"archives\": {\n    \"" +
                      build.mission_archive.filename().generic_string() + "\": \"" + mission_result.archive_sha256 + "\"" +
                      texts_json + "\n  }\n}\n";
    write_text(build.directory / "build.json", json);
    return build;
}

std::vector<std::string> archive_builds(const AuthoringProject& project) {
    std::vector<std::string> builds;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(project.directory / "dist", error))
        if (entry.is_directory()) builds.push_back(entry.path().filename().string());
    std::ranges::sort(builds, std::greater{});
    return builds;
}

std::vector<ProjectDeployment> deploy_build(AuthoringProject& project, const std::string& build,
                                            const std::filesystem::path& test_install) {
    const auto directory = project.directory / "dist" / build;
    if (!std::filesystem::is_directory(directory)) throw std::runtime_error("No build " + build);
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> archives;  // workspace, game-relative
    archives.emplace_back(project.directory / "mission", project.slot.archive);
    if (project.texts && std::filesystem::is_regular_file(directory / project.texts->archive.filename()))
        archives.emplace_back(project.directory / "texts", project.texts->archive);
    std::vector<ProjectDeployment> deployed;
    try {
        for (const auto& [workspace, archive] : archives) {
            const auto result = ModProject::load(workspace).deploy_package(directory / archive.filename(), test_install,
                                                                           archive, PakOptions{}, false);
            deployed.push_back({build, archive, result.manifest_path});
        }
    } catch (...) {
        for (auto it = deployed.rbegin(); it != deployed.rend(); ++it) ModProject::rollback(it->manifest);
        throw;
    }
    project.local.test_install = test_install;
    for (const auto& deployment : deployed) project.local.deployments.push_back(deployment);
    return deployed;
}

DeploymentState deployment_state(const ProjectDeployment& deployment) {
    std::ifstream input(deployment.manifest);
    std::string magic, target, record, relative, before, after;
    std::getline(input, magic);
    input >> std::quoted(target) >> record >> std::quoted(relative) >> std::quoted(before) >> std::quoted(after);
    if (!input || magic != "csf-deployment-1") return DeploymentState::missing;
    const auto file = std::filesystem::path(target) / relative;
    std::error_code error;
    if (!std::filesystem::is_regular_file(file, error)) return before.empty() ? DeploymentState::rolled_back : DeploymentState::missing;
    const auto current = file_hash(file);
    if (current == after) return DeploymentState::active;
    if (current == before) return DeploymentState::rolled_back;
    return DeploymentState::replaced;
}

const char* deployment_state_name(const DeploymentState state) noexcept {
    switch (state) {
    case DeploymentState::active: return "deployed";
    case DeploymentState::rolled_back: return "rolled back";
    case DeploymentState::replaced: return "replaced since";
    case DeploymentState::missing: return "missing";
    }
    return "?";
}

void roll_back_build(const AuthoringProject& project, const std::string& build) {
    for (auto it = project.local.deployments.rbegin(); it != project.local.deployments.rend(); ++it)
        if (it->build == build && deployment_state(*it) == DeploymentState::active) ModProject::rollback(it->manifest);
}

} // namespace csf
