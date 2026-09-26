#pragma once

#include "rwsman/chunk_lookup.hpp"
#include "rwsman/commands.hpp"
#include "rwsman/diagnostics.hpp"
#include "rwsman/discovery.hpp"
#include "rwsman/history.hpp"
#include "rwsman/log.hpp"
#include "rwsman/problems.hpp"
#include "rwsman/search_index.hpp"
#include "rwsman/settings.hpp"
#include "csf/animation_catalog.hpp"
#include "csf/authoring_project.hpp"
#include "csf/mission_flow.hpp"
#include "csf/mission_recipes.hpp"
#include "csf/cmo.hpp"
#include "csf/document.hpp"
#include "csf/mission_edit.hpp"
#include "csf/mod_project.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "csf/program.hpp"
#include "geometry_preview.hpp"
#include "rws/animation.hpp"
#include "rws/document.hpp"
#include "rws/world_queries.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

struct GLFWwindow;

namespace rwsman {

class MissionLoader;
struct PendingDialog;

// Loaded actor and weapon model prototypes by normalized resource path.
struct ActorModelCache {
    std::unordered_map<std::string, std::shared_ptr<const rws::Document>> models;
};

enum class Workspace {
    mission,
    script,
    animation,
    scene,
    geometry,
    inspector,
};

[[nodiscard]] const char* workspace_name(Workspace workspace);

// The dockable panels. Each workspace docks its own set of them
// (ui/layout.cpp); the modes Mission, Script and Inspect group the workspaces.
enum class Panel : std::uint8_t {
    explorer,
    center,
    inspector,
    render_settings,
    console,  // titled Output
    diagnostics,
    references,
    changes,
    missions,
    // Mission mode (docs/plans/editor-ux-redesign.md, §3 B).
    outliner,
    properties,
    assets,
    problems,
    history,
    texts,
    timeline,
    mission_settings,
    objectives,
    behaviours,
    build,
    flow,
    count
};
inline constexpr std::size_t panel_count = static_cast<std::size_t>(Panel::count);

enum class Mode : std::uint8_t { mission, script, inspect };
[[nodiscard]] constexpr Mode mode_of(const Workspace workspace) {
    return workspace == Workspace::mission  ? Mode::mission
           : workspace == Workspace::script ? Mode::script
                                            : Mode::inspect;
}

// Stable identity of a scene record. Entry indices shift when records are added
// or removed, so selections are carried across edits by gameplay ID.
struct MissionRecordKey {
    // `placement`: a project placement (project.csfproj), `id` its index.
    enum class Kind : std::uint8_t { none, actor, dummy, light, area, effect, nav_group, nav_point, placement };
    Kind kind{Kind::none};
    std::int32_t id{}, sub_id{};
    friend bool operator==(const MissionRecordKey&, const MissionRecordKey&) = default;
};

// Everything that exists only while a CSF mission is open. Resetting the
// struct closes the mission.
struct MissionState {
    std::unique_ptr<csf::MissionGraph> graph;
    // The graph's index with the project's and imported files overlaid
    // (MissionEditor::resource_index); resolve assets through it.
    csf::ResourceIndex resources;
    std::unique_ptr<csf::Document> document;
    std::unique_ptr<csf::MissionScene> scene;
    std::unique_ptr<csf::MissionSymbolIndex> symbols;
    std::unique_ptr<csf::ObjectDatabase> objects;
    std::unique_ptr<csf::AnimationCatalog> animations;
    csf::ScriptAnimationIndex script_animations;
    std::vector<std::pair<std::filesystem::path, csf::CutsceneTimeline>> cutscenes;
    std::vector<std::pair<std::filesystem::path, csf::ProgramDocument>> programs;
    std::vector<csf::Document> program_documents;
    csf::ProgramReferenceIndex program_references;
    std::vector<csf::ActorAssociation> actor_associations;
    std::unique_ptr<csf::WeaponDatabase> weapons;
    // Actor and weapon models by normalized path, shared by loads and edits.
    std::shared_ptr<ActorModelCache> model_cache;

    // Mission editing. The editor owns the authoritative scene, programs and
    // databases; the projections above are rebuilt from it after every change
    // (editor->revision() != applied_revision). `project` is the mod project the
    // edits are saved into, once there is one.
    std::unique_ptr<csf::MissionEditor> editor;
    std::unique_ptr<csf::ModProject> project;
    std::uint64_t applied_revision{};
    std::filesystem::path original_archive; // shipped maps/<Mission>.pak for export
    // A camera to restore once the preview has reloaded its scene (a map
    // rebuilt under an open mission).
    std::optional<CameraSnapshot> restore_camera;
    // Applied by the next refresh, once the views show the edit that created
    // the target: a scene record to select, and a script (program, ID).
    std::optional<MissionRecordKey> pending_selection;
    std::optional<std::pair<std::size_t, std::int32_t>> pending_script;
    // Revision of the editor's visual map the loaded map document reflects.
    std::uint64_t map_revision{};

