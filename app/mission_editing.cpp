#include "mission_editing.hpp"

#include "mission_authoring.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "authoring.hpp"
#include "mission_loader.hpp"
#include "mission_overlays.hpp"
#include "viewport_tools.hpp"
#include "navigation.hpp"
#include "rwsman/chunk_lookup.hpp"

#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <bit>
#include <numbers>

#ifndef RWSMAN_VERSION
#define RWSMAN_VERSION "dev"
#endif

namespace rwsman {
namespace {

constexpr float radians_per_degree = std::numbers::pi_v<float> / 180.0F;

// Offset for duplicates: a little over one actor width (map units are cm).
constexpr csf::Vec3 duplicate_offset{150.0F, 0.0F, 0.0F};

csf::Vec3 csf_point(const rws::Vec3 value) { return {value.x, value.y, value.z}; }

const csf::MissionActor* find_actor(const csf::MissionScene& scene, const std::int32_t id) {
    const auto found = std::ranges::find(scene.actors(), std::optional{id}, &csf::MissionActor::id);
    return found == scene.actors().end() ? nullptr : &*found;
}

const csf::MissionDummy* find_dummy(const csf::MissionScene& scene, const std::int32_t id) {
    const auto found = std::ranges::find(scene.dummies(), std::optional{id}, &csf::MissionDummy::id);
    return found == scene.dummies().end() ? nullptr : &*found;
}

std::string record_label(const MissionRecordKey& key) {
    switch (key.kind) {
    case MissionRecordKey::Kind::actor: return "actor " + std::to_string(key.id);
    case MissionRecordKey::Kind::dummy: return "dummy " + std::to_string(key.id);
    case MissionRecordKey::Kind::light: return "light " + std::to_string(key.id);
    case MissionRecordKey::Kind::area: return "area " + std::to_string(key.id);
    case MissionRecordKey::Kind::effect: return "effect " + std::to_string(key.id);
    case MissionRecordKey::Kind::nav_group: return "navigation group " + std::to_string(key.id);
    case MissionRecordKey::Kind::nav_point:
        return "navigation point " + std::to_string(key.id) + ":" + std::to_string(key.sub_id);
    case MissionRecordKey::Kind::none: break;
    }
    return "record";
}

// Remaps a selection that names a scene entry; other selections are unchanged.
SelectionRef remap_selection(const SelectionRef& ref, const csf::MissionScene& before,
                             const csf::MissionScene& after) {
    if (ref.kind != SelectionRef::Kind::mission_entry) return ref;
    // Project placements keep their entries (their index in the project).
    if (static_cast<std::uint32_t>(ref.a) >= placement_entry_base) return ref;
    const auto key = mission_record_key(before, static_cast<std::uint32_t>(ref.a));
    if (key.kind == MissionRecordKey::Kind::none) return {};
    const auto entry = mission_record_entry(after, key);
    return entry ? SelectionRef::mission_entry(*entry) : SelectionRef{};
}

std::optional<std::size_t> loaded_map_file(const AppState& state) {
    if (!state.mission.editor || !state.document) return std::nullopt;
    const auto& editor = *state.mission.editor;
    const auto file = editor.file_of_kind(csf::MissionFileKind::visual_map);
    if (!file) return std::nullopt;
    std::error_code error;
    const auto a = std::filesystem::weakly_canonical(editor.package_path(*file), error);
    const auto b = std::filesystem::weakly_canonical(state.document->source_path(), error);
    if (csf::ResourceIndex::normalize_path(a) != csf::ResourceIndex::normalize_path(b)) return std::nullopt;
    return file;
}

float read_real(const std::vector<std::byte>& bytes, const std::uint64_t at) {
    std::uint32_t bits{};
    for (unsigned i = 0; i < 4; ++i)
        bits |= std::to_integer<std::uint32_t>(bytes[static_cast<std::size_t>(at + i)]) << (8U * i);
    return std::bit_cast<float>(bits);
}

// Brings the loaded visual map (and its preview) in step with the editor's copy.
void refresh_map(AppState& state) {
    auto& mission = state.mission;
    const auto file = loaded_map_file(state);
    if (!file) return;
    const auto& map = mission.editor->files()[*file];
    if (map.revision == mission.map_revision) return;
    mission.map_revision = map.revision;
    if (map.raw.size() != state.document->bytes().size()) return;
    for (const auto& instance : state.document->scene_instances()) {
        if (instance.offset + 108 > map.raw.size()) continue;
        std::array<float, 9> rotation{};
        for (std::size_t i = 0; i < rotation.size(); ++i) rotation[i] = read_real(map.raw, instance.offset + 60 + i * 4);
        const rws::Vec3 position{read_real(map.raw, instance.offset + 96), read_real(map.raw, instance.offset + 100),
                                 read_real(map.raw, instance.offset + 104)};
        if (rotation != instance.rotation || position.x != instance.position.x ||
            position.y != instance.position.y || position.z != instance.position.z)
            state.preview.place_scene_instance(instance.offset, rotation, position);
    }
    state.document->replace_bytes(map.raw);
}

} // namespace

bool map_instances_editable(const AppState& state) {
    return mission_editable(state) && loaded_map_file(state).has_value();
}

float map_instance_yaw(const rws::SceneInstance& instance) {
    return std::atan2(instance.rotation[6], instance.rotation[8]);
}

void set_map_instance_pose(AppState& state, const rws::SceneInstance& instance, const csf::Vec3 position,
                           const float yaw_radians) {
    if (!map_instances_editable(state)) return;
    GeometryPreview::EditDrag drag;
    drag.rotation = instance.rotation;
    drag.start_heading_radians = map_instance_yaw(instance);
    drag.heading_radians = yaw_radians;
    apply_mission_edit(state, state.mission.editor->set_map_instance_transform(
                                  instance.offset, GeometryPreview::instance_rotation_for(drag), position));
}

MissionRecordKey mission_record_key(const csf::MissionScene& scene, const std::uint32_t entry) {
    using Kind = MissionRecordKey::Kind;
    for (const auto& actor : scene.actors())
        if (actor.source.entry_index == entry && actor.id) return {Kind::actor, *actor.id, 0};
    for (const auto& dummy : scene.dummies())
        if (dummy.source.entry_index == entry && dummy.id) return {Kind::dummy, *dummy.id, 0};
    for (const auto& light : scene.lights())
        if (light.source.entry_index == entry && light.id) return {Kind::light, *light.id, 0};
    for (const auto& area : scene.areas())
        if (area.source.entry_index == entry && area.id) return {Kind::area, *area.id, 0};
    for (const auto& effect : scene.effects())
        if (effect.source.entry_index == entry && effect.id) return {Kind::effect, *effect.id, 0};
    for (const auto& group : scene.navigation()) {
        if (group.source.entry_index == entry && group.id) return {Kind::nav_group, *group.id, 0};
        for (const auto& point : group.points)
            if (point.source.entry_index == entry && point.group_id && point.id)
                return {Kind::nav_point, *point.group_id, *point.id};
        // A link between points (a line in the viewport) stands for its route.
        for (const auto& connection : group.connections)
            if (connection.source.entry_index == entry && group.id) return {Kind::nav_group, *group.id, 0};
    }
    for (const auto& connection : scene.cross_group_connections())
        if (connection.source.entry_index == entry && connection.origin_group)
            return {Kind::nav_group, *connection.origin_group, 0};
    return {};
}

std::optional<std::uint32_t> mission_record_entry(const csf::MissionScene& scene, const MissionRecordKey& key) {
    using Kind = MissionRecordKey::Kind;
    const auto by_id = [&](const auto& records) -> std::optional<std::uint32_t> {
        for (const auto& record : records)
            if (record.id == key.id) return record.source.entry_index;
        return std::nullopt;
    };
    switch (key.kind) {
    case Kind::actor: return by_id(scene.actors());
    case Kind::dummy: return by_id(scene.dummies());
    case Kind::light: return by_id(scene.lights());
    case Kind::area: return by_id(scene.areas());
    case Kind::effect: return by_id(scene.effects());
    case Kind::nav_group: return by_id(scene.navigation());
    case Kind::nav_point:
        if (const auto* point = scene.navigation_point(key.id, key.sub_id)) return point->source.entry_index;
        return std::nullopt;
    case Kind::placement:
        if (key.id < 0) return std::nullopt;
        return placement_entry_base + static_cast<std::uint32_t>(key.id);
    case Kind::none: break;
    }
    return std::nullopt;
}

MissionRecordKey selected_mission_record(const AppState& state) {
    if (!state.mission.scene) return {};
    const auto key = [&](const std::uint32_t entry) -> MissionRecordKey {
        if (const auto index = placement_index(state, entry))
            return {MissionRecordKey::Kind::placement, static_cast<std::int32_t>(*index), 0};
        return mission_record_key(*state.mission.scene, entry);
    };
    if (const auto entry = state.preview.selected_mission_entry()) return key(*entry);
    if (state.selection.kind == SelectionRef::Kind::mission_entry)
        return key(static_cast<std::uint32_t>(state.selection.a));
    return {};
}

void select_mission_record(AppState& state, const MissionRecordKey& key, const bool frame) {
    if (!state.mission.scene) return;
    if (const auto entry = mission_record_entry(*state.mission.scene, key))
        navigate_to(state, SelectionRef::mission_entry(*entry), {frame});
}

bool mission_editable(const AppState& state) {
    return state.mission.editor && state.mission.scene && !mission_load_active(state);
}

bool apply_mission_edit(AppState& state, const csf::EditResult& result) {
    if (!result.applied) {
        state.notify(LogLevel::warn, result.message);
        return false;
    }
    state.ok(result.message);
    for (const auto& warning : result.warnings) state.warn("  " + warning);
    if (!result.warnings.empty())
        state.notify(LogLevel::warn, result.message + " (" + std::to_string(result.warnings.size()) +
                                         " warning" + (result.warnings.size() == 1 ? "" : "s") +
                                         "; see the Console)");
    return true;
}

void select_after_refresh(AppState& state, const MissionRecordKey& key) {
    state.mission.pending_selection = key;
}

void refresh_mission_from_editor(AppState& state) {
    auto& mission = state.mission;
    if (!mission.editor || !mission.graph || !mission.scene ||
        mission.editor->revision() == mission.applied_revision)
        return;
    auto& editor = *mission.editor;
    mission.resources = editor.resource_index(mission.graph->index());
    const auto& index = mission.resources;
    refresh_map(state);
    try {
        auto document = std::make_unique<csf::Document>(editor.scene_document());
        auto scene = std::make_unique<csf::MissionScene>(csf::MissionScene::project(*document));
        auto symbols = std::make_unique<csf::MissionSymbolIndex>();
        symbols->add_scene(*scene);
        std::vector<std::pair<std::filesystem::path, csf::ProgramDocument>> programs;
        std::vector<csf::Document> program_documents;
        std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>> cutscenes;
        csf::ScriptAnimationIndex script_animations;
        auto weapons = std::make_unique<csf::WeaponDatabase>();
        auto animations = std::make_unique<csf::AnimationCatalog>();
        for (std::size_t i = 0; i < editor.files().size(); ++i) {
            const auto& file = editor.files()[i];
            if (!file.present || !file.tree || i == editor.scene_file()) continue;
            using Kind = csf::MissionFileKind;
            if (file.kind == Kind::model_index || file.kind == Kind::animation_index ||
                file.kind == Kind::texture_index || file.kind == Kind::physics_index ||
                file.kind == Kind::visual_index || file.kind == Kind::asset)
                continue;
            auto parsed = editor.document(i);
            symbols->add_document(parsed);
            if (file.kind == Kind::weapons) *weapons = csf::WeaponDatabase::project(parsed);
            if (file.kind == Kind::animations) *animations = csf::AnimationCatalog::project(parsed, &index);
            if (file.kind == Kind::mission_script || file.kind == Kind::cutscene_script) {
                if (file.kind == Kind::mission_script) script_animations.add_document(parsed);
                if (file.kind == Kind::cutscene_script)
                    cutscenes.emplace_back(parsed.source_path(), csf::CutsceneTimeline::project(parsed));
                programs.emplace_back(parsed.source_path(), csf::ProgramDocument::project(parsed));
                program_documents.push_back(std::move(parsed));
            }
        }
        auto objects = std::make_unique<csf::ObjectDatabase>(editor.objects());
        csf::ProgramReferenceIndex references;
        for (const auto& [path, program] : programs) {
            (void)path;
            references.add_program(program, scene.get(), animations.get());
        }
        auto associations = csf::associate_actors(*scene, *objects, index);
        if (!mission.model_cache) mission.model_cache = std::make_shared<ActorModelCache>();
        auto models = build_actor_models(*scene, associations, *weapons, index, *mission.model_cache);
        auto overlays = make_mission_overlays(*scene, objects.get());
        append_cutscene_camera_overlays(*scene, cutscenes, overlays);
        append_actor_collision_overlays(*scene, associations, overlays);
        append_placement_overlays(state, overlays);

        // Carry every entry-based selection over to the new entry numbering.
        const auto& before = *mission.scene;
        const auto selected = state.preview.selected_mission_entry();
        std::optional<std::uint32_t> selected_after;
        if (selected) {
            const auto remapped = remap_selection(SelectionRef::mission_entry(*selected), before, *scene);
            if (!remapped.empty()) selected_after = static_cast<std::uint32_t>(remapped.a);
        }
        state.selection = remap_selection(state.selection, before, *scene);
        if (state.ui.inspector_pin) {
            state.ui.inspector_pin = remap_selection(*state.ui.inspector_pin, before, *scene);
            if (state.ui.inspector_pin->empty()) state.ui.inspector_pin.reset();
        }
        for (auto& inspector : state.ui.extra_inspectors)
            inspector.ref = remap_selection(inspector.ref, before, *scene);
        std::optional<std::uint32_t> animated;
        if (mission.animated_actor) {
            const auto remapped =
                remap_selection(SelectionRef::mission_entry(*mission.animated_actor), before, *scene);
            if (!remapped.empty()) animated = static_cast<std::uint32_t>(remapped.a);
        }

        mission.document = std::move(document);
        mission.scene = std::move(scene);
        mission.symbols = std::move(symbols);
        mission.programs = std::move(programs);
        mission.program_documents = std::move(program_documents);
        mission.cutscenes = std::move(cutscenes);
        mission.script_animations = std::move(script_animations);
        mission.program_references = std::move(references);
        mission.actor_associations = std::move(associations);
        mission.objects = std::move(objects);
        mission.weapons = std::move(weapons);
        mission.animations = std::move(animations);
        if (animated) mission.animated_actor = animated;
        else mission.reset_playback();
        state.preview.update_mission(std::move(overlays), std::move(models), selected_after);
        state.tracker.last_entry = selected_after;
        if (state.selected_program_document >= mission.programs.size()) {
            state.selected_program_document = 0;
            state.selected_program_script = 0;
        } else if (state.selected_program_script >=
                   mission.programs[state.selected_program_document].second.scripts().size()) {
            state.selected_program_script = 0;
        }
        rebuild_search_index(state);
        rebuild_diagnostics(state);
    } catch (const std::exception& error) {
        state.error(std::string("Could not refresh the mission views: ") + error.what());
    }
    mission.applied_revision = editor.revision();
    // Entries are renumbered: hidden and locked records map to new entries.
    state.ui.record_states_dirty = true;
    if (const auto key = std::exchange(mission.pending_selection, std::nullopt))
        select_mission_record(state, *key);
    if (const auto script = std::exchange(mission.pending_script, std::nullopt);
        script && script->first < mission.programs.size()) {
        const auto& scripts = mission.programs[script->first].second.scripts();
        for (std::size_t i = 0; i < scripts.size(); ++i)
            if (scripts[i].id == script->second) {
                state.selected_program_document = script->first;
                state.selected_program_script = i;
            }
    }
}

void update_authoring_views(AppState& state) {
    // The Outliner's hidden and locked records, as viewport entries.
    if (state.ui.record_states_dirty && state.mission.scene) {
        state.ui.record_states_dirty = false;
        const auto& scene = *state.mission.scene;
        const auto entries_of = [&](const std::vector<MissionRecordKey>& keys) {
            std::unordered_set<std::uint32_t> entries;
            for (const auto& key : keys) {
                if (const auto entry = mission_record_entry(scene, key)) entries.insert(*entry);
                // A navigation group takes its points and links with it.
                if (key.kind == MissionRecordKey::Kind::nav_group)
                    for (const auto& group : scene.navigation())
                        if (group.id == key.id) {
                            for (const auto& point : group.points) entries.insert(point.source.entry_index);
                            for (const auto& link : group.connections) entries.insert(link.source.entry_index);
                        }
            }
            return entries;
        };
        state.preview.set_mission_entry_states(entries_of(state.ui.hidden_records),
                                               entries_of(state.ui.locked_records));
    }

    // Problems: rebuilt when the mission, the flow, the height report or the
    // project changed.
    const auto* flow = state.mission.scene && state.mission.editor ? mission_flow(state) : nullptr;
    std::uint64_t key = 1469598103934665603ULL;
    const auto mix = [&key](const std::uint64_t value) { key = (key ^ value) * 1099511628211ULL; };
    mix(state.mission.editor ? state.mission.editor->revision() : 0);
    mix(state.mission.applied_revision);
    mix(reinterpret_cast<std::uintptr_t>(state.mission.scene.get()));
    mix(reinterpret_cast<std::uintptr_t>(flow));
    mix(state.authoring.findings.size());
    mix(reinterpret_cast<std::uintptr_t>(state.authoring.project.get()));
    mix(state.authoring.project ? state.authoring.project->strings.size() + state.authoring.project->placements.size() : 0);
    if (key == state.problems_key) return;
    state.problems_key = key;
    ProblemInputs inputs;
    inputs.scene = state.mission.scene.get();
    inputs.objects = state.mission.objects.get();
    inputs.flow = flow;
    inputs.heights = state.authoring.findings;
    if (state.authoring.project) inputs.project_checks = state.authoring.project->check();
    state.problems = collect_problems(inputs);
}

void mission_undo(AppState& state) {
    if (!mission_editable(state) || !state.mission.editor->can_undo()) return;
    const auto label = state.mission.editor->history_labels()[state.mission.editor->history_position() - 1];
    state.mission.editor->undo();
    state.info("Undid: " + label);
}

void mission_redo(AppState& state) {
    if (!mission_editable(state) || !state.mission.editor->can_redo()) return;
    const auto label = state.mission.editor->history_labels()[state.mission.editor->history_position()];
    state.mission.editor->redo();
    state.info("Redid: " + label);
}

std::string mission_name(const AppState& state) {
    if (state.mission.editor) return path_utf8(state.mission.editor->package_root().filename());
    if (state.mission.graph) return path_utf8(state.mission.graph->scene_path().stem());
    return "Mission";
}

std::filesystem::path default_project_workspace(const AppState& state) {
    const auto root = state.settings.projects_root.empty() ? state.config_dir / "projects"
                                                           : state.settings.projects_root;
    return root / std::filesystem::path(std::u8string(
                      reinterpret_cast<const char8_t*>(mission_name(state).data()), mission_name(state).size()));
}

std::filesystem::path guess_original_archive(const AppState& state) {
    if (!state.mission.original_archive.empty()) return state.mission.original_archive;
    if (state.settings.game_root.empty()) return {};
    const auto name = mission_name(state) + ".pak";
    const auto file = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(name.data()), name.size()));
    std::error_code error;
    for (const auto* directory : {"maps", "Maps", "MAPS"}) {
        const auto candidate = state.settings.game_root / directory / file;
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
    }
    return {};
}

