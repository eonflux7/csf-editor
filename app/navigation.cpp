#include "navigation.hpp"

#include "app_util.hpp"
#include "viewport_tools.hpp"
#include "rwsman/index_builders.hpp"
#include "rwsman/mission_lookup.hpp"

#include <algorithm>
#include <cstdio>

namespace rwsman {
namespace {

std::string hex(const std::uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
    return buffer;
}

std::string file_name(const std::filesystem::path& path) {
    return path_utf8(path.filename());
}

const csf::ProgramScript* script_at(const AppState& state, const std::size_t document,
                                    const std::size_t script) {
    if (document >= state.mission.programs.size()) return nullptr;
    const auto& scripts = state.mission.programs[document].second.scripts();
    return script < scripts.size() ? &scripts[script] : nullptr;
}

const csf::ProgramInstruction* find_instruction(const csf::ProgramScript& script,
                                                const std::uint32_t entry) {
    for (const auto* list : {&script.conditions, &script.actions})
        for (const auto& instruction : *list)
            if (instruction.source.entry_index == entry) return &instruction;
    return nullptr;
}

const csf::ObjectDefinition* find_definition(const AppState& state, const std::string& file,
                                             const std::uint32_t entry) {
    if (!state.mission.objects) return nullptr;
    for (const auto& definition : state.mission.objects->definitions())
        if (definition.source.entry_index == entry && path_utf8(definition.source.file) == file)
            return &definition;
    return nullptr;
}

const csf::AnimationRecord* find_animation_record(const AppState& state, const std::string& file,
                                                  const std::uint32_t entry) {
    if (!state.mission.animations) return nullptr;
    for (const auto& record : state.mission.animations->records())
        if (record.source.entry_index == entry && path_utf8(record.source.file) == file)
            return &record;
    return nullptr;
}

// Path of chunks from the root to the chunk at `offset`.
bool chunk_path(const std::vector<rws::Chunk>& chunks, const std::uint64_t offset,
                std::vector<const rws::Chunk*>& path) {
    for (const auto& chunk : chunks) {
        path.push_back(&chunk);
        if (chunk.offset == offset || chunk_path(chunk.children, offset, path)) return true;
        path.pop_back();
    }
    return false;
}

std::string mission_group(const std::string& kind) {
    if (kind == "Actor") return "Actors";
    if (kind.starts_with("Navigation")) return "Navigation";
    if (kind == "Cross-group connection") return "Navigation";
    if (kind == "Dummy") return "Dummies";
    if (kind == "Area") return "Areas";
    if (kind == "Light") return "Lights";
    if (kind == "Effect") return "Effects";
    if (kind == "Folder") return "Folders";
    if (kind == "Scene-object animation") return "Scene objects";
    if (kind == "Bridge") return "Bridges";
    if (kind == "Water") return "Water";
    return kind;
}

SelectionRef ref_for_chunk_or_instance(const AppState& state, const std::uint64_t offset) {
    if (state.document && find_instance(state.document->scene_instances(), offset) &&
        !find_chunk(state.document->chunks(), offset))
        return SelectionRef::scene_instance(offset);
    return SelectionRef::chunk(offset);
}

} // namespace

bool workspace_available(const AppState& state, const Workspace workspace) {
    switch (workspace) {
    case Workspace::mission:
    case Workspace::animation:
        return state.mission.scene != nullptr;
    case Workspace::script:
        return !state.mission.programs.empty();
    case Workspace::scene:
    case Workspace::geometry:
    case Workspace::inspector:
        return state.document != nullptr;
    }
    return false;
}

bool set_workspace(AppState& state, const Workspace workspace) {
    if (!workspace_available(state, workspace)) return false;
    state.workspace = workspace;
    return true;
}

void reset_navigation(AppState& state) {
    state.history.clear();
    state.tracker = {};
    state.selection = {};
    state.ui.script_focus_entry.reset();
    state.ui.script_scroll_pending = false;
    // Pinned and duplicated inspectors refer to records of the previous document.
    state.ui.inspector_pin.reset();
    state.ui.extra_inspectors.clear();
}

void track_selection(AppState& state) {
    auto& tracker = state.tracker;
    const std::optional<std::uint32_t> raw_entry =
        state.mission.scene ? state.preview.selected_mission_entry() : std::nullopt;
    const std::optional<std::uint64_t> raw_chunk = state.document ? state.selected : std::nullopt;
    std::optional<std::pair<std::size_t, std::size_t>> raw_script;
    if (state.workspace == Workspace::script &&
        script_at(state, state.selected_program_document, state.selected_program_script))
        raw_script = {state.selected_program_document, state.selected_program_script};

    bool changed = false;
    if (raw_entry != tracker.last_entry) {
        tracker.last_changed = SelectionTracker::Slot::mission_entry;
        changed = true;
    } else if (raw_chunk != tracker.last_chunk) {
        tracker.last_changed = SelectionTracker::Slot::chunk;
        changed = true;
    } else if (raw_script != tracker.last_script) {
        tracker.last_changed = SelectionTracker::Slot::script;
        changed = true;
    }
    if (changed) tracker.detached = {};
    tracker.last_entry = raw_entry;
    tracker.last_chunk = raw_chunk;
    tracker.last_script = raw_script;

    // Selecting in the script outline, or opening the Script workspace, makes the
    // script the subject; otherwise the most recently changed slot wins and an
    // emptied slot falls back to whichever slot still has something.
    SelectionRef current;
    const auto from_slot = [&](const SelectionTracker::Slot slot) -> SelectionRef {
        switch (slot) {
        case SelectionTracker::Slot::mission_entry:
            return raw_entry ? SelectionRef::mission_entry(*raw_entry) : SelectionRef{};
        case SelectionTracker::Slot::chunk:
            return raw_chunk ? ref_for_chunk_or_instance(state, *raw_chunk) : SelectionRef{};
        case SelectionTracker::Slot::script:
            return raw_script ? SelectionRef::program_script(raw_script->first, raw_script->second)
                              : SelectionRef{};
        case SelectionTracker::Slot::none:
            break;
        }
        return {};
    };
    if (!tracker.detached.empty()) {
        current = tracker.detached;
    } else {
        current = from_slot(tracker.last_changed);
        if (current.empty()) {
            const bool script_workspace = state.workspace == Workspace::script;
            for (const auto slot : {script_workspace ? SelectionTracker::Slot::script
                                                     : SelectionTracker::Slot::mission_entry,
                                    SelectionTracker::Slot::mission_entry,
                                    SelectionTracker::Slot::chunk,
                                    SelectionTracker::Slot::script}) {
                current = from_slot(slot);
                if (!current.empty()) break;
            }
        }
        // A script selection with an instruction focus keeps the finer target.
        if (current.kind == SelectionRef::Kind::program_script && state.ui.script_focus_entry)
            current = SelectionRef::program_instruction(current.a, current.b,
                                                        *state.ui.script_focus_entry);
    }
    state.selection = current;

    const auto workspace_id = static_cast<int>(state.workspace);
    if (!current.empty()) {
        const auto* top = state.history.current();
        if (!top || top->selection != current || top->workspace != workspace_id)
            state.history.push({workspace_id, current, state.preview.camera()});
    }
    if (state.history.current()) state.history.update_current_camera(state.preview.camera());
    state.previous_selection = state.selected;
}

void navigate_to(AppState& state, const SelectionRef& ref, const NavigateOptions options) {
    using Kind = SelectionRef::Kind;
    switch (ref.kind) {
    case Kind::none:
        return;
    case Kind::chunk:
    case Kind::scene_instance:
        if (!state.document) return;
        state.selected = ref.a;
        state.preview.clear_mission_selection();
        if (state.workspace == Workspace::mission || state.workspace == Workspace::animation ||
            state.workspace == Workspace::script)
            state.workspace = Workspace::scene;
        if (options.frame && state.workspace == Workspace::scene)
            state.preview.frame_selection(ref.a);
        break;
    case Kind::mission_entry:
        if (!state.mission.scene) return;
        state.preview.select_mission_entry(static_cast<std::uint32_t>(ref.a));
        if (state.workspace != Workspace::mission && state.workspace != Workspace::animation)
            state.workspace = Workspace::mission;
        if (options.frame) state.preview.frame_selection(std::nullopt);
        break;
    case Kind::program_script:
    case Kind::program_instruction:
        if (!script_at(state, ref.a, ref.b)) return;
        state.selected_program_document = ref.a;
        state.selected_program_script = ref.b;
        state.workspace = Workspace::script;
        if (ref.kind == Kind::program_instruction) {
            state.ui.script_focus_entry = static_cast<std::uint32_t>(ref.c);
            state.ui.script_scroll_pending = true;
        } else {
            state.ui.script_focus_entry.reset();
        }
        break;
    case Kind::database_record:
    case Kind::resource_path:
        state.tracker.detached = ref;
        break;
    }
}

namespace {

void restore(AppState& state, const HistoryEntry& entry) {
    const auto workspace = static_cast<Workspace>(entry.workspace);
    navigate_to(state, entry.selection, {.frame = false});
    if (workspace_available(state, workspace)) state.workspace = workspace;
    state.preview.set_camera(entry.camera);
}

} // namespace

void go_back(AppState& state) {
    if (const auto* entry = state.history.back()) restore(state, *entry);
}

void go_forward(AppState& state) {
    if (const auto* entry = state.history.forward()) restore(state, *entry);
}

std::optional<std::uint64_t> selection_offset(const AppState& state, const SelectionRef& ref) {
    using Kind = SelectionRef::Kind;
    switch (ref.kind) {
    case Kind::chunk:
    case Kind::scene_instance:
        return ref.a;
    case Kind::mission_entry: {
        if (placement_index(state, static_cast<std::uint32_t>(ref.a))) return std::nullopt;
        std::string kind, label;
        if (state.mission.scene)
            if (const auto* source = find_mission_source(*state.mission.scene,
                                                         static_cast<std::uint32_t>(ref.a), kind,
                                                         label))
                return source->range.offset;
        return std::nullopt;
    }
    case Kind::program_script:
    case Kind::program_instruction: {
        const auto* script = script_at(state, ref.a, ref.b);
        if (!script) return std::nullopt;
        if (ref.kind == Kind::program_instruction)
            if (const auto* instruction =
                    find_instruction(*script, static_cast<std::uint32_t>(ref.c)))
                return instruction->source.range.offset;
        return script->source.range.offset;
    }
    case Kind::database_record:
        if (const auto* definition =
                find_definition(state, ref.path, static_cast<std::uint32_t>(ref.a)))
            return definition->source.range.offset;
        if (const auto* record =
                find_animation_record(state, ref.path, static_cast<std::uint32_t>(ref.a)))
            return record->source.range.offset;
        return std::nullopt;
    case Kind::none:
    case Kind::resource_path:
        return std::nullopt;
    }
    return std::nullopt;
}

std::string selection_identity(const AppState& state, const SelectionRef& ref) {
    using Kind = SelectionRef::Kind;
    const auto offset = selection_offset(state, ref);
    const std::string at = offset ? " @" + hex(*offset) : "";
    switch (ref.kind) {
    case Kind::none:
        return {};
    case Kind::chunk:
        return (state.document ? file_name(state.document->source_path()) : std::string{}) +
               ":chunk" + at;
    case Kind::scene_instance:
        return (state.document ? file_name(state.document->source_path()) : std::string{}) +
               ":instance" + at;
    case Kind::mission_entry:
        return (state.mission.scene ? file_name(state.mission.scene->source_path()) : std::string{}) +
               ":entry " + std::to_string(ref.a) + at;
    case Kind::program_script:
    case Kind::program_instruction: {
        const auto* script = script_at(state, ref.a, ref.b);
        const auto file = ref.a < state.mission.programs.size()
                              ? file_name(state.mission.programs[ref.a].first)
                              : std::string{};
        if (!script) return file;
        const auto entry = ref.kind == Kind::program_instruction ? static_cast<std::uint32_t>(ref.c)
                                                                 : script->source.entry_index;
        return file + ":entry " + std::to_string(entry) + at;
    }
    case Kind::database_record:
        return file_name(ref.path) + ":entry " + std::to_string(ref.a) + at;
    case Kind::resource_path:
        return ref.path;
    }
    return {};
}

std::string selection_id_text(const AppState& state, const SelectionRef& ref) {
    using Kind = SelectionRef::Kind;
    switch (ref.kind) {
    case Kind::chunk:
    case Kind::scene_instance:
        return hex(ref.a);
    case Kind::mission_entry:
    case Kind::database_record:
        return std::to_string(ref.a);
    case Kind::program_script:
    case Kind::program_instruction:
        if (const auto* script = script_at(state, ref.a, ref.b))
            return std::to_string(ref.kind == Kind::program_instruction ? ref.c
                                                                        : script->source.entry_index);
        return {};
    case Kind::resource_path:
    case Kind::none:
        return {};
    }
    return {};
}

std::filesystem::path selection_source_path(const AppState& state, const SelectionRef& ref) {
    using Kind = SelectionRef::Kind;
    switch (ref.kind) {
    case Kind::chunk:
    case Kind::scene_instance:
        return state.document ? state.document->source_path() : std::filesystem::path{};
    case Kind::mission_entry:
        return state.mission.scene ? state.mission.scene->source_path() : std::filesystem::path{};
    case Kind::program_script:
    case Kind::program_instruction:
        return ref.a < state.mission.programs.size() ? state.mission.programs[ref.a].first
                                                     : std::filesystem::path{};
    case Kind::database_record:
    case Kind::resource_path:
        return std::filesystem::path(ref.path);
    case Kind::none:
        break;
    }
    return {};
}

std::string selection_title(const AppState& state, const SelectionRef& ref) {
    using Kind = SelectionRef::Kind;
    switch (ref.kind) {
    case Kind::none:
        return {};
    case Kind::chunk: {
        if (!state.document) return {};
        const auto* chunk = find_chunk(state.document->chunks(), ref.a);
        if (!chunk) return hex(ref.a);
        std::string title(rws::chunk_name(chunk->type));
        if (const auto name = state.display_names.find(chunk->offset);
            name != state.display_names.end())
            title += " \"" + name->second + "\"";
        return title;
    }
    case Kind::scene_instance: {
        if (!state.document) return {};
        const auto* instance = find_instance(state.document->scene_instances(), ref.a);
        if (!instance) return hex(ref.a);
        return instance->prototype_name.empty() ? "Prototype " + std::to_string(instance->prototype_id)
                                                : instance->prototype_name;
    }
    case Kind::mission_entry: {
        if (const auto index = placement_index(state, static_cast<std::uint32_t>(ref.a)))
            return state.authoring.project->placements[*index].id;
        std::string kind, label;
        if (state.mission.scene &&
            find_mission_source(*state.mission.scene, static_cast<std::uint32_t>(ref.a), kind, label))
            return label.empty() ? kind + " #" + std::to_string(ref.a) : label;
        return "entry " + std::to_string(ref.a);
    }
    case Kind::program_script:
    case Kind::program_instruction: {
        const auto* script = script_at(state, ref.a, ref.b);
        if (!script) return {};
        if (ref.kind == Kind::program_instruction)
            if (const auto* instruction =
                    find_instruction(*script, static_cast<std::uint32_t>(ref.c)))
                return instruction->opcode;
        return script->name;
    }
    case Kind::database_record:
        if (const auto* definition =
                find_definition(state, ref.path, static_cast<std::uint32_t>(ref.a)))
            return definition->name.value_or("class " + (definition->class_id
                                                              ? std::to_string(*definition->class_id)
                                                              : std::string("?")));
        if (const auto* record =
                find_animation_record(state, ref.path, static_cast<std::uint32_t>(ref.a)))
            return record->logical_name;
        return "entry " + std::to_string(ref.a);
    case Kind::resource_path:
        return file_name(ref.path);
    }
    return {};
}

std::string selection_kind_label(const AppState& state, const SelectionRef& ref) {
    using Kind = SelectionRef::Kind;
    switch (ref.kind) {
    case Kind::none:
        return {};
    case Kind::chunk:
        return "Chunk";
    case Kind::scene_instance:
        return "Scene instance";
    case Kind::mission_entry: {
        if (placement_index(state, static_cast<std::uint32_t>(ref.a))) return "Project placement";
        std::string kind, label;
        if (state.mission.scene &&
            find_mission_source(*state.mission.scene, static_cast<std::uint32_t>(ref.a), kind, label))
            return kind;
        return "Mission entry";
    }
    case Kind::program_script:
        return "Script";
    case Kind::program_instruction:
        return "Instruction";
    case Kind::database_record:
        return find_animation_record(state, ref.path, static_cast<std::uint32_t>(ref.a))
                   ? "Animation record"
                   : "Class record";
    case Kind::resource_path:
        return "Resource";
    }
    return {};
}

std::vector<BreadcrumbSegment> selection_breadcrumb(const AppState& state, const SelectionRef& ref) {
    using Kind = SelectionRef::Kind;
    std::vector<BreadcrumbSegment> segments;
    switch (ref.kind) {
    case Kind::none:
        break;
    case Kind::chunk:
    case Kind::scene_instance: {
        if (!state.document) break;
        segments.push_back({file_name(state.document->source_path()), {}});
        if (ref.kind == Kind::scene_instance) {
            segments.push_back({"Scene instances", {}});
            segments.push_back({selection_title(state, ref), ref});
            break;
        }
        std::vector<const rws::Chunk*> path;
        if (chunk_path(state.document->chunks(), ref.a, path))
            for (const auto* chunk : path) {
                std::string label(rws::chunk_name(chunk->type));
                if (const auto name = state.display_names.find(chunk->offset);
                    name != state.display_names.end())
                    label += " \"" + name->second + "\"";
                segments.push_back({label, SelectionRef::chunk(chunk->offset)});
            }
        break;
    }
    case Kind::mission_entry: {
        if (!state.mission.scene) break;
        std::string kind, label;
        const auto entry = static_cast<std::uint32_t>(ref.a);
        if (!find_mission_source(*state.mission.scene, entry, kind, label)) break;
        segments.push_back({file_name(state.mission.scene->source_path()), {}});
        segments.push_back({mission_group(kind), {}});
        segments.push_back({label.empty() ? kind + " #" + std::to_string(entry) : label, ref});
        // Actor -> class -> BDD record: the provenance chain the inspector shows.
        for (const auto& association : state.mission.actor_associations)
            if (association.actor.entry_index == entry) {
                if (association.class_id && association.definitions.size() == 1) {
                    const auto& definition = *association.definitions.front();
                    const auto target = SelectionRef::database_record(
                        path_utf8(definition.source.file), definition.source.entry_index);
                    segments.push_back({"class " + hex(static_cast<std::uint32_t>(*association.class_id)), target});
                    segments.push_back({file_name(definition.source.file) + "#" +
                                            std::to_string(definition.source.entry_index),
                                        target});
                } else if (association.class_id) {
                    segments.push_back({"class " + hex(static_cast<std::uint32_t>(*association.class_id)), {}});
                }
                break;
            }
        break;
    }
    case Kind::program_script:
    case Kind::program_instruction: {
        const auto* script = script_at(state, ref.a, ref.b);
        if (!script) break;
        segments.push_back({file_name(state.mission.programs[ref.a].first), {}});
        if (!script->folder.empty()) segments.push_back({script->folder, {}});
        segments.push_back({script->name, SelectionRef::program_script(ref.a, ref.b)});
        if (ref.kind == Kind::program_instruction)
            segments.push_back({selection_title(state, ref), ref});
        break;
    }
    case Kind::database_record:
        segments.push_back({file_name(ref.path), {}});
        segments.push_back({selection_title(state, ref), ref});
        break;
    case Kind::resource_path:
        segments.push_back({"Resources", {}});
        segments.push_back({file_name(ref.path), ref});
        break;
    }
    return segments;
}

void rebuild_diagnostics(AppState& state) {
    DiagnosticInputs inputs;
    inputs.document = state.document.get();
    inputs.scene = state.mission.scene.get();
    inputs.graph = state.mission.graph.get();
    inputs.scene_document = state.mission.document.get();
    inputs.objects = state.mission.objects.get();
    inputs.animations = state.mission.animations.get();
    inputs.programs = &state.mission.programs;
    inputs.associations = &state.mission.actor_associations;
    state.diagnostics = collect_diagnostics(inputs);
    state.diagnostic_targets.clear();
    state.diagnostic_offsets.clear();
    for (const auto& row : state.diagnostics) {
        if (!row.target.empty()) state.diagnostic_targets.insert(row.target);
        // Diagnostics inside a script or mission subtree also mark the row's
        // owner: the collector already mapped them to their nearest selectable.
        if (row.source == "RWS" && row.offset) state.diagnostic_offsets.push_back(*row.offset);
    }
    std::ranges::sort(state.diagnostic_offsets);
}

void rebuild_search_index(AppState& state) {
    state.search_index.clear();
    if (state.document) {
        index_chunks(state.search_index, state.document->chunks(), state.display_names);
        index_scene_instances(state.search_index, state.document->scene_instances());
    }
    if (state.mission.scene) {
        MissionIndexInputs inputs;
        inputs.scene = state.mission.scene.get();
        inputs.graph = state.mission.graph.get();
        inputs.objects = state.mission.objects.get();
        inputs.animations = state.mission.animations.get();
        inputs.programs = &state.mission.programs;
        index_mission(state.search_index, inputs);
    }
    // The project's placements (radio-house, farmhouse), selected as in the Outliner.
    if (const auto* project = state.authoring.project.get())
        for (std::size_t index = 0; index < project->placements.size(); ++index) {
            const auto& placement = project->placements[index];
            SearchEntry entry;
            entry.kind = SymbolKind::placement;
            entry.label = placement.id;
            entry.detail = placement.kind == csf::ProjectPlacement::Kind::building ? "building " + placement.asset
                           : placement.kind == csf::ProjectPlacement::Kind::piece
                               ? "piece" + (placement.donor.empty() ? std::string() : " of " + placement.donor)
                               : "donor props";
            for (const auto& group : placement.lightmaps) entry.haystack += group + ' ';
            entry.target = {SelectionRef::Kind::mission_entry, placement_entry_base + index, 0, 0};
            state.search_index.add(std::move(entry));
        }
}

} // namespace rwsman