    // Selected-actor animation playback.
    std::shared_ptr<const rws::AnimationClip> active_clip;
    std::optional<std::uint32_t> animated_actor;
    std::string active_animation;
    float animation_time{}, animation_speed{1.0F}, animation_accumulator{};
    int animation_fps{30};
    bool animation_playing{}, animation_loop{true};

    void reset_playback() {
        active_clip.reset();
        animated_actor.reset();
        active_animation.clear();
        animation_playing = false;
    }
};

// An authoring project (project.csfproj) around the open mission: its map is
// rebuilt in the background when a Blender export changes, and the height
// report lists what no longer stands where its height rule says.
struct AuthoringSession {
    std::unique_ptr<csf::AuthoringProject> project;
    // Export modification times the last build saw, to notice new exports.
    std::map<std::filesystem::path, std::filesystem::file_time_type> export_times;
    // project.csfproj as rws-man last read or wrote it; another writer (the
    // Blender add-on registering an asset) makes it reload the project.
    std::filesystem::file_time_type project_time;
    // project.csfproj's text as last saved: edits stay in memory until Save
    // (Ctrl+S saves the mission and the project together).
    std::string saved_text;
    // When Blender's Send last changed an export ("HH:MM"), for the status bar.
    std::string last_send;
    double next_check{};
    struct Outcome {
        std::optional<csf::AuthoringProject> project;
        csf::ProjectBuildReport report;
        std::vector<csf::HeightFinding> findings;
        std::string error;
        std::string started_text;  // project.csfproj as the build started from it
    };
    std::future<Outcome> job;
    // Started by poll_authoring once the mission views are current, so the
    // report sees the actors where the mission has them.
    bool job_queued{}, job_force{};
    std::vector<csf::HeightFinding> findings;
    bool checked{};  // a check has finished since the project was opened
    bool show_heights{};

    // Project edits (placements, texts, resnaps) as undo steps beside the
    // mission editor's (docs/plans/editor-ux-redesign.md, E2). Each keeps the
    // project text before and after, and the mission editor's history
    // position it followed: undo takes whichever step is newest.
    struct Step {
        std::string label, before, after;
        std::size_t anchor{};
    };
    std::vector<Step> steps;
    std::size_t cursor{};
};

// The mission editing panel's Assets and Presets tabs (editor plan stage 4).
struct AuthoringTools {
    // The New trigger form (E10) and whether unverified events and actions are offered.
    csf::Trigger trigger_draft{csf::Trigger::When::enter_zone, 0, {}, 0.0F, std::nullopt,
                               {{csf::TriggerAction::Kind::complete_objective, 1, {}}}, {}, {}};
    bool show_unverified{};
    // Classes of every discovered mission, for placing and importing.
    struct CatalogEntry {
        std::filesystem::path package;  // the mission package root it comes from
        std::string package_name;
        std::int32_t class_id{};
        std::string name, type;
    };
    std::vector<CatalogEntry> catalog;
    std::filesystem::path catalog_root;  // the resource root it was built for
    std::array<char, 96> asset_filter{};
    int asset_category{};  // the Assets panel's chip: 0 all, then characters, vehicles, props, pickups, buildings
    // What an asset row being dragged into the viewport places.
    std::optional<CatalogEntry> drag_entry;
    std::string drag_building;
    // Ground heights of the open mission (its collision map), built on demand.
    std::shared_ptr<const rws::GroundQuery> ground;
    const rws::Document* ground_source{};  // the collision document it was built from
    // The preset being set up.
    enum class Preset : std::uint8_t { guard_patrol, guard_idle, animal_patrol, cover_group, walk_grid };
    Preset preset{Preset::guard_patrol};
    int class_id{};
    std::array<char, 64> name{}, route_name{}, script_name{};
    std::vector<csf::Vec3> points;  // route or cover points, collected from the view
    float heading{}, pause{3.0F}, cover_facing{90.0F};
    int cover_group{};              // 0: none
    std::array<char, 128> idle_loop{};  // "1881:2-4,1385"
    int walk_animation{};
    float grid_spacing{1000.0F}, grid_avoid{300.0F};
    // The walk grid the preset would make, drawn in the viewport while
    // previewing, and what it was computed from.
    bool preview_grid{};
    std::vector<csf::Vec3> grid_points;
    std::vector<std::pair<std::int32_t, std::int32_t>> grid_links;
    std::tuple<float, float, std::uint64_t> grid_key{-1.0F, -1.0F, 0};

