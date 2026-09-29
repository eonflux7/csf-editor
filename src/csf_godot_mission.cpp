#include "csf/godot_export.hpp"

#include "csf/mission_flow.hpp"
#include "csf/mission_recipes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <set>

namespace csf {
namespace {

constexpr std::string_view mission_format = "opencsf-mission";
// A VAR or ARRAYID operand with this bit names a global variable; without it,
// one of its script's own (the VM's global table bit, KB-scripting-20).
constexpr std::int32_t global_variable_bit = 0x800000;

std::string upper(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string json_string(const std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') out += {'\\', c};
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        } else out += c;
    }
    return out + '"';
}

std::string json_number(const float value) {
    std::array<char, 32> buffer{};
    const auto end = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value == 0.0F ? 0.0F : value).ptr;
    return {buffer.data(), end};
}

template <typename Items, typename Format>
std::string json_list(const Items& items, Format format) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : items) {
        out += (first ? "" : ",") + format(item);
        first = false;
    }
    return out + "]";
}

std::string json_strings(const std::vector<std::string>& items) { return json_list(items, json_string); }
std::string json_integers(const std::vector<std::int32_t>& items) {
    return json_list(items, [](const std::int32_t value) { return std::to_string(value); });
}

// A word the source text can show bare: EVENT names, FLI g017, enum values.
bool bare_word(const std::string_view text) {
    return !text.empty() && (std::isalpha(static_cast<unsigned char>(text.front())) || text.front() == '_') &&
           std::ranges::all_of(text, [](const unsigned char c) { return std::isalnum(c) || c == '_'; });
}

// A name printed after its number: bare unless it has spaces, brackets or quotes.
bool bare_name(const std::string_view text) {
    return !text.empty() && std::ranges::none_of(text, [](const unsigned char c) {
        return std::isspace(c) || c == '(' || c == ')' || c == '"' || c == '#';
    });
}

std::string quote_text(const std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += {'\\', c};
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + '"';
}

std::string scalar_text(const ProgramOperand& operand) {
    if (const auto* value = std::get_if<std::int32_t>(&operand.value)) return std::to_string(*value);
    if (const auto* value = std::get_if<float>(&operand.value)) return script_number(*value);
    if (const auto* value = std::get_if<std::string>(&operand.value)) return bare_word(*value) ? *value : quote_text(*value);
    return {};
}

std::optional<std::int32_t> number_of(const ProgramOperand& operand) {
    if (const auto* value = std::get_if<std::int32_t>(&operand.value)) return *value;
    if (const auto* value = std::get_if<float>(&operand.value)) return static_cast<std::int32_t>(*value);
    return std::nullopt;
}

bool is_scalar(const ProgramOperand& operand) { return operand.tag.empty() && operand.children.empty(); }

// The names behind the numbers scripts use, and which of them they use.
class Namer {
public:
    explicit Namer(const GodotMissionSources& sources) : sources_(sources) {
        if (sources.scene) {
            for (const auto& actor : sources.scene->actors())
                if (actor.id) actors_[*actor.id] = actor.name.value_or("");
            for (const auto& dummy : sources.scene->dummies())
                if (dummy.id) dummies_[*dummy.id] = dummy.name.value_or("");
            for (const auto& area : sources.scene->areas())
                if (area.id) areas_[*area.id] = area.name.value_or("");
            for (const auto& group : sources.scene->navigation()) {
                if (!group.id) continue;
                routes_[*group.id] = group.name.value_or("");
                for (const auto& point : group.points)
                    if (point.id) points_[{*group.id, *point.id}] = point.name.value_or("");
            }
        }
        for (const auto* program : {sources.mission, sources.cutscene}) {
            if (!program) continue;
            auto& names = program == sources.mission ? mission_scripts_ : cutscene_scripts_;
            for (const auto& script : program->scripts()) names[script.id] = script.name;
        }
        if (sources.mission)
            for (const auto& variable : sources.mission->global_variables()) globals_[variable.id] = variable.name;
    }

