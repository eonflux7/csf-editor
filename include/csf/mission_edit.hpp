#pragma once

#include "csf/animation_catalog.hpp"
#include "csf/document.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "csf/program.hpp"
#include "csf/tree.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace csf {

class ModProject;

// Outcome of one editor operation. A rejected operation leaves every file
// unchanged; `message` then explains why.
struct EditResult {
    bool applied{};
    std::string message;
    std::vector<std::string> warnings;

    explicit operator bool() const noexcept { return applied; }
};

enum class MissionFileKind {
    scene,
    mission_script,
    cutscene_script,
    objects,
    animations,
    weapons,
    database,
    model_index,
    animation_index,
    texture_index,
    physics_index,
    visual_index,
    visual_map, // the map's RenderWare stream, edited in place (scene instances)
    asset,
};

struct MissionFile {
    MissionFileKind kind{MissionFileKind::asset};
    std::filesystem::path relative_path; // package-relative, exactly as stored in the mission PAK
    std::filesystem::path source_path;   // file the current baseline was read from; empty if added
    std::string source_sha256;           // hash of the unedited source package file; empty if added
    std::optional<Tree> tree;            // CSFFBS files
    std::vector<std::byte> raw;          // everything else
    bool present{true};                  // false after undoing the operation that added it
    bool added{};                        // does not exist in the source package
    std::uint64_t revision{};            // increments whenever the content changes

    [[nodiscard]] std::vector<std::byte> bytes() const;
};

struct ActorPlacement {
    Vec3 position;
    float heading_degrees{};
    float pitch_degrees{};
};

// A new actor with every field given (MissionEditor::add_actor_record).
struct ActorSpec {
    std::optional<std::int32_t> id;  // the next free ID when empty
    std::string name;                // UTF-8
    std::int32_t class_id{};
    ActorPlacement placement;
    // Its placement point; none (-1 -1, as prop actors in shipped missions)
    // when empty.
    std::optional<std::pair<std::int32_t, std::int32_t>> cell;
    std::vector<std::int32_t> scripts;
    std::optional<std::string> portrait;  // UTF-8, e.g. Menus\\Retratos\\FotoOficial.fbs
};

struct NavPointSpec {
    Vec3 position;
    float rotation_radians{};
    float pitch_radians{};
};

struct ActorAnimationOverride {
    std::int32_t animation_id{};
    std::string slot; // UTF-8; for example DISTRAIDO_IDLE_ARMA1
    friend bool operator==(const ActorAnimationOverride&, const ActorAnimationOverride&) = default;
};

struct LightEdit {
    std::optional<Vec3> position;
    std::optional<std::int32_t> color;
    std::optional<std::int32_t> modulate;
    std::optional<float> radius;
};

using ScalarValue = std::variant<std::int32_t, float, std::string>; // strings are UTF-8

// Non-destructive, transactional mission editing over canonical CSFFBS trees.
//
// Every operation either applies completely (one undo step) or changes
// nothing. Records are addressed by their stable gameplay IDs, never by entry
// index, because structural edits renumber entries. Unedited files serialize
// byte-for-byte as shipped; edited files follow the same canonical layout.
// Why an area polygon would break the level load, one message per problem
// (KB-scn-9): the loader triangulates every area, and its only gate misses
// repeated points, zero-length edges and collinear runs, which then fault the
// convex decomposition. Checked in the XZ plane, where the zone test works.
[[nodiscard]] std::vector<std::string> area_polygon_problems(const std::vector<Vec3>& points);

class MissionEditor {
public:
    // `scene` is the mission .scn inside `package_root` (the unpacked mission
    // PAK). Files that `project` has authored replace their source copies.
    [[nodiscard]] static MissionEditor open(const std::filesystem::path& scene,
                                            const std::filesystem::path& package_root,
                                            const ModProject* project = nullptr,
                                            const ResourceIndex* prepared_index = nullptr);
    MissionEditor(const MissionEditor&) = delete;
    MissionEditor& operator=(const MissionEditor&) = delete;
    MissionEditor(MissionEditor&&) noexcept = default;
    MissionEditor& operator=(MissionEditor&&) noexcept = default;

