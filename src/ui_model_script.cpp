#include "rwsman/ui_script.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <utility>

namespace rwsman {
namespace {

struct OpSpec {
    std::string_view name;
    std::size_t min_args, max_args;
};

constexpr std::array ops{
    OpSpec{"open", 1, 1},           OpSpec{"open-project", 1, 1},  OpSpec{"wait", 1, 2},
    OpSpec{"command", 1, 1},        OpSpec{"undo", 0, 0},          OpSpec{"redo", 0, 0},
    OpSpec{"goto", 1, 1},           OpSpec{"palette", 1, 1},       OpSpec{"goto-palette", 1, 1},
    OpSpec{"click", 1, 2},          OpSpec{"double-click", 1, 2},  OpSpec{"hover", 1, 1},
    OpSpec{"click-world", 3, 4},    OpSpec{"drag-world", 6, 7},    OpSpec{"key", 1, 1},
    OpSpec{"type", 1, 1},           OpSpec{"expect", 2, 2},        OpSpec{"expect-not", 2, 2},
    OpSpec{"expect-contains", 2, 2}, OpSpec{"screenshot", 1, 1},   OpSpec{"compare", 1, 2},
    OpSpec{"dump-state", 1, 1},     OpSpec{"lint", 1, 1},          OpSpec{"resize", 1, 1},
    OpSpec{"list-items", 0, 1},     OpSpec{"remember", 1, 1},      OpSpec{"expect-same", 1, 1},
    OpSpec{"expect-changed", 1, 1},  OpSpec{"require", 1, 1},     OpSpec{"drag-to-world", 4, 4},  OpSpec{"copy", 2, 2},          OpSpec{"monkey", 2, 2},        OpSpec{"undo-all", 0, 0},       OpSpec{"add-component", 1, 1},  OpSpec{"setting", 2, 2},  OpSpec{"remove", 1, 1},
};

// Splits one line into steps of tokens. Quotes group, '#' outside quotes ends
// the line, ';' outside quotes ends a step.
std::vector<std::vector<std::string>> tokenize(const std::string_view line, std::string& error) {
    std::vector<std::vector<std::string>> steps(1);
    std::string token;
    bool in_token = false, quoted = false;
    const auto finish_token = [&] {
        if (in_token) steps.back().push_back(std::move(token));
        token.clear();
        in_token = false;
    };
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
                token += line[++i];
            } else if (c == '"') {
                quoted = false;
            } else {
                token += c;
            }
            continue;
        }
        if (c == '"') {
            quoted = in_token = true;
        } else if (c == '#' && !in_token) {
            break;
        } else if (c == ';') {
            finish_token();
            steps.emplace_back();
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            finish_token();
        } else {
            token += c;
            in_token = true;
        }
    }
    if (quoted) error = "unterminated quote";
    finish_token();
    std::erase_if(steps, [](const auto& step) { return step.empty(); });
    return steps;
}

std::string_view visible_part(const std::string_view label) {
    const auto hashes = label.find("##");
    return hashes == std::string_view::npos ? label : label.substr(0, hashes);
}

std::string_view id_part(const std::string_view label) {
    const auto hashes = label.rfind("##");
    if (hashes == std::string_view::npos) return {};
    auto id = label.substr(hashes + 2);
    if (!id.empty() && id.front() == '#') id.remove_prefix(1);  // "###id"
    return id;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// Drops leading icon glyphs (Lucide's private-use code points, U+E000-U+F8FF,
// encoded EE 80 80 - EF A3 BF) and the spaces after them: "<icon> Save" is "Save".
std::string_view without_icons(std::string_view text) {
    while (text.size() >= 3) {
        const auto lead = static_cast<unsigned char>(text[0]), next = static_cast<unsigned char>(text[1]);
        if (!(lead == 0xEE || (lead == 0xEF && next <= 0xA3))) break;
        text = trim(text.substr(3));
    }
    return text;
}

bool label_matches(const std::string_view wanted, const std::string_view label) {
    if (wanted == label) return true;
    const auto visible = trim(visible_part(label));
    if (!visible.empty() && (wanted == visible || wanted == without_icons(visible))) return true;
    const auto id = id_part(label);
    return !id.empty() && wanted == id;
}

void append_json_string(std::string& out, const std::string_view text) {
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                static constexpr char digits[] = "0123456789abcdef";
                out += "\\u00";
                out += digits[(static_cast<unsigned char>(c) >> 4U) & 0xFU];
                out += digits[static_cast<unsigned char>(c) & 0xFU];
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

} // namespace

UiScript parse_ui_script(const std::string_view text) {
    UiScript script;
    std::size_t line_number = 0, start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        ++line_number;
        std::string error;
        for (auto& tokens : tokenize(line, error)) {
            UiScriptStep step{line_number, std::move(tokens.front()), {}};
            step.args.assign(std::make_move_iterator(tokens.begin() + 1), std::make_move_iterator(tokens.end()));
            const auto spec = std::ranges::find(ops, step.op, &OpSpec::name);
            if (spec == ops.end()) {
                script.errors.push_back("line " + std::to_string(line_number) + ": unknown step '" + step.op + "'");
                continue;
            }
            if (step.args.size() < spec->min_args || step.args.size() > spec->max_args) {
                script.errors.push_back("line " + std::to_string(line_number) + ": '" + step.op + "' takes " +
                                        std::to_string(spec->min_args) +
                                        (spec->max_args != spec->min_args ? "-" + std::to_string(spec->max_args) : "") +
                                        " argument(s), got " + std::to_string(step.args.size()));
                continue;
            }
            script.steps.push_back(std::move(step));
        }
        if (!error.empty()) script.errors.push_back("line " + std::to_string(line_number) + ": " + error);
        if (end == text.size()) break;
        start = end + 1;
    }
    return script;
}