    // One line of a script: its instruction, and "# ..." comments for the texts it shows.
    std::string line(const ProgramInstruction& instruction, const ProgramScript& script, const bool cutscene) {
        script_ = &script;
        cutscene_ = cutscene;
        off_ = false;
        std::vector<std::string> notes;
        const auto key = upper(instruction.opcode);
        std::string out;
        if (key == "COMENTARIO" && instruction.operands.size() == 1 && is_scalar_text(instruction.operands.front())) {
            return "# " + std::get<std::string>(instruction.operands.front().value);
        }
        if (key == "COMENTARIO_ACCION") {
            // An action switched off in the editor: shown, marked, not run.
            out = "# off: ";
            off_ = true;
            for (std::size_t i = 0; i < instruction.operands.size(); ++i)
                out += (i ? " " : "") + call(instruction.operands[i], notes);
        } else {
            out = instruction.opcode;
            uses_.insert(key);  // and its nested operations, below
            for (const auto& operand : instruction.operands) out += " " + operand_text(operand, notes);
        }
        for (std::size_t i = 0; i < notes.size(); ++i) out += (i ? "; " : "  # ") + notes[i];
        return out;
    }

    // The instructions the lines given since the last call ran (switched-off ones left out).
    std::vector<std::string> take_uses() {
        std::vector<std::string> out(uses_.begin(), uses_.end());
        uses_.clear();
        return out;
    }

    // "names": every kind the scripts used, by number, with its name ("" when
    // it has none, or nothing has that number: then it is in `unresolved`).
    [[nodiscard]] std::string names_json() const {
        std::string out = "{";
        bool first_kind = true;
        for (const auto& [kind, used] : used_) {
            out += std::string(first_kind ? "\n" : ",\n") + json_string(kind) + ":{";
            bool first = true;
            for (const auto& [key, use] : used) {
                const auto name = use.name.value_or("");
                out += (first ? "" : ",") + json_string(key_text(key)) + ":";
                if (kind == "anim") {
                    out += "{\"name\":" + json_string(name);
                    if (const auto roles = sources_.animation_roles.find(lower(name)); roles != sources_.animation_roles.end())
                        out += ",\"roles\":" + json_strings(roles->second);
                    out += "}";
                } else {
                    out += json_string(name);
                }
                first = false;
            }
            out += "}";
            first_kind = false;
        }
        return out + "\n}";
    }

    [[nodiscard]] std::string unresolved_json() const {
        std::string out = "[";
        bool first = true;
        for (const auto& [kind, used] : used_)
            for (const auto& [key, use] : used)
                if (!use.name) {
                    // `off`: only switched-off actions name it.
                    out += std::string(first ? "\n" : ",\n") + "{\"kind\":" + json_string(kind) +
                           ",\"id\":" + json_string(key_text(key)) + (use.live ? "" : ",\"off\":true") + "}";
                    first = false;
                }
        return out + (first ? "]" : "\n]");
    }

    [[nodiscard]] const std::set<std::string>& texts_used() const noexcept { return texts_used_; }

    // The ID a .fli writes for a script's FLI operand: as given, else upper case (g017 -> G017).
    [[nodiscard]] std::string text_id(const std::string& value) const {
        if (sources_.texts.contains(value) || sources_.global_texts.contains(value)) return value;
        return upper(value);
    }
    [[nodiscard]] const std::string* text(const std::string& id) const {
        for (const auto* table : {&sources_.texts, &sources_.global_texts})
            if (const auto found = table->find(id); found != table->end()) return &found->second;
        return nullptr;
    }

private:
    using Key = std::vector<std::int32_t>;
    struct Use {
        std::optional<std::string> name;  // none: nothing has that number
        bool live{};                      // named by an action that runs
    };

