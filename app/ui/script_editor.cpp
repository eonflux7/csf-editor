// The Script mode source editor (docs/plans/editor-ux-redesign.md, S2): an
// InputTextMultiline whose own text is hidden and drawn again on top in
// syntax colours, with the error line marked, Tab completion of opcodes and
// operand tags from the signature table, the signature of the current line's
// opcode, and Ctrl+click on an operand to go to what it names.
#include "ui/script_editor.hpp"

#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include "csf/script_signatures.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace rwsman::ui {
namespace {

// Opcode and operand-tag names, most used first.
const std::vector<std::string_view>& words(const bool operands) {
    static std::vector<std::string_view> opcodes, tags;
    if (opcodes.empty()) {
        std::map<std::string_view, std::uint32_t> uses[2];
        for (const auto& signature : csf::script_signatures()) uses[signature.instruction ? 0 : 1][signature.head] += signature.uses;
        for (int k = 0; k < 2; ++k) {
            std::vector<std::pair<std::uint32_t, std::string_view>> ranked;
            for (const auto& [head, n] : uses[k]) ranked.emplace_back(n, head);
            std::ranges::sort(ranked, std::greater{});
            for (const auto& [n, head] : ranked) (k == 0 ? opcodes : tags).push_back(head);
        }
    }
    return operands ? tags : opcodes;
}

struct CallbackData {
    std::string* text{};
    int cursor{};
};

int callback(ImGuiInputTextCallbackData* data) {
    auto* user = static_cast<CallbackData*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        user->text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = user->text->data();
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        // Tab: the first suggestion for the word being typed, else two spaces.
        const std::string_view buffer(data->Buf, static_cast<std::size_t>(data->BufTextLen));
        const auto context = completion_context(buffer, static_cast<std::size_t>(data->CursorPos));
        const auto suggestions = context ? complete_word(context->prefix, words(context->operand), 1)
                                         : std::vector<std::string_view>{};
        if (!suggestions.empty()) {
            const std::string word(suggestions.front());
            data->DeleteChars(static_cast<int>(context->begin), data->CursorPos - static_cast<int>(context->begin));
            data->InsertChars(data->CursorPos, word.c_str());
        } else {
            data->InsertChars(data->CursorPos, "  ");
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        user->cursor = data->CursorPos;
    }
    return 0;
}

ImU32 color_of(const SyntaxKind kind) {
    switch (kind) {
    case SyntaxKind::opcode: return syntax_color(Syntax::opcode);
    case SyntaxKind::tag: return syntax_color(Syntax::tag);
    case SyntaxKind::number: return syntax_color(Syntax::number);
    case SyntaxKind::string: return syntax_color(Syntax::string);
    case SyntaxKind::label: return syntax_color(Syntax::label);
    case SyntaxKind::punctuation: return syntax_color(Syntax::punctuation);
    case SyntaxKind::plain: break;
    }
    return syntax_color(Syntax::plain);
}

// Line starts of a text.
std::vector<std::size_t> line_starts(const std::string& text) {
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') starts.push_back(i + 1);
    return starts;
}

std::string_view line_at(const std::string& text, const std::vector<std::size_t>& starts, const std::size_t line) {
    const auto begin = starts[line];
    const auto end = line + 1 < starts.size() ? starts[line + 1] - 1 : text.size();
    return std::string_view(text).substr(begin, end - begin);
}

} // namespace

std::string signature_hint(const std::string_view opcode) {
    const auto* signature = csf::find_script_signature(opcode, true);
    if (!signature) return {};
    std::string hint(signature->head);
    std::string_view positions = signature->positions;
    for (std::size_t n = 0; n < signature->maximum_arity; ++n) {
        const auto semicolon = positions.find(';');
        const auto shapes = positions.substr(0, semicolon);
        positions = semicolon == std::string_view::npos ? std::string_view{} : positions.substr(semicolon + 1);
        std::string shown(shapes.substr(0, 60));
        if (shapes.size() > 60) shown += "...";
        hint += (n < signature->minimum_arity ? " (" : " [(") + (shown.empty() ? std::string("?") : shown) +
                (n < signature->minimum_arity ? ")" : ")]");
    }
    return hint;
}

