#include "csf/animation_catalog.hpp"
#include "csf/cmo.hpp"
#include "csf/document.hpp"
#include "csf/export.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "csf/program.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

std::string lower_extension(const std::filesystem::path& path);
std::string lower_filename(const std::filesystem::path& path);

std::string escaped(const std::string_view value) {
    std::ostringstream output;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '\\':
            output << "\\\\";
            break;
        case '"':
            output << "\\\"";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (byte < 0x20) {
                output << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(byte) << std::dec << std::setfill(' ');
            } else {
                output << character;
            }
        }
    }
    return output.str();
}

std::string node_label(const csf::Document& document, const csf::Node& node) {
    if (!node.identifier_index) return {};
    const auto* value = document.identifier(*node.identifier_index);
    return value ? value->display_utf8() : "<invalid-identifier>";
}

void print_nodes(const csf::Document& document, const std::vector<csf::Node>& nodes,
                 const unsigned depth = 0) {
    for (const auto& node : nodes) {
        const auto& entry = document.entries().at(node.entry_index);
        std::cout << std::string(depth * 2, ' ') << '[' << node.entry_index << " @0x" << std::hex
                  << entry.source.offset << std::dec << "] ";
        const auto kind = entry.kind();
        std::cout << (kind ? csf::value_kind_name(*kind) : "unknown");
        const auto label = node_label(document, node);
        if (!label.empty()) std::cout << " " << escaped(label);
        if (const auto* value = std::get_if<std::int32_t>(&node.scalar)) {
            std::cout << " = " << *value;
        } else if (const auto* value = std::get_if<float>(&node.scalar)) {
            std::cout << " = " << *value;
        } else if (const auto* index = std::get_if<std::uint32_t>(&node.scalar)) {
            const auto* value = document.string(*index);
            std::cout << " = \"" << (value ? escaped(value->display_utf8()) : "<invalid-string>")
                      << '"';
        } else if (kind && (*kind == csf::ValueKind::group || *kind == csf::ValueKind::array)) {
            std::cout << " (declared children=" << entry.raw_value_or_size << ')';
        }
        std::cout << '\n';
        print_nodes(document, node.children, depth + 1);
    }
}

void print_diagnostics(const csf::Document& document) {
    for (const auto& diagnostic : document.diagnostics()) {
        std::cerr << (diagnostic.severity == csf::Diagnostic::Severity::error ? "error" : "warning")
                  << " at 0x" << std::hex << diagnostic.offset << std::dec;
        if (diagnostic.entry_index) std::cerr << " entry " << *diagnostic.entry_index;
        std::cerr << ": " << diagnostic.message << '\n';
    }
}

void print_summary(const csf::Document& document) {
    std::cout << "State: " << csf::parse_state_name(document.state()) << '\n'
              << "Bytes: " << document.bytes().size() << '\n';
    if (document.state() == csf::ParseState::non_csffbs) return;
    const auto& header = document.header();
    std::cout << "Version: " << header.version << '\n'
              << "Reserved: 0x" << std::hex << std::to_integer<unsigned>(header.reserved[1])
              << std::setw(2) << std::setfill('0') << std::to_integer<unsigned>(header.reserved[0])
              << std::dec << std::setfill(' ') << '\n'
              << "Entries: " << document.entries().size() << '/' << header.entry_count << '\n'
              << "Identifiers: " << document.identifiers().size() << '/' << header.identifier_count
              << '\n'
              << "Strings: " << document.strings().size() << '/' << header.string_count << '\n'
              << "Roots: " << document.roots().size() << '\n'
              << "Trailing bytes: " << document.trailing_bytes().size() << '\n'
              << "Diagnostics: " << document.diagnostics().size() << '\n';
}

void usage() {
    std::cerr
        << "Usage: csf-info <file> [--summary|--validate|--tree|--strings|--find <text>\n"
           "                            |--export-text <new-path|->|--export-json <new-path|->]\n"
           "       csf-info corpus <directory>\n"
           "       csf-info cmo <file.cmo>\n"
           "       csf-info animations <Anims.bdd> [--root <resource-root>]\n"
           "       csf-info script-animations <file.gsc>\n"
           "       csf-info program <file.gsc|file.csc> [--summary|--scripts|--script <id>\n"
           "                            |--references|--diagnostics|--json -]\n"
           "       csf-info program-corpus <directory>\n"
           "       csf-info cutscene <file.csc> [--references]\n"
           "       csf-info mission <scene-or-directory> "
           "[--summary|--dependencies|--missing|--objects|--navigation|--spatial|--associations\n"
           "                        |--scene-objects|--bridges|--water|--metadata]\n"
           "                       [--graph <new-json>] [--package-root <directory>] "
           "[--duplicates]\n"
           "                       [--scene-json <new-json>] [--symbols <exact-name>]\n"
           "                       [--script-uses <actor|dummy|area>:<id>]\n"
           "                       [--root <resource-root>]\n"
           "       csf-info uses <asset> --root <resource-root>\n"
           "       csf-info compare <scene-a> <scene-b>\n";
}

std::string program_scalar(const csf::ProgramOperand& operand) {
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) return "-";
            else if constexpr (std::is_same_v<T, std::string>) return value;
            else return std::to_string(value);
        },
        operand.value);
}

void print_program_operand(const csf::ProgramOperand& operand, const unsigned depth) {
    std::cout << std::string(depth * 2, ' ') << "OPERAND\tentry=" << operand.source.entry_index
              << "\toffset=" << operand.source.range.offset << "\ttag=" << operand.tag
              << "\tvalue=" << program_scalar(operand) << '\n';
    for (const auto& child : operand.children) print_program_operand(child, depth + 1);
}

void print_program_instructions(const std::vector<csf::ProgramInstruction>& instructions,
                                const char* section) {
    for (const auto& row : csf::program_structure(instructions)) {
        const auto& instruction = *row.instruction;
        std::cout << section << "\tdepth=" << row.depth
                  << "\tentry=" << instruction.source.entry_index
                  << "\toffset=" << instruction.source.range.offset
                  << "\topcode=" << instruction.opcode
                  << "\toperands=" << instruction.operands.size() << '\n';
        for (const auto& operand : instruction.operands) print_program_operand(operand, 1);
    }
}

