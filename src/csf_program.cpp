#include "csf/program.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>

namespace csf {
namespace {

std::string label(const Document& document, const Node& node) {
    if (!node.identifier_index) return {};
    const auto* value = document.identifier(*node.identifier_index);
    return value ? value->display_utf8() : std::string{};
}
CsfSourceId source(const Document& document, const Node& node) {
    const auto& entry = document.entries().at(node.entry_index);
    return {document.source_path(), node.entry_index, entry.source};
}
const Node* child(const Document& document, const Node& parent, const std::string_view name) {
    const auto found = std::ranges::find_if(
        parent.children, [&](const Node& value) { return label(document, value) == name; });
    return found == parent.children.end() ? nullptr : &*found;
}
const Node* find_field(const Document& document, const std::string_view name) {
    const auto visit = [&](const auto& self, const Node& node) -> const Node* {
        if (label(document, node) == name) return &node;
        for (const auto& value : node.children)
            if (const auto* result = self(self, value)) return result;
        return nullptr;
    };
    for (const auto& root : document.roots())
        if (const auto* result = visit(visit, root)) return result;
    return nullptr;
}
std::optional<std::int32_t> integer(const Node* node) {
    if (node)
        if (const auto* value = std::get_if<std::int32_t>(&node->scalar)) return *value;
    return std::nullopt;
}
std::optional<std::string> text(const Document& document, const Node* node) {
    if (node)
        if (const auto* index = std::get_if<std::uint32_t>(&node->scalar))
            if (const auto* value = document.string(*index)) return value->display_utf8();
    return std::nullopt;
}
std::string upper(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

using Scalar = std::variant<std::monostate, std::int32_t, float, std::string>;
Scalar scalar(const Document& document, const Node& node) {
    if (const auto* value = std::get_if<std::int32_t>(&node.scalar)) return *value;
    if (const auto* value = std::get_if<float>(&node.scalar)) return *value;
    if (const auto value = text(document, &node)) return *value;
    return {};
}

ProgramOperand operand(const Document& document, const Node& node) {
    ProgramOperand result;
    result.source = source(document, node);
    if (!node.children.empty()) {
        if (const auto tag = text(document, &node.children.front())) result.tag = *tag;
        const std::size_t begin = result.tag.empty() ? 0 : 1;
        for (std::size_t i = begin; i < node.children.size(); ++i) {
            const auto& item = node.children[i];
            const auto value = scalar(document, item);
            if (std::holds_alternative<std::monostate>(result.value) &&
                !std::holds_alternative<std::monostate>(value))
                result.value = value;
            else
                result.children.push_back(operand(document, item));
        }
    } else {
        result.tag = label(document, node);
        result.value = scalar(document, node);
    }
    return result;
}

ProgramOperand initial_value(const Document& document, const Node* node, const std::string& type) {
    if (!node) return {};
    ProgramOperand result = operand(document, *node);
    result.tag = type;
    return result;
}

ProgramVariable variable(const Document& document, const Node& node) {
    ProgramVariable result;
    result.source = source(document, node);
    result.id = integer(child(document, node, ".ID")).value_or(0);
    result.type = text(document, child(document, node, ".TYPE")).value_or("");
    result.name = text(document, child(document, node, ".NOMBRE")).value_or("");
    result.is_array = integer(child(document, node, ".ARRAY")).value_or(0) != 0;
    result.initial_value = initial_value(document, child(document, node, ".VALOR"), result.type);
    return result;
}

ProgramInstruction instruction(const Document& document, const Node& node) {
    ProgramInstruction result;
    result.source = source(document, node);
    if (!node.children.empty()) result.opcode = text(document, &node.children.front()).value_or("");
    const std::size_t begin = result.opcode.empty() ? 0 : 1;
    for (std::size_t i = begin; i < node.children.size(); ++i)
        result.operands.push_back(operand(document, node.children[i]));
    return result;
}

std::vector<ProgramInstruction> instructions(const Document& document, const Node* node) {
    std::vector<ProgramInstruction> result;
    if (node)
        for (const auto& item : node->children) result.push_back(instruction(document, item));
    return result;
}

std::string escape(const std::string_view value) {
    std::ostringstream out;
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (byte < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(byte)
                    << std::dec << std::setfill(' ');
            else
                out << static_cast<char>(byte);
        }
    }
    return out.str();
}
void json_source(std::ostringstream& out, const CsfSourceId& value) {
    out << "{\"file\":\"" << escape(value.file.generic_string()) << "\",\"entry\":"
        << value.entry_index << ",\"offset\":" << value.range.offset << ",\"size\":"
        << value.range.size << '}';
}
void json_scalar(std::ostringstream& out, const Scalar& value) {
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>) out << "null";
            else if constexpr (std::is_same_v<T, std::string>) out << '"' << escape(item) << '"';
            else if constexpr (std::is_floating_point_v<T>) {
                if (std::isfinite(item)) out << item;
                else out << "null";
            } else out << item;
        },
        value);
}
void json_operand(std::ostringstream& out, const ProgramOperand& value) {
    out << "{\"source\":";
    json_source(out, value.source);
    out << ",\"tag\":\"" << escape(value.tag) << "\",\"value\":";
    json_scalar(out, value.value);
    out << ",\"children\":[";
    for (std::size_t i = 0; i < value.children.size(); ++i) {
        if (i) out << ',';
        json_operand(out, value.children[i]);
    }
    out << "]}";
}
std::string display(const Scalar& value) {
    return std::visit(
        [](const auto& item) -> std::string {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>) return {};
            else if constexpr (std::is_same_v<T, std::string>) return item;
            else return std::to_string(item);
        },
        value);
}

