#pragma once

#include "rws/decoded.hpp"
#include "rws/world_source.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
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

} // namespace csf
