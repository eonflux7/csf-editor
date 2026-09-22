#pragma once

#include "csf/document.hpp"
#include "csf/mission_scene.hpp"
#include "csf/program.hpp"
#include "geometry_preview.hpp"
#include "rws/document.hpp"

#include <cstdint>
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
void draw_changes(AppState& state);
void draw_render_settings(AppState& state);
void draw_shortcuts_window(AppState& state);
void draw_about_window(AppState& state);
void draw_workspace_tabs(AppState& state);
// Toasts for user-initiated actions, and the mission load progress overlay.
void draw_toasts(AppState& state);
void draw_missions(AppState& state);
void draw_preferences(AppState& state);
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
void draw_mission_record(AppState& state, std::uint32_t entry);
void draw_actor_animation(AppState& state, const csf::ActorAssociation& association,
                          std::uint32_t entry);
} // namespace rwsman::ui