void print_program_script(const csf::ProgramScript& script) {
    const auto flag = [](const std::optional<bool> value) {
        return value ? (*value ? "1" : "0") : "-";
    };
    std::cout << "SCRIPT\tentry=" << script.source.entry_index << "\toffset="
              << script.source.range.offset << "\tid=" << script.id << "\tname=" << script.name
              << "\tfolder=" << script.folder << "\ttrigger=" << flag(script.flags.trigger)
              << "\tenabled=" << flag(script.flags.enabled) << "\tvalid="
              << flag(script.flags.valid) << "\tlocals=" << script.local_variables.size()
              << "\tevents=" << script.events.size() << "\tconditions="
              << script.conditions.size() << "\tactions=" << script.actions.size() << '\n';
    for (const auto& variable : script.local_variables)
        std::cout << "VARIABLE\tscope=" << script.id << "\tentry=" << variable.source.entry_index
                  << "\tid=" << variable.id << "\ttype=" << variable.type << "\tname="
                  << variable.name << "\tarray=" << variable.is_array
                  << "\tvalue=" << program_scalar(variable.initial_value) << '\n';
    for (const auto& event : script.events)
        std::cout << "EVENT\tentry=" << event.source.entry_index << "\tname=" << event.name << '\n';
    print_program_instructions(script.conditions, "CONDITION");
    print_program_instructions(script.actions, "ACTION");
}

int program_command(const int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 1;
    }
    const auto document = csf::Document::load(argv[2]);
    const auto program = csf::ProgramDocument::project(document);
    const std::string_view mode = argc >= 4 ? argv[3] : "--summary";
    if (mode == "--summary") {
        std::size_t locals{}, events{}, conditions{}, actions{}, operands{};
        const auto count_operand = [&](const auto& self, const csf::ProgramOperand& value) -> void {
            ++operands;
            for (const auto& child : value.children) self(self, child);
        };
        for (const auto& script : program.scripts()) {
            locals += script.local_variables.size();
            events += script.events.size();
            conditions += script.conditions.size();
            actions += script.actions.size();
            for (const auto* list : {&script.conditions, &script.actions})
                for (const auto& instruction : *list)
                    for (const auto& operand : instruction.operands)
                        count_operand(count_operand, operand);
        }
        std::cout << "PROGRAM\tstate=" << csf::parse_state_name(document.state())
                  << "\tresources=" << program.resources().size()
                  << "\tglobals=" << program.global_variables().size()
                  << "\tscripts=" << program.scripts().size() << "\tlocals=" << locals
                  << "\tevents=" << events << "\tconditions=" << conditions
                  << "\tactions=" << actions << "\toperands=" << operands
                  << "\tdiagnostics=" << program.diagnostics().size() << '\n';
    } else if (mode == "--scripts") {
        for (const auto& list : program.resources())
            std::cout << "RESOURCE\tentry=" << list.source.entry_index << "\tname=" << list.name
                      << "\tvalues=" << list.values.size() << '\n';
        for (const auto& variable : program.global_variables())
            std::cout << "VARIABLE\tscope=global\tentry=" << variable.source.entry_index
                      << "\tid=" << variable.id << "\ttype=" << variable.type << "\tname="
                      << variable.name << "\tarray=" << variable.is_array
                      << "\tvalue=" << program_scalar(variable.initial_value) << '\n';
        for (const auto& script : program.scripts()) print_program_script(script);
    } else if (mode == "--script") {
        if (argc != 5) throw std::runtime_error("--script requires a numeric script ID");
        std::size_t used{};
        const auto id = std::stoll(argv[4], &used);
        if (used != std::string_view(argv[4]).size())
            throw std::runtime_error("Invalid script ID");
        const auto* script = program.find_script(static_cast<std::int32_t>(id));
        if (!script) throw std::runtime_error("Script ID is missing or ambiguous");
        print_program_script(*script);
    } else if (mode == "--references") {
        csf::ProgramReferenceIndex references;
        references.add_program(program);
        for (const auto& reference : references.references())
            std::cout << "REFERENCE\tentry=" << reference.source.entry_index
                      << "\tscript=" << reference.owner_script << "\tkind="
                      << csf::program_reference_kind_name(reference.kind) << "\ttag="
                      << reference.tag << "\tvalue=" << reference.display_value << "\tstatus="
                      << csf::program_reference_status_name(reference.status)
                      << "\ttargets=" << reference.targets.size() << '\n';
    } else if (mode == "--diagnostics") {
        for (const auto& diagnostic : program.diagnostics())
            std::cout << "DIAGNOSTIC\tentry=" << diagnostic.source.entry_index << "\tcode="
                      << diagnostic.code << "\tmessage=" << diagnostic.message << '\n';
    } else if (mode == "--json") {
        if (argc != 5 || std::string_view(argv[4]) != "-")
            throw std::runtime_error("Read-only program JSON requires --json -");
        std::cout << csf::program_json(program) << '\n';
    } else {
        throw std::runtime_error("Unknown program option: " + std::string(mode));
    }
    return document.has_errors() ? 2 : 0;
}

struct ProgramCorpusStats {
    std::size_t files{}, scripts{}, action_scripts{}, actions{}, conditions{}, events{}, globals{},
        locals{}, operands{};
    std::map<std::string, std::size_t> opcodes;
    std::map<std::string, std::size_t> operand_tags;
    std::map<std::string, std::size_t> variable_types;
    std::map<std::string, std::size_t> flag_combinations;
    std::map<std::string, std::size_t> resources;
};

void add_program_stats(ProgramCorpusStats& stats, const csf::ProgramDocument& program) {
    ++stats.files;
    stats.globals += program.global_variables().size();
    const auto variable = [&](const csf::ProgramVariable& value) { ++stats.variable_types[value.type]; };
    for (const auto& value : program.global_variables()) variable(value);
    const auto add_operand = [&](const auto& self, const csf::ProgramOperand& value) -> void {
        ++stats.operands;
        ++stats.operand_tags[value.tag.empty() ? "(anonymous)" : value.tag];
        for (const auto& child : value.children) self(self, child);
    };
    for (const auto& script : program.scripts()) {
        ++stats.scripts;
        if (!script.actions.empty()) ++stats.action_scripts;
        stats.actions += script.actions.size();
        stats.conditions += script.conditions.size();
        stats.events += script.events.size();
        stats.locals += script.local_variables.size();
        for (const auto& value : script.local_variables) variable(value);
        const auto flag = [](const std::optional<bool> value) {
            return value ? (*value ? '1' : '0') : '-';
        };
        std::string flags;
        flags += flag(script.flags.trigger);
        flags += flag(script.flags.enabled);
        flags += flag(script.flags.valid);
        ++stats.flag_combinations[flags];
        for (const auto* list : {&script.conditions, &script.actions})
            for (const auto& instruction : *list) {
                ++stats.opcodes[instruction.opcode];
                for (const auto& operand : instruction.operands) add_operand(add_operand, operand);
            }
    }
    for (const auto& list : program.resources()) stats.resources[list.name] += list.values.size();
}

