#include "authoring.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "mission_editing.hpp"

#include <chrono>

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
        try {
            outcome.report = project.build_world(force);
            outcome.findings = project.height_report(actors);
            outcome.project = std::move(project);
        } catch (const std::exception& error) {
            outcome.error = error.what();
        }
        return outcome;
    });
}

void reload_mission(AppState& state) {
    if (!state.mission.graph || !state.mission.project) return;
    state.authoring.reload_when_saved = false;
    start_mission_load(state, state.mission.graph->scene_path(), state.mission.project->workspace_root);
}

void finish_job(AppState& state, AuthoringSession::Outcome outcome) {
    auto& session = state.authoring;
    if (!outcome.error.empty()) {
        state.notify(LogLevel::error, "Project map not rebuilt: " + outcome.error);
        return;
    }
    *session.project = std::move(*outcome.project);
    try {
        session.project->save();
        session.project_time = project_time(*session.project);
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Project not saved: ") + error.what());
    }
    for (const auto& line : outcome.report.lines) state.info("Project: " + line);
    session.findings = std::move(outcome.findings);
    if (!session.findings.empty()) {
        session.show_heights = true;
        state.warn(std::to_string(session.findings.size()) + " placements or actors are off their height rules");
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
    if (state.mission.editor && state.mission.editor->dirty()) {
        session.reload_when_saved = true;
        state.notify(LogLevel::warn, "Project map rebuilt; save the mission to reload it");
    } else {
        state.ok("Project map rebuilt; reloading the mission");
        reload_mission(state);
    }
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
    if (session.reload_when_saved && state.mission.editor && !state.mission.editor->dirty() &&
        !mission_load_active(state))
        reload_mission(state);
    if (authoring_busy(state) || mission_load_active(state)) return;
    const bool views_current = !state.mission.editor || state.mission.editor->revision() == state.mission.applied_revision;
    if (session.job_queued && views_current) {
        start_job(state);
        return;
    }
    if (now_seconds() < session.next_check) return;
    session.next_check = now_seconds() + 1.0;
    if (project_time(*session.project) != session.project_time) {
        try {
            auto reloaded = csf::AuthoringProject::load(session.project->directory);
            *session.project = std::move(reloaded);
            session.project_time = project_time(*session.project);
            state.info("Project: project.csfproj changed; checking the map");
            queue_job(state, false);
        } catch (const std::exception& error) {
            // Possibly half written; try again on the next check.
            state.warn(std::string("Project file unreadable: ") + error.what());
        }
        return;
    }
    if (export_times(*session.project) != session.export_times) {
        state.info("Project: an asset export changed; rebuilding the map");
        queue_job(state, false);
    }
}

void rebuild_authoring_map(AppState& state, const bool force) { queue_job(state, force); }

void resnap_authoring_heights(AppState& state) {
    auto& session = state.authoring;
    if (!session.project || authoring_pending(state)) return;
    std::size_t placements = 0, actors = 0;
    session.project->resnap(session.findings);
    try {
        session.project->save();
        session.project_time = project_time(*session.project);
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Project not saved: ") + error.what());
    }
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

} // namespace rwsman