ScriptEditorOutcome script_code_editor(const char* label, std::string& text, const ImVec2 size, const bool read_only,
                                       const std::optional<std::size_t> error_line) {
    ScriptEditorOutcome outcome;
    const ImGuiID id = ImGui::GetID(label);
    static CallbackData data;
    data.text = &text;
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways;
    if (read_only) flags |= ImGuiInputTextFlags_ReadOnly;
    else flags |= ImGuiInputTextFlags_CallbackCompletion;
    // ImGui's text is invisible; the coloured copy is drawn over it below.
    ImGui::PushStyleColor(ImGuiCol_Text, transparent());
    outcome.changed = ImGui::InputTextMultiline(label, text.data(), text.capacity() + 1, size, flags, callback, &data);
    ImGui::PopStyleColor();
    const ImRect frame(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    const bool hovered = ImGui::IsItemHovered();
    char name[512];
    std::snprintf(name, sizeof(name), "%s/%s_%08X", ImGui::GetCurrentWindow()->Name, label, id);
    const auto* child = ImGui::FindWindowByName(name);
    if (!child) return outcome;
    const ImVec2 origin = child->DC.CursorStartPos;
    auto* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const auto starts = line_starts(text);
    const auto x_of = [&](const std::string_view line, const std::size_t column) {
        return font->CalcTextSizeA(font_size, FLT_MAX, 0.0F, line.data(), line.data() + std::min(column, line.size())).x;
    };
    auto* draw = ImGui::GetWindowDrawList();
    const ImRect clip(ImVec2(frame.Min.x + 1, frame.Min.y + 1), ImVec2(child->InnerClipRect.Max.x, frame.Max.y - 1));
    draw->PushClipRect(clip.Min, clip.Max, true);
    const auto first = static_cast<std::size_t>(std::max(0.0F, std::floor((clip.Min.y - origin.y) / font_size)));
    for (auto line = first; line < starts.size(); ++line) {
        const float y = origin.y + static_cast<float>(line) * font_size;
        if (y > clip.Max.y) break;
        const auto content = line_at(text, starts, line);
        if (error_line && *error_line == line + 1) {
            draw->AddRectFilled({clip.Min.x, y}, {clip.Max.x, y + font_size}, color_u32(Token::error, 0.14F));
            draw->AddRectFilled({clip.Min.x, y}, {clip.Min.x + 3.0F, y + font_size}, color_u32(Token::error));
        }
        std::size_t at = 0;
        const auto put = [&](const std::size_t end, const ImU32 colour) {
            if (end <= at) return;
            draw->AddText(font, font_size, {origin.x + x_of(content, at), y}, colour, content.data() + at, content.data() + end);
            at = end;
        };
        for (const auto& span : highlight_script_line(content)) {
            put(span.begin, syntax_color(Syntax::plain));
            put(span.end, color_of(span.kind));
        }
        put(content.size(), syntax_color(Syntax::plain));
    }
    // The cursor ImGui would have drawn in the text colour.
    const bool active = ImGui::GetActiveID() == id;
    if (active) {
        const auto cursor = static_cast<std::size_t>(std::clamp(data.cursor, 0, static_cast<int>(text.size())));
        const auto line = static_cast<std::size_t>(std::upper_bound(starts.begin(), starts.end(), cursor) - starts.begin() - 1);
        const auto content = line_at(text, starts, line);
        const float x = origin.x + x_of(content, cursor - starts[line]);
        const float y = origin.y + static_cast<float>(line) * font_size;
        const auto* state = ImGui::GetInputTextState(id);
        const bool blink_on = !ImGui::GetIO().ConfigInputTextCursorBlink || !state || state->CursorAnim <= 0.0F ||
                              std::fmod(state->CursorAnim, 1.20F) <= 0.80F;
        if (blink_on) draw->AddLine({x, y}, {x, y + font_size - 1.0F}, syntax_color(Syntax::plain));
        outcome.cursor_line = std::string(content);
        // Completion suggestions under the word being typed.
        const auto context = read_only ? std::nullopt : completion_context(text, cursor);
        const auto suggestions = context ? complete_word(context->prefix, words(context->operand), 8)
                                         : std::vector<std::string_view>{};
        if (!suggestions.empty()) {
            auto* front = ImGui::GetForegroundDrawList();
            const float word_x = origin.x + x_of(content, context->begin - starts[line]);
            const ImVec2 top{word_x, y + font_size + 2.0F};
            float width = 0.0F;
            for (const auto word : suggestions)
                width = std::max(width, font->CalcTextSizeA(font_size, FLT_MAX, 0.0F, word.data(), word.data() + word.size()).x);
            const float row = font_size + 2.0F;
            front->AddRectFilled(top, {top.x + width + 12.0F, top.y + row * static_cast<float>(suggestions.size()) + 4.0F},
                                 color_u32(Token::bg2), 3.0F);
            front->AddRect(top, {top.x + width + 12.0F, top.y + row * static_cast<float>(suggestions.size()) + 4.0F},
                           color_u32(Token::line), 3.0F);
            for (std::size_t k = 0; k < suggestions.size(); ++k) {
                const ImVec2 at{top.x + 6.0F, top.y + 2.0F + row * static_cast<float>(k)};
                if (k == 0) front->AddRectFilled({top.x + 1, at.y}, {top.x + width + 11.0F, at.y + row}, color_u32(Token::accent_dim));
                front->AddText(font, font_size, at, color_of(context->operand ? SyntaxKind::tag : SyntaxKind::opcode),
                               suggestions[k].data(), suggestions[k].data() + suggestions[k].size());
            }
            outcome.suggestions = suggestions.size();
        }
    }
    draw->PopClipRect();
    // Ctrl+click: the operand under the pointer.
    if (hovered && ImGui::GetIO().KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const auto mouse = ImGui::GetIO().MousePos;
        const auto line = static_cast<std::size_t>(std::max(0.0F, (mouse.y - origin.y) / font_size));
        if (line < starts.size()) {
            const auto content = line_at(text, starts, line);
            std::size_t column = 0;
            while (column < content.size() && origin.x + x_of(content, column + 1) <= mouse.x) ++column;
            outcome.ctrl_clicked = operand_at(content, column);
        }
    }
    return outcome;
}

} // namespace rwsman::ui