    static std::string lower(std::string value) {
        std::ranges::transform(value, value.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }
    static std::string key_text(const Key& key) {
        std::string out;
        for (std::size_t i = 0; i < key.size(); ++i) out += (i ? "/" : "") + std::to_string(key[i]);
        return out;
    }
    static bool is_scalar_text(const ProgramOperand& operand) {
        return operand.tag == "CADENA" && std::holds_alternative<std::string>(operand.value);
    }

    // A call inside an operand, (CUTSCENE_EXE (CUTSCENE 3)), without its parentheses.
    std::string call(const ProgramOperand& operand, std::vector<std::string>& notes) {
        const auto text = operand_text(operand, notes);
        return text.size() >= 2 && text.front() == '(' ? text.substr(1, text.size() - 2) : text;
    }

    std::string operand_text(const ProgramOperand& operand, std::vector<std::string>& notes) {
        if (is_scalar(operand)) return scalar_text(operand);
        // A tag without a value of its own is an operation: (THIS), (GET_VIDA (BICHO 3)).
        if (!off_ && std::holds_alternative<std::monostate>(operand.value)) uses_.insert(upper(operand.tag));
        std::string out = "(" + operand.tag;
        if (!std::holds_alternative<std::monostate>(operand.value)) out += " " + scalar_text(operand);
        for (const auto& child : operand.children)
            if (is_scalar(child)) out += " " + scalar_text(child);
        if (const auto name = name_of(operand, notes)) out += " " + (bare_name(*name) ? *name : quote_text(*name));
        for (const auto& child : operand.children)
            if (!is_scalar(child)) out += " " + operand_text(child, notes);
        return out + ")";
    }

    // Records the use and returns the name to print, if the tag names something.
    std::optional<std::string> name_of(const ProgramOperand& operand, std::vector<std::string>& notes) {
        const auto tag = upper(operand.tag);
        if (tag == "FLI") {
            if (const auto* value = std::get_if<std::string>(&operand.value)) {
                const auto id = text_id(*value);
                texts_used_.insert(id);
                const auto* found = text(id);
                notes.push_back(id + ": " + (found ? quote_text(*found) : std::string("(no text)")));
            }
            return std::nullopt;
        }
        const auto number = number_of(operand);
        if (!number) return std::nullopt;
        const auto use = [&](const std::string& kind, const Key& key, const std::string* name) -> std::optional<std::string> {
            auto& entry = used_[kind][key];
            if (name) entry.name = *name;
            entry.live |= !off_;
            if (!name || name->empty()) return std::nullopt;
            return *name;
        };
        const auto in = [](const auto& table, const auto& key) -> const std::string* {
            const auto found = table.find(key);
            return found == table.end() ? nullptr : &found->second;
        };
        if (*number == 0 && (tag == "BICHO" || tag == "DUMMY")) return std::nullopt;  // null handles
        if (tag == "BICHO") return use("actor", {*number}, in(actors_, *number));
        if (tag == "DUMMY") return use("dummy", {*number}, in(dummies_, *number));
        if (tag == "ZONA") return use("area", {*number}, in(areas_, *number));
        if (tag == "GRUPO_PATHPOINT") return use("route", {*number}, in(routes_, *number));
        if (tag == "PATHPOINT") {
            const auto point = std::ranges::find_if(operand.children, is_scalar);
            if (point == operand.children.end() || !number_of(*point)) return std::nullopt;
            const std::pair key{*number, *number_of(*point)};
            const auto* route = in(routes_, *number);
            const auto* name = in(points_, key);
            std::string full;
            if (route && name) full = *route + "/" + (name->empty() ? std::to_string(key.second) : *name);
            return use("point", {key.first, key.second}, route && name ? &full : nullptr);
        }
        if (tag == "SCRIPT" || tag == "TRIGGER") {
            // A program's scripts call their own; the cutscene program also calls the mission's.
            const auto* name = in(cutscene_ ? cutscene_scripts_ : mission_scripts_, *number);
            if (!name && cutscene_) name = in(mission_scripts_, *number);
            return use(cutscene_ ? "cutscene_script" : "script", {*number}, name);
        }
        if (tag == "CUTSCENE") return use("cutscene", {*number}, in(cutscene_scripts_, *number));
        if (tag == "VAR" || tag == "ARRAYID") {
            if (*number & global_variable_bit) return use("variable", {*number}, in(globals_, *number));
            for (const auto& variable : script_->local_variables)
                if (variable.id == *number) return variable.name;
            return std::nullopt;
        }
        if (tag == "ANM_BDD") return use("anim", {*number}, in(sources_.animations, *number));
        if (tag == "SONIDO_BDD") return use("sound", {*number}, in(sources_.sounds, *number));
        if (tag == "CLASSID") return use("class", {*number}, in(sources_.classes, *number));
        if (tag == "ARMA_CLASSID") return use("weapon", {*number}, in(sources_.weapons, *number));
        if (tag == "EFECTO_CLASSID") return use("effect", {*number}, in(sources_.effects, *number));
        return std::nullopt;
    }

    const GodotMissionSources& sources_;
    std::map<std::int32_t, std::string> actors_, dummies_, areas_, routes_, mission_scripts_, cutscene_scripts_, globals_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::string> points_;
    std::map<std::string, std::map<Key, Use>> used_;
    std::set<std::string> texts_used_, uses_;
    const ProgramScript* script_{};
    bool cutscene_{}, off_{};
};

std::string variable_value(const ProgramVariable& variable) {
    const auto& value = variable.initial_value.value;
    if (const auto* number = std::get_if<std::int32_t>(&value)) {
        if (upper(variable.type) == "BOOL") return *number ? "true" : "false";
        return std::to_string(*number);
    }
    if (const auto* number = std::get_if<float>(&value)) return json_number(*number);
    if (const auto* text = std::get_if<std::string>(&value)) {
        if (upper(variable.type) == "BOOL" && (upper(*text) == "TRUE" || upper(*text) == "FALSE"))
            return upper(*text) == "TRUE" ? "true" : "false";
        return json_string(*text);
    }
    return "null";
}

std::string lines_json(const std::vector<ProgramInstruction>& instructions, const ProgramScript& script, const bool cutscene,
                       Namer& namer) {
    std::string out = "[";
    bool first = true;
    for (const auto& row : program_structure(instructions)) {
        if (!row.instruction) continue;
        out += std::string(first ? "\n" : ",\n") +
               json_string(std::string(row.depth * 2, ' ') + namer.line(*row.instruction, script, cutscene));
        first = false;
    }
    return out + (first ? "]" : "\n]");
}

} // namespace

