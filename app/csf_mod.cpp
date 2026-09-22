#include "csf/authoring.hpp"
#include "csf/mission_edit.hpp"
#include "csf/mod_project.hpp"
#include "csf/source_text.hpp"
#include "csf/tree.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
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
