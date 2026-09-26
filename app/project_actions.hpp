#pragma once

#include "app_state.hpp"

#include "csf/project_pipeline.hpp"

#include <filesystem>
#include <string>
#include <vector>

// The authoring project's lifecycle in the GUI (docs/plans/editor-ux-redesign.md,
// E8, E9 and E15): the New project wizard, the archives, deploying and rolling
// back, the playtest log and Edit in Blender. The work is csf/project_pipeline;
// these report to the log and toasts and keep the open project current.
namespace rwsman {

// The shipped missions under `corpus` a project can take the slot of (cached).
[[nodiscard]] const std::vector<csf::MissionSlot>& project_slots(AppState& state, const std::filesystem::path& corpus);
// Where new projects go: the projects folder setting, or <config>/projects.
[[nodiscard]] std::filesystem::path projects_folder(const AppState& state);
// Creates a project and opens it; false when it could not (reported).
bool create_project(AppState& state, const std::filesystem::path& folder, const std::string& name,
                    const std::string& slot, const std::filesystem::path& corpus, float terrain_size);

// The untouched archive a build starts from (maps/<Mission>.pak, GlobalEK.pak).
[[nodiscard]] std::optional<std::filesystem::path> original_archive(AppState& state, const std::filesystem::path& archive);
void set_original_archive(AppState& state, const std::filesystem::path& archive, const std::filesystem::path& file);
void set_test_install(AppState& state, const std::filesystem::path& folder);
// Saves, then builds both archives into dist/<build-id>/.
void build_project_archives(AppState& state);
void deploy_project_build(AppState& state, const std::string& build);
void roll_back_project_build(AppState& state, const std::string& build);
// A deployment's state, hashed once per change (deploying, rolling back).
[[nodiscard]] csf::DeploymentState cached_deployment_state(AppState& state, const csf::ProjectDeployment& deployment);
void add_playtest(AppState& state, const std::string& build, bool worked, const std::string& note);

// Runs Blender on the terrain asset's .blend (made from its export when there
// is none yet) with the CSF add-on and the project set; Send there rebuilds here.
void edit_in_blender(AppState& state);

} // namespace rwsman