void save_mission_project(AppState& state, const std::filesystem::path& workspace) {
    auto& mission = state.mission;
    if (!mission_editable(state)) return;
    try {
        if (!mission.project) {
            const auto target = workspace.empty() ? default_project_workspace(state) : workspace;
            std::error_code error;
            if (std::filesystem::is_regular_file(target / ".csf-mod-state", error)) {
                auto existing = csf::ModProject::load(target);
                const auto info = csf::read_mission_project_info(target);
                const auto& scene = mission.editor->files()[mission.editor->scene_file()].relative_path;
                if (!info || csf::ResourceIndex::normalize_path(info->scene) !=
                                 csf::ResourceIndex::normalize_path(scene))
                    throw std::runtime_error(path_utf8(target) + " is a project for another mission");
                mission.project = std::make_unique<csf::ModProject>(std::move(existing));
            } else {
                mission.project = std::make_unique<csf::ModProject>(csf::ModProject::create(
                    target, mission.editor->package_root(), mission_name(state), RWSMAN_VERSION));
            }
            if (mission.original_archive.empty()) mission.original_archive = guess_original_archive(state);
            csf::write_mission_project_info(
                target, {mission.editor->files()[mission.editor->scene_file()].relative_path,
                         mission.original_archive});
        }
        const auto written = mission.editor->save(*mission.project);
        save_authoring_project(state);  // one save for the mission and its project (E2)
        state.notify(LogLevel::ok,
                     "Saved " + std::to_string(mission.project->files.size()) + " edited file" +
                         (mission.project->files.size() == 1 ? "" : "s") + " to project " +
                         path_utf8(mission.project->workspace_root),
                     mission.project->workspace_root);
        (void)written;
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Project not saved: ") + error.what());
    }
}

