#pragma once

#include "csf/document.hpp"
#include "csf/mission_scene.hpp"
#include "csf/program.hpp"
#include "geometry_preview.hpp"
#include "rws/document.hpp"

#include <cstdint>
#include <functional>
#include <string>

// Text of a program operand value (shared with the script listing).
#include "rwsman/index_builders.hpp"

namespace rwsman {
struct AppState;
}

// Every draw_* entry point renders one panel from AppState. Panels do not own
// state and do not create their own window; the shell decides where they live.
namespace rwsman::ui {

void draw_menus(AppState& state);
void draw_status_bar(AppState& state);
void draw_explorer(AppState& state);
void draw_inspector(AppState& state);
// Draws the extra inspector windows opened with the duplicate button.
void draw_extra_inspectors(AppState& state);
void draw_script_view(AppState& state);
void draw_hex_view(AppState& state);
// Draws the center panel: the 3D viewport, the script listing, the geometry
// preview, or the hex/chunk inspector, depending on the workspace.
void draw_center(AppState& state, Workspace workspace);
void draw_start_page(AppState& state);
void draw_palette(AppState& state);
void draw_console(AppState& state);
void draw_diagnostics(AppState& state);
void draw_references(AppState& state);
// Inspect mode: session byte edits and the mission's changed files.
void draw_changes(AppState& state);
// Mission mode panels (docs/plans/editor-ux-redesign.md, §3 B).
void draw_outliner(AppState& state);
void draw_properties(AppState& state);
// The recipe that made a mission record, as an editable card (Properties).
void draw_component_card(AppState& state, const MissionRecordKey& key);
void draw_component_card(AppState& state, std::int32_t component_id);
// The cards of the components whose recipe is one of `ops` (objectives, equipment, tips).
void draw_component_cards(AppState& state, std::initializer_list<std::string_view> ops);
// A trigger's When / If / Do fields; `change` applies an edit (now, or later
// for an eyedropper pick). With `typed_texts` a message is the text itself.
using TriggerChange = std::function<void(const std::function<void(csf::Trigger&)>&)>;
void trigger_fields(AppState& state, const csf::Trigger& trigger, const TriggerChange& change, bool typed_texts);
void draw_problems(AppState& state);
void draw_history_panel(AppState& state);
void draw_mission_settings(AppState& state);
void draw_assets_panel(AppState& state);
void draw_behaviours(AppState& state);
void draw_flow_panel(AppState& state);
void draw_objectives_panel(AppState& state);
void draw_texts_panel(AppState& state);
void draw_timeline(AppState& state);
void draw_build(AppState& state);
// The strip under the menu bar in Mission mode: one entry per authoring step.
void draw_mission_bar(AppState& state);
// The edit cards of the scene record at `entry` (Properties, and the
// Inspector's Edit section).
void draw_record_editor(AppState& state, std::uint32_t entry);
// The Inspector's Edit section for the scene record at `entry`.
void draw_mission_edit_section(AppState& state, std::uint32_t entry);
// The Inspector's Edit section for a static map prop (scene instance).
void draw_map_instance_edit_section(AppState& state, const rws::SceneInstance& instance);
// Export and unsaved-edits dialogs.
void draw_mission_dialogs(AppState& state);
// The mission flow as a graph (S3): events, scripts, objectives.
void draw_flow_graph(AppState& state);
// An event that starts scripts but that no script raises.
[[nodiscard]] bool flow_event_unraised(const csf::FlowEvent& event);
// The New project wizard and the deploy confirmation (app/ui/project_panels.cpp).
void draw_project_dialogs(AppState& state);
// The Build panel's archives, test install and playtest cards.
void draw_project_pipeline(AppState& state);
void draw_render_settings(AppState& state);
void draw_shortcuts_window(AppState& state);
void draw_about_window(AppState& state);
void draw_workspace_tabs(AppState& state);
// Toasts for user-initiated actions, and the mission load progress overlay.
void draw_toasts(AppState& state);
void draw_missions(AppState& state);
void draw_preferences(AppState& state);
void draw_height_report(AppState& state);
// Modal asking whether to replace an existing export (policy: confirm overwrite).
void draw_overwrite_dialog(AppState& state);
void draw_load_overlay(AppState& state);
// One registry command as a menu item (shortcut hint, check mark, enabled state).
void draw_command_menu_item(AppState& state, const char* command_id);
// The right-click menu for the current selection: Frame, Isolate, Copy ID,
// Copy path, Show in hex, Show references, Export. Every entry is a registry
// command, so it also has a palette entry and a shortcut hint.
void draw_selection_context_menu(AppState& state);

// Shared helpers.
void draw_hex(AppState& state, std::uint64_t begin, std::uint64_t size);
void draw_typed_details(AppState& state, const rws::Chunk& chunk, std::uint32_t parent_type = 0);
void draw_csf_subtree(const csf::Document& document, const csf::Node& node);
// Sections for the mission record at CSFFBS entry `entry`, drawn inside the
// Inspector as property grids with provenance badges and raw-byte disclosure.
// `with_editor` puts the Edit section first (the Inspector); Properties draws
// its own editor and shows these sections as developer details.
void draw_mission_record(AppState& state, std::uint32_t entry, bool with_editor = true);
void draw_actor_animation(AppState& state, const csf::ActorAssociation& association,
                          std::uint32_t entry);
} // namespace rwsman::ui
