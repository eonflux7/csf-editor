#pragma once

#include "csf/mission_edit.hpp"
#include "csf/mission_ops.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Components: recipes that can be edited again (docs/archive/editor/editor-ux-redesign.md,
// E1). A component is the mission operation lines (csf/mission_ops.hpp, with
// explicit IDs) that made some records, and the list of those records. Editing
// it deletes the records and runs its new lines in their places
// (MissionEditor::replace_in_place), so the files are what a fresh build of the
// edited lines would give. The list is the editor's components.csfops, a
// mission file like any other: undo and save include it, packaging does not.
//
//   # csf-editor components
//   component 3 owns=actor:11,group:5,script:9011 fingerprint=1f0c...
//     guard-patrol id=11 name=GE_CAMP ... route=5 ... script=9011 script-name=GE_CAMP
//     link-nearest group=5 target=1
//
// The fingerprint is of the owned records as the lines made them: when it no
// longer matches, someone edited them by hand, and the component is Modified
// until the user keeps those edits (detach) or regenerates it.
namespace csf {

struct MissionComponent {
    std::int32_t id{};
    std::vector<std::string> lines;  // operation lines, in order
    std::vector<MissionRecordId> owns;
    std::string fingerprint;

    // The first line's operation ("guard-patrol", "objective" for objectives).
    [[nodiscard]] std::string op() const;
};

// Throws std::invalid_argument on a malformed list.
[[nodiscard]] std::vector<MissionComponent> parse_components(std::string_view text);
[[nodiscard]] std::string format_components(const std::vector<MissionComponent>& components);
// The mission's components; empty when it has none or its list does not parse
// (then `error` says why).
[[nodiscard]] std::vector<MissionComponent> mission_components(const MissionEditor& editor,
                                                               std::string* error = nullptr);
[[nodiscard]] std::optional<MissionComponent> component_owning(const MissionEditor& editor, MissionRecordId record);

// "Guard patrol", "Objectives", ...; and the name the lines give ("GE_CAMP").
[[nodiscard]] std::string component_kind_title(std::string_view op);
[[nodiscard]] std::string component_title(const MissionComponent& component);

// Of the owned records and the cross-group links its link-nearest lines make.
[[nodiscard]] std::string component_fingerprint(const MissionEditor& editor, const MissionComponent& component);
enum class ComponentState : std::uint8_t {
    clean,     // the owned records are as the lines made them
    modified,  // edited or deleted by hand since
};
[[nodiscard]] ComponentState component_state(const MissionEditor& editor, const MissionComponent& component);

// Record IDs the lines leave out are filled in with free IDs first, so the
// component regenerates the same records. One undo step each.
EditResult add_component(MissionEditor& editor, std::string_view lines, const MissionOpsOptions& options = {},
                         std::int32_t* new_id = nullptr);
// Replaces the component's lines and regenerates its records in place.
EditResult update_component(MissionEditor& editor, std::int32_t id, std::string_view lines,
                            const MissionOpsOptions& options = {});
EditResult regenerate_component(MissionEditor& editor, std::int32_t id, const MissionOpsOptions& options = {});
// Deletes the component and every record it owns.
EditResult delete_component(MissionEditor& editor, std::int32_t id);
// Keeps the records as they are and forgets the component (they become
// ordinary records, edited by hand from then on).
EditResult detach_component(MissionEditor& editor, std::int32_t id);

// An operations file where each recipe (guard-patrol, guard-idle,
// animal-patrol, cover-group, walk-grid, objective...objectives,
// kit...equipment, tips, shot...intro) becomes a component, and a
// link-nearest from a group a component owns joins that component. Other
// lines run as plain operations. Stops at the first rejected line.
[[nodiscard]] std::vector<MissionOpOutcome> run_component_ops(MissionEditor& editor, std::string_view text,
                                                              const MissionOpsOptions& options = {});
// Regenerates each component from its own lines and puts everything back:
// the IDs of those whose regeneration would change any file. Drops redo steps.
[[nodiscard]] std::vector<std::int32_t> components_that_drift(MissionEditor& editor,
                                                              const MissionOpsOptions& options = {});

[[nodiscard]] const char* record_type_name(MissionRecordId::Type type) noexcept;

} // namespace csf