void open_mission_project(AppState& state, const std::filesystem::path& folder) {
    try {
        // An authoring project (project.csfproj) packages its mission in mission/.
        const auto workspace = std::filesystem::is_regular_file(folder / "project.csfproj") ? folder / "mission" : folder;
        set_authoring_project(state, workspace == folder ? std::filesystem::path{} : folder);
        const auto info = csf::read_mission_project_info(workspace);
        if (!info) throw std::runtime_error("The folder has no .csf-mission file (not a mission project)");
        const auto project = csf::ModProject::load(workspace);
        start_mission_load(state, project.source_root / info->scene, workspace);
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Cannot open project: ") + error.what());
    }
}

void export_mission_archive(AppState& state, const std::filesystem::path& original,
                            const std::filesystem::path& output, const bool overwrite, const bool install) {
    auto& mission = state.mission;
    if (!mission_editable(state)) return;
    if (!mission.project || mission.editor->dirty()) save_mission_project(state);
    if (!mission.project || mission.editor->dirty()) return; // Saving failed; it reported why.
    try {
        csf::MissionPakOptions options;
        options.overwrite = overwrite;
        const auto result = mission.project->export_mission_pak(original, output, options);
        mission.original_archive = original;
        csf::write_mission_project_info(
            mission.project->workspace_root,
            {mission.editor->files()[mission.editor->scene_file()].relative_path, original});
        state.notify(LogLevel::ok,
                     "Exported " + path_utf8(output.filename()) + ": " + std::to_string(result.replaced) +
                         " replaced, " + std::to_string(result.added) + " added, " +
                         std::to_string(result.copied) + " unchanged entries",
                     output.parent_path());
        if (!install) return;
        // The original archive's own folder decides where the game looks for it.
        const auto maps = original.parent_path();
        const auto game_root = maps.parent_path();
        const auto relative = maps.filename() / original.filename();
        csf::PakOptions pak;
        const auto deployed = mission.project->deploy_package(output, game_root, relative, pak, false);
        state.notify(LogLevel::ok,
                     "Installed " + path_utf8(relative) + " into " + path_utf8(game_root) +
                         "; the original is backed up (rollback: " + path_utf8(deployed.manifest_path) + ")",
                     deployed.backup_root);
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Export failed: ") + error.what());
    }
}

