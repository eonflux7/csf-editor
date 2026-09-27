#include "project_actions.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "authoring.hpp"
#include "mission_editing.hpp"

#include <algorithm>
#include <map>

#ifndef RWSMAN_VERSION
#define RWSMAN_VERSION "dev"
#endif

namespace rwsman {
namespace {

// Deployment states by state file; cleared whenever a deployment changes.
std::map<std::filesystem::path, csf::DeploymentState> deployment_states;

} // namespace

const std::vector<csf::MissionSlot>& project_slots(AppState&, const std::filesystem::path& corpus) {
    static std::filesystem::path cached_for;
    static std::vector<csf::MissionSlot> slots;
    if (corpus != cached_for) {
        cached_for = corpus;
        slots.clear();
        try {
            if (!corpus.empty()) slots = csf::mission_slots(corpus);
        } catch (const std::exception&) {
            slots.clear();
        }
    }
    return slots;
}

std::filesystem::path projects_folder(const AppState& state) {
    return state.settings.projects_root.empty() ? state.config_dir / "projects" : state.settings.projects_root;
}

bool create_project(AppState& state, const std::filesystem::path& folder, const std::string& name,
                    const std::string& slot, const std::filesystem::path& corpus, const float terrain_size) {
    csf::NewProjectOptions options;
    options.directory = folder;
    options.name = name;
    options.slot = slot;
    options.corpus = corpus;
    options.projects_root = projects_folder(state);
    options.terrain_size = terrain_size;
    options.blender = state.settings.blender;
    options.test_install = state.settings.game_root;
    options.tool_version = RWSMAN_VERSION;
    try {
        const auto created = csf::create_authoring_project(options);
        for (const auto& line : created.lines)
            if (!line.starts_with("note\t")) state.info("New project: " + line);
        state.notify(LogLevel::ok, "Created " + name + " in the " + slot + " slot", created.project.directory);
        state.settings.add_recent_file(created.project.directory, true);
        state.settings_dirty = true;
        open_mission_project(state, created.project.directory);
        return true;
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Project not created: ") + error.what());
        return false;
    }
}

std::optional<std::filesystem::path> original_archive(AppState& state, const std::filesystem::path& archive) {
    const auto* project = state.authoring.project.get();
    if (!project) return std::nullopt;
    if (auto found = csf::find_original_archive(*project, archive)) return found;
    // The game install's copy, when nothing was ever deployed over it.
    if (!state.settings.game_root.empty() && project->local.test_install.empty()) {
        std::error_code error;
        const auto candidate = state.settings.game_root / archive;
        if (std::filesystem::is_regular_file(candidate, error) &&
            !std::filesystem::exists(state.settings.game_root / ".csf-mod-backups", error))
            return candidate;
    }
    return std::nullopt;
}

void set_original_archive(AppState& state, const std::filesystem::path& archive, const std::filesystem::path& file) {
    if (!state.authoring.project || archive.empty()) return;
    state.authoring.project->local.originals[archive] = file;
    save_project_local(state);
}

void set_test_install(AppState& state, const std::filesystem::path& folder) {
    if (!state.authoring.project) return;
    state.authoring.project->local.test_install = folder;
    save_project_local(state);
    deployment_states.clear();
}

void build_project_archives(AppState& state) {
    auto* project = state.authoring.project.get();
    if (!project || !mission_editable(state)) return;
    if (authoring_pending(state)) return state.warn("The map is still building; build the archives after it");
    const auto mission = original_archive(state, project->slot.archive);
    if (!mission) return state.warn("Choose the untouched " + path_utf8(project->slot.archive) + " first");
    std::optional<std::filesystem::path> texts;
    if (project->texts && !project->strings.empty()) {
        texts = original_archive(state, project->texts->archive);
        if (!texts) return state.warn("Choose the untouched " + path_utf8(project->texts->archive) + " first");
    }
    // Archives package what is saved.
    if (edits_unsaved(state) || !state.mission.project) save_mission_project(state);
    if (edits_unsaved(state)) return;  // Saving failed; it reported why.
    try {
        const auto build = csf::build_archives(*project, *mission, texts.value_or(""), RWSMAN_VERSION);
        state.authoring.saved_text = project->project_text();
        for (const auto& line : build.lines)
            if (!line.starts_with("note\t")) state.info("Build: " + line);
        state.ui.selected_build = build.id;
        state.notify(LogLevel::ok, "Built " + build.id + ": " + path_utf8(build.mission_archive.filename()) +
                                       (build.texts_archive.empty() ? "" : " and " + path_utf8(build.texts_archive.filename())),
                     build.directory);
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Archives not built: ") + error.what());
    }
}

void deploy_project_build(AppState& state, const std::string& build) {
    auto* project = state.authoring.project.get();
    if (!project) return;
    if (project->local.test_install.empty()) return state.warn("Choose the test install first");
    try {
        const auto deployed = csf::deploy_build(*project, build, project->local.test_install);
        save_project_local(state);
        deployment_states.clear();
        std::string archives;
        for (const auto& deployment : deployed) archives += (archives.empty() ? "" : " and ") + path_utf8(deployment.archive);
        state.notify(LogLevel::ok, "Deployed " + build + " (" + archives + ") into " +
                                       path_utf8(project->local.test_install) + "; Roll back restores what was there");
    } catch (const std::exception& error) {
        deployment_states.clear();
        state.notify(LogLevel::error, std::string("Not deployed: ") + error.what());
    }
}

void roll_back_project_build(AppState& state, const std::string& build) {
    const auto* project = state.authoring.project.get();
    if (!project) return;
    try {
        csf::roll_back_build(*project, build);
        deployment_states.clear();
        state.notify(LogLevel::ok, "Rolled back " + build + ": the test install has what it had before");
    } catch (const std::exception& error) {
        deployment_states.clear();
        state.notify(LogLevel::error, std::string("Not rolled back: ") + error.what());
    }
}

csf::DeploymentState cached_deployment_state(AppState&, const csf::ProjectDeployment& deployment) {
    const auto found = deployment_states.find(deployment.manifest);
    if (found != deployment_states.end()) return found->second;
    csf::DeploymentState value = csf::DeploymentState::missing;
    try {
        value = csf::deployment_state(deployment);
    } catch (const std::exception&) {
    }
    return deployment_states[deployment.manifest] = value;
}

void add_playtest(AppState& state, const std::string& build, const bool worked, const std::string& note) {
    edit_authoring_project(
        state, std::string("Playtest of ") + build + (worked ? ": worked" : ": failed"),
        [&](csf::AuthoringProject& project) {
            project.playtests.push_back({build, worked, note});
            return true;
        },
        false);
}

void edit_in_blender(AppState& state) {
    const auto* project = state.authoring.project.get();
    if (!project) return;
    const auto terrain = std::ranges::find(project->assets, csf::ProjectAsset::Kind::terrain, &csf::ProjectAsset::kind);
    if (terrain == project->assets.end()) return state.warn("The project has no terrain asset to open");
    const auto blend = project->directory / terrain->blend;
    const auto exported = project->directory / terrain->export_path;
    // The add-on and this launcher live in the source tree's tools/blender.
    std::filesystem::path script = std::filesystem::path(RWSMAN_SOURCE_DIR) / "tools" / "blender" / "open_project.py";
    std::error_code error;
    if (!std::filesystem::is_regular_file(script, error))
        script = executable_directory() / "tools" / "blender" / "open_project.py";
    if (!std::filesystem::is_regular_file(script, error))
        return state.warn("tools/blender/open_project.py was not found beside csf-editor");
    auto csf_mod = executable_directory() / "csf-mod";
#ifdef _WIN32
    csf_mod += ".exe";
#endif
    const auto program = !project->local.blender.empty()  ? project->local.blender
                         : !state.settings.blender.empty() ? state.settings.blender
                                                           : std::filesystem::path("blender");
    std::vector<std::string> arguments{path_utf8(program)};
    if (std::filesystem::is_regular_file(blend, error)) arguments.push_back(path_utf8(blend));
    for (const auto& value : {std::string("--python"), path_utf8(script), std::string("--"), path_utf8(project->directory),
                              path_utf8(blend), path_utf8(exported), path_utf8(csf_mod)})
        arguments.push_back(value);
    if (!launch_detached(arguments))
        return state.notify(LogLevel::error, "Could not start " + path_utf8(program) +
                                                 "; set the Blender executable in Preferences");
    state.notify(LogLevel::info, "Opened " + path_utf8(terrain->blend) + " in Blender: Send (CSF tab) rebuilds the map here");
}

} // namespace rwsman