void print_program_corpus_stats(const char* scope, const ProgramCorpusStats& value) {
    std::cout << "TOTAL\t" << scope << "\tfiles=" << value.files << "\tscripts="
              << value.scripts << "\taction-scripts=" << value.action_scripts << "\tactions="
              << value.actions << "\tconditions=" << value.conditions << "\tevents="
              << value.events << "\tglobals=" << value.globals << "\tlocals=" << value.locals
              << "\toperands=" << value.operands << '\n';
}

int program_corpus_command(const int argc, char** argv) {
    if (argc != 3) {
        usage();
        return 1;
    }
    const std::filesystem::path root = argv[2];
    if (!std::filesystem::is_directory(root))
        throw std::runtime_error("Program corpus path is not a directory");
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        const auto extension = lower_extension(entry.path());
        if (entry.is_regular_file() && (extension == ".gsc" || extension == ".csc"))
            paths.push_back(entry.path());
    }
    std::ranges::sort(paths);
    ProgramCorpusStats path_stats, distinct_stats;
    std::map<std::string, std::vector<std::filesystem::path>> contents;
    std::map<std::string, std::size_t> sections;
    for (const auto& path : paths) {
        const auto document = csf::Document::load(path);
        if (document.state() == csf::ParseState::non_csffbs) continue;
        const auto program = csf::ProgramDocument::project(document);
        add_program_stats(path_stats, program);
        for (const auto& root_node : document.roots())
            for (const auto& section : root_node.children)
                ++sections[node_label(document, section)];
        const auto bytes = document.bytes();
        std::string key(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        auto& copies = contents[key];
        if (copies.empty()) add_program_stats(distinct_stats, program);
        copies.push_back(path);
        std::cout << "FILE\t" << path.string() << "\tstate="
                  << csf::parse_state_name(document.state()) << "\tscripts="
                  << program.scripts().size() << "\tdiagnostics=" << program.diagnostics().size()
                  << '\n';
    }
    print_program_corpus_stats("paths", path_stats);
    print_program_corpus_stats("distinct", distinct_stats);
    for (const auto& [name, count] : sections) std::cout << "SECTION\t" << name << '\t' << count << '\n';
    for (const auto& [name, count] : path_stats.opcodes)
        std::cout << "OPCODE\t" << name << "\tpaths=" << count
                  << "\tdistinct=" << distinct_stats.opcodes[name] << '\n';
    for (const auto& [name, count] : path_stats.operand_tags)
        std::cout << "OPERAND\t" << name << "\tpaths=" << count
                  << "\tdistinct=" << distinct_stats.operand_tags[name] << '\n';
    for (const auto& [name, count] : path_stats.variable_types)
        std::cout << "VARIABLE-TYPE\t" << name << "\tpaths=" << count
                  << "\tdistinct=" << distinct_stats.variable_types[name] << '\n';
    for (const auto& [name, count] : path_stats.flag_combinations)
        std::cout << "FLAGS\t" << name << "\tpaths=" << count
                  << "\tdistinct=" << distinct_stats.flag_combinations[name] << '\n';
    for (const auto& [name, count] : path_stats.resources)
        std::cout << "RESOURCE\t" << name << "\tpaths=" << count
                  << "\tdistinct=" << distinct_stats.resources[name] << '\n';
    for (const auto& [content, copies] : contents) {
        (void)content;
        if (copies.size() < 2) continue;
        std::cout << "DUPLICATE\t" << copies.size();
        for (const auto& path : copies) std::cout << '\t' << path.string();
        std::cout << '\n';
    }
    return 0;
}

int animations_command(const int argc, char** argv) {
    if (argc != 3 && argc != 5) {
        usage();
        return 1;
    }
    std::optional<csf::ResourceIndex> resources;
    if (argc == 5) {
        if (std::string_view(argv[3]) != "--root") {
            usage();
            return 1;
        }
        resources.emplace();
        resources->add_root(argv[4]);
        resources->build();
    }
    const auto document = csf::Document::load(argv[2]);
    const auto catalog =
        csf::AnimationCatalog::project(document, resources ? &*resources : nullptr);
    std::cout << "ANIMATION-CATALOG\trecords=" << catalog.records().size()
              << "\tdiagnostics=" << catalog.diagnostics().size() << '\n';
    for (const auto& record : catalog.records()) {
        std::cout << "ANIMATION\tentry=" << record.source.entry_index
                  << "\tid=" << record.id.value_or(-1) << "\tname=" << record.logical_name
                  << "\tloop=" << (record.loop ? (*record.loop ? "yes" : "no") : "unknown")
                  << "\tblend=" << (record.blend_in ? std::to_string(*record.blend_in) : "unknown")
                  << "\tvariants=" << record.variants.size() << "\tsounds=" << record.sounds.size()
                  << '\n';
        for (const auto& variant : record.variants) {
            std::cout << "  FILE\t" << variant.reference << "\t"
                      << (variant.resolution
                              ? csf::resolution_status_name(variant.resolution->status)
                              : "not-resolved")
                      << "\tentry=" << variant.source.entry_index
                      << "\tsounds=" << variant.sounds.size() << '\n';
            for (const auto& sound : variant.sounds)
                std::cout << "    SOUND\t" << sound.logical_id << "\ttime="
                          << (sound.time ? std::to_string(*sound.time) : "runtime/unknown")
                          << '\n';
        }
        for (const auto& sound : record.sounds)
            std::cout << "  SOUND\t" << sound.logical_id
                      << "\ttime=" << (sound.time ? std::to_string(*sound.time) : "runtime/unknown")
                      << '\n';
    }
    return 0;
}

int script_animations_command(const int argc, char** argv) {
    if (argc != 3) {
        usage();
        return 1;
    }
    csf::ScriptAnimationIndex index;
    index.add_document(csf::Document::load(argv[2]));
    std::cout << "SCRIPT-ANIMATIONS\tuses=" << index.uses().size() << '\n';
    for (const auto& use : index.uses())
        std::cout << "USE\tentry=" << use.source.entry_index << "\tscript=" << use.script_id
                  << "\tname=" << use.script_name << "\topcode=" << use.opcode
                  << "\tanimation=" << use.animation_id << "\ttarget="
                  << (use.targets_this ? "THIS"
                      : use.actor_id   ? std::to_string(*use.actor_id)
                                       : "unknown")
                  << '\n';
    return 0;
}

