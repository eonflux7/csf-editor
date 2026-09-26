#pragma once

#include "csf/authoring_project.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// An authoring project's life outside the editor's panels
// (docs/plans/editor-ux-redesign.md, E8 and E9): creating one in a shipped
// mission's slot, building its two archives (the mission and GlobalEK's
// texts) into dist/<build-id>/, deploying them into a test install with
// rollback records, and the playtest log. GUI-free; `csf-mod project-new`,
// `project-archives` and `project-deploy` run the same code.
namespace csf {

// A shipped mission whose slot a project can take: its scene and archive, and
// the map beside the scene that the project's World replaces.
struct MissionSlot {
    std::string mission;                              // Convoy
    std::filesystem::path scene;                      // Maps/FR03/Convoy.scn
    std::filesystem::path archive;                    // maps/Convoy.pak
    std::filesystem::path visual_map, collision_map;  // Maps/FR03/FR03.rws, Maps/FR03/FR03_col.rws
};
// The corpus' missions with a <mission>.scn beside a map, by name.
[[nodiscard]] std::vector<MissionSlot> mission_slots(const std::filesystem::path& corpus);

struct NewProjectOptions {
    std::filesystem::path directory;  // must not exist, or be empty
    std::string name;                 // display name (UTF-8)
    std::string slot;                 // a mission of mission_slots(corpus)
    std::filesystem::path corpus;
    // Other projects (each folder with project.csfproj), so the text ID range
    // is one no other project of the slot uses; empty: only the donor's IDs count.
    std::filesystem::path projects_root;
    // A flat starter terrain this many centimetres a side, made of the donor
    // map's most used ground texture and surface; 0: none.
    float terrain_size{8000.0F};
    std::filesystem::path blender, test_install;  // local settings, optional
    std::string tool_version;
};
struct NewProject {
    AuthoringProject project;
    std::vector<std::string> lines;  // what was made
};
// Writes project.csfproj and local.csfproj, the starter terrain and its build,
// and the mission workspace mission/ with the slot emptied (new_mission) and
// the built map files registered. Throws std::runtime_error with the reason.
NewProject create_authoring_project(const NewProjectOptions& options);

// A free text ID range of 100 IDs for `file` (Texts/Convoy.fli): above the
// donor's own numeric IDs and clear of the other projects' ranges.
[[nodiscard]] std::pair<std::int32_t, std::int32_t> free_text_range(const std::filesystem::path& corpus,
                                                                    const std::filesystem::path& texts_archive,
                                                                    const std::filesystem::path& texts_file,
                                                                    const std::filesystem::path& projects_root,
                                                                    const std::filesystem::path& except = {});

// The untouched shipped archive for `archive` (game-relative): the project's
// `original` setting, else the oldest deployment backup of it in the test
// install (what it held before the first deployment), else the test
// install's own file when no deployment backed it up. Nothing when unknown.
[[nodiscard]] std::optional<std::filesystem::path> find_original_archive(const AuthoringProject& project,
                                                                         const std::filesystem::path& archive);

struct ArchiveBuild {
    std::string id;                  // yyyymmdd-hhmmss
    std::filesystem::path directory; // dist/<id>
    std::filesystem::path mission_archive;
    std::filesystem::path texts_archive;  // empty without project strings
    std::vector<std::string> lines;
};
// Brings the map, texts and lightmaps up to date, then writes both archives
// and build.json (hashes of the archives and their originals) into a new
// dist/<id>/. The mission archive packages the saved mission workspace; the
// texts archive packages build/<texts archive stem>/<file> through the texts/
// workspace (made on first use). Saves the project's build records.
ArchiveBuild build_archives(AuthoringProject& project, const std::filesystem::path& original_mission,
                            const std::filesystem::path& original_texts, const std::string& tool_version = {});
// The builds in dist/, newest first.
[[nodiscard]] std::vector<std::string> archive_builds(const AuthoringProject& project);

// Deploys a build's archives into the test install (each with a rollback
// state file, recorded in the project's local settings; save them). Throws
// when an archive cannot be deployed; one that was deployed before the
// failure is rolled back.
std::vector<ProjectDeployment> deploy_build(AuthoringProject& project, const std::string& build,
                                            const std::filesystem::path& test_install);
enum class DeploymentState : std::uint8_t {
    active,       // the test install holds the deployed archive
    rolled_back,  // it holds what was there before
    replaced,     // something else has replaced it since
    missing,      // the state file or the target is gone
};
[[nodiscard]] DeploymentState deployment_state(const ProjectDeployment& deployment);
[[nodiscard]] const char* deployment_state_name(DeploymentState state) noexcept;
// Rolls back every active deployment of `build` (newest first).
void roll_back_build(const AuthoringProject& project, const std::string& build);

} // namespace csf
