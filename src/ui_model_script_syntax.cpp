#include "rwsman/script_syntax.hpp"

#include <algorithm>
#include <cctype>

namespace rwsman {
namespace {

bool identifier_start(const char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identifier_char(const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string upper(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

} // namespace

std::vector<SyntaxSpan> highlight_script_line(const std::string_view line) {
    std::vector<SyntaxSpan> spans;
    bool line_start = true;  // nothing but whitespace (or '{') so far
    for (std::size_t i = 0; i < line.size();) {
        const char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '"') {
            auto end = i + 1;
            while (end < line.size() && line[end] != '"') end += line[end] == '\\' ? 2 : 1;
            end = std::min(end + 1, line.size());
            spans.push_back({i, end, SyntaxKind::string});
            i = end;
            line_start = false;
            continue;
        }
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}') {
            spans.push_back({i, i + 1, SyntaxKind::punctuation});
            line_start = c == '{' || (line_start && c != '(' && c != '[');
            ++i;
            continue;
        }
        if (c == '.' && i + 1 < line.size() && identifier_start(line[i + 1])) {
            auto end = i + 1;
            while (end < line.size() && identifier_char(line[end])) ++end;
            spans.push_back({i, end, SyntaxKind::label});
            i = end;
            line_start = false;
            continue;
        }
        const bool signed_number = (c == '-' || c == '+') && i + 1 < line.size() &&
                                   std::isdigit(static_cast<unsigned char>(line[i + 1]));
        if (std::isdigit(static_cast<unsigned char>(c)) || signed_number) {
            auto end = i + 1;
            while (end < line.size() && (std::isdigit(static_cast<unsigned char>(line[end])) || line[end] == '.' ||
                                         line[end] == 'e' || line[end] == 'E' || line[end] == '-'))
                ++end;
            spans.push_back({i, end, SyntaxKind::number});
            i = end;
            line_start = false;
            continue;
        }
        if (identifier_start(c)) {
            auto end = i + 1;
            while (end < line.size() && identifier_char(line[end])) ++end;
            const bool after_paren = i > 0 && line[i - 1] == '(';
            spans.push_back({i, end, after_paren ? SyntaxKind::tag : line_start ? SyntaxKind::opcode : SyntaxKind::plain});
            i = end;
            line_start = false;
            continue;
        }
        ++i;
        line_start = false;
    }
    return spans;
}

std::optional<CompletionContext> completion_context(const std::string_view text, const std::size_t cursor) {
    if (cursor > text.size()) return std::nullopt;
    auto begin = cursor;
    while (begin > 0 && identifier_char(text[begin - 1])) --begin;
    if (begin == cursor || !identifier_start(text[begin])) return std::nullopt;
    if (begin > 0 && text[begin - 1] == '.') return std::nullopt;  // a label
    CompletionContext context{begin, std::string(text.substr(begin, cursor - begin)), begin > 0 && text[begin - 1] == '('};
    return context;
}

std::vector<std::string_view> complete_word(const std::string_view prefix, const std::span<const std::string_view> words,
                                            const std::size_t limit) {
    const auto wanted = upper(prefix);
    std::vector<std::string_view> starts, contains;
    for (const auto word : words) {
        const auto name = upper(word);
        if (name == wanted) continue;
        if (name.starts_with(wanted)) starts.push_back(word);
        else if (name.find(wanted) != std::string::npos) contains.push_back(word);
    }
    starts.insert(starts.end(), contains.begin(), contains.end());
    if (starts.size() > limit) starts.resize(limit);
    return starts;
}

std::optional<OperandAt> operand_at(const std::string_view line, const std::size_t column) {
    // The '(' that opens the innermost group containing the column.
    std::vector<std::size_t> open;
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') i += line[i] == '\\' ? 2 : 1;
            continue;
        }
        if (line[i] == '(') open.push_back(i);
        if (line[i] == ')' && !open.empty()) {
            if (open.back() <= column && column <= i) {
                found = open.back();
                break;
            }
            open.pop_back();
        }
    }
    if (!found && !open.empty() && open.back() <= column) found = open.back();
    if (!found) return std::nullopt;
    OperandAt operand;
    std::size_t i = *found + 1;
    while (i < line.size() && identifier_char(line[i])) operand.tag.push_back(line[i++]);
    if (operand.tag.empty()) return std::nullopt;
    while (i < line.size() && line[i] != ')') {
        while (i < line.size() && line[i] == ' ') ++i;
        if (i >= line.size() || line[i] == ')' || line[i] == '(') break;
        std::string value;
        if (line[i] == '"') {
            for (++i; i < line.size() && line[i] != '"'; ++i) value.push_back(line[i]);
            ++i;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != ')') value.push_back(line[i++]);
        }
        operand.values.push_back(std::move(value));
    }
    return operand;
}

std::string line_opcode(const std::string_view line) {
    for (const auto& span : highlight_script_line(line))
        if (span.kind == SyntaxKind::opcode) return std::string(line.substr(span.begin, span.end - span.begin));
    return {};
}

} // namespace rwsman