int cutscene_command(const int argc, char** argv) {
    if (argc != 3 && !(argc == 4 && std::string_view(argv[3]) == "--references")) {
        usage();
        return 1;
    }
    const auto document = csf::Document::load(argv[2]);
    if (argc == 4) {
        const auto program = csf::ProgramDocument::project(document);
        csf::ProgramReferenceIndex references;
        references.add_program(program);
        for (const auto& reference : references.references())
            std::cout << "REFERENCE\tentry=" << reference.source.entry_index
                      << "\tscript=" << reference.owner_script << "\tkind="
                      << csf::program_reference_kind_name(reference.kind) << "\tvalue="
                      << reference.display_value << "\tstatus="
                      << csf::program_reference_status_name(reference.status) << '\n';
        return 0;
    }
    const auto timeline = csf::CutsceneTimeline::project(document);
    std::cout << "CUTSCENES\tscripts=" << timeline.scripts().size()
              << "\tdiagnostics=" << timeline.diagnostics().size() << '\n';
    for (const auto& script : timeline.scripts()) {
        std::cout << "SCRIPT\tentry=" << script.source.entry_index << "\tname=" << script.name
                  << "\tactions=" << script.actions.size() << "\tblocks=" << script.blocks.size()
                  << '\n';
        for (const auto& block : script.blocks) {
            std::cout << "  BLOCK\t" << block.index << "\twait=" << block.runtime_wait
                      << "\tconditional=" << block.conditional << "\tsuccessors=";
            for (auto successor : block.successors)
                std::cout << successor << ',';
            std::cout << '\n';
            for (auto index : block.action_indices) {
                const auto& action = script.actions[index];
                std::cout << "    ACTION\t" << csf::cutscene_action_kind_name(action.kind)
                          << "\tentry=" << action.source.entry_index << "\topcode=" << action.opcode
                          << "\tref=" << action.reference << "\ttime="
                          << (action.explicit_time ? std::to_string(*action.explicit_time)
                                                   : "ordered/unknown")
                          << "\tduration="
                          << (action.duration ? std::to_string(*action.duration)
                                              : "runtime/unknown")
                          << "\tvalue="
                          << (action.numeric_value ? std::to_string(*action.numeric_value) : "-")
                          << '\n';
            }
        }
    }
    return 0;
}

int cmo_command(const int argc, char** argv) {
    if (argc != 3) {
        usage();
        return 1;
    }
    const auto document = csf::CmoDocument::load(argv[2]);
    std::cout << "CMO\tbytes=" << document.source().size()
              << "\ttokens=" << document.tokens().size() << "\troots=" << document.roots().size()
              << "\tshapes=" << document.shapes().size()
              << "\tdiagnostics=" << document.diagnostics().size() << '\n';
    for (const auto& shape : document.shapes()) {
        std::cout << "SHAPE\toffset=" << shape.range.offset
                  << "\ttype=" << csf::cmo_shape_kind_name(shape.kind)
                  << "\tspelling=" << shape.spelling << "\tbone=" << shape.bone_index.value_or(-1)
                  << "\tlabel=" << shape.label.value_or("") << "\texternal=" << shape.external
                  << "\thot-points=" << shape.hot_points.size();
        if (shape.center)
            std::cout << "\tcenter=" << shape.center->x << ',' << shape.center->y << ','
                      << shape.center->z;
        if (shape.dimensions)
            std::cout << "\tdimensions=" << shape.dimensions->x << ',' << shape.dimensions->y << ','
                      << shape.dimensions->z;
        if (shape.radius) std::cout << "\tradius=" << *shape.radius;
        std::cout << '\n';
    }
    for (const auto& diagnostic : document.diagnostics())
        std::cerr << (diagnostic.severity == csf::CmoDiagnostic::Severity::error ? "error"
                                                                                 : "warning")
                  << " at " << diagnostic.range.offset << ": " << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    return std::ranges::any_of(
               document.diagnostics(),
               [](const auto& d) { return d.severity == csf::CmoDiagnostic::Severity::error; })
               ? 2
               : 0;
}

void print_mission_summary(const csf::MissionGraph& graph) {
    std::map<std::string, std::uint64_t> kinds;
    std::map<std::string, std::uint64_t> statuses;
    for (const auto& node : graph.nodes())
        ++kinds[csf::resource_kind_name(node.kind)];
    for (const auto& edge : graph.edges())
        ++statuses[csf::resolution_status_name(edge.status)];
    std::cout << "Scene: " << graph.scene_path().string() << '\n'
              << "Package root: " << graph.package_root().string() << '\n'
              << "Indexed files: " << graph.index().resources().size() << '\n'
              << "Nodes: " << graph.nodes().size() << '\n'
              << "Edges: " << graph.edges().size() << '\n'
              << "Diagnostics: " << graph.diagnostics().size() << '\n';
    for (const auto& [kind, count] : kinds)
        std::cout << "Kind " << kind << ": " << count << '\n';
    for (const auto& [status, count] : statuses)
        std::cout << "Resolution " << status << ": " << count << '\n';
}

void print_mission_edges(const csf::MissionGraph& graph, const bool missing_only) {
    for (const auto& edge : graph.edges()) {
        if (missing_only && edge.status != csf::ResolutionStatus::missing &&
            edge.status != csf::ResolutionStatus::ambiguous &&
            edge.status != csf::ResolutionStatus::outside_root)
            continue;
        std::cout << edge.source << '\t';
        if (edge.target)
            std::cout << *edge.target;
        else
            std::cout << '-';
        std::cout << '\t' << csf::dependency_kind_name(edge.kind) << '\t'
                  << csf::resolution_status_name(edge.status) << '\t' << edge.original_reference
                  << '\t' << edge.evidence.file.string() << ":0x" << std::hex
                  << edge.evidence.offset << std::dec << '\n';
        for (const auto& candidate : edge.candidates)
            std::cout << "  candidate\t" << candidate.string() << '\n';
    }
}

void print_scene_objects(const csf::MissionScene& scene) {
    std::cout << "PLAYER\tactive=" << scene.player().active_player.value_or(-1)
              << "\tcommando=" << scene.player().commando_start.value_or(-1)
              << "\tsniper=" << scene.player().sniper_start.value_or(-1)
              << "\tspy=" << scene.player().spy_start.value_or(-1) << '\n';
    for (const auto& actor : scene.actors()) {
        std::cout << "ACTOR\tentry=" << actor.source.entry_index << "\tid=" << actor.id.value_or(-1)
                  << "\tclass=" << actor.class_id.value_or(-1)
                  << "\tname=" << actor.name.value_or("");
        if (actor.position)
            std::cout << "\tpos=" << actor.position->x << ',' << actor.position->y << ','
                      << actor.position->z;
        if (actor.group) std::cout << "\tgroup=" << *actor.group;
        if (actor.cell) std::cout << "\tpoint=" << *actor.cell;
        if (actor.script) std::cout << "\tscript=" << *actor.script;
        if (!actor.script_ids.empty()) {
            std::cout << "\tscript_ids=";
            for (const auto id : actor.script_ids)
                std::cout << id << ',';
        }
        std::cout << '\n';
    }
}

