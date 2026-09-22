#pragma once

#include "csf/animation_catalog.hpp"
#include "csf/document.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "csf/program.hpp"
#include "rws/document.hpp"
#include "rwsman/selection.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rwsman {

enum class DiagnosticSeverity : std::uint8_t { note, warning, error };

[[nodiscard]] const char* diagnostic_severity_name(DiagnosticSeverity severity) noexcept;

// One row of the merged Diagnostics table. RWS, CSFFBS, mission, resource, and
// database diagnostics all reduce to this shape; `target` says what to select
// when the row is clicked.
struct DiagnosticRow {
    DiagnosticSeverity severity{DiagnosticSeverity::warning};
    std::string source;  // "RWS", "CSFFBS", "Mission", "Resources", "Objects", ...
    std::string file;    // File name only.
    std::optional<std::uint32_t> entry;
    std::optional<std::uint64_t> offset;
    std::string code;
    std::string message;
    SelectionRef target;
};

struct DiagnosticInputs {
    const rws::Document* document{};
    const csf::MissionScene* scene{};
    const csf::MissionGraph* graph{};
    const csf::Document* scene_document{};
    const csf::ObjectDatabase* objects{};
    const csf::AnimationCatalog* animations{};
    const std::vector<std::pair<std::filesystem::path, csf::ProgramDocument>>* programs{};
    const std::vector<csf::ActorAssociation>* associations{};
    // Extra free-form diagnostics (for example texture decoding), already reduced.
    const std::vector<DiagnosticRow>* extra{};
};

[[nodiscard]] std::vector<DiagnosticRow> collect_diagnostics(const DiagnosticInputs& inputs);

struct DiagnosticFilter {
    bool notes{true}, warnings{true}, errors{true};
    std::string text;   // Case-insensitive substring over source, file, code, and message.
    std::string source; // Exact source name, or empty for all.
};

enum class DiagnosticColumn : std::uint8_t { severity, source, file, entry, offset, message };

// Indexes into `rows` that pass the filter, ordered by `column`. Ties keep the
// original order so the table is stable.
[[nodiscard]] std::vector<std::size_t> select_diagnostics(const std::vector<DiagnosticRow>& rows,
                                                          const DiagnosticFilter& filter,
                                                          DiagnosticColumn column,
                                                          bool ascending);

} // namespace rwsman