    [[nodiscard]] const std::filesystem::path& package_root() const noexcept { return package_root_; }
    [[nodiscard]] const std::vector<MissionFile>& files() const noexcept { return files_; }
    // Case-insensitive, separator-insensitive lookup of a present file.
    [[nodiscard]] std::optional<std::size_t> find_file(const std::filesystem::path& relative) const;
    [[nodiscard]] std::optional<std::size_t> file_of_kind(MissionFileKind kind) const;
    // `base` (the package's index) with every present file that is not read
    // from its package path overlaid: project and imported files resolve to the
    // file their content came from, whose directory also holds their textures.
    [[nodiscard]] ResourceIndex resource_index(const ResourceIndex& base) const;
    [[nodiscard]] std::size_t scene_file() const noexcept { return scene_file_; }
    // Parsed view of a CSFFBS file; its source path is the package path.
    [[nodiscard]] Document document(std::size_t file) const;
    [[nodiscard]] std::filesystem::path package_path(std::size_t file) const;

    // Typed projections of the current state, rebuilt lazily after changes.
    // The returned references stay valid until the next change.
    [[nodiscard]] const Document& scene_document() const;
    [[nodiscard]] const MissionScene& scene() const;
    [[nodiscard]] const ObjectDatabase& objects() const;
    [[nodiscard]] const AnimationCatalog& animations() const;

    // Increments after every applied change, undo and redo.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    // Files whose content differs from the source package (or that are new).
    [[nodiscard]] std::vector<std::size_t> modified_files() const;
    [[nodiscard]] bool dirty() const noexcept { return saved_position_ != cursor_; }

    [[nodiscard]] bool can_undo() const noexcept { return cursor_ > 0; }
    [[nodiscard]] bool can_redo() const noexcept { return cursor_ < history_.size(); }
    bool undo();
    bool redo();
    [[nodiscard]] std::vector<std::string> history_labels() const;
    [[nodiscard]] std::size_t history_position() const noexcept { return cursor_; }

    // Actors. Moving an actor also moves its placement navigation point (the
    // CELDA point that mirrors its POS and heading in shipped missions).
    EditResult set_actor_placement(std::int32_t actor_id, const ActorPlacement& placement);
    EditResult set_actor_class(std::int32_t actor_id, std::int32_t class_id, bool force = false);
    // Gives the actor the model (body, LOD and bounding box) of another class
    // while keeping its behaviour: the actor moves to a copy of its class with
    // that look (new IDs from 500), or to an existing class that matches, and
    // the physics descriptor gets an entry for the copy. A copy nothing uses
    // any more is removed.
    EditResult set_actor_look(std::int32_t actor_id, std::int32_t look_class_id);
    EditResult set_actor_name(std::int32_t actor_id, std::string_view utf8);
    // .COLISION, .FLAGS or .SEGUNDA_EXPLOSION.
    EditResult set_actor_integer(std::int32_t actor_id, std::string_view field, std::int32_t value);
    EditResult set_actor_faction(std::int32_t actor_id, std::optional<std::string> utf8);
    EditResult set_actor_scripts(std::int32_t actor_id, std::vector<std::int32_t> script_ids,
                                 bool force = false);
    EditResult set_actor_animations(std::int32_t actor_id,
                                    std::vector<ActorAnimationOverride> overrides,
                                    bool force = false);
    EditResult duplicate_actor(std::int32_t actor_id, Vec3 offset,
                               std::int32_t* new_id = nullptr);
    // A new actor gets a placement point in `navigation_group`, or in the group
    // of the nearest placed actor when omitted.
    EditResult add_actor(std::int32_t class_id, const ActorPlacement& placement,
                         std::string_view utf8_name, std::optional<std::int32_t> navigation_group = {},
                         std::int32_t* new_id = nullptr);
    // Refuses when scripts or mission properties still reference the actor,
    // unless `force` is set.
    EditResult delete_actor(std::int32_t actor_id, bool force = false);
    // An actor with every field given; its cell must name an existing point.
    EditResult add_actor_record(const ActorSpec& spec, std::int32_t* new_id = nullptr);