void print_actor_associations(const csf::MissionScene& scene, const csf::MissionGraph& graph) {
    const auto object_node = std::ranges::find_if(graph.nodes(), [](const auto& node) {
        return lower_filename(node.resolved_path) == "objetos.bdd" &&
               node.state == csf::LoadState::available;
    });
    if (object_node == graph.nodes().end()) {
        std::cout << "DIAGNOSTIC\tObjetos.bdd is unresolved\n";
        return;
    }
    const auto object_document = csf::Document::load(object_node->resolved_path);
    const auto database = csf::ObjectDatabase::project(object_document);
    const auto associations = csf::associate_actors(scene, database, graph.index());
    std::cout << "OBJECT-DATABASE\tdefinitions=" << database.definitions().size()
              << "\tdiagnostics=" << database.diagnostics().size() << '\n';
    for (const auto& association : associations) {
        std::cout << "ACTOR\tentry=" << association.actor.entry_index
                  << "\tclass=" << association.class_id.value_or(-1)
                  << "\tdefinitions=" << association.definitions.size() << '\n';
        const auto print = [&](const char* kind,
                               const std::vector<csf::AssociationEvidence>& evidence) {
            for (const auto& item : evidence) {
                std::cout << "  " << kind << '\t'
                          << csf::resolution_status_name(item.resolution.status) << '\t'
                          << item.resolution.original_reference << '\t';
                if (item.resolved_path)
                    std::cout << item.resolved_path->string();
                else
                    std::cout << '-';
                std::cout << "\tevidence=" << item.source.file.filename().string()
                          << ":entry=" << item.source.entry_index << ':' << item.field << '\n';
            }
        };
        print("VISUAL", association.visual_models);
        print("LOD", association.lod_models);
        print("CMO", association.collision_models);
        print("PHYSICS", association.physics_models);
        print("RAGDOLL", association.ragdolls);
        print("ANIMATION", association.animations);
        for (const auto& diagnostic : association.diagnostics)
            std::cout << "  DIAGNOSTIC\t" << diagnostic << '\n';
    }
}

void print_navigation(const csf::MissionScene& scene) {
    const auto& stats = scene.navigation_stats();
    std::cout << "NAVIGATION\tgroups=" << stats.groups << "\tpoints=" << stats.points
              << "\tconnections=" << stats.connections
              << "\tcomponents=" << stats.connected_components
              << "\torphans=" << stats.orphan_points << "\tinvalid=" << stats.invalid_connections
              << "\tduplicate-groups=" << stats.duplicate_group_ids
              << "\tduplicate-points=" << stats.duplicate_point_ids << '\n';
    for (const auto& group : scene.navigation()) {
        std::cout << "GROUP\tentry=" << group.source.entry_index << "\tid=" << group.id.value_or(-1)
                  << "\tname=" << group.name.value_or("") << "\tpoints=" << group.points.size()
                  << "\tconnections=" << group.connections.size() << '\n';
        for (const auto& point : group.points) {
            std::cout << "POINT\tentry=" << point.source.entry_index
                      << "\tgroup=" << point.group_id.value_or(-1)
                      << "\tid=" << point.id.value_or(-1) << "\tname=" << point.name.value_or("");
            if (point.position)
                std::cout << "\tpos=" << point.position->x << ',' << point.position->y << ','
                          << point.position->z;
            std::cout << '\n';
        }
    }
}

void print_spatial(const csf::MissionScene& scene) {
    for (const auto& value : scene.dummies()) {
        std::cout << "DUMMY\tentry=" << value.source.entry_index << "\tid=" << value.id.value_or(-1)
                  << "\tname=" << value.name.value_or("");
        if (value.position)
            std::cout << "\tpos=" << value.position->x << ',' << value.position->y << ','
                      << value.position->z;
        std::cout << '\n';
    }
    for (const auto& value : scene.areas())
        std::cout << "AREA\tentry=" << value.source.entry_index << "\tid=" << value.id.value_or(-1)
                  << "\tname=" << value.name.value_or("") << "\tpoints=" << value.points.size()
                  << "\theight=" << value.height.value_or(0) << '\n';
    for (const auto& value : scene.lights()) {
        std::cout << "LIGHT\tentry=" << value.source.entry_index << "\tid=" << value.id.value_or(-1)
                  << "\tname=" << value.name.value_or("")
                  << "\tradius=" << value.radius.value_or(0);
        if (value.position)
            std::cout << "\tpos=" << value.position->x << ',' << value.position->y << ','
                      << value.position->z;
        std::cout << '\n';
    }
    for (const auto& value : scene.effects())
        std::cout << "EFFECT\tentry=" << value.source.entry_index
                  << "\tid=" << value.id.value_or(-1) << "\tname=" << value.name.value_or("")
                  << "\tclass=" << value.class_id.value_or(-1)
                  << "\tdummy=" << value.dummy_id.value_or(-1)
                  << "\tpriority=" << value.priority.value_or(-1)
                  << "\tshare-group=" << value.share_group.value_or(-1) << '\n';
    for (const auto& value : scene.folders())
        std::cout << "FOLDER\tentry=" << value.source.entry_index << "\tpath=" << value.path
                  << "\telements=" << value.element_ids.size() << '\n';
}

void print_remaining_scene_records(const csf::MissionScene& scene, const std::string_view mode,
                                   const csf::AnimationCatalog* animations = nullptr) {
    if (mode == "--metadata") {
        std::cout << "METADATA\tsector-map=" << scene.metadata().sector_map.value_or("")
                  << "\tmaximum-score=" << scene.metadata().maximum_score.value_or(0)
                  << "\tminimum-score=" << scene.metadata().minimum_score.value_or(0) << '\n';
        for (const auto& field : scene.environment())
            if (const auto* value = std::get_if<csf::Vec3>(&field.value))
                std::cout << "VECTOR\tentry=" << field.source.entry_index << "\tname="
                          << field.name << "\tvalue=" << value->x << ',' << value->y << ','
                          << value->z << '\n';
    } else if (mode == "--scene-objects") {
        for (const auto& value : scene.scene_objects())
            std::cout << "SCENE-OBJECT\tentry=" << value.source.entry_index
                      << "\tid=" << value.id.value_or("")
                      << "\tanimation=" << value.animation_id.value_or(-1)
                      << "\toffset-type=" << value.offset_type.value_or(-1)
                      << "\toffset=" << value.offset.value_or(0) << "\tresolution="
                      << (!value.animation_id || !animations
                              ? "not-loaded"
                              : animations->find_id(*value.animation_id) ? "resolved" : "missing")
                      << '\n';
    } else if (mode == "--bridges") {
        for (const auto& value : scene.bridges()) {
            std::cout << "BRIDGE\tentry=" << value.source.entry_index
                      << "\tvisual=" << value.visual_rws.value_or("")
                      << "\tphysics=" << value.physics_rws.value_or("")
                      << "\tpoints=" << value.control_points.size() << '\n';
            for (const auto& point : value.control_points) {
                std::cout << "  CONTROL\tentry=" << point.source.entry_index
                          << "\ttype=" << point.type.value_or(-1)
                          << "\theight=" << point.height.value_or(0)
                          << "\ttarget=" << point.target_scene.value_or("");
                if (point.p1) std::cout << "\tp1=" << point.p1->x << ',' << point.p1->y << ',' << point.p1->z;
                if (point.p2) std::cout << "\tp2=" << point.p2->x << ',' << point.p2->y << ',' << point.p2->z;
                std::cout << '\n';
            }
        }
    } else if (mode == "--water") {
        for (const auto& value : scene.waters()) {
            std::cout << "WATER\tentry=" << value.source.entry_index
                      << "\tfields=" << value.fields.size() << '\n';
            for (const auto& field : value.fields) {
                std::cout << "  FIELD\tentry=" << field.source.entry_index << "\tname="
                          << field.name << "\tvalue=";
                std::visit([](const auto& item) { std::cout << item; }, field.value);
                std::cout << '\n';
            }
        }
    }
}

