#pragma once

#include "csf/program.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A read-only view of how a mission's scripts connect (editor plan stage 5):
// which events start which scripts, who raises the custom ones, which scripts
// define and complete objectives, and what looks broken. It reads the
// programs as they are and never rewrites them; a finding is evidence to look
// at, not proof of runtime behaviour.
namespace csf {

struct FlowScript {
    std::string program;  // "mission" (.gsc) or "cutscene" (.csc)
    std::int32_t id{};
    std::string name;
    bool trigger{}, enabled{true};
    std::vector<std::string> events;  // what starts it (.EVENTOS)
    std::vector<std::string> sends;   // SEND_EVENT
    std::vector<std::int32_t> defines, completes;  // objective numbers
    std::vector<std::int32_t> cutscenes;           // CUTSCENE_EXE targets
    std::vector<std::string> texts;                // FLI string IDs it shows or labels with
    bool mission_success{}, mission_failure{};
};

struct FlowObjective {
    std::int32_t number{};
    std::optional<bool> secondary;  // SET_OBJETIVO's flag, as hello world uses it
    std::string label;              // FLI string ID
    std::vector<std::int32_t> defined_by, completed_by;  // script IDs
};

struct FlowEvent {
    std::string name;
    bool builtin{};  // raised by the engine (the 44-entry name table, KB-scripting-14)
    std::vector<std::int32_t> listeners, senders;
};

struct FlowFinding {
    enum class Severity : std::uint8_t { info, warning, error };
    Severity severity{Severity::warning};
    std::optional<std::int32_t> script;
    std::string message;
};

class MissionFlow {
public:
    // `mission` is the .gsc, `cutscene` the .csc (either may be missing).
    [[nodiscard]] static MissionFlow build(const ProgramDocument* mission, const ProgramDocument* cutscene);

    [[nodiscard]] const std::vector<FlowScript>& scripts() const noexcept { return scripts_; }
    [[nodiscard]] const std::vector<FlowObjective>& objectives() const noexcept { return objectives_; }
    [[nodiscard]] const std::vector<FlowEvent>& events() const noexcept { return events_; }
    [[nodiscard]] const std::vector<FlowFinding>& findings() const noexcept { return findings_; }
    [[nodiscard]] const FlowScript* script(std::string_view program, std::int32_t id) const noexcept;

private:
    std::vector<FlowScript> scripts_;
    std::vector<FlowObjective> objectives_;
    std::vector<FlowEvent> events_;
    std::vector<FlowFinding> findings_;
};

// The engine's own event names (KB-scripting-14, g_szEventNameTable).
[[nodiscard]] bool is_builtin_event(std::string_view name) noexcept;
[[nodiscard]] const char* flow_severity_name(FlowFinding::Severity severity) noexcept;

} // namespace csf