    // Runs several operations as one undo step. When `body` returns a result
    // that did not apply (its operation was rejected), everything it applied
    // is undone and forgotten.
    EditResult batch(std::string label, const std::function<EditResult()>& body);
    // Empties the scene for a new mission in this slot: actors, effects,
    // water, navigation, dummies, areas, lights, scene objects, bridges and
    // their folders, and every script of the mission and cutscene programs.
    // The environment (.MUNDOVIS), player and scores, and the databases stay.
    EditResult new_mission();

    // Dummies, lights, navigation and areas.
    EditResult set_dummy_placement(std::int32_t dummy_id, Vec3 position, float rotation_radians,
                                   float pitch_radians);
    EditResult duplicate_dummy(std::int32_t dummy_id, Vec3 offset, std::int32_t* new_id = nullptr);
    EditResult delete_dummy(std::int32_t dummy_id, bool force = false);
    EditResult add_dummy(std::string_view utf8_name, Vec3 position, float rotation_radians, float pitch_radians,
                         std::optional<std::int32_t> id = {}, std::int32_t* new_id = nullptr);
    EditResult set_light(std::int32_t light_id, const LightEdit& edit);
    EditResult duplicate_light(std::int32_t light_id, Vec3 offset, std::int32_t* new_id = nullptr);
    EditResult delete_light(std::int32_t light_id);
    // Moving a placement point moves the actor placed on it.
    EditResult set_navigation_point(std::int32_t group_id, std::int32_t point_id, Vec3 position,
                                    std::optional<float> rotation_radians = {});
    EditResult add_navigation_point(std::int32_t group_id, Vec3 position,
                                    std::int32_t* new_point_id = nullptr);
    EditResult delete_navigation_point(std::int32_t group_id, std::int32_t point_id,
                                       bool force = false);
    EditResult connect_navigation_points(std::int32_t origin_group, std::int32_t origin_point,
                                         std::int32_t destination_group,
                                         std::int32_t destination_point);
    EditResult disconnect_navigation_points(std::int32_t origin_group, std::int32_t origin_point,
                                            std::int32_t destination_group,
                                            std::int32_t destination_point);
    // A group (.TIPO 0 for routes and walk grids, 3 for cover) with its points
    // (IDs 1..n) and links between them (pairs of point IDs).
    EditResult add_navigation_group(std::string_view utf8_name, std::int32_t type,
                                    const std::vector<NavPointSpec>& points,
                                    const std::vector<std::pair<std::int32_t, std::int32_t>>& links,
                                    std::optional<std::int32_t> id = {}, std::int32_t* new_id = nullptr);
    // Refuses while actors stand on its points or scripts name it, unless
    // `force`; its cross-group links go with it.
    EditResult delete_navigation_group(std::int32_t group_id, bool force = false);
    EditResult set_area_point(std::int32_t area_id, std::size_t index, Vec3 position);
    EditResult insert_area_point(std::int32_t area_id, std::size_t index, Vec3 position);
    EditResult remove_area_point(std::int32_t area_id, std::size_t index);
    EditResult set_area_height(std::int32_t area_id, float height);
    // A zone polygon (XZ, at the points' height) `height` tall.
    EditResult add_area(std::string_view utf8_name, float height, const std::vector<Vec3>& points,
                        std::optional<std::int32_t> id = {}, std::int32_t* new_id = nullptr);
    // Refuses while scripts name the zone, unless `force`.
    EditResult delete_area(std::int32_t area_id, bool force = false);

    // Static map props: the CSF scene-instance records in the visual map
    // stream, addressed by record offset. Only the rotation and position are
    // rewritten in place; the collision map (a baked BSP) does not follow.
    EditResult set_map_instance_transform(std::uint64_t instance_offset,
                                          const std::array<float, 9>& rotation, Vec3 position);
    struct MapInstanceTransform {
        std::array<float, 9> rotation{}; // RwMatrix right, up and at vectors
        Vec3 position;
    };
    [[nodiscard]] std::optional<MapInstanceTransform> map_instance_transform(std::uint64_t instance_offset) const;