void write_new_file(const std::filesystem::path& path, const std::string_view contents) {
    if (std::filesystem::exists(path))
        throw std::runtime_error("Output already exists: " + path.string());
    auto temporary = path;
    temporary += ".csf-info.tmp";
#ifdef _WIN32
    const auto handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot exclusively create temporary output: " +
                                 temporary.string());
    std::size_t offset{};
    while (offset < contents.size()) {
        const auto amount =
            static_cast<DWORD>(std::min<std::size_t>(contents.size() - offset, 1U << 30U));
        DWORD written{};
        if (!WriteFile(handle, contents.data() + offset, amount, &written, nullptr) ||
            written != amount) {
            CloseHandle(handle);
            std::filesystem::remove(temporary);
            throw std::runtime_error("Cannot write output: " + path.string());
        }
        offset += written;
    }
    if (!CloseHandle(handle)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Cannot close output: " + path.string());
    }
#else
    if (std::filesystem::exists(temporary))
        throw std::runtime_error("Temporary output already exists: " + temporary.string());
    std::ofstream output(temporary, std::ios::binary | std::ios::out);
    if (!output) throw std::runtime_error("Cannot create output: " + path.string());
    output << contents;
    if (!output) {
        output.close();
        std::filesystem::remove(temporary);
        throw std::runtime_error("Cannot write output: " + path.string());
    }
#endif
    std::error_code copy_error;
    if (!std::filesystem::copy_file(temporary, path, std::filesystem::copy_options::none,
                                    copy_error)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("Cannot create output: " + copy_error.message());
    }
    std::filesystem::remove(temporary);
}

int mission_command(const int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 1;
    }
    csf::MissionOptions options{std::filesystem::path(argv[2])};
    std::string_view mode = "--summary";
    bool mode_was_set = false;
    std::optional<std::filesystem::path> graph_output;
    std::optional<std::filesystem::path> scene_output;
    std::optional<std::string> symbol;
    std::optional<std::string> script_use;
    const auto set_mode = [&](const std::string_view requested) {
        if (mode_was_set) throw std::runtime_error("Mission output modes are mutually exclusive");
        mode = requested;
        mode_was_set = true;
    };
    for (int argument = 3; argument < argc; ++argument) {
        const std::string_view value = argv[argument];
        if (value == "--root") {
            if (++argument >= argc) throw std::runtime_error("--root requires a directory");
            options.resource_roots.emplace_back(argv[argument]);
        } else if (value == "--package-root") {
            if (++argument >= argc) throw std::runtime_error("--package-root requires a directory");
            options.package_root = std::filesystem::path(argv[argument]);
        } else if (value == "--duplicates") {
            options.detect_duplicate_scenes = true;
        } else if (value == "--graph") {
            if (++argument >= argc) throw std::runtime_error("--graph requires a new output path");
            graph_output = std::filesystem::path(argv[argument]);
            set_mode("--graph");
        } else if (value == "--scene-json") {
            if (++argument >= argc)
                throw std::runtime_error("--scene-json requires a new output path");
            scene_output = std::filesystem::path(argv[argument]);
            set_mode("--scene-json");
        } else if (value == "--symbols") {
            if (++argument >= argc) throw std::runtime_error("--symbols requires an exact symbol");
            symbol = argv[argument];
            set_mode("--symbols");
        } else if (value == "--script-uses") {
            if (++argument >= argc)
                throw std::runtime_error("--script-uses requires category:id");
            script_use = argv[argument];
            set_mode("--script-uses");
        } else if (value == "--summary" || value == "--dependencies" || value == "--missing" ||
                   value == "--objects" || value == "--navigation" || value == "--spatial" ||
                   value == "--associations" || value == "--scene-objects" ||
                   value == "--bridges" || value == "--water" || value == "--metadata") {
            set_mode(value);
        } else
            throw std::runtime_error("Unknown mission option: " + std::string(value));
    }
    const auto graph = csf::MissionGraph::load(options);
    std::optional<csf::Document> scene_document;
    std::optional<csf::MissionScene> scene;
    if (mode == "--objects" || mode == "--navigation" || mode == "--spatial" ||
        mode == "--associations" || mode == "--scene-json" || mode == "--symbols" ||
        mode == "--scene-objects" || mode == "--bridges" || mode == "--water" ||
        mode == "--metadata" || mode == "--script-uses") {
        scene_document = csf::Document::load(graph.scene_path());
        scene = csf::MissionScene::project(*scene_document);
    }
    if (mode == "--dependencies")
        print_mission_edges(graph, false);
    else if (mode == "--missing")
        print_mission_edges(graph, true);
    else if (mode == "--objects")
        print_scene_objects(*scene);
    else if (mode == "--navigation")
        print_navigation(*scene);
    else if (mode == "--spatial")
        print_spatial(*scene);
    else if (mode == "--associations")
        print_actor_associations(*scene, graph);
    else if (mode == "--scene-objects" || mode == "--bridges" || mode == "--water" ||
             mode == "--metadata") {
        std::optional<csf::AnimationCatalog> animations;
        if (mode == "--scene-objects") {
            const auto found = std::ranges::find_if(graph.nodes(), [](const auto& node) {
                return lower_filename(node.resolved_path) == "anims.bdd" &&
                       node.state == csf::LoadState::available;
            });
            if (found != graph.nodes().end())
                animations = csf::AnimationCatalog::project(csf::Document::load(found->resolved_path),
                                                            &graph.index());
        }
        print_remaining_scene_records(*scene, mode, animations ? &*animations : nullptr);
    } else if (mode == "--script-uses") {
        const auto separator = script_use->find(':');
        if (separator == std::string::npos)
            throw std::runtime_error("--script-uses requires category:id");
        const auto category = script_use->substr(0, separator);
        const auto id = static_cast<std::int32_t>(std::stol(script_use->substr(separator + 1)));
        const csf::CsfSourceId* target{};
        if (category == "actor") {
            const auto found = std::ranges::find_if(scene->actors(),
                                                    [&](const auto& value) { return value.id == id; });
            if (found != scene->actors().end()) target = &found->source;
        } else if (category == "dummy") {
            const auto found = std::ranges::find_if(scene->dummies(),
                                                    [&](const auto& value) { return value.id == id; });
            if (found != scene->dummies().end()) target = &found->source;
        } else if (category == "area") {
            const auto found = std::ranges::find_if(scene->areas(),
                                                    [&](const auto& value) { return value.id == id; });
            if (found != scene->areas().end()) target = &found->source;
        } else {
            throw std::runtime_error("Unsupported script-use category: " + category);
        }
        if (!target) throw std::runtime_error("Mission object is missing or ambiguous");
        csf::ProgramReferenceIndex references;
        for (const auto& node : graph.nodes()) {
            if (node.state != csf::LoadState::available ||
                (node.kind != csf::ResourceKind::mission_script &&
                 node.kind != csf::ResourceKind::cutscene_script))
                continue;
            const auto document = csf::Document::load(node.resolved_path);
            references.add_program(csf::ProgramDocument::project(document), &*scene);
        }
        const auto uses = references.uses(*target);
        for (const auto* use : uses)
            std::cout << "SCRIPT-USE\tscript=" << use->owner_script << "\tkind="
                      << csf::program_reference_kind_name(use->kind) << "\tvalue="
                      << use->display_value << "\tfile=" << use->source.file.string()
                      << "\tentry=" << use->source.entry_index << '\n';
        std::cout << "USES\t" << uses.size() << '\n';
    }
    else if (mode == "--scene-json")
        write_new_file(*scene_output, csf::mission_scene_json(*scene));
    else if (mode == "--symbols") {
        csf::MissionSymbolIndex index;
        index.add_scene(*scene);
        for (const auto& node : graph.nodes()) {
            if (node.resolved_path.empty() || node.resolved_path == graph.scene_path()) continue;
            if (node.kind != csf::ResourceKind::mission_script &&
                node.kind != csf::ResourceKind::cutscene_script &&
                node.kind != csf::ResourceKind::database)
                continue;
            const auto document = csf::Document::load(node.resolved_path);
            if (document.state() != csf::ParseState::non_csffbs) index.add_document(document);
        }
        for (const auto* site : index.exact(*symbol))
            std::cout << csf::symbol_role_name(site->role) << '\t'
                      << csf::symbol_category_name(site->category) << '\t'
                      << site->source.file.string() << ":entry=" << site->source.entry_index << "\t"
                      << site->field << '\n';
    } else if (mode == "--graph") {
        write_new_file(*graph_output, csf::mission_graph_json(graph));
    } else
        print_mission_summary(graph);
    return std::ranges::any_of(graph.diagnostics(),
                               [](const auto& diagnostic) {
                                   return diagnostic.severity ==
                                          csf::MissionDiagnostic::Severity::error;
                               })
               ? 2
               : 0;
}