std::string godot_mission_json(const std::string& mission, const GodotMissionSources& sources) {
    const auto flow = MissionFlow::build(sources.mission, sources.cutscene);
    Namer namer(sources);
    std::string out = "{\n\"format\":" + json_string(mission_format) + ",\n\"version\":1,\n\"mission\":" + json_string(mission);

    // What the scene itself sets at load: who the player is, and per actor its
    // attached scripts and portrait.
    if (sources.scene) {
        const auto& player = sources.scene->player();
        std::string fields;
        const auto add = [&](const char* key, const std::optional<std::int32_t> value) {
            if (value) fields += std::string(fields.empty() ? "" : ",") + json_string(key) + ":" + std::to_string(*value);
        };
        add("actor", player.active_player);
        add("commando_start", player.commando_start);
        add("sniper_start", player.sniper_start);
        add("spy_start", player.spy_start);
        out += ",\n\"player\":{" + fields + "}";
        out += ",\n\"actors\":[";
        bool first = true;
        for (const auto& actor : sources.scene->actors()) {
            if (!actor.id || (actor.script_ids.empty() && !actor.portrait)) continue;
            out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(*actor.id) +
                   ",\"name\":" + json_string(actor.name.value_or(""));
            if (!actor.script_ids.empty()) out += ",\"scripts\":" + json_integers(actor.script_ids);
            if (actor.portrait) out += ",\"portrait\":" + json_string(*actor.portrait);
            out += "}";
            first = false;
        }
        out += first ? "]" : "\n]";
    }

    out += ",\n\"objectives\":[";
    bool first = true;
    for (const auto& objective : flow.objectives()) {
        out += std::string(first ? "\n" : ",\n") + "{\"number\":" + std::to_string(objective.number);
        if (objective.secondary) out += std::string(",\"secondary\":") + (*objective.secondary ? "true" : "false");
        if (!objective.label.empty()) out += ",\"label\":" + json_string(namer.text_id(objective.label));
        out += ",\"defined_by\":" + json_integers(objective.defined_by) + ",\"completed_by\":" + json_integers(objective.completed_by) + "}";
        first = false;
    }
    out += first ? "]" : "\n]";

    out += ",\n\"variables\":[";
    first = true;
    if (sources.mission)
        for (const auto& variable : sources.mission->global_variables()) {
            out += std::string(first ? "\n" : ",\n") + "{\"id\":" + std::to_string(variable.id) +
                   ",\"name\":" + json_string(variable.name) + ",\"type\":" + json_string(variable.type);
            if (variable.is_array) out += ",\"array\":true";
            else out += ",\"value\":" + variable_value(variable);
            out += "}";
            first = false;
        }
    out += first ? "]" : "\n]";

    out += ",\n\"events\":[";
    first = true;
    for (const auto& event : flow.events()) {
        out += std::string(first ? "\n" : ",\n") + "{\"name\":" + json_string(event.name) +
               ",\"builtin\":" + (event.builtin ? "true" : "false") + ",\"listeners\":" + json_integers(event.listeners) +
               ",\"senders\":" + json_integers(event.senders) + "}";
        first = false;
    }
    out += first ? "]" : "\n]";

    out += ",\n\"scripts\":[";
    first = true;
    for (const auto* program : {sources.mission, sources.cutscene}) {
        if (!program) continue;
        const bool cutscene = program == sources.cutscene;
        const auto program_name = cutscene ? "cutscene" : "mission";
        for (const auto& script : program->scripts()) {
            out += std::string(first ? "\n" : ",\n") + "{\"program\":" + json_string(program_name) +
                   ",\"id\":" + std::to_string(script.id) + ",\"name\":" + json_string(script.name);
            if (!script.folder.empty()) out += ",\"folder\":" + json_string(script.folder);
            out += std::string(",\"trigger\":") + (script.flags.trigger.value_or(false) ? "true" : "false") +
                   ",\"enabled\":" + (script.flags.enabled.value_or(true) ? "true" : "false");
            std::vector<std::string> events;
            for (const auto& event : script.events) events.push_back(event.name);
            if (!events.empty()) out += ",\"events\":" + json_strings(events);
            if (const auto* flowing = flow.script(program_name, script.id)) {
                if (!flowing->sends.empty()) out += ",\"sends\":" + json_strings(flowing->sends);
                if (!flowing->defines.empty()) out += ",\"defines\":" + json_integers(flowing->defines);
                if (!flowing->completes.empty()) out += ",\"completes\":" + json_integers(flowing->completes);
                if (!flowing->cutscenes.empty()) out += ",\"cutscenes\":" + json_integers(flowing->cutscenes);
                if (flowing->mission_success) out += ",\"mission_success\":true";
                if (flowing->mission_failure) out += ",\"mission_failure\":true";
            }
            if (!script.local_variables.empty()) {
                out += ",\"variables\":" + json_list(script.local_variables, [](const ProgramVariable& variable) {
                           return "{\"id\":" + std::to_string(variable.id) + ",\"name\":" + json_string(variable.name) +
                                  ",\"type\":" + json_string(variable.type) + "}";
                       });
            }
            if (!script.conditions.empty()) out += ",\"conditions\":" + lines_json(script.conditions, script, cutscene, namer);
            out += ",\"actions\":" + lines_json(script.actions, script, cutscene, namer);
            // The instructions it runs, for the parity table (docs/parity.md in godot-opencsf).
            out += ",\"uses\":" + json_strings(namer.take_uses()) + "}";
            first = false;
        }
    }
    out += first ? "]" : "\n]";

    // Every string of the mission's own text file, and the global ones its scripts show.
    out += ",\n\"texts\":{";
    first = true;
    std::map<std::string, std::string> texts = sources.texts;
    for (const auto& id : namer.texts_used())
        if (const auto found = sources.global_texts.find(id); found != sources.global_texts.end()) texts.try_emplace(id, found->second);
    for (const auto& [id, text] : texts) {
        out += std::string(first ? "\n" : ",\n") + json_string(id) + ":" + json_string(text);
        first = false;
    }
    out += first ? "}" : "\n}";

    out += ",\n\"names\":" + namer.names_json();
    std::vector<std::string> missing_texts;
    for (const auto& id : namer.texts_used())
        if (!namer.text(id)) missing_texts.push_back(id);
    out += ",\n\"unresolved\":" + namer.unresolved_json();
    if (!missing_texts.empty()) out += ",\n\"missing_texts\":" + json_strings(missing_texts);

    out += ",\n\"findings\":[";
    first = true;
    for (const auto& finding : flow.findings()) {
        out += std::string(first ? "\n" : ",\n") + "{\"severity\":" + json_string(flow_severity_name(finding.severity));
        if (finding.script) out += ",\"script\":" + std::to_string(*finding.script);
        out += ",\"message\":" + json_string(finding.message) + "}";
        first = false;
    }
    out += first ? "]" : "\n]";
    return out + "\n}\n";
}

} // namespace csf