void duplicate_selected_record(AppState& state) {
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    const auto key = selected_mission_record(state);
    std::int32_t id{};
    csf::EditResult result;
    MissionRecordKey created = key;
    switch (key.kind) {
    case MissionRecordKey::Kind::actor: result = editor.duplicate_actor(key.id, duplicate_offset, &id); break;
    case MissionRecordKey::Kind::dummy: result = editor.duplicate_dummy(key.id, duplicate_offset, &id); break;
    case MissionRecordKey::Kind::light: result = editor.duplicate_light(key.id, duplicate_offset, &id); break;
    default:
        state.warn("Only actors, dummies and lights can be duplicated");
        return;
    }
    created.id = id;
    if (apply_mission_edit(state, result)) select_after_refresh(state, created);
}

void delete_selected_record(AppState& state, const bool force) {
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    const auto key = selected_mission_record(state);
    csf::EditResult result;
    if (auto deleted = delete_component_record(state, key)) {
        // A component's record goes with its component (a route point with its line).
        if (apply_mission_edit(state, *deleted)) {
            notify_undoable(state, deleted->message);
            if (key.kind != MissionRecordKey::Kind::nav_point) {
                state.preview.clear_mission_selection();
                state.selection = {};
            }
        }
        return;
    }
    switch (key.kind) {
    case MissionRecordKey::Kind::actor: result = editor.delete_actor(key.id, force); break;
    case MissionRecordKey::Kind::dummy: result = editor.delete_dummy(key.id, force); break;
    case MissionRecordKey::Kind::light: result = editor.delete_light(key.id); break;
    case MissionRecordKey::Kind::nav_point:
        result = editor.delete_navigation_point(key.id, key.sub_id, force);
        break;
    case MissionRecordKey::Kind::nav_group: result = editor.delete_navigation_group(key.id, force); break;
    case MissionRecordKey::Kind::area: result = editor.delete_area(key.id, force); break;
    default:
        state.warn("Select an actor, dummy, light, area, navigation group or point to delete it");
        return;
    }
    if (apply_mission_edit(state, result)) {
        notify_undoable(state, result.message);
        state.preview.clear_mission_selection();
        state.selection = {};
    } else if (!force && result.message.find("does not exist") == std::string::npos) {
        // Refused because something still refers to it: the dialog lists what.
        state.ui.force_delete_reason = result.message;
    }
}