    // Viewport tools (Phase 2): the asset the Place tool places (a class, or
    // a project building asset), its heading and options, and the points of
    // a route, zone or cover group being drawn.
    std::optional<CatalogEntry> place_entry;
    std::string place_building_asset;
    float place_heading{};
    bool place_random{}, place_keep{};
    std::vector<csf::Vec3> sketch;
    bool route_loop{true};
    float zone_height{300.0F};

    // The Flow tab's view of the mission programs, rebuilt when they change.
    std::shared_ptr<const csf::MissionFlow> flow;
    std::uint64_t flow_revision{~std::uint64_t{}};
    // The Objectives tab: forms for the objectives, the players' starting
    // equipment and the mission tips. With an authoring project open the
    // text fields hold the strings themselves; otherwise FLI string IDs.
    struct ObjectiveForm {
        int kind{};  // csf::Objective::Kind
        int target{};
        bool secondary{};
        std::array<char, 160> label{}, done{}, prompt{};
    };
    std::vector<ObjectiveForm> objectives;
    std::array<char, 32> success_message{"g014"};
    struct KitForm {
        int actor{};
        std::array<char, 128> weapons{};  // <class>[@<ammo>/<ammo>],...
        int selected{}, disguise{};
    };
    std::vector<KitForm> kits;
    std::array<char, 128> tips{};  // FLI IDs, comma-separated
    std::array<char, 160> new_text{};

    // The Cutscene tab: travelling shots captured from the viewport.
    struct ShotForm {
        csf::Vec3 camera, target;
        float travel_x{-200.0F}, travel_z{};  // cm the camera moves during the shot
        float seconds{4.0F};
    };
    std::vector<ShotForm> shots;
    bool send_init{true};
    // Playing the shots in the viewport: seconds since start, when playing.
    std::optional<double> preview_started;
};

// Transient UI state that is not part of a document.
struct UiState {
    bool maximize_viewport{};     // Ctrl+Space: hide every panel but the center.
    bool reset_layout{};          // Rebuild the current workspace's default layout.
    bool screenshot_requested{};  // F12: save the back buffer after this frame renders.
    // Panels the user has not closed (the Render settings panel starts closed).
    std::array<bool, panel_count> panel_open = [] {
        std::array<bool, panel_count> open{};
        open.fill(true);
        open[static_cast<std::size_t>(Panel::render_settings)] = false;
        return open;
    }();
    // A panel to bring to the front, asked for over a few frames: a dock that
    // was hidden restores its tabs over a frame or two.
    std::optional<Panel> focus_panel;
    int focus_panel_frames{};
    bool show_shortcuts{}, show_preferences{}, show_about{};
    // The Inspect mode's workspace last used (Ctrl+3 returns to it).
    Workspace inspect_workspace{Workspace::scene};

    std::array<char, 128> mission_filter{};

    // Command palette / Go to.
    enum class PaletteMode { closed, commands, go_to };
    PaletteMode palette{PaletteMode::closed};
    bool palette_just_opened{};
    bool palette_prefilled{};
    std::array<char, 256> palette_query{};
    int palette_cursor{};

    // Script listing: scroll to and highlight this instruction entry once.
    std::optional<std::uint32_t> script_focus_entry;
    bool script_scroll_pending{}; // Scroll the listing to script_focus_entry once.

    // Explorer: one search box per workspace, plus shared filter chips.
    std::array<std::array<char, 128>, 6> explorer_query{};
    bool explorer_diagnostics_only{};
    bool explorer_dirty_only{};
    unsigned explorer_kind_mask{~0U};

    // Inspector: a pinned selection replaces the live one, and duplicated
    // inspectors are extra windows pinned to the selection they were opened on.
    std::optional<SelectionRef> inspector_pin;
    struct ExtraInspector {
        int id{};
        SelectionRef ref;
        bool open{true};
    };
    std::vector<ExtraInspector> extra_inspectors;
    int next_extra_inspector_id{1};

    // Hex view: byte range to highlight, and whether to scroll to it once.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> hex_highlight; // [begin, end)
    bool hex_scroll_to_highlight{};

