#include "csf/authoring.hpp"
#include "csf/authoring_project.hpp"
#include "csf/mission_components.hpp"
#include "csf/mission_edit.hpp"
#include "csf/mission_flow.hpp"
#include "csf/mission_ops.hpp"
#include "csf/mission.hpp"
#include "csf/mod_project.hpp"
#include "csf/object_database.hpp"
#include "csf/project_pipeline.hpp"
#include "csf/source_text.hpp"
#include "csf/tree.hpp"
#include "rws/document.hpp"
#include "rws/map_assembly.hpp"
#include "rws/scene_export.hpp"
#include "rws/texture_image.hpp"
#include "rws/world_model.hpp"
#include "rws/world_queries.hpp"
#include "rws/world_source.hpp"
#include "rws/world_recovery.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void usage() {
    std::cerr
        << "Usage:\n"
           "  csf-mod edit <source> <new-output> [--int <entry> <value> | --real <entry> <value>\n"
           "                                      | --string <entry> <value>\n"
           "                                      | --global-string <entry> <value>]...\n"
           "  csf-mod audit <resource-root>\n"
           "  csf-mod world-audit <resource-root|file.rws>... [--rebuild]\n"
           "  csf-mod world-rebuild <map.rws> <new-map.rws> [--max-sector-triangles N] [--overwrite]\n"
           "  csf-mod world-build <source.csfworld> <donor-map.rws> <new-map.rws>\n"
           "                      [--keep-props] [--max-sector-triangles N] [--overwrite]\n"
           "                      [--texture <name> <image.png|dds> <like-donor-texture>]...\n"
           "                      [--lightmap <name> <image.png|dds>]...\n"
           "  csf-mod world-source <map.rws> <new.csfworld> [--check] [--overwrite]\n"
           "  csf-mod mission-ops <workspace> <scene.scn> <ops-file> [--package <root>] [--ground <source.csfworld>]\n"
           "                      [--components]\n"
           "  csf-mod mission-components <workspace> <scene.scn> [--package <root>] [--ground <source.csfworld>] list | check\n"
           "                      | set <id> <line> <key>=<value>... | regenerate <id> | detach <id> | delete <id>\n"
           "  csf-mod mission-flow <scene.scn> [--package <root>] [--workspace <dir>]\n"
           "  csf-mod project-new <project-dir> --slot <mission> [--name <text>] [--corpus <root>]\n"
           "                      [--flat <size-cm> | --no-terrain] [--projects-root <dir>] [--test-install <dir>]\n"
           "  csf-mod project-archives <project-dir> [--original-mission <pak>] [--original-texts <pak>]\n"
           "  csf-mod project-deploy <project-dir> <build-id> [<test-install>]\n"
           "  csf-mod project-rollback <project-dir> <build-id>\n"
           "  csf-mod project-playtest <project-dir> <build-id> worked|failed <note>\n"
           "  csf-mod project-build <project-dir> [--force] [--verbose]\n"
           "  csf-mod project-heights <project-dir> [--resnap]\n"
           "  csf-mod project-reference <project-dir> <out-dir>\n"
           "  csf-mod project-asset <project-dir> <id> <terrain|building> <blend> <export>\n"
           "  csf-mod project-lightmap <project-dir> <name> <source.png>\n"
           "  csf-mod project-place <project-dir> <id> <building-asset> <x> <y> <z> [<yaw>] [--ground <offset>]\n"
           "  csf-mod project-lightmaps <project-dir>\n"
           "  csf-mod sector-build <source.csfworld> <new.sec> [--overwrite]\n"
           "  csf-mod world-ground <source.csfworld> <x> <z> [<x> <z>]...\n"
           "  csf-mod init <workspace> <source-root> <name>\n"
           "  csf-mod add <workspace> <game-relative-path> <authored-file> [change-manifest]\n"
           "              [--target <semantic-target>]...\n"
           "  csf-mod validate <workspace>\n"
           "  csf-mod build <workspace> <staging-directory>\n"
           "  csf-mod package <workspace> <staging-directory> <archive.pak>\n"
           "                  [--pakman <pakman-cli>] [--type stored|compressed]\n"
           "                  [--platform pc|ps2|xbox|ps2-prototype] [--overwrite]\n"
           "  csf-mod conflicts <workspace-a> <workspace-b>\n"
           "  csf-mod deploy-pak <workspace> <archive.pak> <test-install> <relative-pak-path>\n"
           "                     [--pakman <pakman-cli>] [--apply]\n"
           "  csf-mod deploy-loose <workspace> <staging-directory> <test-install> [--apply]\n"
           "  csf-mod rollback <deployment.state>\n"
           "  csf-mod decompile <file.scn|.gsc|.csc|.bdd> [<new-text-file>]\n"
           "  csf-mod compile <text-file> <template.csffbs> <new-output>\n"
           "  csf-mod mission-edit <workspace> <scene.scn> [--package <mission-root>] <operation>...\n"
           "      --move-actor <id> <x> <y> <z> [<heading-degrees>]\n"
           "      --actor-class <id> <class-id>         --actor-name <id> <name>\n"
           "      --actor-look <id> <class-id> (that class's model, own behaviour)\n"
           "      --actor-anim <id> <slot> <anim-id>    --clear-actor-anims <id>\n"
           "      --actor-scripts <id> <script-id,...>  --player <id>\n"
           "      --duplicate-actor <id> <dx> <dy> <dz> --delete-actor <id>\n"
           "      --add-actor <class-id> <x> <y> <z> <heading-degrees> <name>\n"
           "      --move-dummy <id> <x> <y> <z>         --move-light <id> <x> <y> <z>\n"
           "      --move-instance <map-offset> <x> <y> <z> (a static prop in the map .rws)\n"
           "      --duplicate-dummy <id> <dx> <dy> <dz> --delete-dummy <id>  --delete-light <id>\n"
           "      --add-nav-point <group> <x> <y> <z>   --move-nav-point <group> <point> <x> <y> <z>\n"
           "      --delete-nav-point <group> <point>\n"
           "      --link-nav <group> <point> <group> <point> (also --unlink-nav)\n"
           "      --move-area-point <area> <index> <x> <y> <z> (also --insert-area-point)\n"
           "      --remove-area-point <area> <index>    --area-height <area> <height>\n"
           "      --print-script <id>                   --set-script <id> <text-file>\n"
           "      --add-script <text-file>              --delete-script <id>\n"
           "      --import-class <donor-mission-root> <class-id>\n"
           "      --import-anim <donor-mission-root> <anim-id>\n"
           "      --force (applies to the following operations)\n"
           "  csf-mod export-mission <workspace> <original-mission.pak> <new-output.pak>\n"
           "                         [--pakman <pakman-cli>] [--overwrite]\n";
}

std::uint32_t u32(const std::string_view text) {
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid unsigned integer: " + std::string(text));
    return value;
}

std::int32_t i32(const std::string_view text) {
    std::int32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid integer: " + std::string(text));
    return value;
}

float real(const std::string_view text) {
    float value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Invalid real: " + std::string(text));
    return value;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    const auto size = input.tellg();
    if (size < 0) throw std::runtime_error("Cannot determine file size");
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (!input && !result.empty()) throw std::runtime_error("Cannot read complete file");
    return result;
}

// <map>_col.rws next to a map, matched without regard to case as the shipped
// folders spell it either way.
std::filesystem::path collision_path(const std::filesystem::path& map) {
    auto wanted = map.stem().string() + "_col" + map.extension().string();
    const auto fold = [](std::string text) {
        std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(map.parent_path().empty() ? "." : map.parent_path(), error))
        if (fold(entry.path().filename().string()) == fold(wanted)) return entry.path();
    return map.parent_path() / wanted;
}

void write_file(const std::filesystem::path& path, const std::span<const std::byte> bytes) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("Cannot write " + path.generic_string());
}

// An authoring project's mission, opened through its workspace (mission/), if it
// has one.
struct ProjectMission {
    std::optional<csf::ModProject> workspace;
    std::optional<csf::MissionEditor> editor;
};

ProjectMission open_project_mission(const csf::AuthoringProject& project) {
    ProjectMission result;
    const auto workspace = project.directory / "mission";
    if (const auto info = csf::read_mission_project_info(workspace)) {
        result.workspace = csf::ModProject::load(workspace);
        result.editor = csf::MissionEditor::open(project.package_root() / info->scene, project.package_root(),
                                                 &*result.workspace);
    }
    return result;
}