void update_mission_gizmo(AppState& state) {
    auto& preview = state.preview;
    preview.set_tool_strip(state.workspace == Workspace::mission && mission_editable(state));
    if (!mission_editable(state) || state.workspace != Workspace::mission) {
        preview.set_edit_handle(std::nullopt);
        return;
    }
    const auto& scene = *state.mission.scene;
    const auto key = selected_mission_record(state);
    const auto entry = mission_record_entry(scene, key);
    std::optional<GeometryPreview::EditHandle> handle;
    if (entry) {
        using Kind = MissionRecordKey::Kind;
        if (key.kind == Kind::actor) {
            if (const auto* actor = find_actor(scene, key.id))
                if (const auto spawn = scene.actor_spawn_position(*actor))
                    handle = {*entry, rws_point(*spawn), actor->heading.value_or(0) * radians_per_degree, true};
        } else if (key.kind == Kind::dummy) {
            if (const auto* dummy = find_dummy(scene, key.id); dummy && dummy->position)
                handle = {*entry, rws_point(*dummy->position), dummy->heading.value_or(0), true};
        } else if (key.kind == Kind::light) {
            for (const auto& light : scene.lights())
                if (light.id == key.id && light.position) handle = {*entry, rws_point(*light.position), 0, false};
        } else if (key.kind == Kind::nav_point) {
            if (const auto* point = scene.navigation_point(key.id, key.sub_id); point && point->position)
                handle = {*entry, rws_point(*point->position), point->heading.value_or(0), true};
        }
    }
    if (!handle && state.selection.kind == SelectionRef::Kind::mission_entry)
        if (const auto* placement = selected_placement(state))
            handle = GeometryPreview::EditHandle{static_cast<std::uint32_t>(state.selection.a), placement->position,
                                                 placement->yaw_degrees * radians_per_degree, true};
    if (!handle && state.selection.kind == SelectionRef::Kind::scene_instance && map_instances_editable(state))
        if (const auto* instance = find_instance(state.document->scene_instances(), state.selection.a)) {
            handle = GeometryPreview::EditHandle{0, instance->position, map_instance_yaw(*instance), true,
                                                 instance->offset, instance->rotation};
        }
    preview.set_edit_handle(handle);

    const auto drag = preview.take_edit_drag();
    if (!drag) return;
    if (drag->phase == GeometryPreview::EditDrag::Phase::active) {
        state.ui.animating = true;
        return;
    }
    if (drag->phase != GeometryPreview::EditDrag::Phase::finished) return;
    if (!drag->map_instance && apply_group_drag(state, *drag)) return;
    if (drag->map_instance) {
        const auto result = state.mission.editor->set_map_instance_transform(
            *drag->map_instance, GeometryPreview::instance_rotation_for(*drag), csf_point(drag->position));
        if (result.applied) apply_mission_edit(state, result);
        else state.mission.applied_revision = ~std::uint64_t{};
        return;
    }
    const auto dragged = mission_record_key(scene, drag->source_entry);
    const auto position = csf_point(drag->position);
    auto& editor = *state.mission.editor;
    csf::EditResult result;
    using Kind = MissionRecordKey::Kind;
    // What a component made moves through its lines, so it stays re-editable.
    if (auto moved = move_component_record(state, dragged, position, drag->heading_radians)) {
        result = std::move(*moved);
    } else switch (dragged.kind) {
    case Kind::actor:
        if (const auto* actor = find_actor(scene, dragged.id))
            result = editor.set_actor_placement(
                dragged.id, {position, drag->heading_radians / radians_per_degree, actor->pitch.value_or(0)});
        break;
    case Kind::dummy:
        if (const auto* dummy = find_dummy(scene, dragged.id))
            result = editor.set_dummy_placement(dragged.id, position, drag->heading_radians,
                                                dummy->pitch.value_or(0));
        break;
    case Kind::light:
        result = editor.set_light(dragged.id, {position, std::nullopt, std::nullopt, std::nullopt});
        break;
    case Kind::nav_point:
        result = editor.set_navigation_point(dragged.id, dragged.sub_id, position, drag->heading_radians);
        break;
    default:
        return;
    }
    // A click on a handle without moving changes nothing and is not worth a
    // warning; either way the views are rebuilt so the preview drops the drag pose.
    if (result.applied) {
        apply_mission_edit(state, result);
    } else {
        if (!result.message.starts_with("No change")) state.notify(LogLevel::warn, result.message);
        state.mission.applied_revision = ~std::uint64_t{}; // Next frame drops the drag pose.
    }
}

} // namespace rwsman