    // Mission properties.
    EditResult set_player_actor(std::int32_t actor_id);
    EditResult set_start_availability(bool commando, bool sniper, bool spy);
    EditResult set_scores(std::int32_t maximum, std::int32_t minimum);
    // A scalar field of .MUNDOVIS (environment); the value kind must match.
    EditResult set_environment(std::string_view field, const ScalarValue& value);

    // Any scalar of a CSFFBS file, addressed by its entry index in the current
    // document(file). The value kind must match the stored kind.
    EditResult set_scalar(std::size_t file, std::uint32_t entry_index, const ScalarValue& value);

    // Scripts, as recompilable source text (see csf/source_text.hpp).
    [[nodiscard]] std::optional<std::string> script_text(std::size_t file,
                                                         std::int32_t script_id) const;
    EditResult set_script_text(std::size_t file, std::int32_t script_id, std::string_view text);
    EditResult add_script(std::size_t file, std::string_view text, std::int32_t* new_id = nullptr);
    EditResult delete_script(std::size_t file, std::int32_t script_id, bool force = false);
    [[nodiscard]] std::int32_t next_script_id(std::size_t file) const;
    [[nodiscard]] static std::string script_template(std::int32_t id, std::string_view utf8_name);
    // Scripts that an actor may run: non-trigger scripts of the mission program.
    [[nodiscard]] std::vector<std::pair<std::int32_t, std::string>> actor_script_choices() const;

    // Cross-mission import from another unpacked mission package. Copies the
    // database record and every file and index entry it needs that this
    // package lacks. Already present data is left untouched.
    EditResult import_class(const std::filesystem::path& donor_package_root, std::int32_t class_id);
    EditResult import_animation(const std::filesystem::path& donor_package_root,
                                std::int32_t animation_id);

    // Writes every modified file under `project.workspace_root/authored`, with
    // a change manifest, registers it in the project and saves the project.
    std::vector<std::filesystem::path> save(ModProject& project);

    // How many script operands reference a record, for delete checks and UI.
    [[nodiscard]] std::size_t script_references(std::string_view tag, std::int32_t id) const;

private:
    friend class MissionTransaction;
    // Equal-size raw edits (the map stream) keep only the changed byte runs.
    struct BytePatch {
        std::size_t offset{};
        std::vector<std::byte> before, after;
    };
    struct FileSnapshot {
        std::size_t file{};
        bool present_before{}, present_after{};
        std::vector<std::byte> before, after;
        std::vector<BytePatch> patches;
    };
    struct HistoryEntry {
        std::string label;
        std::vector<FileSnapshot> files;
    };
    struct Cache;

    MissionEditor();
    void restore(const FileSnapshot& snapshot, bool forward);
    void changed(std::size_t file);
    std::size_t add_file(MissionFile file);

    std::filesystem::path package_root_;
    std::vector<MissionFile> files_;
    std::vector<std::vector<std::byte>> baselines_; // source package bytes per file
    std::size_t scene_file_{};
    std::vector<HistoryEntry> history_;
    std::size_t cursor_{};
    std::size_t saved_position_{};
    std::uint64_t revision_{};
    std::shared_ptr<Cache> cache_;
};

// Which mission a mod project edits: stored beside the project as
// .csf-mission (local, like the project state) so the project can be reopened.
struct MissionProjectInfo {
    std::filesystem::path scene;            // package-relative .scn path
    std::filesystem::path original_archive; // shipped maps/<Mission>.pak, if known
};
[[nodiscard]] std::optional<MissionProjectInfo> read_mission_project_info(
    const std::filesystem::path& workspace);
void write_mission_project_info(const std::filesystem::path& workspace, const MissionProjectInfo& info);

// Animation slot names recovered from the CommXPC.exe slot table, sorted.
[[nodiscard]] std::span<const std::string_view> animation_slot_names() noexcept;
[[nodiscard]] const char* mission_file_kind_name(MissionFileKind kind) noexcept;

} // namespace csf