std::vector<UiScriptStep> ui_script_from_commands(const std::vector<std::string>& commands) {
    std::vector<UiScriptStep> steps;
    std::size_t index = 0;
    for (const auto& command : commands) {
        ++index;
        if (command.starts_with("goto:")) {
            steps.push_back({index, "goto", {command.substr(5)}});
        } else if (command.starts_with("goto-palette:")) {
            steps.push_back({index, "goto-palette", {command.substr(13)}});
        } else if (command.starts_with("palette:")) {
            steps.push_back({index, "palette", {command.substr(8)}});
        } else if (!command.empty()) {
            steps.push_back({index, "command", {command}});
        }
    }
    return steps;
}

std::string format_ui_script_step(const UiScriptStep& step) {
    std::string text = step.op;
    for (const auto& argument : step.args) {
        text += ' ';
        const bool quote = argument.empty() || argument.find_first_of(" \t\"#;\\") != std::string::npos;
        if (!quote) {
            text += argument;
            continue;
        }
        text += '"';
        for (const char c : argument) {
            if (c == '"' || c == '\\') text += '\\';
            text += c;
        }
        text += '"';
    }
    return text;
}

UiTarget parse_ui_target(const std::string_view text) {
    const auto separator = text.find("::");
    if (separator == std::string_view::npos) return {{}, std::string(text)};
    return {std::string(text.substr(0, separator)), std::string(text.substr(separator + 2))};
}

bool ui_window_matches(const std::string_view qualifier, const std::string_view window) {
    // Child windows are named "Parent/Child_XXXXXXXX"; any path segment may match.
    std::size_t start = 0;
    while (start <= window.size()) {
        auto end = window.find('/', start);
        if (end == std::string_view::npos) end = window.size();
        if (label_matches(qualifier, window.substr(start, end - start))) return true;
        if (end == window.size()) break;
        start = end + 1;
    }
    return false;
}

bool ui_target_matches(const UiTarget& target, const std::string_view window, const std::string_view label) {
    return label_matches(target.label, label) && (target.window.empty() || ui_window_matches(target.window, window));
}

std::string state_snapshot_json(const StateSnapshot& snapshot) {
    std::string out = "{\n";
    bool first = true;
    for (const auto& [key, value] : snapshot) {
        if (!first) out += ",\n";
        first = false;
        out += "  ";
        append_json_string(out, key);
        out += ": ";
        append_json_string(out, value);
    }
    out += "\n}\n";
    return out;
}

std::vector<std::string> lint_commands(const CommandRegistry& registry) {
    std::vector<std::string> problems;
    std::set<std::pair<std::string, std::string>> palette_labels;
    for (const auto& command : registry.commands()) {
        const auto& id = command.id;
        const bool id_ok = !id.empty() && id.front() != '.' && id.back() != '.' &&
                           id.find('.') != std::string::npos && id.find("..") == std::string::npos &&
                           std::ranges::all_of(id, [](const char c) {
                               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
                           });
        if (!id_ok) problems.push_back(id + ": the ID is not dotted lower-case words");
        if (command.label.empty()) problems.push_back(id + ": no label");
        if (command.category.empty()) problems.push_back(id + ": no category");
        if (!command.run) problems.push_back(id + ": no action");
        if (command.in_palette && !palette_labels.emplace(command.category, command.label).second)
            problems.push_back(id + ": another palette command in " + command.category + " is labelled '" +
                               command.label + "'");
    }
    for (const auto& conflict : registry.conflicts())
        problems.push_back(conflict.first + " / " + conflict.second + ": shortcut " + conflict.shortcut);
    return problems;
}

} // namespace rwsman
