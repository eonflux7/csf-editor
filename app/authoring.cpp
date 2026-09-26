#include "authoring.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "mission_editing.hpp"

#include "csf/authoring.hpp"

#include <chrono>
#include <ctime>
#include <limits>
#include <fstream>
#include <iterator>
#include <span>

namespace rwsman {
namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::map<std::filesystem::path, std::filesystem::file_time_type> export_times(const csf::AuthoringProject& project) {
    std::map<std::filesystem::path, std::filesystem::file_time_type> times;
    for (const auto& asset : project.assets) {
        std::error_code error;
        const auto time = std::filesystem::last_write_time(project.directory / asset.export_path, error);
        if (!error) times[asset.export_path] = time;
    }
    return times;
}

// Whether project.csfproj or local.csfproj on disk differ from the project.
bool project_changed_on_disk(const csf::AuthoringProject& project) {
    const auto text = [&](const char* name) {
        std::ifstream input(project.directory / name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    };
    return text("project.csfproj") != project.project_text() || text("local.csfproj") != project.local_text();
}

// Writes local.csfproj (the build's output records) when it changed.
void save_local_state(const csf::AuthoringProject& project) {
    const auto path = project.directory / "local.csfproj";
    std::ifstream input(path, std::ios::binary);
    const std::string current((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    const auto text = project.local_text();
    if (current == text) return;
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << text;
        if (!output) return;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
}

std::filesystem::file_time_type project_time(const csf::AuthoringProject& project) {
    std::error_code error;
    return std::filesystem::last_write_time(project.directory / "project.csfproj", error);
}

csf::ActorPositions actor_positions(const AppState& state) {
    csf::ActorPositions actors;
    if (!state.mission.scene) return actors;
    for (const auto& actor : state.mission.scene->actors())
        if (actor.id && actor.position) actors[*actor.id] = {actor.position->x, actor.position->y, actor.position->z};
    return actors;
}

void queue_job(AppState& state, const bool force) {
    state.authoring.job_queued = true;
    state.authoring.job_force = state.authoring.job_force || force;
}

// Builds what is stale (everything, with `force`) and reports heights.
void start_job(AppState& state) {
    auto& session = state.authoring;
    const bool force = session.job_force;
    session.job_queued = session.job_force = false;
    session.export_times = export_times(*session.project);
    session.job = std::async(std::launch::async, [project = *session.project, actors = actor_positions(state),
                                                  force]() mutable {
        AuthoringSession::Outcome outcome;
        outcome.started_text = project.project_text();
        try {
            outcome.report = project.build_world(force);
            for (auto part : {project.build_texts(force), project.build_lightmaps(force)}) {
                outcome.report.lines.insert(outcome.report.lines.end(), part.lines.begin(), part.lines.end());
                outcome.report.rebuilt = outcome.report.rebuilt || part.rebuilt;
            }
            outcome.findings = project.height_report(actors);
            outcome.project = std::move(project);
        } catch (const std::exception& error) {
            outcome.error = error.what();
        }
        return outcome;
    });
}

// Lists the project's built lightmaps in the open mission's texture list and
// packages them from build/ (an undoable edit; saving keeps it).
void register_lightmaps(AppState& state) {
    const auto& project = *state.authoring.project;
    auto* workspace = state.mission.project.get();
    if (project.lightmaps.empty() || !workspace || !mission_editable(state)) return;
    std::vector<std::string> entries;
    for (const auto& lightmap : project.lightmaps) {
        const auto relative = project.lightmap_package_path(lightmap);
        const auto built = project.directory / "build" / relative;
        std::error_code error;
        if (!std::filesystem::is_regular_file(built, error)) continue;
        if (std::ranges::none_of(workspace->files, [&](const csf::ModFile& file) { return file.relative_path == relative; })) {
            csf::ModFile file;
            file.relative_path = relative;
            file.authored_path = std::filesystem::absolute(built);
            std::ifstream input(built, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            file.output_sha256 = csf::sha256(std::as_bytes(std::span(bytes)));
            workspace->add_file(std::move(file));
        }
        auto entry = path_utf8(relative);
        std::ranges::replace(entry, '/', '\\');
        entries.push_back(entry);
    }
    auto& editor = *state.mission.editor;
    const auto file = editor.file_of_kind(csf::MissionFileKind::texture_index);
    if (!file || entries.empty()) return;
    const auto& raw = editor.files()[*file].raw;
    const auto listed = lower_ascii(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()));
    if (std::ranges::all_of(entries, [&](const std::string& e) { return listed.find(lower_ascii(e)) != std::string::npos; }))
        return;
    if (apply_mission_edit(state, editor.add_texture_list_entries(entries)))
        state.notify(LogLevel::info, "Listed the project's lightmaps in the mission; save to keep them");
}

// The project's rebuilt outputs that the open mission packages (the maps, the
// sector map, lightmaps) replace the mission's copies outside its history, and
// the viewport shows the new map: no reload, so undo and unsaved edits stay.
void refresh_generated_files(AppState& state) {
    auto& session = state.authoring;
    auto* workspace = state.mission.project.get();
    if (!workspace || !mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    bool map_changed = false;
    for (const auto& output : session.project->outputs) {
        const auto built = session.project->directory / output.path;
        std::error_code error;
        const auto file = std::ranges::find_if(workspace->files, [&](const csf::ModFile& value) {
            return std::filesystem::equivalent(value.authored_path, built, error);
        });
        if (file == workspace->files.end()) continue;
        std::ifstream input(built, std::ios::binary);
        std::vector<std::byte> bytes;
        for (std::istreambuf_iterator<char> it(input), end; it != end; ++it) bytes.push_back(static_cast<std::byte>(*it));
        // The collision map is not a mission file the editor changes; the
        // loaded document follows the build directly.
        if (state.collision_document &&
            lower_ascii(path_utf8(state.collision_document->source_path().filename())) ==
                lower_ascii(path_utf8(file->relative_path.filename()))) {
            state.collision_document->replace_bytes(bytes);
            map_changed = true;
        }
        const auto index = editor.find_file(file->relative_path);
        if (!index || editor.files()[*index].tree || editor.files()[*index].raw == bytes) continue;
        editor.rebase_file(*index, bytes);
        if (editor.files()[*index].kind == csf::MissionFileKind::visual_map && state.document) {
            state.document->replace_bytes(bytes);
            state.mission.map_revision = editor.files()[*index].revision;
            map_changed = true;
        }
    }
    if (!map_changed) return;
    // The preview rebuilds its scene from the new map, keeping the mission's
    // markers, selection and textures; the mission views follow at the next
    // refresh, and the camera stays where it was.
    state.mission.restore_camera = state.preview.camera();
    state.preview.reload_scene();
    state.mission.applied_revision = ~std::uint64_t{};
}

void finish_job(AppState& state, AuthoringSession::Outcome outcome) {
    auto& session = state.authoring;
    if (!outcome.error.empty()) {
        state.notify(LogLevel::error, "Project map not rebuilt: " + outcome.error);
        return;
    }
    // Edits made while the build ran stay; the build's output records are
    // taken, and a newer build follows.
    if (session.project->project_text() == outcome.started_text) {
        *session.project = std::move(*outcome.project);
    } else {
        session.project->outputs = outcome.project->outputs;
        queue_job(state, false);
    }
    // The build's records of its outputs are saved at once unless the project
    // has unsaved edits (then they go with the next Save).
    if (authoring_dirty(state)) save_local_state(*session.project);
    else save_authoring_project(state);
    for (const auto& line : outcome.report.lines)
        if (!line.starts_with("note\t")) state.info("Project: " + line);
    register_lightmaps(state);
    // The first check after opening sets what counts as known.
    const auto before = session.checked ? session.findings.size() : std::numeric_limits<std::size_t>::max();
    session.checked = true;
    session.findings = std::move(outcome.findings);
    if (!session.findings.empty()) {
        // Open the report after a rebuild or when something new is off; known
        // findings on opening the project only go to the status bar.
        if (outcome.report.rebuilt || session.findings.size() > before) session.show_heights = true;
        state.warn(std::to_string(session.findings.size()) + " placements or actors are off their height rules" +
                   (session.show_heights ? "" : " (see Problems)"));
    }
    if (!outcome.report.rebuilt) return;
    // The build refreshed the workspace's records of the files it rebuilt;
    // the open mission project must not save its older hashes over them.
    if (auto* project = state.mission.project.get()) {
        for (auto& file : project->files)
            for (const auto& output : session.project->outputs) {
                std::error_code error;
                if (std::filesystem::equivalent(file.authored_path, session.project->directory / output.path, error))
                    file.output_sha256 = output.hash.substr(7);  // without "sha256:"
            }
    }
    refresh_generated_files(state);
    state.ok("Project map rebuilt");
}

} // namespace

bool authoring_open(const AppState& state) { return state.authoring.project != nullptr; }

bool authoring_busy(const AppState& state) {
    return state.authoring.job.valid() &&
           state.authoring.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

bool authoring_pending(const AppState& state) { return authoring_busy(state) || state.authoring.job_queued; }

void set_authoring_project(AppState& state, const std::filesystem::path& folder) {
    auto& session = state.authoring;
    if (session.job.valid()) session.job.wait();
    session = {};
    if (folder.empty() || !std::filesystem::is_regular_file(folder / "project.csfproj")) return;
    try {
        session.project = std::make_unique<csf::AuthoringProject>(csf::AuthoringProject::load(folder));
        session.project_time = project_time(*session.project);
        session.saved_text = session.project->project_text();
        session.next_check = now_seconds() + 1.0;
        // A check, not a forced build: rebuilds only what is stale.
        queue_job(state, false);
    } catch (const std::exception& error) {
        session.project.reset();
        state.notify(LogLevel::error, std::string("Cannot read the authoring project: ") + error.what());
    }
}

void poll_authoring(AppState& state) {
    auto& session = state.authoring;
    if (!session.project) return;
    // Another mission was opened: the project no longer applies.
    if (!mission_load_active(state) && state.mission.graph) {
        std::error_code error;
        if (!state.mission.project ||
            !std::filesystem::equivalent(state.mission.project->workspace_root, session.project->directory / "mission", error)) {
            set_authoring_project(state, {});
            return;
        }
    }
    if (session.job.valid() && session.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto outcome = session.job.get();
        finish_job(state, std::move(outcome));
    }
    if (authoring_busy(state) || mission_load_active(state)) return;
    const bool views_current = !state.mission.editor || state.mission.editor->revision() == state.mission.applied_revision;
    if (session.job_queued && views_current) {
        start_job(state);
        return;
    }
    if (now_seconds() < session.next_check) return;
    session.next_check = now_seconds() + 1.0;
    if (project_time(*session.project) != session.project_time) {
        session.project_time = project_time(*session.project);
        if (authoring_dirty(state)) {
            // Keep the unsaved edits and take what another writer (the Blender
            // add-on's Send) registers: assets and lightmaps.
            try {
                const auto disk = csf::AuthoringProject::load(session.project->directory);
                session.project->assets = disk.assets;
                session.project->lightmaps = disk.lightmaps;
                session.saved_text = disk.project_text();
                state.info("Project: took the assets Blender registered; the project still has unsaved edits");
                queue_job(state, false);
            } catch (const std::exception& error) {
                state.warn(std::string("Project file unreadable: ") + error.what());
            }
            return;
        }
        try {
            auto reloaded = csf::AuthoringProject::load(session.project->directory);
            *session.project = std::move(reloaded);
            session.project_time = project_time(*session.project);
            session.saved_text = session.project->project_text();
            // Undo steps hold texts of the project before the outside change.
            session.steps.clear();
            session.cursor = 0;
            state.info("Project: project.csfproj changed; checking the map");
            queue_job(state, false);
        } catch (const std::exception& error) {
            // Possibly half written; try again on the next check.
            state.warn(std::string("Project file unreadable: ") + error.what());
        }
        return;
    }
    if (export_times(*session.project) != session.export_times) {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char text[8];
        std::strftime(text, sizeof(text), "%H:%M", std::localtime(&now));
        session.last_send = text;
        state.info("Project: an asset export changed; rebuilding the map");
        queue_job(state, false);
    }
}

void rebuild_authoring_map(AppState& state, const bool force) { queue_job(state, force); }

void save_authoring_project(AppState& state) {
    auto& session = state.authoring;
    if (!session.project) return;
    try {
        if (project_changed_on_disk(*session.project)) {
            session.project->save();
            session.project_time = project_time(*session.project);
        }
        session.saved_text = session.project->project_text();
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Project not saved: ") + error.what());
    }
}

namespace {

// A project's text without its build records (`output` lines): those follow
// the builds and are not edits.
std::string without_outputs(const std::string& text) {
    std::string result;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto end = std::min(text.find('\n', begin), text.size());
        if (text.compare(begin, 7, "output ") != 0) result.append(text, begin, end - begin + 1);
        begin = end + 1;
    }
    return result;
}

} // namespace

bool authoring_dirty(const AppState& state) {
    return state.authoring.project &&
           without_outputs(state.authoring.project->project_text()) != without_outputs(state.authoring.saved_text);
}

void save_project_local(AppState& state) {
    if (state.authoring.project) save_local_state(*state.authoring.project);
}

bool edits_unsaved(const AppState& state) {
    return (state.mission.editor && state.mission.editor->dirty()) || authoring_dirty(state);
}

void resnap_authoring_heights(AppState& state) {
    auto& session = state.authoring;
    if (!session.project || authoring_pending(state)) return;
    std::size_t placements = 0, actors = 0;
    const auto findings = session.findings;
    edit_authoring_project(state, "Resnap placement heights", [&](csf::AuthoringProject& project) {
        project.resnap(findings);
        return true;
    }, false);
    for (const auto& finding : session.findings) {
        if (!finding.resolved) continue;
        if (finding.subject == csf::HeightFinding::Subject::placement) {
            ++placements;
            continue;
        }
        if (!mission_editable(state)) continue;
        const auto& scene = *state.mission.scene;
        const auto actor = std::ranges::find_if(scene.actors(), [&](const auto& a) { return a.id == finding.actor_id; });
        if (actor == scene.actors().end() || !actor->position) continue;
        const csf::ActorPlacement placement{{actor->position->x, *finding.resolved, actor->position->z},
                                           actor->heading.value_or(0), actor->pitch.value_or(0)};
        if (apply_mission_edit(state, state.mission.editor->set_actor_placement(finding.actor_id, placement))) ++actors;
    }
    session.findings.clear();
    state.ok("Resnapped " + std::to_string(placements) + " placements and " + std::to_string(actors) +
             " actors" + (actors ? "; save the mission to keep the actor moves" : ""));
    // Placement heights are World inputs: rebuild (and report again).
    queue_job(state, false);
}

namespace {

std::size_t mission_position(const AppState& state) {
    return state.mission.editor ? state.mission.editor->history_position() : 0;
}

// Replaces the open project with `text` (a step's before or after), saves it
// and rebuilds the map.
void restore_project(AppState& state, const std::string& text) {
    auto& session = state.authoring;
    if (!session.project) return;
    try {
        auto restored = csf::AuthoringProject::parse(text, session.project->local_text());
        restored.directory = session.project->directory;
        restored.outputs = session.project->outputs;  // generated-output records follow the files, not the edit
        *session.project = std::move(restored);
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Could not restore the project: ") + error.what());
        return;
    }
    state.mission.applied_revision = ~std::uint64_t{}; // The viewport's placements follow.
    queue_job(state, false);
}

// The project step undo would take: the newest applied one, when no mission
// edit came after it.
const AuthoringSession::Step* project_step_to_undo(const AppState& state) {
    const auto& session = state.authoring;
    if (!session.project || session.cursor == 0) return nullptr;
    const auto& step = session.steps[session.cursor - 1];
    return step.anchor == mission_position(state) ? &step : nullptr;
}

const AuthoringSession::Step* project_step_to_redo(const AppState& state) {
    const auto& session = state.authoring;
    if (!session.project || session.cursor >= session.steps.size()) return nullptr;
    const auto& step = session.steps[session.cursor];
    return step.anchor == mission_position(state) ? &step : nullptr;
}

} // namespace

bool edit_authoring_project(AppState& state, std::string label,
                            const std::function<bool(csf::AuthoringProject&)>& change, const bool rebuild) {
    auto& session = state.authoring;
    if (!session.project) return false;
    auto copy = *session.project;
    if (!change(copy)) return false;
    if (const auto problems = copy.check(); !problems.empty()) {
        state.warn(problems.front());
        return false;
    }
    const auto before = session.project->project_text(), after = copy.project_text();
    if (before == after) return false;
    *session.project = std::move(copy);
    session.steps.resize(session.cursor);
    session.steps.push_back({std::move(label), before, after, mission_position(state)});
    session.cursor = session.steps.size();
    state.mission.applied_revision = ~std::uint64_t{}; // The viewport's placements follow.
    if (rebuild) queue_job(state, false);
    return true;
}

bool can_undo_edit(const AppState& state) {
    return project_step_to_undo(state) != nullptr || (mission_editable(state) && state.mission.editor->can_undo());
}

bool can_redo_edit(const AppState& state) {
    return project_step_to_redo(state) != nullptr || (mission_editable(state) && state.mission.editor->can_redo());
}

void undo_edit(AppState& state) {
    if (const auto* step = project_step_to_undo(state)) {
        const auto text = step->before, label = step->label;
        --state.authoring.cursor;
        restore_project(state, text);
        state.info("Undid " + label);
        return;
    }
    mission_undo(state);
}

void redo_edit(AppState& state) {
    if (const auto* step = project_step_to_redo(state)) {
        const auto text = step->after, label = step->label;
        ++state.authoring.cursor;
        restore_project(state, text);
        state.info("Redid " + label);
        return;
    }
    mission_redo(state);
}

std::pair<std::size_t, std::size_t> history_point(const AppState& state) {
    return {mission_position(state), state.authoring.cursor};
}

void notify_undoable(AppState& state, std::string message) {
    state.notify(LogLevel::ok, std::move(message));
    state.toasts.back().undo_at = history_point(state);
}

std::pair<std::vector<std::string>, std::size_t> edit_history(const AppState& state) {
    std::vector<std::string> mission;
    std::size_t mission_applied = 0;
    if (state.mission.editor) {
        mission = state.mission.editor->history_labels();
        mission_applied = state.mission.editor->history_position();
    }
    // Interleave: each project step follows the mission step it was anchored to.
    std::vector<std::string> labels;
    std::size_t applied = 0, next_project = 0;
    const auto& steps = state.authoring.steps;
    for (std::size_t m = 0; m <= mission.size(); ++m) {
        while (next_project < steps.size() && steps[next_project].anchor == m) {
            labels.push_back("Project: " + steps[next_project].label);
            if (next_project < state.authoring.cursor) applied = labels.size();
            ++next_project;
        }
        if (m == mission.size()) break;
        labels.push_back(mission[m]);
        if (m < mission_applied) applied = std::max(applied, labels.size());
    }
    for (; next_project < steps.size(); ++next_project) labels.push_back("Project: " + steps[next_project].label);
    return {labels, applied};
}

} // namespace rwsman