int uses_command(const int argc, char** argv) {
    if (argc != 5 || std::string_view(argv[3]) != "--root") {
        usage();
        return 1;
    }
    const auto root = std::filesystem::path(argv[4]);
    const auto asset_argument = std::filesystem::path(argv[2]);
    const auto asset = std::filesystem::absolute(
                           asset_argument.is_absolute() ? asset_argument : root / asset_argument)
                           .lexically_normal();
    std::vector<std::filesystem::path> scenes;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied)) {
        if (entry.is_regular_file() && lower_extension(entry.path()) == ".scn")
            scenes.push_back(entry.path());
    }
    std::ranges::sort(scenes);
    const auto package_for = [](const std::filesystem::path& scene) {
        auto current = scene.parent_path();
        std::error_code error;
        while (!current.empty()) {
            if (std::filesystem::is_directory(current / "Maps", error) ||
                std::filesystem::is_directory(current / "BDD", error))
                return current;
            error.clear();
            const auto parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        return scene.parent_path();
    };
    csf::ResourceIndex shared_index;
    for (const auto& scene : scenes)
        shared_index.add_root(package_for(scene));
    shared_index.build();
    std::uint64_t count{};
    for (const auto& scene : scenes) {
        try {
            csf::MissionOptions options{scene};
            options.package_root = package_for(scene);
            options.prepared_index = &shared_index;
            const auto graph = csf::MissionGraph::load(options);
            for (const auto* edge : graph.uses(asset)) {
                ++count;
                std::cout << scene.string() << '\t' << csf::dependency_kind_name(edge->kind) << '\t'
                          << edge->original_reference << '\t' << edge->evidence.file.string()
                          << ":0x" << std::hex << edge->evidence.offset << std::dec << '\n';
            }
        } catch (const std::exception& exception) {
            std::cerr << "warning: cannot inspect " << scene.string() << ": " << exception.what()
                      << '\n';
        }
    }
    std::cout << "Uses: " << count << '\n';
    return 0;
}

int compare_command(const int argc, char** argv) {
    if (argc != 4) {
        usage();
        return 1;
    }
    const auto left = csf::MissionGraph::load({std::filesystem::path(argv[2])});
    const auto right = csf::MissionGraph::load({std::filesystem::path(argv[3])});
    if (left.nodes().front().content_hash == right.nodes().front().content_hash &&
        left.nodes().front().size == right.nodes().front().size)
        std::cout << "=\tSCN content is identical (FNV-1a signature)\n";
    else
        std::cout << "!\tSCN content differs\n";
    std::set<std::string> left_edges, right_edges;
    for (const auto& edge : left.edges())
        left_edges.insert(std::string(csf::dependency_kind_name(edge.kind)) + "\t" +
                          edge.normalized_key);
    for (const auto& edge : right.edges())
        right_edges.insert(std::string(csf::dependency_kind_name(edge.kind)) + "\t" +
                           edge.normalized_key);
    for (const auto& value : left_edges)
        if (!right_edges.contains(value)) std::cout << "-\t" << value << '\n';
    for (const auto& value : right_edges)
        if (!left_edges.contains(value)) std::cout << "+\t" << value << '\n';
    return 0;
}

bool file_has_magic(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::array<std::byte, 6> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return input.gcount() == static_cast<std::streamsize>(bytes.size()) &&
           csf::Document::sniff(bytes);
}

std::string lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension.empty() ? "<none>" : extension;
}

