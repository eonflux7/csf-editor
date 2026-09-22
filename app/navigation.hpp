#pragma once

#include "app_state.hpp"

#include <string>
#include <vector>

// Selection, history, and cross-reference navigation over AppState.
namespace rwsman {

[[nodiscard]] bool workspace_available(const AppState& state, Workspace workspace);
// Switches workspace when it is available; returns whether it is now current.
bool set_workspace(AppState& state, Workspace workspace);

// Call once per frame before drawing panels: derives AppState::selection from
// the viewport, explorer, and script outline, and pushes a history entry when
// it changed.
void track_selection(AppState& state);
// Forget the selection and history (a document was loaded or closed).
void reset_navigation(AppState& state);

struct NavigateOptions {
    bool frame{true}; // Frame the target in the viewport when it has a location.
};
// Selects `ref`, switching workspace when the target lives elsewhere (an actor
// operand in a script selects the actor in Mission). The change is recorded in
// the history by the next track_selection().
void navigate_to(AppState& state, const SelectionRef& ref, NavigateOptions options = {});
void go_back(AppState& state);
void go_forward(AppState& state);

// "FR01.scn:entry 1234 @0x2C79D6": stable identity of a selection.
[[nodiscard]] std::string selection_identity(const AppState& state, const SelectionRef& ref);
// Numeric ID text: the entry index for mission and script records, the hex
// offset for chunks.
[[nodiscard]] std::string selection_id_text(const AppState& state, const SelectionRef& ref);
// File the selection was read from (mission scene, script, database, or the
// visual map). Empty when there is none.
[[nodiscard]] std::filesystem::path selection_source_path(const AppState& state,
                                                          const SelectionRef& ref);
// Short display name: actor name, script title, chunk type.
[[nodiscard]] std::string selection_title(const AppState& state, const SelectionRef& ref);
[[nodiscard]] std::string selection_kind_label(const AppState& state, const SelectionRef& ref);
[[nodiscard]] std::optional<std::uint64_t> selection_offset(const AppState& state,
                                                            const SelectionRef& ref);

struct BreadcrumbSegment {
    std::string label;
    SelectionRef target; // Empty when the segment is not a navigation target.
};
[[nodiscard]] std::vector<BreadcrumbSegment> selection_breadcrumb(const AppState& state,
                                                                  const SelectionRef& ref);

// Rebuilds the symbol index for the loaded document or mission.
void rebuild_search_index(AppState& state);
// Rebuilds the merged diagnostics table for the loaded document or mission.
void rebuild_diagnostics(AppState& state);

} // namespace rwsman