ProgramReferenceKind kind_for(const std::string& value) {
    const auto key = upper(value);
    if (key == "BICHO") return ProgramReferenceKind::actor;
    if (key == "DUMMY") return ProgramReferenceKind::dummy;
    if (key == "ZONA") return ProgramReferenceKind::area;
    if (key == "PATHPOINT" || key == "GRUPO_PATHPOINT")
        return ProgramReferenceKind::navigation_point;
    if (key == "SCRIPT" || key == "CUTSCENE") return ProgramReferenceKind::script;
    if (key == "TRIGGER") return ProgramReferenceKind::trigger;
    if (key == "EVENT") return ProgramReferenceKind::event;
    if (key == "ANM_BDD") return ProgramReferenceKind::animation;
    if (key == "CLASSID") return ProgramReferenceKind::object_class;
    if (key == "EFECTO_CLASSID") return ProgramReferenceKind::effect_class;
    if (key == "ARMA_CLASSID") return ProgramReferenceKind::weapon_class;
    if (key == "SONIDO_BDD") return ProgramReferenceKind::sound;
    if (key == "VAR") return ProgramReferenceKind::variable;
    if (key == "ARRAYID") return ProgramReferenceKind::array;
    return ProgramReferenceKind::unknown;
}

void walk_operand(const ProgramOperand& operand_value, const auto& function) {
    function(operand_value);
    for (const auto& child_value : operand_value.children) walk_operand(child_value, function);
}

} // namespace

