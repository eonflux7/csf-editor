#pragma once

#include "app_state.hpp"
#include "ui/theme.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rwsman {

struct ReferenceRow {
    std::string group;   // "Script uses", "Definitions and exact uses", ...
    std::string label;
    std::string detail;
    SelectionRef target; // Empty when the source is not navigable.
    ui::Provenance provenance{ui::Provenance::inferred};
    std::string evidence; // Tooltip text explaining the provenance.
};

// Reverse and forward references of a selection, from the resolver's typed
// references and the exact symbol index. Provenance comes straight from the
// resolver status, never from a UI heuristic.
[[nodiscard]] std::vector<ReferenceRow> collect_references(const AppState& state,
                                                          const SelectionRef& ref);

// (document index, script index) of the script with `id` in `file`.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
find_script_by_id(const AppState& state, const std::filesystem::path& file, std::int32_t id);
// The script whose entry range most plausibly contains `entry` in `file`.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
find_script_containing(const AppState& state, const std::filesystem::path& file,
                       std::uint32_t entry);

// A selection for `entry` in a program file: the instruction that owns it (the instruction
// entry or one of its operands), or else the owning script (its header, events, variables).
[[nodiscard]] SelectionRef program_entry_ref(const AppState& state,
                                             const std::filesystem::path& file,
                                             std::uint32_t entry);

[[nodiscard]] ui::Provenance provenance_of(csf::ProgramReferenceStatus status);
[[nodiscard]] ui::Provenance provenance_of(csf::ResolutionStatus status);

} // namespace rwsman
