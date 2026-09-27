#pragma once

#include "rwsman/entity_kind.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace csf {
class MissionFlow;
class MissionScene;
class ObjectDatabase;
struct HeightFinding;
struct LightmapFinding;
} // namespace csf

// "What is wrong with my mission": one list gathered from every check the
// editor runs (docs/archive/editor/editor-ux-redesign.md, E7). Each problem names its
// subject so the UI can select it, and many name a command that fixes it.
namespace rwsman {

struct ProblemSubject {
    enum class Kind : std::uint8_t { none, actor, area, nav_group, dummy, script, placement };
    Kind kind{Kind::none};
    std::int32_t id{};
    std::string text;  // a placement ID, or the program of a script ("mission", "cutscene")
    friend bool operator==(const ProblemSubject&, const ProblemSubject&) = default;
};

struct Problem {
    enum class Severity : std::uint8_t { error, warning, note };
    Severity severity{Severity::warning};
    std::string id;       // stable across rebuilds: source, subject and message
    std::string source;   // "Flow", "Heights", "Project", "Zones", "Classes"
    std::string message;
    EntityKind kind{EntityKind::script};  // for the icon
    ProblemSubject subject;
    std::string fix_command;  // a command registry ID, or empty
    std::string fix_label;    // the button text for it
};

struct ProblemInputs {
    const csf::MissionScene* scene{};
    const csf::ObjectDatabase* objects{};
    const csf::MissionFlow* flow{};
    std::span<const csf::HeightFinding> heights;
    std::span<const csf::LightmapFinding> lightmaps;  // AuthoringProject::lightmap_brightness()
    std::vector<std::string> project_checks;  // AuthoringProject::check()
    // An authoring project's text range and the IDs it has strings for: a
    // script showing an ID in the range that has no string shows nothing.
    struct ProjectTexts {
        std::int32_t first{}, last{};
        std::vector<std::string> ids;
    };
    std::optional<ProjectTexts> project_texts;
};

// Errors first, then warnings, then notes; within a severity by source.
[[nodiscard]] std::vector<Problem> collect_problems(const ProblemInputs& inputs);
[[nodiscard]] std::size_t count_problems(std::span<const Problem> problems, Problem::Severity severity);

} // namespace rwsman