ProgramDocument ProgramDocument::project(const Document& document) {
    ProgramDocument result;
    result.source_path_ = document.source_path();
    if (const auto* resources = find_field(document, ".RECURSOS")) {
        for (const auto& list_node : resources->children) {
            ProgramResourceList list{source(document, list_node), label(document, list_node), {}};
            for (const auto& value : list_node.children) {
                const auto item = scalar(document, value);
                if (const auto* integer_value = std::get_if<std::int32_t>(&item))
                    list.values.emplace_back(*integer_value);
                else if (const auto* real_value = std::get_if<float>(&item))
                    list.values.emplace_back(*real_value);
                else if (const auto* text_value = std::get_if<std::string>(&item))
                    list.values.emplace_back(*text_value);
            }
            result.resources_.push_back(std::move(list));
        }
    }
    if (const auto* globals = find_field(document, ".VARIABLES"))
        for (const auto& item : globals->children) result.globals_.push_back(variable(document, item));
    const auto* scripts = find_field(document, ".SCRIPTS");
    if (!scripts) return result;
    std::map<std::int32_t, std::size_t> ids;
    for (const auto& node : scripts->children) {
        ProgramScript script;
        script.source = source(document, node);
        script.id = integer(child(document, node, ".ID")).value_or(0);
        script.name = text(document, child(document, node, ".NOMBRE")).value_or("");
        script.folder = text(document, child(document, node, ".CARPETA")).value_or("");
        if (const auto* flags = child(document, node, ".FLAGS")) {
            if (const auto value = integer(child(document, *flags, ".TRIGGER")))
                script.flags.trigger = *value != 0;
            if (const auto value = integer(child(document, *flags, ".ENABLED")))
                script.flags.enabled = *value != 0;
            if (const auto value = integer(child(document, *flags, ".VALIDO")))
                script.flags.valid = *value != 0;
        }
        if (const auto* locals = child(document, node, ".VARIABLES"))
            for (const auto& item : locals->children)
                script.local_variables.push_back(variable(document, item));
        if (const auto* events = child(document, node, ".EVENTOS"))
            for (const auto& item : events->children) {
                std::string name;
                if (const auto value = text(document, &item)) name = *value;
                else if (!item.children.empty()) name = text(document, &item.children.front()).value_or("");
                script.events.push_back({source(document, item), std::move(name)});
            }
        script.conditions = instructions(document, child(document, node, ".CONDICIONES"));
        script.actions = instructions(document, child(document, node, ".ACCIONES"));
        if (++ids[script.id] > 1)
            result.diagnostics_.push_back({Diagnostic::Severity::warning, script.source,
                                           "duplicate-script-id",
                                           "Script ID " + std::to_string(script.id) +
                                               " is not unique in this document"});
        (void)program_structure(script.conditions, &result.diagnostics_);
        (void)program_structure(script.actions, &result.diagnostics_);
        result.scripts_.push_back(std::move(script));
    }
    return result;
}

const ProgramScript* ProgramDocument::find_script(const std::int32_t id) const noexcept {
    const ProgramScript* result{};
    for (const auto& script : scripts_)
        if (script.id == id) {
            if (result) return nullptr;
            result = &script;
        }
    return result;
}

std::vector<ProgramStructureRow>
program_structure(const std::vector<ProgramInstruction>& values,
                  std::vector<TypedDiagnostic>* diagnostics) {
    std::vector<ProgramStructureRow> result;
    std::vector<std::pair<std::string, CsfSourceId>> scopes;
    const auto opener = [](const std::string& key) -> std::string {
        if (key == "IF") return "ENDIF";
        if (key == "WHILE") return "WEND";
        if (key == "FOREACH") return "ENDFOR";
        return {};
    };
    for (const auto& value : values) {
        const auto key = upper(value.opcode);
        const bool middle = key == "ELSE";
        const bool closer = key == "ENDIF" || key == "WEND" || key == "ENDFOR";
        if (middle || closer) {
            const auto expected = middle ? std::string("ENDIF") : key;
            if (scopes.empty()) {
                if (diagnostics)
                    diagnostics->push_back({Diagnostic::Severity::warning, value.source,
                                            "unbalanced-program-marker",
                                            "Unexpected structural marker " + value.opcode});
            } else {
                if (scopes.back().first != expected && diagnostics)
                    diagnostics->push_back(
                        {Diagnostic::Severity::warning, value.source,
                         "mismatched-program-marker",
                         "Structural marker " + value.opcode + " closes " +
                             scopes.back().first});
                scopes.pop_back();
            }
        }
        const auto close = opener(key);
        result.push_back({&value, scopes.size(), closer || middle, !close.empty() || middle});
        if (!close.empty()) scopes.emplace_back(close, value.source);
        else if (middle) scopes.emplace_back("ENDIF", value.source);
    }
    if (diagnostics)
        for (const auto& [expected, where] : scopes)
            diagnostics->push_back({Diagnostic::Severity::warning, where,
                                    "unbalanced-program-marker",
                                    "Missing structural marker " + expected});
    return result;
}