// Resource names are matched without case, as the mission loader does.
std::string lower_filename(const std::filesystem::path& path) {
    auto name = path.filename().string();
    std::ranges::transform(name, name.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return name;
}

int scan_corpus(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root))
        throw std::runtime_error("Corpus path is not a directory: " + root.string());
    std::vector<std::filesystem::path> files;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) throw std::runtime_error("Cannot scan corpus: " + error.message());
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            std::cerr << "warning: corpus traversal: " << error.message() << '\n';
            error.clear();
            continue;
        }
        if (iterator->is_regular_file(error) && !error) files.push_back(iterator->path());
        error.clear();
    }
    std::ranges::sort(files);

    std::uint64_t csffbs_files{};
    std::uint64_t non_csffbs_files{};
    std::uint64_t error_files{};
    std::map<std::string, std::uint64_t> extensions;
    std::map<std::string, std::uint64_t> headers;
    std::map<std::uint16_t, std::uint64_t> entry_types;
    std::map<std::string, std::uint64_t> identifiers;
    std::map<std::string, std::uint64_t> diagnostics;

    std::cout
        << "FILE\tstate\textension\tbytes\tentries\tidentifiers\tstrings\tdiagnostics\tpath\n";
    for (const auto& path : files) {
        if (!file_has_magic(path)) {
            ++non_csffbs_files;
            continue;
        }
        ++csffbs_files;
        const auto document = csf::Document::load(path);
        const auto extension = lower_extension(path);
        ++extensions[extension];
        std::ostringstream header_key;
        header_key << "reserved=" << std::hex << std::setw(2) << std::setfill('0')
                   << std::to_integer<unsigned>(document.header().reserved[0]) << std::setw(2)
                   << std::to_integer<unsigned>(document.header().reserved[1]) << std::dec
                   << ",version=" << document.header().version;
        ++headers[header_key.str()];
        for (const auto& entry : document.entries())
            ++entry_types[entry.raw_type];
        for (const auto& identifier : document.identifiers())
            ++identifiers[escaped(identifier.display_utf8())];
        for (const auto& diagnostic : document.diagnostics()) {
            const auto prefix =
                diagnostic.severity == csf::Diagnostic::Severity::error ? "error: " : "warning: ";
            ++diagnostics[prefix + diagnostic.message];
        }
        if (document.has_errors()) ++error_files;
        std::cout << "FILE\t" << csf::parse_state_name(document.state()) << '\t' << extension
                  << '\t' << document.bytes().size() << '\t' << document.entries().size() << '\t'
                  << document.identifiers().size() << '\t' << document.strings().size() << '\t'
                  << document.diagnostics().size() << '\t' << path.string() << '\n';
    }
    std::cout << "TOTAL\tall-files\t" << files.size() << '\n'
              << "TOTAL\tCSFFBS\t" << csffbs_files << '\n'
              << "TOTAL\tnon-CSFFBS\t" << non_csffbs_files << '\n'
              << "TOTAL\terror-files\t" << error_files << '\n';
    for (const auto& [value, count] : extensions)
        std::cout << "EXTENSION\t" << value << '\t' << count << '\n';
    for (const auto& [value, count] : headers)
        std::cout << "HEADER\t" << value << '\t' << count << '\n';
    for (const auto& [value, count] : entry_types) {
        const csf::Entry entry{0, 0, -1, value, 0, {}};
        const auto kind = entry.kind();
        std::cout << "ENTRY-TYPE\t" << value << '\t'
                  << (kind ? csf::value_kind_name(*kind) : "unknown") << '\t' << count << '\n';
    }
    for (const auto& [value, count] : identifiers)
        std::cout << "IDENTIFIER\t" << value << '\t' << count << '\n';
    for (const auto& [value, count] : diagnostics)
        std::cout << "DIAGNOSTIC\t" << value << '\t' << count << '\n';
    return error_files == 0 ? 0 : 2;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    try {
        if (std::string_view(argv[1]) == "mission") return mission_command(argc, argv);
        if (std::string_view(argv[1]) == "cmo") return cmo_command(argc, argv);
        if (std::string_view(argv[1]) == "animations") return animations_command(argc, argv);
        if (std::string_view(argv[1]) == "program") return program_command(argc, argv);
        if (std::string_view(argv[1]) == "program-corpus")
            return program_corpus_command(argc, argv);
        if (std::string_view(argv[1]) == "script-animations")
            return script_animations_command(argc, argv);
        if (std::string_view(argv[1]) == "cutscene") return cutscene_command(argc, argv);
        if (std::string_view(argv[1]) == "uses") return uses_command(argc, argv);
        if (std::string_view(argv[1]) == "compare") return compare_command(argc, argv);
        if (std::string_view(argv[1]) == "corpus") {
            if (argc != 3) {
                usage();
                return 1;
            }
            return scan_corpus(std::filesystem::path(argv[2]));
        }
        const auto document = csf::Document::load(std::filesystem::path(argv[1]));
        const std::string_view mode = argc >= 3 ? argv[2] : "--summary";
        if (mode == "--summary" || mode == "--validate") {
            print_summary(document);
        } else if (mode == "--tree") {
            print_nodes(document, document.roots());
        } else if (mode == "--strings") {
            for (const auto& value : document.identifiers()) {
                std::cout << "identifier[" << value.table_index << "] @0x" << std::hex
                          << value.source.offset << std::dec << " \""
                          << escaped(value.display_utf8()) << "\"\n";
            }
            for (const auto& value : document.strings()) {
                std::cout << "string[" << value.table_index << "] @0x" << std::hex
                          << value.source.offset << std::dec << " \""
                          << escaped(value.display_utf8()) << "\"\n";
            }
        } else if (mode == "--find") {
            if (argc < 4) throw std::runtime_error("--find requires a search string");
            const std::string_view needle = argv[3];
            for (const auto& value : document.identifiers()) {
                const auto text = value.display_utf8();
                if (text.find(needle) != std::string::npos)
                    std::cout << "identifier[" << value.table_index << "] " << escaped(text)
                              << '\n';
            }
            for (const auto& value : document.strings()) {
                const auto text = value.display_utf8();
                if (text.find(needle) != std::string::npos)
                    std::cout << "string[" << value.table_index << "] " << escaped(text) << '\n';
            }
        } else if (mode == "--export-text" || mode == "--export-json") {
            if (argc < 4)
                throw std::runtime_error(std::string(mode) + " requires an output path or -");
            const auto contents =
                mode == "--export-text" ? csf::export_text(document) : csf::export_json(document);
            if (std::string_view(argv[3]) == "-") {
                std::cout << contents;
            } else {
                csf::write_new_export(document, std::filesystem::path(argv[3]), contents);
            }
        } else {
            usage();
            return 1;
        }
        print_diagnostics(document);
        return document.has_errors() ? 2 : 0;
    } catch (const std::exception& exception) {
        std::cerr << "csf-info: " << exception.what() << '\n';
        return 1;
    }
}