    // An action (loading another mission, exiting) waiting for the user to save
    // or discard unsaved mission edits.
    std::function<void()> pending_discard;
    std::string pending_discard_label;
    // The New project wizard (Home, File > New project...).
    bool show_new_project{};
    std::array<char, 128> new_project_name{};
    std::array<char, 1024> new_project_folder{}, new_project_corpus{};
    std::string new_project_slot;
    bool new_project_terrain{true};
    float new_project_size{8000.0F};
    // The Build panel: the archive the next file dialog is the original of,
    // a deployment waiting for confirmation, the build shown, the playtest form.
    std::filesystem::path original_archive_for;
    std::string confirm_deploy;
    std::string selected_build;
    std::array<char, 256> playtest_note{};
    bool playtest_worked{true};
    // A delete refused because scripts or the mission still refer to the
    // selection: the dialog lists the references and can delete anyway.
    std::string force_delete_reason;

    // Mission export dialog.
    bool show_export_dialog{};
    std::array<char, 1024> export_original{}, export_output{};
    bool export_overwrite{}, export_install{};
    // Mission editing pickers in the Changes panel.
    std::array<char, 64> class_filter{};
    std::array<char, 1024> import_donor{};
    int import_class_id{-1};

    // Set during a frame by anything that changes on screen without input (a
    // playing animation); the frame loop then keeps drawing instead of idling.
    bool animating{};
    // Records selected besides the primary selection (Shift/Ctrl clicks,
    // boxes), and the primary as it was last frame (for Shift/Ctrl clicks).
    std::vector<MissionRecordKey> selection_extra;
    MissionRecordKey last_primary;
    // A form field waiting for the user to pick a record (the eyedropper).
    struct PickRequest {
        std::vector<MissionRecordKey::Kind> kinds;
        std::string hint;
        std::function<void(const MissionRecordKey&)> done;
        MissionRecordKey restore;  // selected before the pick started
        std::string field;         // the eyedropper button that asked, drawn active
    };
    std::optional<PickRequest> pick;
    // Outliner: search text, and records hidden or locked in the viewport
    // (kept by gameplay ID, so they survive edits that renumber entries).
    std::array<char, 128> outliner_query{};
    MissionRecordKey outliner_opened_for;  // the selection whose group the Outliner last opened
    std::set<std::string> outliner_open_rows;  // rows without a record (the intro) opened by a click
    std::vector<MissionRecordKey> hidden_records, locked_records;
    bool record_states_dirty{};
    // Problems dismissed by the user (Problem::id), for this session.
    std::vector<std::string> dismissed_problems;

    // UI test runs: frames must not depend on the machine or the moment, so
    // frame rates, build dates and local paths are left out. Messages shown on
    // screen replace each (prefix, name) path prefix with its name.
    bool deterministic{};
    std::vector<std::pair<std::string, std::string>> path_aliases;
    [[nodiscard]] std::string shown(std::string text) const {
        for (const auto& [prefix, name] : path_aliases)
            for (auto at = text.find(prefix); !prefix.empty() && at != std::string::npos; at = text.find(prefix, at))
                text.replace(at, prefix.size(), name);
        return text;
    }
};

// Measured by the frame loop, shown in the status bar and the viewport HUD.
struct FrameStats {
    double fps{};    // Average over recent frames that were drawn back to back.
    double cpu_ms{}; // CPU time of the latest frame, event handling to buffer swap.
    bool idle{};     // The loop is waiting for input between frames.
};

// A short-lived message for the result of a user-initiated action.
struct Toast {
    std::uint64_t id{}; // Stable ImGui window identity while older toasts expire.
    LogLevel level{LogLevel::info};
    std::string message;
    std::filesystem::path folder; // Shown as an "Open folder" action when set.
    double age{};                 // Seconds since it appeared.
    // A destructive edit's toast offers Undo while the combined history
    // (mission position, project cursor) is still where the edit left it.
    std::optional<std::pair<std::size_t, std::size_t>> undo_at;
};

// One edited byte in the Hex workspace, kept so the Changes panel and the
// explorer's dirty markers can say exactly what changed.
struct ByteEdit {
    std::uint64_t offset{};
    std::uint8_t before{}, after{};
};

// Tracks which selection slot changed last, so one SelectionRef describes what
// the user is looking at even though the viewport, explorer, and script outline
// each own a slot.
struct SelectionTracker {
    enum class Slot { none, mission_entry, chunk, script };
    std::optional<std::uint64_t> last_chunk;
    std::optional<std::uint32_t> last_entry;
    std::optional<std::pair<std::size_t, std::size_t>> last_script;
    Slot last_changed{Slot::none};
    // Selection kinds without a slot (database record, resource): held here until
    // one of the slots changes.
    SelectionRef detached;
};

struct AppState {
    GLFWwindow* window{};

