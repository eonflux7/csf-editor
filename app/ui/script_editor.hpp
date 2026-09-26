#pragma once

#include "rwsman/script_syntax.hpp"

#include <imgui.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace rwsman::ui {

struct ScriptEditorOutcome {
    bool changed{};
    std::optional<OperandAt> ctrl_clicked;  // Ctrl+click on an operand
    std::string cursor_line;                // the line being edited (while active)
    std::size_t suggestions{};              // completions shown (Tab takes the first)
};

// A script source editor: syntax colours, the 1-based `error_line` marked,
// Tab completion of opcodes and operand tags, and Ctrl+click on an operand.
ScriptEditorOutcome script_code_editor(const char* label, std::string& text, ImVec2 size, bool read_only,
                                       std::optional<std::size_t> error_line);
// "OPCODE (TAG|TAG) [(optional)]" from the signature table; empty when unknown.
[[nodiscard]] std::string signature_hint(std::string_view opcode);

} // namespace rwsman::ui
