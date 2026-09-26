#pragma once

#include "app_state.hpp"
#include "rwsman/ui_script.hpp"

namespace rwsman {

// The flat state view that UI scripts `expect` against and `dump-state`
// writes (docs/plans/editor-ux-redesign.md, T5). Keys are stable; add new
// ones rather than renaming. Call between frames.
[[nodiscard]] StateSnapshot snapshot_state(const AppState& state);

// Name of a mission record kind as snapshots and scripts spell it ("actor").
[[nodiscard]] const char* mission_record_kind_name(MissionRecordKey::Kind kind);

} // namespace rwsman
