#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Script source text as the Script mode editor sees it
// (docs/plans/editor-ux-redesign.md, S2): highlighting spans, the word being
// completed, and the operand under a click. Text only; the opcode and tag
// names come from the caller (csf_script_signatures).
namespace rwsman {

enum class SyntaxKind : unsigned char { plain, opcode, tag, number, string, label, punctuation };

struct SyntaxSpan {
    std::size_t begin{}, end{};  // byte offsets in the line
    SyntaxKind kind{SyntaxKind::plain};
};

// One line: an identifier after '(' is an operand tag, the first identifier
// of a line (or after '{') an opcode, ".NAME" a label; numbers, strings and
// brackets. Unmarked bytes are plain.
[[nodiscard]] std::vector<SyntaxSpan> highlight_script_line(std::string_view line);

struct CompletionContext {
    std::size_t begin{};  // where the word starts
    std::string prefix;   // what is typed of it
    bool operand{};       // after '(': an operand tag; otherwise an opcode
};
// The word ending at `cursor`, when it is an identifier of at least one letter.
[[nodiscard]] std::optional<CompletionContext> completion_context(std::string_view text, std::size_t cursor);
// Up to `limit` of `words` for `prefix`: those it starts first, then those
// containing it, each in the given order (case-insensitive).
[[nodiscard]] std::vector<std::string_view> complete_word(std::string_view prefix, std::span<const std::string_view> words,
                                                          std::size_t limit);

// The innermost "(TAG value ...)" around `column` of a line: its tag and values.
struct OperandAt {
    std::string tag;
    std::vector<std::string> values;
};
[[nodiscard]] std::optional<OperandAt> operand_at(std::string_view line, std::size_t column);

// The first identifier of a line (its opcode), if any.
[[nodiscard]] std::string line_opcode(std::string_view line);

} // namespace rwsman