void ProgramReferenceIndex::add_program(const ProgramDocument& program, const MissionScene* scene,
                                        const AnimationCatalog* animations) {
    for (const auto& item : program.scripts())
        script_definitions_.push_back({item.id, item.flags.trigger == true, item.source});
    for (auto& reference : references_) {
        if (upper(reference.tag) != "CUTSCENE" ||
            (reference.status != ProgramReferenceStatus::missing &&
             reference.status != ProgramReferenceStatus::candidate))
            continue;
        std::int32_t id{};
        try {
            id = static_cast<std::int32_t>(std::stol(reference.display_value));
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& definition : script_definitions_)
            if (definition.id == id && definition.source.file != reference.source.file &&
                std::ranges::none_of(reference.targets, [&](const auto& target) {
                    return target.file == definition.source.file &&
                           target.entry_index == definition.source.entry_index;
                }))
                reference.targets.push_back(definition.source);
        if (!reference.targets.empty()) reference.status = ProgramReferenceStatus::candidate;
    }
    std::map<std::int32_t, std::vector<const MissionActor*>> actors;
    std::map<std::int32_t, std::vector<const MissionDummy*>> dummies;
    std::map<std::int32_t, std::vector<const MissionArea*>> areas;
    if (scene) {
        for (const auto& item : scene->actors()) if (item.id) actors[*item.id].push_back(&item);
        for (const auto& item : scene->dummies()) if (item.id) dummies[*item.id].push_back(&item);
        for (const auto& item : scene->areas()) if (item.id) areas[*item.id].push_back(&item);
    }
    std::map<std::int32_t, std::vector<const ProgramScript*>> scripts;
    for (const auto& item : program.scripts()) scripts[item.id].push_back(&item);

    const auto inspect = [&](const ProgramOperand& operand_value, const ProgramScript* owner) {
        ProgramReference reference;
        reference.source = operand_value.source;
        reference.owner_script = owner ? owner->id : 0;
        reference.tag = operand_value.tag;
        reference.kind = kind_for(operand_value.tag);
        if (reference.kind == ProgramReferenceKind::unknown) return;
        reference.display_value = display(operand_value.value);
        const auto* numeric = std::get_if<std::int32_t>(&operand_value.value);
        const auto finish = [&](const auto& candidates) {
            for (const auto* candidate : candidates) reference.targets.push_back(candidate->source);
            reference.status = candidates.empty()   ? ProgramReferenceStatus::missing
                               : candidates.size() == 1 ? ProgramReferenceStatus::resolved
                                                        : ProgramReferenceStatus::ambiguous;
        };
        if (reference.kind == ProgramReferenceKind::actor && numeric) finish(actors[*numeric]);
        else if (reference.kind == ProgramReferenceKind::dummy && numeric) finish(dummies[*numeric]);
        else if (reference.kind == ProgramReferenceKind::area && numeric) finish(areas[*numeric]);
        else if ((reference.kind == ProgramReferenceKind::script ||
                  reference.kind == ProgramReferenceKind::trigger) && numeric) {
            auto candidates = scripts[*numeric];
            if (reference.kind == ProgramReferenceKind::trigger)
                std::erase_if(candidates, [](const auto* item) { return item->flags.trigger != true; });
            finish(candidates);
            if (candidates.empty() && upper(reference.tag) == "CUTSCENE") {
                for (const auto& definition : script_definitions_)
                    if (definition.id == *numeric && definition.source.file != reference.source.file)
                        reference.targets.push_back(definition.source);
                if (!reference.targets.empty()) reference.status = ProgramReferenceStatus::candidate;
            }
        } else if (reference.kind == ProgramReferenceKind::animation && numeric && animations) {
            if (const auto* candidate = animations->find_id(*numeric)) {
                reference.targets.push_back(candidate->source);
                reference.status = ProgramReferenceStatus::resolved;
            } else reference.status = ProgramReferenceStatus::missing;
        } else if (reference.kind == ProgramReferenceKind::navigation_point && numeric && scene &&
                   !operand_value.children.empty()) {
            const auto* point_id = std::get_if<std::int32_t>(&operand_value.children.front().value);
            if (point_id) {
                if (const auto* point = scene->navigation_point(*numeric, *point_id)) {
                    reference.targets.push_back(point->source);
                    reference.status = ProgramReferenceStatus::resolved;
                } else {
                    reference.status = ProgramReferenceStatus::missing;
                }
            }
        } else if ((reference.kind == ProgramReferenceKind::variable ||
                    reference.kind == ProgramReferenceKind::array) && numeric) {
            std::vector<const ProgramVariable*> candidates;
            if (owner)
                for (const auto& item : owner->local_variables)
                    if (item.id == *numeric &&
                        (reference.kind != ProgramReferenceKind::array || item.is_array))
                        candidates.push_back(&item);
            if (candidates.empty())
                for (const auto& item : program.global_variables())
                    if (item.id == *numeric &&
                        (reference.kind != ProgramReferenceKind::array || item.is_array))
                        candidates.push_back(&item);
            finish(candidates);
        } else if (reference.kind == ProgramReferenceKind::event) {
            for (const auto& script : program.scripts())
                for (const auto& event : script.events)
                    if (event.name == reference.display_value) reference.targets.push_back(event.source);
            reference.status = reference.targets.empty() ? ProgramReferenceStatus::candidate
                                                         : ProgramReferenceStatus::resolved;
        } else {
            reference.status = ProgramReferenceStatus::unclassified;
            reference.detail = "Target database or sentinel classification is not loaded";
        }
        references_.push_back(std::move(reference));
    };
    for (const auto& item : program.global_variables()) walk_operand(item.initial_value, [&](const auto& v) { inspect(v, nullptr); });
    for (const auto& script : program.scripts()) {
        for (const auto& item : script.local_variables) walk_operand(item.initial_value, [&](const auto& v) { inspect(v, &script); });
        const auto scan = [&](const auto& list) {
            for (const auto& item : list)
                for (const auto& value : item.operands)
                    walk_operand(value, [&](const auto& v) { inspect(v, &script); });
        };
        scan(script.conditions);
        scan(script.actions);
    }
}

