#pragma once

#include "app_state.hpp"

#include <cstdint>
#include <optional>
#include <vector>

// Editing in the viewport (docs/archive/editor/editor-ux-redesign.md, Phase 2): the
// Place, Route, Zone and Cover tools, multi-selection, project placements as
// viewport records, and picking a record for a form field.
namespace rwsman {

// Project placements (the map's buildings and props, project.csfproj) appear
// among the viewport's markers with entries from this base upwards (the index
// in AuthoringProject::placements). Mission entries never reach it.
inline constexpr std::uint32_t placement_entry_base = 0xF0000000U;
[[nodiscard]] std::optional<std::size_t> placement_index(const AppState& state, std::uint32_t entry);
// The project placement the selection is, if it is one.
[[nodiscard]] const csf::ProjectPlacement* selected_placement(const AppState& state);
// Adds the project's placements to viewport markers.
void append_placement_overlays(const AppState& state, GeometryPreview::MissionOverlaySet& overlays);

// Switches the viewport tool; leaving an authoring tool drops its sketch.
void set_viewport_tool(AppState& state, GeometryPreview::EditTool tool);
// The Place tool with this asset: the next clicks in the viewport place it.
void arm_place_tool(AppState& state, const AuthoringTools::CatalogEntry& entry);

// Every selected record: the primary selection first, then the others
// (Shift or Ctrl clicks, Shift+drag boxes, Outliner Shift/Ctrl clicks).
[[nodiscard]] std::vector<MissionRecordKey> selected_records(const AppState& state);
// A click on a record in a list: plain replaces the selection, Shift adds,
// Ctrl toggles.
void select_with_modifiers(AppState& state, const MissionRecordKey& key, bool shift, bool ctrl);

// Picking a record for a field ("the objective's zone"): the next selection
// of one of `kinds` goes to `done` instead of staying selected.
void request_pick(AppState& state, std::vector<MissionRecordKey::Kind> kinds, std::string hint,
                  std::function<void(const MissionRecordKey&)> done, std::string field = {});
// An eyedropper button for a form field: starts a pick (a click in the
// viewport or the Outliner answers it), or cancels the one it started.
void pick_button(AppState& state, const char* id, std::vector<MissionRecordKey::Kind> kinds, const std::string& hint,
                 std::function<void(const MissionRecordKey&)> done);

// Once per frame in the viewport window, after the preview drew: consumes the
// viewport's clicks and selection events, draws the active tool's preview
// and its options, and finishes sketches on Enter.
void update_viewport_tools(AppState& state);

// Group operations on every selected record, each one undo step.
void align_selection_to_ground(AppState& state);
void delete_selection(AppState& state, bool force);
void duplicate_selection(AppState& state);
// A finished gizmo drag of the primary selection: moves every selected record
// by the same offset (placements through the project). False when the drag is
// a single mission record's, which the gizmo applies itself.
bool apply_group_drag(AppState& state, const GeometryPreview::EditDrag& drag);

} // namespace rwsman
