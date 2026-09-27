#include "csf/mission_flow.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <set>

namespace csf {
namespace {

// g_szEventNameTable (0x008b1750), the populated slots (KB-scripting-14).
constexpr std::array<std::string_view, 22> builtin_events{
    "START_GAME",      "DISPARO",         "IMPACTO",         "IMPACTO_AL_AIRE",         "MORIBUNDO",
    "CURADO",          "MUERTO",          "BICHO_ENT_ZONA",  "BICHO_SAL_ZONA",          "CHECK_MISSION_COMPLETED",
    "BICHO_DESTRUIDO", "ALWAYS",          "IA_CHANGE_STATE", "OBJETO_COGIDO",           "DISFRAZ_COGIDO",
    "EVT_GHOST_USADO", "KILLER_MOVE",     "ENTRADA_EN_ESCENARIO", "SALIDA_DE_ESCENARIO", "PUERTA_USADA",
    "ALARM_SET_OFF",   "DEMO_VERSION"};

std::string upper(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::optional<std::int32_t> number_of(const ProgramOperand& operand) {
    if (const auto* value = std::get_if<std::int32_t>(&operand.value)) return *value;
    if (const auto* value = std::get_if<float>(&operand.value)) return static_cast<std::int32_t>(*value);
    return std::nullopt;
}

std::string text_of(const ProgramOperand& operand) {
    if (const auto* value = std::get_if<std::string>(&operand.value)) return *value;
    if (const auto number = number_of(operand)) return std::to_string(*number);
    return {};
}

// Calls `visit(opcode, operands)` for an instruction and for every call nested
// in its operands (CONTINUE (CUTSCENE_EXE (CUTSCENE 28)), IF (AND ...)).
void walk(const std::string& opcode, const std::vector<ProgramOperand>& operands,
          const std::function<void(const std::string&, const std::vector<ProgramOperand>&)>& visit) {
    visit(opcode, operands);
    for (const auto& operand : operands)
        if (!operand.children.empty()) walk(operand.tag, operand.children, visit);
}

} // namespace

bool is_builtin_event(const std::string_view name) noexcept {
    if (name.empty()) return false;
    const auto wanted = upper(name);
    return std::ranges::find(builtin_events, std::string_view(wanted)) != builtin_events.end();
}

const char* flow_severity_name(const FlowFinding::Severity severity) noexcept {
    switch (severity) {
    case FlowFinding::Severity::info: return "info";
    case FlowFinding::Severity::warning: return "warning";
    case FlowFinding::Severity::error: return "error";
    }
    return "warning";
}

const FlowScript* MissionFlow::script(const std::string_view program, const std::int32_t id) const noexcept {
    const auto found = std::ranges::find_if(scripts_, [&](const FlowScript& s) { return s.program == program && s.id == id; });
    return found == scripts_.end() ? nullptr : &*found;
}

MissionFlow MissionFlow::build(const ProgramDocument* mission, const ProgramDocument* cutscene) {
    MissionFlow flow;
    std::map<std::int32_t, FlowObjective> objectives;
    std::map<std::string, FlowEvent> events;
    bool zone_activation = false, ghost_enabled = false;
    const auto event = [&](const std::string& name) -> FlowEvent& {
        const auto key = upper(name);
        auto& value = events[key];
        if (value.name.empty()) {
            value.name = key;
            value.builtin = is_builtin_event(key);
        }
        return value;
    };
    for (const auto& [program, document] : {std::pair{"mission", mission}, std::pair{"cutscene", cutscene}}) {
        if (!document) continue;
        for (const auto& source : document->scripts()) {
            FlowScript script;
            script.program = program;
            script.id = source.id;
            script.name = source.name;
            script.trigger = source.flags.trigger.value_or(false);
            script.enabled = source.flags.enabled.value_or(true);
            for (const auto& e : source.events) {
                script.events.push_back(upper(e.name));
                event(e.name).listeners.push_back(source.id);
            }
            const auto visit = [&](const std::string& opcode, const std::vector<ProgramOperand>& operands) {
                const auto operand = [&](const std::size_t i) -> const ProgramOperand* {
                    return i < operands.size() ? &operands[i] : nullptr;
                };
                for (const auto& o : operands)
                    if (o.tag == "FLI")
                        if (auto id = text_of(o); !id.empty() && std::ranges::find(script.texts, id) == script.texts.end())
                            script.texts.push_back(std::move(id));
                if (opcode == "SEND_EVENT" || opcode == "SEND_EVENT_BICHO") {
                    for (const auto& o : operands)
                        if (o.tag == "EVENT") {
                            script.sends.push_back(upper(text_of(o)));
                            event(text_of(o)).senders.push_back(source.id);
                        }
                } else if (opcode == "SET_OBJETIVO") {
                    if (const auto* n = operand(0); n && number_of(*n)) {
                        auto& objective = objectives[*number_of(*n)];
                        objective.number = *number_of(*n);
                        objective.defined_by.push_back(source.id);
                        script.defines.push_back(objective.number);
                        if (const auto* flag = operand(1); flag && flag->tag == "BOOL")
                            objective.secondary = upper(text_of(*flag)) == "TRUE";
                    }
                } else if (opcode == "SET_OBJETIVO_LABEL") {
                    if (const auto* n = operand(0); n && number_of(*n))
                        if (const auto* label = operand(1)) {
                            auto& objective = objectives[*number_of(*n)];
                            objective.number = *number_of(*n);
                            objective.label = text_of(*label);
                        }
                } else if (opcode == "SET_OBJETIVO_SUCCESS") {
                    if (const auto* n = operand(0); n && number_of(*n)) {
                        auto& objective = objectives[*number_of(*n)];
                        objective.number = *number_of(*n);
                        objective.completed_by.push_back(source.id);
                        script.completes.push_back(objective.number);
                    }
                } else if (opcode == "SET_MISSION_SUCCESS") {
                    if (const auto* flag = operand(0); !flag || upper(text_of(*flag)) != "FALSE") script.mission_success = true;
                } else if (opcode == "SET_MISSION_FAILED" || opcode == "SET_MISSION_FAILURE") {
                    script.mission_failure = true;
                } else if (opcode == "CUTSCENE_EXE") {
                    if (const auto* n = operand(0); n && number_of(*n)) script.cutscenes.push_back(*number_of(*n));
                } else if (opcode == "ACT_BICHO_EVENT_ZONA") {
                    zone_activation = true;
                } else if (opcode == "HABILITAR_GHOST") {
                    if (const auto* flag = operand(1); !flag || upper(text_of(*flag)) != "FALSE") ghost_enabled = true;
                }
            };
            for (const auto* list : {&source.conditions, &source.actions})
                for (const auto& instruction : *list) walk(instruction.opcode, instruction.operands, visit);
            flow.scripts_.push_back(std::move(script));
        }
    }
    // Numbers below 1 only label other text slots (Convoy labels -1..-3).
    for (auto& [number, objective] : objectives)
        if (number > 0) flow.objectives_.push_back(std::move(objective));
    for (auto& [name, value] : events) flow.events_.push_back(std::move(value));

    auto& findings = flow.findings_;
    using Severity = FlowFinding::Severity;
    for (const auto& e : flow.events_) {
        if (e.builtin) continue;
        if (!e.listeners.empty() && e.senders.empty())
            findings.push_back({Severity::warning, e.listeners.front(),
                                "Mission event " + e.name + " starts " + std::to_string(e.listeners.size()) +
                                    " script(s), but no script raises it (SEND_EVENT), so they probably never run"});
        else if (e.listeners.empty() && !e.senders.empty())
            findings.push_back({Severity::info, e.senders.front(),
                                "Mission event " + e.name + " is raised, but no script listens for it"});
    }
    const auto listened = [&](const std::string_view name) {
        return std::ranges::any_of(flow.events_, [&](const FlowEvent& e) { return e.name == name && !e.listeners.empty(); });
    };
    for (const auto& objective : flow.objectives_) {
        const auto label = "Objective " + std::to_string(objective.number);
        if (objective.defined_by.empty())
            findings.push_back({Severity::warning, objective.completed_by.empty() ? std::nullopt
                                                                                  : std::optional(objective.completed_by.front()),
                                label + " is completed but never set up (SET_OBJETIVO)"});
        if (objective.completed_by.empty())
            findings.push_back({Severity::warning, objective.defined_by.empty() ? std::nullopt
                                                                                : std::optional(objective.defined_by.front()),
                                label + " is set up but nothing completes it (SET_OBJETIVO_SUCCESS)"});
        const auto names = [&](const std::vector<std::int32_t>& ids) {
            std::string text;
            for (const auto id : ids) {
                const auto* script = flow.script("mission", id);
                text += (text.empty() ? "" : ", ") + (script && !script->name.empty() ? script->name : std::to_string(id));
            }
            return text;
        };
        if (std::set(objective.defined_by.begin(), objective.defined_by.end()).size() > 1)
            findings.push_back({Severity::warning, objective.defined_by[1],
                                label + " is set up by " + std::to_string(objective.defined_by.size()) + " scripts (" +
                                    names(objective.defined_by) + "): two objective lists use the same number"});
        if (const std::set completers(objective.completed_by.begin(), objective.completed_by.end()); completers.size() > 1) {
            // The objectives recipe's completion script also checks for the
            // mission's success, and runs only while its objective is open:
            // another script completing it first skips that check.
            const auto checks = std::ranges::find_if(completers, [&](const std::int32_t id) {
                const auto* script = flow.script("mission", id);
                return script && script->mission_success;
            });
            if (checks != completers.end())
                findings.push_back({Severity::warning, *checks,
                                    label + " is completed by " + std::to_string(completers.size()) + " scripts (" +
                                        names(objective.completed_by) + "). The one that also checks for the mission's "
                                        "success may never run if another completes it first, and then the mission is "
                                        "never won; complete each objective in one place"});
            else
                findings.push_back({Severity::info, objective.completed_by.front(),
                                    label + " is completed by " + std::to_string(completers.size()) + " scripts (" +
                                        names(objective.completed_by) + ")"});
        }
        if (objective.label.empty() && !objective.defined_by.empty())
            findings.push_back({Severity::info, objective.defined_by.front(), label + " has no text (SET_OBJETIVO_LABEL)"});
    }
    if (!flow.objectives_.empty() &&
        std::ranges::none_of(flow.scripts_, [](const FlowScript& s) { return s.mission_success; }))
        findings.push_back({Severity::warning, std::nullopt,
                            "No script ends the mission in success (SET_MISSION_SUCCESS)"});
    if (listened("BICHO_ENT_ZONA") && !zone_activation)
        findings.push_back({Severity::warning, std::nullopt,
                            "Scripts wait for zone entries (BICHO_ENT_ZONA), but no zone's events are switched on "
                            "(ACT_BICHO_EVENT_ZONA)"});
    if (listened("EVT_GHOST_USADO") && !ghost_enabled)
        findings.push_back({Severity::warning, std::nullopt,
                            "Scripts wait for a ghost to be used (EVT_GHOST_USADO), but none is enabled "
                            "(HABILITAR_GHOST): the player gets no prompt"});
    for (const auto& script : flow.scripts_)
        for (const auto target : script.cutscenes)
            if (!flow.script("cutscene", target))
                findings.push_back({Severity::error, script.id,
                                    "Script " + std::to_string(script.id) + " runs cutscene " + std::to_string(target) +
                                        ", which the cutscene program (.csc) does not have"});
    return flow;
}

} // namespace csf