std::vector<const ProgramReference*> ProgramReferenceIndex::uses(const CsfSourceId& target) const {
    std::vector<const ProgramReference*> result;
    for (const auto& reference : references_)
        if (std::ranges::any_of(reference.targets, [&](const auto& value) {
                return value.file == target.file && value.entry_index == target.entry_index;
            }))
            result.push_back(&reference);
    return result;
}

std::string program_json(const ProgramDocument& program) {
    std::ostringstream out;
    out << "{\"source\":\"" << escape(program.source_path().generic_string())
        << "\",\"resources\":[";
    for (std::size_t i = 0; i < program.resources().size(); ++i) {
        if (i) out << ',';
        const auto& list = program.resources()[i];
        out << "{\"source\":";
        json_source(out, list.source);
        out << ",\"name\":\"" << escape(list.name) << "\",\"values\":[";
        for (std::size_t j = 0; j < list.values.size(); ++j) {
            if (j) out << ',';
            std::visit(
                [&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::string>)
                        out << '"' << escape(value) << '"';
                    else if constexpr (std::is_floating_point_v<T>) {
                        if (std::isfinite(value)) out << value;
                        else out << "null";
                    } else
                        out << value;
                },
                list.values[j]);
        }
        out << "]}";
    }
    out << "],\"globals\":[";
    const auto json_variable = [&](const ProgramVariable& value) {
        out << "{\"source\":"; json_source(out, value.source);
        out << ",\"id\":" << value.id << ",\"type\":\"" << escape(value.type)
            << "\",\"name\":\"" << escape(value.name) << "\",\"array\":"
            << (value.is_array ? "true" : "false") << ",\"initial\":";
        json_operand(out, value.initial_value); out << '}';
    };
    for (std::size_t i = 0; i < program.global_variables().size(); ++i) {
        if (i) out << ',';
        json_variable(program.global_variables()[i]);
    }
    out << "],\"scripts\":[";
    for (std::size_t i = 0; i < program.scripts().size(); ++i) {
        if (i) out << ',';
        const auto& script = program.scripts()[i];
        out << "{\"source\":"; json_source(out, script.source);
        out << ",\"id\":" << script.id << ",\"name\":\"" << escape(script.name)
            << "\",\"folder\":\"" << escape(script.folder) << "\",\"flags\":{"
            << "\"trigger\":" << (script.flags.trigger ? (*script.flags.trigger ? "true" : "false") : "null")
            << ",\"enabled\":" << (script.flags.enabled ? (*script.flags.enabled ? "true" : "false") : "null")
            << ",\"valid\":" << (script.flags.valid ? (*script.flags.valid ? "true" : "false") : "null")
            << "},\"variables\":[";
        for (std::size_t j = 0; j < script.local_variables.size(); ++j) {
            if (j) out << ',';
            json_variable(script.local_variables[j]);
        }
        out << "],\"events\":[";
        for (std::size_t j = 0; j < script.events.size(); ++j) {
            if (j) out << ',';
            out << "{\"source\":";
            json_source(out, script.events[j].source);
            out << ",\"name\":\"" << escape(script.events[j].name) << "\"}";
        }
        const auto json_instructions = [&](const auto& list) {
            out << '[';
            for (std::size_t j = 0; j < list.size(); ++j) {
                if (j) out << ',';
                out << "{\"source\":";
                json_source(out, list[j].source);
                out << ",\"opcode\":\"" << escape(list[j].opcode) << "\",\"operands\":[";
                for (std::size_t k = 0; k < list[j].operands.size(); ++k) {
                    if (k) out << ',';
                    json_operand(out, list[j].operands[k]);
                }
                out << "]}";
            }
            out << ']';
        };
        out << "],\"conditions\":"; json_instructions(script.conditions);
        out << ",\"actions\":"; json_instructions(script.actions); out << '}';
    }
    out << "]}";
    return out.str();
}