    std::unique_ptr<rws::Document> document;
    std::unique_ptr<rws::Document> collision_document;
    MissionState mission;
    AuthoringSession authoring;
    AuthoringTools tools;
    std::size_t selected_program_document{}, selected_program_script{};

    std::string collision_status = "No document loaded";
    bool main_is_collision{};

    rwsman::GeometryPreview preview;
    std::optional<std::uint64_t> selected;
    std::optional<std::uint64_t> previous_selection;
    ChunkDisplayNames display_names;

    Workspace workspace = Workspace::scene;

    // Navigation.
    SelectionRef selection;
    SelectionTracker tracker;
    NavigationHistory history;
    SearchIndex search_index;
    std::vector<DiagnosticRow> diagnostics;
    // The author-facing problem list (Problems panel), rebuilt when its inputs change.
    std::vector<Problem> problems;
    std::uint64_t problems_key{~std::uint64_t{}};
    // Selections and RWS offsets that carry a diagnostic, for explorer markers.
    std::unordered_set<SelectionRef, SelectionRefHash> diagnostic_targets;
    std::vector<std::uint64_t> diagnostic_offsets; // Sorted.
    std::vector<ByteEdit> edits;                   // Sorted by offset.
    CommandRegistry commands;

    // Per-user configuration.
    std::filesystem::path config_dir;
    Settings settings;
    bool settings_dirty{};
    UiState ui;
    FrameStats frame_stats;
    LogBuffer log;

    // Native file dialog in flight (see file_dialogs.hpp).
    std::shared_ptr<PendingDialog> dialog;
    // Missions found under settings.resource_root.
    std::vector<DiscoveredMission> discovered;
    // An export waiting for the user to confirm replacing an existing file.
    struct PendingOverwrite {
        std::filesystem::path path;
        std::function<void()> overwrite;    // Replace the existing file.
        std::function<void()> write_unique; // Write a new, numbered file instead.
    };
    std::optional<PendingOverwrite> pending_overwrite;
    // Set only while a confirmed overwrite runs: the one path the user agreed to replace.
    std::optional<std::filesystem::path> confirmed_overwrite;

    // Background mission loading. Created on first use.
    std::shared_ptr<MissionLoader> loader;

    // Content signature of the loaded document (camera bookmarks are keyed by it).
    std::string content_signature;
    std::vector<Toast> toasts;
    std::uint64_t next_toast_id{1};

    // Warnings and errors in the merged diagnostics table (notes excluded).
    [[nodiscard]] std::size_t diagnostic_problem_count() const {
        return static_cast<std::size_t>(std::ranges::count_if(diagnostics, [](const DiagnosticRow& row) {
            return row.severity != DiagnosticSeverity::note;
        }));
    }
    [[nodiscard]] bool has_diagnostic(const SelectionRef& ref) const {
        return diagnostic_targets.contains(ref);
    }
    [[nodiscard]] bool range_has_diagnostic(const std::uint64_t begin, const std::uint64_t end) const {
        const auto found = std::ranges::lower_bound(diagnostic_offsets, begin);
        return found != diagnostic_offsets.end() && *found < end;
    }
    [[nodiscard]] bool range_dirty(const std::uint64_t begin, const std::uint64_t end) const {
        const auto found = std::ranges::lower_bound(edits, begin, {}, &ByteEdit::offset);
        return found != edits.end() && found->offset < end;
    }
    // Records a byte edit, merging repeated edits of the same offset. Editing a
    // byte back to its original value removes the record.
    void record_edit(const std::uint64_t offset, const std::uint8_t before, const std::uint8_t after) {
        auto found = std::ranges::lower_bound(edits, offset, {}, &ByteEdit::offset);
        if (found != edits.end() && found->offset == offset) {
            found->after = after;
            if (found->after == found->before) edits.erase(found);
        } else if (before != after) {
            edits.insert(found, ByteEdit{offset, before, after});
        }
    }

    // Logs `message`; the status bar shows the newest log line.
    void report(const LogLevel level, std::string message) { log.push(level, std::move(message)); }
    // Logs `message` and shows a toast, with an optional folder to open.
    void notify(const LogLevel level, std::string message, std::filesystem::path folder = {}) {
        toasts.push_back({next_toast_id++, level, message, std::move(folder), 0.0});
        if (toasts.size() > 4) toasts.erase(toasts.begin());
        report(level, std::move(message));
    }
    void info(std::string message) { report(LogLevel::info, std::move(message)); }
    void ok(std::string message) { report(LogLevel::ok, std::move(message)); }
    void warn(std::string message) { report(LogLevel::warn, std::move(message)); }
    void error(std::string message) { report(LogLevel::error, std::move(message)); }
};

} // namespace rwsman
