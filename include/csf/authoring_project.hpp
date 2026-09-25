#pragma once

#include "rws/decoded.hpp"
#include "rws/world_source.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace csf {

// An authoring project (docs/plans/editor-project-format.md): the Blender
// sources and the World placements of a mission, around the csf-mod
// workspaces that package it (`mission/`, `texts/`). `project.csfproj` is
// shared; `local.csfproj` holds machine-specific paths. Project paths are
// relative to the project directory; donor paths to the mission package.

struct HeightRule {
    enum class Mode : std::uint8_t { absolute, ground, on };
    Mode mode{Mode::absolute};
    float offset{};
    std::string support_kind;  // for `on`: "actor", "building", "piece" or "prop"
    std::string support_id;
};

struct ProjectAsset {
    enum class Kind : std::uint8_t { terrain, building };
    std::string id;
    Kind kind{Kind::terrain};
    std::filesystem::path blend, export_path;
    // From the asset-export record, once the asset has been exported.
    std::string exporter;
    unsigned exporter_version{};
    std::string export_hash;  // sha256 of the export
};

struct ProjectPlacement {
    enum class Kind : std::uint8_t { building, piece, prop };
    Kind kind{Kind::prop};
    std::string id;
    std::string asset;                           // building
    std::vector<std::uint32_t> donor_instances;  // prop
    rws::Vec3 box_min, box_max;                  // piece
    rws::Vec3 position;                          // the last resolved height is position.y
    float yaw_degrees{};
    HeightRule height;
};

struct ProjectAnchor {
    std::int32_t actor_id{};
    HeightRule height;
};

// A string of the mission's text file (GlobalEK Texts/<mission>.fli), UTF-8.
struct ProjectText {
    std::string id;  // digits, within the project's reserved range
    std::string text;
};

// A baked lightmap of the project's World (the Blender add-on's bake): the
// PNG source, built into a DXT1 DDS at <map folder>/Textures/<name>.dds.
struct ProjectLightmap {
    std::string name;  // e.g. TERRAIN_Lm, as the World materials name it
    std::filesystem::path source;
};

struct ProjectOutput {
    std::filesystem::path path;
    std::string kind;  // "world" or "sectors"
    std::string hash, inputs_hash;
};

struct ProjectLocal {
    std::filesystem::path corpus;  // unpacked game resources, one folder per mission
    std::filesystem::path blender, test_install;
};

// A placement or anchored actor whose height rule resolves to a height more
// than 1 cm from its stored one, or does not resolve at all.
struct HeightFinding {
    enum class Subject : std::uint8_t { placement, actor };
    Subject subject{Subject::placement};
    std::string id;           // placement ID, or the actor ID as text
    std::int32_t actor_id{};  // for actors
    rws::Vec3 position;       // stored
    std::optional<float> resolved;
    std::string problem;      // why it did not resolve
};

// Scene actor positions by gameplay ID (for anchors and `on actor` supports).
using ActorPositions = std::map<std::int32_t, rws::Vec3>;

struct ProjectBuildReport {
    bool rebuilt{};
    std::vector<std::string> lines;  // one per output: what was built or why it was kept
};

class AuthoringProject {
public:
    static constexpr unsigned format_version = 1;

    std::filesystem::path directory;
    std::string name;
    struct Slot {
        std::string mission;             // Convoy
        std::filesystem::path scene;     // Maps/FR03/Convoy.scn
        std::filesystem::path archive;   // maps/Convoy.pak
    } slot;
    struct Texts {
        std::filesystem::path archive, file;
        std::int32_t first{}, last{};
    };
    std::optional<Texts> texts;
    struct DonorMap {
        std::filesystem::path visual, collision;  // package-relative
    } donor_map;
    std::vector<ProjectAsset> assets;
    std::vector<ProjectPlacement> placements;
    std::vector<ProjectAnchor> anchors;
    std::vector<ProjectText> strings;
    std::vector<ProjectLightmap> lightmaps;
    std::vector<ProjectOutput> outputs;
    ProjectLocal local;

    // Reads project.csfproj and, when present, local.csfproj. Errors name the
    // file and line.
    [[nodiscard]] static AuthoringProject load(const std::filesystem::path& directory);
    [[nodiscard]] static AuthoringProject parse(std::string_view project_text, std::string_view local_text = {});
    [[nodiscard]] std::string project_text() const;
    [[nodiscard]] std::string local_text() const;
    // Writes both files (each through a temporary file and a rename).
    void save() const;

    // Unique IDs, known assets, `on` references between placements without
    // cycles. Empty when the project is consistent.
    [[nodiscard]] std::vector<std::string> check() const;

    [[nodiscard]] std::filesystem::path package_root() const;  // local.corpus / slot.mission
    // Built into build/: the visual and collision maps at the donor map's
    // paths and Maps/Secs/<mission>.sec, plus build/world.csfworld (the merged
    // source). Outputs are rebuilt only when their inputs or files changed (or
    // `force`); their records are updated in memory, so save() afterwards.
    ProjectBuildReport build_world(bool force = false);
    // The mission's text file with the project's strings appended to the
    // donor's (GlobalEK in the corpus), into build/<archive stem>/<file>, when
    // stale. Nothing to do without a texts record and strings.
    ProjectBuildReport build_texts(bool force = false);
    // Every lightmap's DDS, into build/<map folder>/Textures/<name>.dds, when
    // stale. The mission must list and package them too: see
    // lightmap_package_path and MissionEditor::add_texture_list_entries.
    ProjectBuildReport build_lightmaps(bool force = false);
    // The package path of a lightmap's DDS (Maps/FR03/Textures/<name>.dds).
    [[nodiscard]] std::filesystem::path lightmap_package_path(const ProjectLightmap& lightmap) const;
    // The next free string ID in the reserved range, if any is left.
    [[nodiscard]] std::optional<std::string> next_text_id() const;

    // Resolves every height rule against the ground: the collision faces of
    // the terrain and building assets (props and pieces are not ground), and
    // `on` supports at their resolved heights. Returns what is off by more than
    // 1 cm or unresolved; absolute placements and unanchored actors are skipped.
    [[nodiscard]] std::vector<HeightFinding> height_report(const ActorPositions& actors = {}) const;
    // Stores the resolved height of each placement finding. Actor findings are
    // left to the caller, which moves the actors in the mission.
    void resnap(const std::vector<HeightFinding>& findings);

private:
    [[nodiscard]] rws::WorldSource merged_source(bool with_donor_placements) const;
};

[[nodiscard]] const char* height_mode_name(HeightRule::Mode mode) noexcept;

// A `.fli` text file: UTF-16 LE with a BOM and CRLF lines; each string is an
// ID line, the quoted string and a blank line. Returns the donor with
// `strings` appended (their IDs must be new to it), as texts.py wrote it.
[[nodiscard]] std::vector<std::byte> append_fli_strings(std::span<const std::byte> donor,
                                                        const std::vector<ProjectText>& strings);

class MissionScene;

// The reference a modelling tool shows around its sources (the Blender
// add-on's "Load reference"), as JSON in game units: the project's placements,
// and the scene's actors (with `actor_models`: class ID -> model file name),
// navigation groups and links, areas and dummies. Nothing in it is edited.
[[nodiscard]] std::string reference_markers_json(const AuthoringProject& project, const MissionScene* scene,
                                                 const std::map<std::int32_t, std::string>& actor_models);

} // namespace csf