const char* program_reference_status_name(const ProgramReferenceStatus value) noexcept {
    switch (value) {
    case ProgramReferenceStatus::resolved: return "resolved";
    case ProgramReferenceStatus::candidate: return "candidate";
    case ProgramReferenceStatus::missing: return "missing";
    case ProgramReferenceStatus::ambiguous: return "ambiguous";
    case ProgramReferenceStatus::unclassified: return "unclassified";
    }
    return "unclassified";
}
const char* program_reference_kind_name(const ProgramReferenceKind value) noexcept {
    switch (value) {
    case ProgramReferenceKind::actor: return "actor";
    case ProgramReferenceKind::dummy: return "dummy";
    case ProgramReferenceKind::area: return "area";
    case ProgramReferenceKind::navigation_point: return "navigation-point";
    case ProgramReferenceKind::script: return "script";
    case ProgramReferenceKind::trigger: return "trigger";
    case ProgramReferenceKind::event: return "event";
    case ProgramReferenceKind::animation: return "animation";
    case ProgramReferenceKind::object_class: return "object-class";
    case ProgramReferenceKind::effect_class: return "effect-class";
    case ProgramReferenceKind::weapon_class: return "weapon-class";
    case ProgramReferenceKind::sound: return "sound";
    case ProgramReferenceKind::variable: return "variable";
    case ProgramReferenceKind::array: return "array";
    case ProgramReferenceKind::unknown: return "unknown";
    }
    return "unknown";
}

} // namespace csf