int report(const csf::OverlayReport& value) {
    for (const auto& diagnostic : value.diagnostics)
        std::cout << csf::overlay_diagnostic_kind_name(diagnostic.kind) << '\t'
                  << diagnostic.relative_path.generic_string() << '\t' << diagnostic.message << '\n';
    std::cout << (value.passed ? "validation passed" : "validation failed") << '\n';
    return value.passed ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) try {
    if (argc < 2) { usage(); return 1; }
    const std::string_view command = argv[1];
    if (command == "audit") {
        if (argc != 3) { usage(); return 1; }
        std::size_t exact{}, structurally_invalid{}, differences{};
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(
                 argv[2], std::filesystem::directory_options::skip_permission_denied, error), end;
             it != end; it.increment(error)) {
            if (error) { error.clear(); continue; }
            if (!it->is_regular_file(error)) continue;
            const auto bytes = read_bytes(it->path());
            if (!csf::Document::sniff(bytes)) continue;
            const auto document = csf::Document::from_bytes(bytes);
            if (document.has_errors()) { ++structurally_invalid; continue; }
            const auto serialized = csf::EditSession::from_document(document).serialize();
            // The canonical tree writer must also reproduce every shipped file,
            // because structural mission edits are serialized through it.
            auto tree = csf::Tree::from_document(document);
            const auto rebuilt = tree.serialize();
            // Recompilable source text must reproduce the same tree.
            tree.roots = csf::parse_source_text(csf::to_source_text(tree.roots));
            const auto recompiled = tree.serialize();
            if (serialized == bytes && rebuilt == bytes && recompiled == bytes) ++exact;
            else { ++differences; std::cout << "difference\t" << it->path().generic_string() << '\n'; }
        }
        std::cout << "exact\t" << exact << "\nstructurally-invalid\t" << structurally_invalid
                  << "\ndifferences\t" << differences << '\n';
        return differences == 0 ? 0 : 2;
    }
    if (command == "world-rebuild") {
        // Rebuilds a map's visual and collision Worlds from their own triangles
        // (new BSP, sectors and plug-ins); Clumps and scene instances are kept.
        if (argc < 4) { usage(); return 1; }
        const std::filesystem::path input = argv[2], output = argv[3];
        bool overwrite = false;
        std::optional<std::size_t> budget;  // default: visual_sector_triangles, collision 1024
        for (int i = 4; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--overwrite") overwrite = true;
            else if (option == "--max-sector-triangles" && i + 1 < argc) budget = u32(argv[++i]);
            else throw std::runtime_error("Unknown world-rebuild option: " + std::string(option));
        }
        const auto sibling_collision = [](const std::filesystem::path& map) {
            const auto wanted = map.stem().string() + "_col" + map.extension().string();
            auto lowered = wanted;
            std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (std::filesystem::is_directory(map.parent_path().empty() ? "." : map.parent_path()))
                for (const auto& entry : std::filesystem::directory_iterator(map.parent_path().empty() ? "." : map.parent_path())) {
                    auto name = entry.path().filename().string();
                    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (name == lowered) return entry.path();
                }
            return map.parent_path() / wanted;
        };
        const auto input_collision = sibling_collision(input);
        const auto output_collision = output.parent_path() / (output.stem().string() + "_col" + output.extension().string());
        if (!overwrite && (std::filesystem::exists(output) || std::filesystem::exists(output_collision)))
            throw std::runtime_error("Output exists (pass --overwrite): " + output.generic_string());
        if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
        for (const auto& [from, to] : {std::pair{input, output}, std::pair{input_collision, output_collision}}) {
            const auto bytes = read_bytes(from);
            const auto offset = rws::find_map_world(bytes);
            if (!offset) throw std::runtime_error("No World in " + from.generic_string());
            std::uint64_t end{};
            const auto parsed = rws::parse_world_model(bytes, *offset, &end);
            if (!parsed) throw std::runtime_error(from.generic_string() + ": " + parsed.error);
            const auto& source = *parsed.value;
            rws::WorldBuildOptions options;
            options.library_id = source.library_id;
            options.format = source.format;
            options.material_list = source.material_list;
            options.visual_plugins = std::ranges::any_of(source.sectors, [](const auto& sector) {
                return std::ranges::any_of(sector.plugins, [](const auto& p) { return p.type == 0x120U; });
            });
            options.max_sector_triangles =
                budget.value_or(options.visual_plugins ? rws::visual_sector_triangles : options.max_sector_triangles);
            const auto built = rws::build_world(rws::world_build_triangles(source), options);
            if (!built) throw std::runtime_error(built.error);
            if (const auto problems = rws::check_world_model(*built.value); !problems.empty())
                throw std::runtime_error("Rebuilt World is invalid: " + problems.front());
            std::vector<std::byte> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(*offset));
            const auto world = rws::write_world_model(*built.value);
            out.insert(out.end(), world.begin(), world.end());
            out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(end), bytes.end());
            const auto document = rws::Document::from_bytes(out);
            const auto recovered = rws::recover_worlds(document.chunks(), document.bytes());
            if (recovered.size() != 1U || recovered[0].status != rws::WorldRecoveryStatus::complete ||
                recovered[0].topology_status != rws::WorldTopologyStatus::complete)
                throw std::runtime_error("Rebuilt World does not recover as complete");
            std::ofstream file(to, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
            if (!file) throw std::runtime_error("Cannot write " + to.generic_string());
            std::cout << "rebuilt\t" << to.generic_string() << '\t' << built.value->triangle_count << " triangles\t"
                      << source.world_sector_count << " -> " << built.value->world_sector_count << " sectors\n";
        }
        return 0;
    }
    if (command == "project-new") {
        // A new authoring project in a shipped mission's slot (include/csf/project_pipeline.hpp).
        if (argc < 3) { usage(); return 1; }
        csf::NewProjectOptions options;
        options.directory = argv[2];
        options.corpus = "../CSF_unpacks";
        options.tool_version = "csf-mod";
        for (int i = 3; i < argc; ++i) {
            const std::string_view option = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(std::string(option) + " needs a value");
                return argv[++i];
            };
            if (option == "--slot") options.slot = value();
            else if (option == "--name") options.name = value();
            else if (option == "--corpus") options.corpus = value();
            else if (option == "--flat") options.terrain_size = std::stof(value());
            else if (option == "--no-terrain") options.terrain_size = 0;
            else if (option == "--projects-root") options.projects_root = value();
            else if (option == "--test-install") options.test_install = value();
            else throw std::runtime_error("Unknown project-new option: " + std::string(option));
        }
        if (options.slot.empty()) throw std::runtime_error("project-new needs --slot <mission>");
        if (options.name.empty()) options.name = std::filesystem::path(argv[2]).filename().string();
        const auto created = csf::create_authoring_project(options);
        for (const auto& line : created.lines)
            if (!line.starts_with("note\t")) std::cout << line << '\n';
        std::cout << "project\t" << created.project.directory.generic_string() << '\n';
        return 0;
    }
    if (command == "project-archives") {
        // Both archives of an authoring project into dist/<build-id>/.
        if (argc < 3) { usage(); return 1; }
        auto project = csf::AuthoringProject::load(argv[2]);
        std::optional<std::filesystem::path> mission, texts;
        for (int i = 3; i + 1 < argc; i += 2) {
            const std::string_view option = argv[i];
            if (option == "--original-mission") mission = argv[i + 1];
            else if (option == "--original-texts") texts = argv[i + 1];
            else throw std::runtime_error("Unknown project-archives option: " + std::string(option));
        }
        if (!mission) mission = csf::find_original_archive(project, project.slot.archive);
        if (!texts && project.texts) texts = csf::find_original_archive(project, project.texts->archive);
        if (!mission) throw std::runtime_error("Where is the untouched " + project.slot.archive.generic_string() +
                                               "? Pass --original-mission or set the test install");
        if (project.texts && !project.strings.empty() && !texts)
            throw std::runtime_error("Where is the untouched " + project.texts->archive.generic_string() +
                                     "? Pass --original-texts");
        const auto build = csf::build_archives(project, *mission, texts.value_or(""), "csf-mod");
        for (const auto& line : build.lines)
            if (!line.starts_with("note\t")) std::cout << line << '\n';
        std::cout << "build\t" << build.id << '\n';
        return 0;
    }
    if (command == "project-deploy" || command == "project-rollback") {
        if (argc < 4) { usage(); return 1; }
        auto project = csf::AuthoringProject::load(argv[2]);
        if (command == "project-rollback") {
            csf::roll_back_build(project, argv[3]);
            std::cout << "rolled back\t" << argv[3] << '\n';
            return 0;
        }
        const std::filesystem::path install = argc > 4 ? std::filesystem::path(argv[4]) : project.local.test_install;
        if (install.empty()) throw std::runtime_error("Which test install? Pass it or set test-install in local.csfproj");
        for (const auto& deployment : csf::deploy_build(project, argv[3], install))
            std::cout << "deployed\t" << deployment.archive.generic_string() << '\t' << deployment.manifest.generic_string()
                      << '\n';
        project.save();
        return 0;
    }
    if (command == "project-playtest") {
        if (argc != 6) { usage(); return 1; }
        const std::string_view result = argv[4];
        if (result != "worked" && result != "failed") throw std::runtime_error("The result is worked or failed");
        auto project = csf::AuthoringProject::load(argv[2]);
        project.playtests.push_back({argv[3], result == "worked", argv[5]});
        project.save();
        std::cout << "playtest\t" << argv[3] << '\t' << result << '\n';
        return 0;
    }
    if (command == "project-build") {
        // Builds an authoring project's World, collision and sector map and its
        // mission text into its build/ directory when they are stale, and
        // records the outputs.
        if (argc < 3) { usage(); return 1; }
        bool force = false, verbose = false;
        for (int i = 3; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--force") force = true;
            else if (option == "--verbose") verbose = true;
            else throw std::runtime_error("Unknown project-build option: " + std::string(option));
        }
        auto project = csf::AuthoringProject::load(argv[2]);
        const auto world = project.build_world(force);
        const auto texts = project.build_texts(force);
        const auto lightmaps = project.build_lightmaps(force);
        project.save();
        for (const auto* report : {&world, &texts, &lightmaps})
            for (const auto& line : report->lines)
                if (verbose || !line.starts_with("note\t")) std::cout << line << '\n';
        for (const auto& finding : project.lightmap_brightness())
            std::cout << "warning\t" << csf::lightmap_finding_text(finding) << '\n';
        return 0;
    }
    if (command == "project-heights") {
        // Placements and anchored actors whose height rules no longer match the
        // ground; --resnap stores the resolved heights (placements in the
        // project, actors in its mission workspace).
        if (argc < 3 || argc > 4) { usage(); return 1; }
        const bool resnap = argc == 4 && std::string_view(argv[3]) == "--resnap";
        if (argc == 4 && !resnap) throw std::runtime_error("Unknown project-heights option: " + std::string(argv[3]));
        auto project = csf::AuthoringProject::load(argv[2]);
        auto [mod, editor] = open_project_mission(project);
        csf::ActorPositions actors;
        if (editor)
            for (const auto& actor : editor->scene().actors())
                if (actor.id && actor.position)
                    actors[*actor.id] = {actor.position->x, actor.position->y, actor.position->z};
        const auto findings = project.height_report(actors);
        for (const auto& finding : findings) {
            std::cout << "height\t" << (finding.subject == csf::HeightFinding::Subject::actor ? "actor" : "placement")
                      << '\t' << finding.id << "\tstored " << finding.position.y << '\t';
            if (finding.resolved)
                std::cout << "resolved " << *finding.resolved << '\t' << std::showpos
                          << *finding.resolved - finding.position.y << std::noshowpos << '\n';
            else
                std::cout << finding.problem << '\n';
        }
        std::cout << findings.size() << " height findings\n";
        if (!resnap) return 0;
        project.resnap(findings);
        project.save();
        std::size_t moved = 0;
        for (const auto& finding : findings) {
            if (finding.subject != csf::HeightFinding::Subject::actor || !finding.resolved || !editor) continue;
            const auto actor = std::ranges::find_if(editor->scene().actors(), [&](const auto& a) {
                return a.id == finding.actor_id;
            });
            if (actor == editor->scene().actors().end()) continue;
            const csf::ActorPlacement placement{{actor->position->x, *finding.resolved, actor->position->z},
                                               actor->heading.value_or(0), actor->pitch.value_or(0)};
            if (!editor->set_actor_placement(finding.actor_id, placement).applied)
                throw std::runtime_error("Cannot move actor " + finding.id);
            ++moved;
        }
        if (moved) (void)editor->save(*mod);
        const auto placements = std::ranges::count_if(findings, [](const csf::HeightFinding& f) {
            return f.subject == csf::HeightFinding::Subject::placement && f.resolved;
        });
        std::cout << "resnapped\t" << placements << " placements\t" << moved << " actors\n";
        return 0;
    }
    if (command == "project-reference") {
        // What a modelling tool shows around its sources: the built map with its
        // props (world.gltf), one model per actor class (models/<class>.gltf)
        // and markers.json (placements, actors, navigation, areas, dummies).
        if (argc != 4) { usage(); return 1; }
        const auto project = csf::AuthoringProject::load(argv[2]);
        const std::filesystem::path out = argv[3];
        std::filesystem::create_directories(out / "models");
        const auto map = rws::Document::load(project.directory / "build" / project.donor_map.visual);
        const auto world = rws::export_scene_gltf(map.chunks(), map.scene_instances(), map.bytes(), out / "world.gltf");
        std::cout << "world\t" << (out / "world.gltf").generic_string() << '\t' << world.triangles << " triangles\n";
        auto [mod, editor] = open_project_mission(project);
        std::map<std::int32_t, std::string> models;
        if (editor) {
            csf::ResourceIndex package;
            package.add_root(project.package_root());
            package.build();
            const auto resources = editor->resource_index(package);
            for (const auto& association : csf::associate_actors(editor->scene(), editor->objects(), resources)) {
                if (!association.class_id || models.contains(*association.class_id) ||
                    association.visual_models.size() != 1 || !association.visual_models.front().resolved_path)
                    continue;
                try {
                    const auto model = rws::Document::load(*association.visual_models.front().resolved_path);
                    const auto name = "models/" + std::to_string(*association.class_id) + ".gltf";
                    (void)rws::export_scene_gltf(model.chunks(), model.scene_instances(), model.bytes(), out / name);
                    models[*association.class_id] = name;
                } catch (const std::exception& error) {
                    std::cout << "note\tclass " << *association.class_id << ": " << error.what() << '\n';
                }
            }
        }
        const auto markers = csf::reference_markers_json(project, editor ? &editor->scene() : nullptr, models);
        std::ofstream(out / "markers.json", std::ios::binary) << markers;
        std::cout << "models\t" << models.size() << "\nmarkers\t" << (out / "markers.json").generic_string() << '\n';
        return 0;
    }
    if (command == "project-place") {
        // Places a building asset of an authoring project (its triangles join the World).
        if (argc < 8) { usage(); return 1; }
        auto project = csf::AuthoringProject::load(argv[2]);
        csf::ProjectPlacement placement;
        placement.kind = csf::ProjectPlacement::Kind::building;
        placement.id = argv[3];
        placement.asset = argv[4];
        placement.position = {real(argv[5]), real(argv[6]), real(argv[7])};
        int i = 8;
        if (i < argc && std::string_view(argv[i]) != "--ground") placement.yaw_degrees = real(argv[i++]);
        if (i + 1 < argc && std::string_view(argv[i]) == "--ground") {
            placement.height = {csf::HeightRule::Mode::ground, real(argv[i + 1]), {}, {}};
            i += 2;
        }
        if (i != argc) { usage(); return 1; }
        if (std::ranges::any_of(project.placements, [&](const auto& p) { return p.id == placement.id; }))
            throw std::runtime_error("A placement is already named " + placement.id);
        project.placements.push_back(placement);
        if (const auto problems = project.check(); !problems.empty()) throw std::runtime_error(problems.front());
        project.save();
        std::cout << "placed\t" << placement.id << '\n';
        return 0;
    }
    if (command == "project-lightmap") {
        // Registers (or updates) a baked lightmap of an authoring project.
        if (argc != 5) { usage(); return 1; }
        auto project = csf::AuthoringProject::load(argv[2]);
        const std::string name = argv[3];
        auto lightmap = std::ranges::find(project.lightmaps, name, &csf::ProjectLightmap::name);
        if (lightmap == project.lightmaps.end()) lightmap = project.lightmaps.insert(project.lightmaps.end(), {name, {}});
        lightmap->source = argv[4];
        if (const auto problems = project.check(); !problems.empty()) throw std::runtime_error(problems.front());
        project.save();
        std::cout << "lightmap\t" << name << '\n';
        return 0;
    }
    if (command == "project-lightmaps") {
        // Lists every built lightmap and copied donor texture in the mission's
        // texture list and packages it from build/ (run project-build first).
        if (argc != 3) { usage(); return 1; }
        const auto project = csf::AuthoringProject::load(argv[2]);
        auto [mod, editor] = open_project_mission(project);
        if (!editor) throw std::runtime_error("The project has no mission workspace");
        std::vector<std::string> entries;
        for (const auto& relative : project.packaged_textures()) {
            const auto built = project.directory / "build" / relative;
            if (!std::filesystem::is_regular_file(built))
                throw std::runtime_error(built.generic_string() + " is not built; run project-build first");
            csf::ModFile file;
            file.relative_path = relative;
            file.authored_path = std::filesystem::absolute(built);
            file.output_sha256 = csf::sha256(read_bytes(built));
            if (const auto source = mod->source_root / relative; std::filesystem::is_regular_file(source))
                file.source_sha256 = csf::sha256(read_bytes(source));
            mod->add_file(std::move(file));
            auto entry = relative.generic_string();
            std::ranges::replace(entry, '/', '\\');
            entries.push_back(entry);
        }
        mod->save();
        const auto result = editor->add_texture_list_entries(entries);
        std::cout << result.message << '\n';
        if (result.applied) (void)editor->save(*mod);
        return 0;
    }
    if (command == "project-asset") {
        // Registers (or updates) a Blender asset of an authoring project.
        if (argc != 7) { usage(); return 1; }
        auto project = csf::AuthoringProject::load(argv[2]);
        const std::string id = argv[3], kind = argv[4];
        if (kind != "terrain" && kind != "building") throw std::runtime_error("Asset kind must be terrain or building");
        auto asset = std::ranges::find(project.assets, id, &csf::ProjectAsset::id);
        if (asset == project.assets.end()) asset = project.assets.insert(project.assets.end(), csf::ProjectAsset{});
        asset->id = id;
        asset->kind = kind == "terrain" ? csf::ProjectAsset::Kind::terrain : csf::ProjectAsset::Kind::building;
        asset->blend = argv[5];
        asset->export_path = argv[6];
        if (const auto problems = project.check(); !problems.empty()) throw std::runtime_error(problems.front());
        project.save();
        std::cout << "asset\t" << id << '\t' << kind << '\n';
        return 0;
    }
    if (command == "sector-build" || command == "world-ground") {
        if (argc < 4) { usage(); return 1; }
        const std::filesystem::path source_path = argv[2];
        std::ifstream input(source_path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read " + source_path.generic_string());
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto source = rws::parse_world_source(text);
        if (!source) throw std::runtime_error(source_path.generic_string() + ": " + source.error);
        if (command == "sector-build") {
            // The sector map of the source's collision faces (props excluded).
            const std::filesystem::path output = argv[3];
            const bool overwrite = argc > 4 && std::string_view(argv[4]) == "--overwrite";
            if (argc > 4 && !overwrite) throw std::runtime_error("Unknown sector-build option: " + std::string(argv[4]));
            if (!overwrite && std::filesystem::exists(output))
                throw std::runtime_error("Output exists (pass --overwrite): " + output.generic_string());
            const auto map = rws::build_sector_map(*source.value);
            std::ofstream file(output, std::ios::binary);
            file.write(reinterpret_cast<const char*>(map.bytes.data()), static_cast<std::streamsize>(map.bytes.size()));
            if (!file) throw std::runtime_error("Cannot write " + output.generic_string());
            std::cout << "sec\t" << output.generic_string() << '\t' << map.vertex_count << " vertices\t"
                      << map.sector_count << " sectors\n";
            return 0;
        }
        // Ground height and normal under each (x, z) of the source's collision faces.
        if ((argc - 3) % 2 != 0) { usage(); return 1; }
        const rws::GroundQuery ground(*source.value);
        for (int i = 3; i + 1 < argc; i += 2) {
            const auto x = std::stof(argv[i]), z = std::stof(argv[i + 1]);
            std::cout << "ground\t" << x << '\t' << z << '\t';
            if (const auto hit = ground.highest(x, z))
                std::cout << hit->height << '\t' << hit->normal.x << ' ' << hit->normal.y << ' ' << hit->normal.z << '\n';
            else
                std::cout << "-\n";
        }
        return 0;
    }
    if (command == "world-build") {
        // Compiles a .csfworld into <new-map>.rws and <new-map>_col.rws, with
        // materials copied from the donor map and its _col.rws.
        if (argc < 5) { usage(); return 1; }
        const std::filesystem::path source_path = argv[2], donor_path = argv[3], output = argv[4];
        bool keep_props = false, overwrite = false;
        rws::WorldCompileOptions options;
        const auto donor_collision = collision_path(donor_path);
        const auto output_collision =
            output.parent_path() / (output.stem().string() + "_col" + output.extension().string());
        // Textures and lightmaps of the map's own: DDS files in <output dir>/Textures/,
        // listed in a copy of the donor's texture list (<map folder>.txl).
        struct OwnImage {
            std::string name;
            std::filesystem::path image;
        };
        std::vector<OwnImage> images;
        for (int i = 5; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--keep-props") keep_props = true;
            else if (option == "--overwrite") overwrite = true;
            else if (option == "--max-sector-triangles" && i + 1 < argc)
                options.max_sector_triangles = options.max_visual_sector_triangles = u32(argv[++i]);
            else if (option == "--texture" && i + 3 < argc) {
                std::string name = argv[++i];
                images.push_back({name, argv[++i]});
                std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                options.new_textures[name] = argv[++i];
            } else if (option == "--lightmap" && i + 2 < argc) {
                images.push_back({argv[i + 1], argv[i + 2]});
                i += 2;
            } else throw std::runtime_error("Unknown world-build option: " + std::string(option));
        }
        const auto donor_txl = [&] {
            auto path = donor_path;
            path.replace_extension(".txl");
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(donor_path.parent_path().empty() ? "." : donor_path.parent_path(), error)) {
                auto name = entry.path().filename().string(), wanted = path.filename().string();
                for (auto* text : {&name, &wanted})
                    std::ranges::transform(*text, text->begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == wanted) return entry.path();
            }
            return path;
        }();
        const auto output_txl = output.parent_path() / donor_txl.filename();
        std::vector<std::filesystem::path> outputs{output, output_collision};
        for (const auto& image : images) outputs.push_back(output.parent_path() / "Textures" / (image.name + ".dds"));
        if (!images.empty()) outputs.push_back(output_txl);
        if (!overwrite)
            for (const auto& path : outputs)
                if (std::filesystem::exists(path))
                    throw std::runtime_error("Output exists (pass --overwrite): " + path.generic_string());
        std::ifstream input(source_path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read " + source_path.generic_string());
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto source = rws::parse_world_source(text);
        if (!source) throw std::runtime_error(source_path.generic_string() + ": " + source.error);
        // Read the texture list before writing anything: the output may replace it.
        std::string txl;
        if (!images.empty()) {
            if (!std::filesystem::is_regular_file(donor_txl))
                throw std::runtime_error("Own textures need the donor's texture list: " + donor_txl.generic_string() +
                                         " is missing");
            const auto bytes = read_bytes(donor_txl);
            txl.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        const auto built = rws::build_map_files(*source.value, read_bytes(donor_path), read_bytes(donor_collision),
                                                options, keep_props);
        if (!built) throw std::runtime_error(built.error);
        std::vector<std::pair<std::filesystem::path, std::vector<std::byte>>> dds;
        for (const auto& image : images) {
            auto source_bytes = read_bytes(image.image);
            auto extension = image.image.extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension != ".dds") {
                int width{}, height{};
                std::vector<std::uint8_t> rgba;
                std::string problem;
                if (!rws::decode_png(source_bytes, width, height, rgba, problem))
                    throw std::runtime_error(image.image.generic_string() + ": " + problem);
                if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0)
                    throw std::runtime_error(image.image.generic_string() + ": width and height must be powers of two");
                source_bytes = rws::encode_dds_dxt1(width, height, rgba);
            }
            dds.emplace_back(output.parent_path() / "Textures" / (image.name + ".dds"), std::move(source_bytes));
        }
        write_file(output, built.value->map);
        write_file(output_collision, built.value->collision);
        for (const auto& [path, bytes] : dds) {
            write_file(path, bytes);
            std::cout << "texture\t" << path.generic_string() << '\n';
        }
        if (!images.empty()) {
            // Entries are game paths: Maps\<map folder>\Textures\<name>.dds.
            const auto folder = std::filesystem::absolute(donor_path).parent_path().filename().string();
            auto present = txl;
            std::ranges::transform(present, present.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const auto& image : images) {
                const auto entry = "Maps\\" + folder + "\\Textures\\" + image.name + ".dds";
                auto folded = entry;
                std::ranges::transform(folded, folded.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (present.find(folded) != std::string::npos) continue;
                if (!txl.empty() && txl.back() != '\n') txl += "\r\n";
                txl += entry + "\r\n";
            }
            write_file(output_txl, std::as_bytes(std::span(txl)));
            std::cout << "texture-list\t" << output_txl.generic_string() << '\n';
        }
        for (const auto& note : built.value->notes) std::cout << "note\t" << note << '\n';
        std::cout << "visual\t" << output.generic_string() << '\t' << built.value->visual_triangles << " triangles\t"
                  << built.value->visual_sectors << " sectors\n"
                  << "collision\t" << output_collision.generic_string() << '\t' << built.value->collision_triangles
                  << " triangles\t" << built.value->collision_sectors << " sectors\n";
        return 0;
    }
    if (command == "world-source") {
        // Decompiles a map and its _col.rws into .csfworld, the source that
        // Blender imports and world-build (with the map as donor) compiles.
        if (argc < 4) { usage(); return 1; }
        const std::filesystem::path map_path = argv[2], output = argv[3];
        bool check = false, overwrite = false;
        for (int i = 4; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--check") check = true;
            else if (option == "--overwrite") overwrite = true;
            else throw std::runtime_error("Unknown world-source option: " + std::string(option));
        }
        if (!overwrite && std::filesystem::exists(output))
            throw std::runtime_error("Output exists (pass --overwrite): " + output.generic_string());
        const auto collision_file = collision_path(map_path);
        const auto map_bytes = read_bytes(map_path), collision_bytes = read_bytes(collision_file);
        const auto load = [](const std::vector<std::byte>& bytes, const std::filesystem::path& path) {
            const auto offset = rws::find_map_world(bytes);
            if (!offset) throw std::runtime_error(path.generic_string() + " has no World");
            auto parsed = rws::parse_world_model(bytes, *offset);
            if (!parsed) throw std::runtime_error(path.generic_string() + ": " + parsed.error);
            return std::move(*parsed.value);
        };
        const auto visual = load(map_bytes, map_path), collision = load(collision_bytes, collision_file);
        const auto source = rws::world_source_from_map(visual, collision);
        if (!source) throw std::runtime_error(map_path.generic_string() + ": " + source.error);
        // The map's path in UTF-8 (the names below are the game's own bytes).
        const auto map_name = std::filesystem::absolute(map_path).generic_u8string();
        const auto text = "# world-source " + std::string(reinterpret_cast<const char*>(map_name.data()), map_name.size()) +
                          "\n" + rws::write_world_source(*source.value);
        // The header must stay the first line.
        const auto header = text.find("csfworld 1\n");
        const auto ordered = text.substr(header, 11) + text.substr(0, header) + text.substr(header + 11);
        write_file(output, std::as_bytes(std::span(ordered)));
        std::size_t visual_faces{}, collision_faces{};
        for (const auto& face : source.value->faces) (face.visual ? visual_faces : collision_faces)++;
        std::cout << "source\t" << output.generic_string() << '\t' << visual_faces << " visual faces\t"
                  << collision_faces << " collision faces\t" << source.value->materials.size() << " materials\n";
        if (!check) return 0;
        // --check: built again with the map as donor, every triangle must come
        // back with the same corners, UVs and material chunk (collision: and Pyro byte).
        const auto reparsed = rws::parse_world_source(ordered);
        if (!reparsed) throw std::runtime_error("written source: " + reparsed.error);
        const auto compiled = rws::compile_world_source(*reparsed.value, visual, collision);
        if (!compiled) throw std::runtime_error("build: " + compiled.error);
        const auto signatures = [](const rws::WorldModel& world, const bool with_uv) {
            const auto list = rws::split_material_list(world.material_list, world.library_id);
            std::multiset<std::string> result;
            for (const auto& triangle : rws::world_build_triangles(world)) {
                std::array<std::string, 3> corners;
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto& v = triangle.vertices[c];
                    std::array<float, 7> values{v.position.x, v.position.y, v.position.z, 0, 0, 0, 0};
                    if (with_uv) {
                        values[3] = v.texcoords[0][0];
                        values[4] = v.texcoords[0][1];
                        values[5] = v.texcoords[1][0];
                        values[6] = v.texcoords[1][1];
                    }
                    // Text keeps a NaN (some shipped UVs) but not its payload.
                    for (auto& value : values)
                        if (std::isnan(value)) value = std::numeric_limits<float>::quiet_NaN();
                    corners[c].assign(reinterpret_cast<const char*>(values.data()), sizeof(values));
                }
                // The same winding from its smallest corner.
                const auto first = static_cast<std::size_t>(std::ranges::min_element(corners) - corners.begin());
                std::string key;
                for (std::size_t c = 0; c < 3; ++c) key += corners[(first + c) % 3];
                if (list && triangle.material < list.value->size()) {
                    const auto& chunk = (*list.value)[triangle.material];
                    key.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
                }
                key += static_cast<char>(with_uv ? 0 : triangle.pyro);
                result.insert(std::move(key));
            }
            return result;
        };
        const auto kept = [](const std::multiset<std::string>& before, const std::multiset<std::string>& after) {
            std::vector<std::string> common;
            std::ranges::set_intersection(before, after, std::back_inserter(common));
            return common.size();
        };
        const auto visual_before = signatures(visual, true), visual_after = signatures(compiled.value->visual, true);
        const auto collision_before = signatures(collision, false),
                   collision_after = signatures(compiled.value->collision, false);
        const auto visual_kept = kept(visual_before, visual_after), collision_kept = kept(collision_before, collision_after);
        std::cout << "check\tvisual\t" << visual_kept << " of " << visual_before.size() << " triangles unchanged\n"
                  << "check\tcollision\t" << collision_kept << " of " << collision_before.size()
                  << " triangles unchanged\n";
        return visual_kept == visual_before.size() && visual_after.size() == visual_before.size() &&
                       collision_kept == collision_before.size() && collision_after.size() == collision_before.size()
                   ? 0
                   : 2;
    }
    if (command == "world-audit") {
        // Every World in a map .rws must survive parse -> write byte for byte,
        // with header counts equal to the parts, before the writer builds new maps.
        if (argc < 3) { usage(); return 1; }
        std::vector<std::filesystem::path> files;
        bool rebuild = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--rebuild") { rebuild = true; continue; }
            if (std::filesystem::is_regular_file(argv[i])) { files.emplace_back(argv[i]); continue; }
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator it(
                     argv[i], std::filesystem::directory_options::skip_permission_denied, error), end;
                 it != end; it.increment(error)) {
                if (error) { error.clear(); continue; }
                auto extension = it->path().extension().string();
                std::ranges::transform(extension, extension.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (it->is_regular_file(error) && extension == ".rws") files.push_back(it->path());
            }
        }
        std::ranges::sort(files);
        std::size_t exact{}, failures{};
        for (const auto& path : files) {
            const auto bytes = read_bytes(path);
            const auto world = rws::find_map_world(bytes);
            if (!world) continue;
            const auto offset = *world;
            std::uint64_t end{};
            const auto parsed = rws::parse_world_model(bytes, offset, &end);
            std::string problem;
            if (!parsed) problem = parsed.error;
            else {
                auto counted = *parsed.value;
                rws::update_world_counts(counted);
                const auto written = rws::write_world_model(
                    *parsed.value, {.reproduce_pyro_size_overstatement = true});
                const auto problems = rws::check_world_model(*parsed.value);
                if (!problems.empty()) problem = "model check: " + problems.front();
                else if (counted.triangle_count != parsed.value->triangle_count ||
                         counted.vertex_count != parsed.value->vertex_count ||
                         counted.plane_sector_count != parsed.value->plane_sector_count ||
                         counted.world_sector_count != parsed.value->world_sector_count)
                    problem = "header counts differ from the parts";
                else if (written.size() != end - offset ||
                         !std::equal(written.begin(), written.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)))
                    problem = "rewritten World differs";
                // --rebuild: a new BSP over the same triangles must recover as
                // complete through the read-only recovery used by the viewer.
                if (problem.empty() && rebuild) {
                    const auto& source = *parsed.value;
                    const auto triangles = rws::world_build_triangles(source);
                    rws::WorldBuildOptions options;
                    options.library_id = source.library_id;
                    options.format = source.format;
                    options.material_list = source.material_list;
                    options.visual_plugins = std::ranges::any_of(source.sectors, [](const auto& sector) {
                        return std::ranges::any_of(sector.plugins, [](const auto& p) { return p.type == 0x120U; });
                    });
                    const auto built = rws::build_world(triangles, options);
                    const auto document = rws::Document::from_bytes(rws::write_world_model(*built.value));
                    const auto recovered = rws::recover_worlds(document.chunks(), document.bytes());
                    const auto checks = rws::check_world_model(*built.value);
                    if (!checks.empty()) problem = "rebuilt model check: " + checks.front();
                    else if (recovered.size() != 1U) problem = "rebuilt World not found";
                    else if (recovered[0].status != rws::WorldRecoveryStatus::complete ||
                             recovered[0].topology_status != rws::WorldTopologyStatus::complete ||
                             recovered[0].recovered_triangles != static_cast<std::int64_t>(triangles.size()))
                        problem = std::string("rebuilt World recovers as ") +
                                  rws::world_recovery_status_name(recovered[0].status) + "/" +
                                  rws::world_topology_status_name(recovered[0].topology_status) +
                                  (recovered[0].diagnostics.empty() ? "" : ": " + recovered[0].diagnostics.front()) +
                                  (recovered[0].topology_diagnostics.empty() ? "" : " / " + recovered[0].topology_diagnostics.front());
                    else if (!document.diagnostics().empty())
                        problem = "rebuilt World has chunk diagnostics: " + document.diagnostics().front().message;
                    else
                        std::cout << "rebuilt\t" << path.generic_string() << '\t' << built.value->sectors.size()
                                  << " sectors\t" << triangles.size() << " triangles\n";
                }
            }
            // Scene-instance records must re-encode byte for byte too.
            if (problem.empty()) {
                const auto map = rws::Document::from_bytes(bytes);
                for (const auto& instance : map.scene_instances()) {
                    const auto encoded = rws::encode_scene_instance(instance, parsed.value->library_id);
                    if (instance.offset + encoded.size() > bytes.size() || encoded.size() != instance.physical_size ||
                        !std::equal(encoded.begin(), encoded.end(), bytes.begin() + static_cast<std::ptrdiff_t>(instance.offset))) {
                        problem = "scene instance " + std::to_string(instance.instance_id) + " re-encodes differently";
                        break;
                    }
                }
                if (problem.empty() && !map.scene_instances().empty())
                    std::cout << "instances\t" << path.generic_string() << '\t' << map.scene_instances().size() << " exact\n";
            }
            if (problem.empty()) {
                ++exact;
                std::cout << "exact\t" << path.generic_string() << '\t' << parsed.value->sectors.size()
                          << " sectors\t" << parsed.value->planes.size() << " planes\n";
            } else {
                ++failures;
                std::cout << "failed\t" << path.generic_string() << '\t' << problem << '\n';
            }
        }
        std::cout << "exact\t" << exact << "\nfailed\t" << failures << '\n';
        return failures == 0 ? 0 : 2;
    }
    if (command == "edit") {
        if (argc < 5) { usage(); return 1; }
        auto session = csf::EditSession::load(argv[2]);
        const auto supported = csf::authorable_fields(session.document());
        for (int i = 4; i < argc;) {
            const std::string_view operation = argv[i++];
            if (i + 1 >= argc) throw std::runtime_error("Edit operation requires entry and value");
            const auto entry = u32(argv[i++]);
            const auto policy = std::ranges::find_if(supported, [&](const csf::EditableField& field) {
                return field.entry_index == entry;
            });
            if (policy == supported.end())
                throw std::runtime_error("Entry is not in the reviewed authoring field set");
            csf::EditValidation validation;
            if (operation == "--int") {
                const auto value = i32(argv[i++]);
                if (policy->category == csf::AuthoringCategory::script_flag && value != 0 && value != 1)
                    throw std::runtime_error("Script flags accept only 0 or 1");
                validation = session.set_integer(entry, value, policy->meaning);
            }
            else if (operation == "--real") validation = session.set_real(entry, real(argv[i++]), policy->meaning);
            else if (operation == "--string") validation = session.set_string(entry, argv[i++], false, policy->meaning);
            else if (operation == "--global-string") validation = session.set_string(entry, argv[i++], true, policy->meaning);
            else throw std::runtime_error("Unknown edit operation: " + std::string(operation));
            if (!validation.valid || validation.blocking) throw std::runtime_error(validation.message);
        }
        const auto saved = session.save_copy(argv[3]);
        std::cout << "saved\t" << saved.output_path.generic_string() << "\nmanifest\t"
                  << saved.manifest_path.generic_string() << "\nsha256\t" << saved.output_sha256 << '\n';
        return 0;
    }
    if (command == "init") {
        if (argc != 5) { usage(); return 1; }
        const auto project = csf::ModProject::create(argv[2], argv[3], argv[4], "0.1.0");
        std::cout << "created\t" << project.workspace_root.generic_string() << '\n';
        return 0;
    }
    if (command == "add") {
        if (argc < 5) { usage(); return 1; }
        auto project = csf::ModProject::load(argv[2]);
        csf::ModFile file;
        file.relative_path = argv[3];
        file.authored_path = std::filesystem::absolute(argv[4]);
        int i = 5;
        if (i < argc && std::string_view(argv[i]) != "--target")
            file.change_manifest = std::filesystem::absolute(argv[i++]);
        while (i < argc) {
            if (std::string_view(argv[i++]) != "--target" || i >= argc)
                throw std::runtime_error("Expected --target <semantic-target>");
            file.semantic_targets.emplace_back(argv[i++]);
        }
        const auto source = project.source_root / file.relative_path;
        if (std::filesystem::is_regular_file(source)) file.source_sha256 = csf::sha256(read_bytes(source));
        file.output_sha256 = csf::sha256(read_bytes(file.authored_path));
        project.add_file(std::move(file));
        project.save();
        std::cout << "added\t" << argv[3] << '\n';
        return 0;
    }
    if (command == "validate") {
        if (argc != 3) { usage(); return 1; }
        return report(csf::ModProject::load(argv[2]).validate());
    }
    if (command == "build") {
        if (argc != 4) { usage(); return 1; }
        return report(csf::ModProject::load(argv[2]).build(argv[3]));
    }
    if (command == "package") {
        if (argc < 5) { usage(); return 1; }
        csf::PakOptions options;
        for (int i = 5; i < argc;) {
            const std::string_view option = argv[i++];
            if (option == "--overwrite") options.overwrite = true;
            else {
                if (i >= argc) throw std::runtime_error(std::string(option) + " requires a value");
                if (option == "--pakman") options.pakman_cli = argv[i++];
                else if (option == "--type") options.type = argv[i++];
                else if (option == "--platform") options.platform = argv[i++];
                else throw std::runtime_error("Unknown package option: " + std::string(option));
            }
        }
        const auto result = csf::ModProject::load(argv[2]).package(argv[3], argv[4], options);
        std::cout << "archive\t" << result.archive_path.generic_string()
                  << "\nmanifest\t" << result.manifest_path.generic_string()
                  << "\nsha256\t" << result.archive_sha256 << '\n';
        return 0;
    }
    if (command == "conflicts") {
        if (argc != 4) { usage(); return 1; }
        const auto conflicts = csf::ModProject::conflicts(csf::ModProject::load(argv[2]),
                                                          csf::ModProject::load(argv[3]));
        for (const auto& conflict : conflicts)
            std::cout << conflict.relative_path.generic_string() << '\t' << conflict.reason << '\n';
        return conflicts.empty() ? 0 : 3;
    }
    if (command == "deploy-pak") {
        if (argc < 6) { usage(); return 1; }
        bool apply{};
        csf::PakOptions options;
        for (int i = 6; i < argc;) {
            const std::string_view option = argv[i++];
            if (option == "--apply") apply = true;
            else if (option == "--pakman") {
                if (i >= argc) throw std::runtime_error("--pakman requires a value");
                options.pakman_cli = argv[i++];
            } else throw std::runtime_error("Unknown deploy-pak option: " + std::string(option));
        }
        const auto result = csf::ModProject::load(argv[2]).deploy_package(
            argv[3], argv[4], argv[5], options, !apply);
        for (const auto& file : result.files)
            std::cout << (apply ? "deploy-pak" : "would-deploy-pak") << '\t'
                      << file.relative_path.generic_string() << '\n';
        if (apply) std::cout << "rollback-manifest\t" << result.manifest_path.generic_string() << '\n';
        return 0;
    }
    if (command == "deploy-loose") {
        if (argc != 5 && argc != 6) { usage(); return 1; }
        const bool apply = argc == 6 && std::string_view(argv[5]) == "--apply";
        if (argc == 6 && !apply) throw std::runtime_error("Only --apply confirms deployment");
        const auto result = csf::ModProject::load(argv[2]).deploy(argv[3], argv[4], !apply);
        for (const auto& file : result.files)
            std::cout << (apply ? "deploy" : "would-deploy") << '\t'
                      << file.relative_path.generic_string() << '\n';
        if (apply) std::cout << "rollback-manifest\t" << result.manifest_path.generic_string() << '\n';
        return 0;
    }
    if (command == "decompile") {
        if (argc != 3 && argc != 4) { usage(); return 1; }
        const auto tree = csf::Tree::from_document(csf::Document::load(argv[2]));
        const auto text = "# Recompilable CSFFBS source text (csf-mod compile)\n" +
                          csf::to_source_text(tree.roots);
        if (argc == 3) {
            std::cout << text;
            return 0;
        }
        if (std::filesystem::exists(argv[3])) throw std::runtime_error("Refusing to overwrite an existing file");
        std::ofstream output(argv[3], std::ios::binary);
        output << text;
        if (!output) throw std::runtime_error("Cannot write text output");
        std::cout << "wrote\t" << argv[3] << '\n';
        return 0;
    }
    if (command == "compile") {
        if (argc != 5) { usage(); return 1; }
        const auto source_bytes = read_bytes(argv[2]);
        auto tree = csf::Tree::from_document(csf::Document::load(argv[3]));
        tree.roots = csf::parse_source_text(
            std::string_view(reinterpret_cast<const char*>(source_bytes.data()), source_bytes.size()));
        const std::filesystem::path output = argv[4];
        if (std::filesystem::exists(output)) throw std::runtime_error("Refusing to overwrite an existing file");
        const auto bytes = tree.serialize();
        if (csf::Document::from_bytes(bytes).has_errors())
            throw std::runtime_error("Compiled output does not reparse cleanly");
        std::ofstream file(output, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("Cannot write compiled output");
        std::cout << "compiled\t" << output.generic_string() << "\nsha256\t" << csf::sha256(bytes) << '\n';
        return 0;
    }
    if (command == "mission-flow") {
        // How the mission's scripts connect: events, objectives and findings.
        if (argc < 3) { usage(); return 1; }
        const std::filesystem::path scene = argv[2];
        std::filesystem::path package, workspace;
        for (int i = 3; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--package" && i + 1 < argc) package = argv[++i];
            else if (option == "--workspace" && i + 1 < argc) workspace = argv[++i];
            else throw std::runtime_error("Unknown mission-flow option: " + std::string(option));
        }
        if (package.empty())
            for (auto dir = std::filesystem::weakly_canonical(scene).parent_path(); !dir.empty(); dir = dir.parent_path()) {
                if (std::filesystem::is_directory(dir / "BDD")) {
                    package = dir;
                    break;
                }
                if (dir == dir.parent_path()) break;
            }
        std::optional<csf::ModProject> project;
        if (!workspace.empty()) project = csf::ModProject::load(workspace);
        const auto editor = csf::MissionEditor::open(scene, package, project ? &*project : nullptr);
        const auto program = [&](const csf::MissionFileKind kind) -> std::optional<csf::ProgramDocument> {
            if (const auto file = editor.file_of_kind(kind)) return csf::ProgramDocument::project(editor.document(*file));
            return std::nullopt;
        };
        const auto mission = program(csf::MissionFileKind::mission_script);
        const auto cutscene = program(csf::MissionFileKind::cutscene_script);
        const auto flow = csf::MissionFlow::build(mission ? &*mission : nullptr, cutscene ? &*cutscene : nullptr);
        for (const auto& e : flow.events()) {
            std::cout << "event\t" << e.name << '\t' << (e.builtin ? "engine" : "mission") << "\tlisteners";
            for (const auto id : e.listeners) std::cout << ' ' << id;
            std::cout << "\tsenders";
            for (const auto id : e.senders) std::cout << ' ' << id;
            std::cout << '\n';
        }
        for (const auto& o : flow.objectives()) {
            std::cout << "objective\t" << o.number << '\t'
                      << (o.secondary ? (*o.secondary ? "secondary" : "primary") : "unset") << "\tlabel " << o.label
                      << "\tset-by";
            for (const auto id : o.defined_by) std::cout << ' ' << id;
            std::cout << "\tcompleted-by";
            for (const auto id : o.completed_by) std::cout << ' ' << id;
            std::cout << '\n';
        }
        for (const auto& f : flow.findings())
            std::cout << csf::flow_severity_name(f.severity) << '\t' << (f.script ? std::to_string(*f.script) : "-")
                      << '\t' << f.message << '\n';
        return 0;
    }
    if (command == "mission-ops") {
        // Runs a mission operations file (include/csf/mission_ops.hpp) and saves
        // the result into the workspace; nothing is saved when a line fails.
        if (argc < 5) { usage(); return 1; }
        const std::filesystem::path workspace = argv[2], scene = argv[3], ops = argv[4];
        std::filesystem::path package, ground_source;
        bool components = false;
        for (int i = 5; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--package" && i + 1 < argc) package = argv[++i];
            else if (option == "--ground" && i + 1 < argc) ground_source = argv[++i];
            else if (option == "--components") components = true;
            else throw std::runtime_error("Unknown mission-ops option: " + std::string(option));
        }
        if (package.empty())
            for (auto dir = std::filesystem::weakly_canonical(scene).parent_path(); !dir.empty(); dir = dir.parent_path()) {
                if (std::filesystem::is_directory(dir / "BDD")) {
                    package = dir;
                    break;
                }
                if (dir == dir.parent_path()) break;
            }
        if (package.empty()) throw std::runtime_error("Cannot find the mission root; pass --package");
        auto project = std::filesystem::is_regular_file(workspace / ".csf-mod-state")
                           ? csf::ModProject::load(workspace)
                           : csf::ModProject::create(workspace, package, scene.stem().string(), "0.1.0");
        auto editor = csf::MissionEditor::open(scene, package, &project);
        if (!csf::read_mission_project_info(workspace))
            csf::write_mission_project_info(workspace, {editor.files()[editor.scene_file()].relative_path, {}});
        csf::MissionOpsOptions options;
        options.base_directory = std::filesystem::absolute(ops).parent_path();
        std::optional<rws::GroundQuery> ground;
        if (!ground_source.empty()) {
            const auto bytes = read_bytes(ground_source);
            const auto source = rws::parse_world_source({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            if (!source) throw std::runtime_error(ground_source.generic_string() + ": " + source.error);
            ground.emplace(*source.value);
            options.ground = [&ground](const float x, const float z) -> std::optional<float> {
                if (const auto hit = ground->highest(x, z)) return hit->height;
                return std::nullopt;
            };
        }
        const auto bytes = read_bytes(ops);
        const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        const auto outcomes = components ? csf::run_component_ops(editor, text, options)
                                         : csf::run_mission_ops(editor, text, options);
        for (const auto& outcome : outcomes) {
            std::cout << (outcome.result.applied ? "applied\t" : "failed\t") << ops.filename().generic_string() << ':'
                      << outcome.line << '\t' << outcome.op << '\t' << outcome.result.message << '\n';
            for (const auto& warning : outcome.result.warnings) std::cout << "warning\t" << outcome.line << '\t' << warning << '\n';
        }
        if (!outcomes.empty() && !outcomes.back().result.applied) return 2;
        for (const auto& path : editor.save(project)) std::cout << "wrote\t" << path.generic_string() << '\n';
        return 0;
    }
    if (command == "mission-components") {
        // Lists, checks and edits the components of a workspace's mission
        // (include/csf/mission_components.hpp); edits are saved.
        if (argc < 5) { usage(); return 1; }
        const std::filesystem::path workspace = argv[2], scene = argv[3];
        int i = 4;
        std::filesystem::path package, ground_source;
        for (; i + 1 < argc; i += 2) {
            const std::string_view option = argv[i];
            if (option == "--package") package = argv[i + 1];
            else if (option == "--ground") ground_source = argv[i + 1];  // walk grids' heights, as mission-ops
            else break;
        }
        if (package.empty())
            for (auto dir = std::filesystem::weakly_canonical(scene).parent_path(); !dir.empty(); dir = dir.parent_path()) {
                if (std::filesystem::is_directory(dir / "BDD")) {
                    package = dir;
                    break;
                }
                if (dir == dir.parent_path()) break;
            }
        if (package.empty()) throw std::runtime_error("Cannot find the mission root; pass --package");
        if (i >= argc) { usage(); return 1; }
        csf::MissionOpsOptions options;
        std::optional<rws::GroundQuery> ground;
        if (!ground_source.empty()) {
            const auto bytes = read_bytes(ground_source);
            const auto source = rws::parse_world_source({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            if (!source) throw std::runtime_error(ground_source.generic_string() + ": " + source.error);
            ground.emplace(*source.value);
            options.ground = [&ground](const float x, const float z) -> std::optional<float> {
                if (const auto hit = ground->highest(x, z)) return hit->height;
                return std::nullopt;
            };
        }
        auto project = csf::ModProject::load(workspace);
        auto editor = csf::MissionEditor::open(scene, package, &project);
        const std::string_view action = argv[i++];
        std::string error;
        const auto components = csf::mission_components(editor, &error);
        if (!error.empty()) throw std::runtime_error("components.csfops: " + error);
        const auto id_argument = [&] {
            if (i >= argc) throw std::runtime_error("Missing the component ID");
            return std::stoi(argv[i++]);
        };
        csf::EditResult result;
        if (action == "list") {
            for (const auto& component : components) {
                std::cout << component.id << '\t' << csf::component_title(component) << '\t'
                          << (csf::component_state(editor, component) == csf::ComponentState::clean ? "clean" : "modified");
                for (const auto& record : component.owns)
                    std::cout << '\t' << csf::record_type_name(record.type) << ':' << record.id;
                std::cout << '\n';
            }
            return 0;
        } else if (action == "check") {
            int status = 0;
            for (const auto& component : components)
                if (csf::component_state(editor, component) != csf::ComponentState::clean) {
                    std::cout << "modified\t" << component.id << '\t' << csf::component_title(component) << '\n';
                    status = 3;
                }
            for (const auto id : csf::components_that_drift(editor, options)) {
                std::cout << "drifts\t" << id << '\n';
                status = 3;
            }
            if (!status) std::cout << components.size() << " components regenerate identically\n";
            return status;
        } else if (action == "set") {
            const auto id = id_argument();
            if (i >= argc) throw std::runtime_error("Missing the line number (1 = the component's first line)");
            const auto line_number = static_cast<std::size_t>(std::stoul(argv[i++]));
            const auto found = std::ranges::find(components, id, &csf::MissionComponent::id);
            if (found == components.end()) throw std::runtime_error("No component " + std::to_string(id));
            auto lines = found->lines;
            if (line_number < 1 || line_number > lines.size()) throw std::runtime_error("No such line");
            auto line = csf::parse_op_line(lines[line_number - 1]);
            for (; i < argc; ++i) {
                const std::string_view pair = argv[i];
                const auto equals = pair.find('=');
                if (equals == std::string_view::npos) throw std::runtime_error("Expected key=value");
                line.set(pair.substr(0, equals), std::string(pair.substr(equals + 1)));
            }
            lines[line_number - 1] = csf::format_op_line(line);
            std::string text;
            for (const auto& value : lines) text += value + '\n';
            result = csf::update_component(editor, id, text, options);
        } else if (action == "regenerate") {
            result = csf::regenerate_component(editor, id_argument(), options);
        } else if (action == "detach") {
            result = csf::detach_component(editor, id_argument());
        } else if (action == "delete") {
            result = csf::delete_component(editor, id_argument());
        } else {
            usage();
            return 1;
        }
        std::cout << (result.applied ? "applied\t" : "failed\t") << result.message << '\n';
        for (const auto& warning : result.warnings) std::cout << "warning\t" << warning << '\n';
        if (!result.applied) return 2;
        for (const auto& path : editor.save(project)) std::cout << "wrote\t" << path.generic_string() << '\n';
        return 0;
    }
    if (command == "mission-edit") {
        if (argc < 4) { usage(); return 1; }
        const std::filesystem::path workspace = argv[2];
        const std::filesystem::path scene = argv[3];
        int i = 4;
        std::filesystem::path package;
        if (i + 1 < argc && std::string_view(argv[i]) == "--package") {
            package = argv[i + 1];
            i += 2;
        }
        if (package.empty())
            for (auto dir = std::filesystem::weakly_canonical(scene).parent_path(); !dir.empty();
                 dir = dir.parent_path()) {
                if (std::filesystem::is_directory(dir / "BDD")) {
                    package = dir;
                    break;
                }
                if (dir == dir.parent_path()) break;
            }
        if (package.empty()) throw std::runtime_error("Cannot find the mission root; pass --package");
        auto project = std::filesystem::is_regular_file(workspace / ".csf-mod-state")
                           ? csf::ModProject::load(workspace)
                           : csf::ModProject::create(workspace, package, scene.stem().string(), "0.1.0");
        auto editor = csf::MissionEditor::open(scene, package, &project);
        if (!csf::read_mission_project_info(workspace))
            csf::write_mission_project_info(
                workspace, {editor.files()[editor.scene_file()].relative_path, {}});
        const auto program = editor.file_of_kind(csf::MissionFileKind::mission_script);
        const auto need = [&](const int count) {
            if (i + count > argc) throw std::runtime_error("Missing operation arguments");
        };
        const auto read_text = [](const char* path) {
            const auto bytes = read_bytes(path);
            return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        };
        bool force = false, failed = false;
        while (i < argc) {
            const std::string_view operation = argv[i++];
            csf::EditResult result{true, {}, {}};
            if (operation == "--force") {
                force = true;
                continue;
            } else if (operation == "--move-actor") {
                need(4);
                const auto id = i32(argv[i]);
                const auto* actor = [&]() -> const csf::MissionActor* {
                    for (const auto& value : editor.scene().actors())
                        if (value.id == id) return &value;
                    return nullptr;
                }();
                csf::ActorPlacement placement{{real(argv[i + 1]), real(argv[i + 2]), real(argv[i + 3])},
                                              actor ? actor->heading.value_or(0) : 0,
                                              actor ? actor->pitch.value_or(0) : 0};
                i += 4;
                if (i < argc && argv[i][0] != '-') placement.heading_degrees = real(argv[i++]);
                result = editor.set_actor_placement(id, placement);
            } else if (operation == "--actor-class") {
                need(2);
                result = editor.set_actor_class(i32(argv[i]), i32(argv[i + 1]), force);
                i += 2;
            } else if (operation == "--actor-look") {
                need(2);
                result = editor.set_actor_look(i32(argv[i]), i32(argv[i + 1]));
                i += 2;
            } else if (operation == "--actor-name") {
                need(2);
                result = editor.set_actor_name(i32(argv[i]), argv[i + 1]);
                i += 2;
            } else if (operation == "--actor-anim") {
                need(3);
                const auto id = i32(argv[i]);
                std::vector<csf::ActorAnimationOverride> overrides;
                for (const auto& value : editor.scene().actors())
                    if (value.id == id)
                        for (const auto& binding : value.animations)
                            if (binding.id && binding.type && *binding.type != argv[i + 1])
                                overrides.push_back({*binding.id, *binding.type});
                overrides.push_back({i32(argv[i + 2]), argv[i + 1]});
                result = editor.set_actor_animations(id, std::move(overrides), force);
                i += 3;
            } else if (operation == "--clear-actor-anims") {
                need(1);
                result = editor.set_actor_animations(i32(argv[i++]), {}, force);
            } else if (operation == "--actor-scripts") {
                need(2);
                const auto id = i32(argv[i]);
                std::vector<std::int32_t> scripts;
                std::string_view list = argv[i + 1];
                while (!list.empty()) {
                    const auto comma = list.find(',');
                    scripts.push_back(i32(list.substr(0, comma)));
                    if (comma == std::string_view::npos) break;
                    list.remove_prefix(comma + 1);
                }
                result = editor.set_actor_scripts(id, std::move(scripts), force);
                i += 2;
            } else if (operation == "--player") {
                need(1);
                result = editor.set_player_actor(i32(argv[i++]));
            } else if (operation == "--duplicate-actor") {
                need(4);
                std::int32_t id{};
                result = editor.duplicate_actor(i32(argv[i]), {real(argv[i + 1]), real(argv[i + 2]), real(argv[i + 3])}, &id);
                if (result) std::cout << "new-actor\t" << id << '\n';
                i += 4;
            } else if (operation == "--add-actor") {
                need(6);
                std::int32_t id{};
                result = editor.add_actor(i32(argv[i]), {{real(argv[i + 1]), real(argv[i + 2]), real(argv[i + 3])},
                                                         real(argv[i + 4]), 0},
                                          argv[i + 5], std::nullopt, &id);
                if (result) std::cout << "new-actor\t" << id << '\n';
                i += 6;
            } else if (operation == "--delete-actor") {
                need(1);
                result = editor.delete_actor(i32(argv[i++]), force);
            } else if (operation == "--move-dummy" || operation == "--move-light") {
                need(4);
                const auto id = i32(argv[i]);
                const csf::Vec3 position{real(argv[i + 1]), real(argv[i + 2]), real(argv[i + 3])};
                i += 4;
                if (operation == "--move-light") {
                    result = editor.set_light(id, {position, std::nullopt, std::nullopt, std::nullopt});
                } else {
                    float rotation{}, pitch{};
                    for (const auto& dummy : editor.scene().dummies())
                        if (dummy.id == id) {
                            rotation = dummy.heading.value_or(0);
                            pitch = dummy.pitch.value_or(0);
                        }
                    result = editor.set_dummy_placement(id, position, rotation, pitch);
                }
            } else if (operation == "--duplicate-dummy") {
                need(4);
                std::int32_t id{};
                result = editor.duplicate_dummy(i32(argv[i]), {real(argv[i + 1]), real(argv[i + 2]), real(argv[i + 3])}, &id);
                if (result) std::cout << "new-dummy\t" << id << '\n';
                i += 4;
            } else if (operation == "--delete-dummy") {
                need(1);
                result = editor.delete_dummy(i32(argv[i++]), force);
            } else if (operation == "--delete-light") {
                need(1);
                result = editor.delete_light(i32(argv[i++]));
            } else if (operation == "--add-nav-point") {
                need(4);
                std::int32_t id{};
                const auto group = i32(argv[i]);
                result = editor.add_navigation_point(group, {real(argv[i + 1]), real(argv[i + 2]), real(argv[i + 3])}, &id);
                if (result) std::cout << "new-nav-point\t" << group << '/' << id << '\n';
                i += 4;
            } else if (operation == "--move-nav-point") {
                need(5);
                result = editor.set_navigation_point(i32(argv[i]), i32(argv[i + 1]),
                                                     {real(argv[i + 2]), real(argv[i + 3]), real(argv[i + 4])});
                i += 5;
            } else if (operation == "--delete-nav-point") {
                need(2);
                result = editor.delete_navigation_point(i32(argv[i]), i32(argv[i + 1]), force);
                i += 2;
            } else if (operation == "--link-nav" || operation == "--unlink-nav") {
                need(4);
                const auto a = i32(argv[i]), b = i32(argv[i + 1]), c = i32(argv[i + 2]), d = i32(argv[i + 3]);
                result = operation == "--link-nav" ? editor.connect_navigation_points(a, b, c, d)
                                                   : editor.disconnect_navigation_points(a, b, c, d);
                i += 4;
            } else if (operation == "--move-area-point" || operation == "--insert-area-point") {
                need(5);
                const auto area = i32(argv[i]);
                const auto index = static_cast<std::size_t>(u32(argv[i + 1]));
                const csf::Vec3 position{real(argv[i + 2]), real(argv[i + 3]), real(argv[i + 4])};
                result = operation == "--move-area-point" ? editor.set_area_point(area, index, position)
                                                          : editor.insert_area_point(area, index, position);
                i += 5;
            } else if (operation == "--remove-area-point") {
                need(2);
                result = editor.remove_area_point(i32(argv[i]), static_cast<std::size_t>(u32(argv[i + 1])));
                i += 2;
            } else if (operation == "--area-height") {
                need(2);
                result = editor.set_area_height(i32(argv[i]), real(argv[i + 1]));
                i += 2;
            } else if (operation == "--move-instance") {
                need(4);
                const auto offset = std::stoull(argv[i], nullptr, 0);
                const auto current = editor.map_instance_transform(offset);
                if (!current) throw std::runtime_error("No scene instance record at that map offset");
                result = editor.set_map_instance_transform(
                    offset, current->rotation, {real(argv[i + 1]), real(argv[i + 2]), real(argv[i + 3])});
                i += 4;
            } else if (operation == "--print-script") {
                need(1);
                if (!program) throw std::runtime_error("The mission has no program");
                const auto text = editor.script_text(*program, i32(argv[i++]));
                if (!text) throw std::runtime_error("Script does not exist");
                std::cout << *text;
                continue;
            } else if (operation == "--set-script") {
                need(2);
                if (!program) throw std::runtime_error("The mission has no program");
                result = editor.set_script_text(*program, i32(argv[i]), read_text(argv[i + 1]));
                i += 2;
            } else if (operation == "--add-script") {
                need(1);
                if (!program) throw std::runtime_error("The mission has no program");
                std::int32_t id{};
                result = editor.add_script(*program, read_text(argv[i++]), &id);
                if (result) std::cout << "new-script\t" << id << '\n';
            } else if (operation == "--delete-script") {
                need(1);
                if (!program) throw std::runtime_error("The mission has no program");
                result = editor.delete_script(*program, i32(argv[i++]), force);
            } else if (operation == "--import-class" || operation == "--import-anim") {
                need(2);
                result = operation == "--import-class" ? editor.import_class(argv[i], i32(argv[i + 1]))
                                                       : editor.import_animation(argv[i], i32(argv[i + 1]));
                i += 2;
            } else {
                throw std::runtime_error("Unknown mission-edit operation: " + std::string(operation));
            }
            std::cout << (result.applied ? "applied" : "rejected") << '\t' << result.message << '\n';
            for (const auto& warning : result.warnings) std::cout << "  warning\t" << warning << '\n';
            failed |= !result.applied;
        }
        if (failed) {
            std::cout << "nothing saved: an operation was rejected\n";
            return 2;
        }
        if (editor.dirty())
            for (const auto& written : editor.save(project))
                std::cout << "saved\t" << written.generic_string() << '\n';
        return 0;
    }
    if (command == "export-mission") {
        if (argc < 5) { usage(); return 1; }
        csf::MissionPakOptions options;
        for (int i = 5; i < argc;) {
            const std::string_view option = argv[i++];
            if (option == "--overwrite") options.overwrite = true;
            else if (option == "--pakman" && i < argc) options.pakman_cli = argv[i++];
            else throw std::runtime_error("Unknown export-mission option: " + std::string(option));
        }
        const auto result = csf::ModProject::load(argv[2]).export_mission_pak(argv[3], argv[4], options);
        std::cout << "archive\t" << result.archive_path.generic_string() << "\nmanifest\t"
                  << result.manifest_path.generic_string() << "\nsha256\t" << result.archive_sha256
                  << "\nreplaced\t" << result.replaced << "\nadded\t" << result.added << "\ncopied\t"
                  << result.copied << "\npacker\t" << result.packer << '\n';
        return 0;
    }
    if (command == "rollback") {
        if (argc != 3) { usage(); return 1; }
        csf::ModProject::rollback(argv[2]);
        std::cout << "rollback complete\n";
        return 0;
    }
    usage();
    return 1;
} catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
}
