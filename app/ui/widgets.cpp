#include "ui/widgets.hpp"

#include "ui/fonts.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace rwsman::ui {

void section(const char* label) {
    std::string upper(label);
    for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    ImGui::Spacing();
    // The hairline ends at the right edge of the current column or group, not the window.
    const float x1 = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::PushFont(font(Font::sans_bold));
    ImGui::TextUnformatted(upper.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    // Hairline filling the rest of the row.
    const ImVec2 text_max = ImGui::GetItemRectMax();
    const ImVec2 text_min = ImGui::GetItemRectMin();
    const float y = (text_min.y + text_max.y) * 0.5F;
    const float x0 = text_max.x + ImGui::GetStyle().ItemSpacing.x;
    if (x1 > x0) ImGui::GetWindowDrawList()->AddLine({x0, y}, {x1, y}, color_u32(Token::line));
}

void dim_text(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::TextWrappedV(format, arguments);
    ImGui::PopStyleColor();
    va_end(arguments);
}

void token_text(const Token token, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    ImGui::PushStyleColor(ImGuiCol_Text, color(token));
    ImGui::TextV(format, arguments);
    ImGui::PopStyleColor();
    va_end(arguments);
}

bool search_input(const char* id, const char* hint, char* buffer, const std::size_t size,
                  const float width) {
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::accent));
    ImGui::TextUnformatted(">");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(width);
    ImGui::PushFont(font(Font::mono));
    const bool edited = ImGui::InputTextWithHint(id, hint, buffer, size);
    ImGui::PopFont();
    return edited;
}

bool icon_button(const char* id, const char* icon, const char* tooltip, const bool active) {
    ImGui::PushStyleColor(ImGuiCol_Button, transparent());
    if (active) ImGui::PushStyleColor(ImGuiCol_Text, color(Token::accent));
    ImGui::PushID(id);
    const bool clicked = ImGui::Button(icon);
    ImGui::PopID();
    if (active) ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

void badge(const Provenance provenance, const char* explanation) {
    ImGui::PushStyleColor(ImGuiCol_Text, color(provenance_token(provenance)));
    ImGui::TextUnformatted(provenance_glyph(provenance));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushStyleColor(ImGuiCol_Text, color(provenance_token(provenance)));
        ImGui::TextUnformatted(provenance_name(provenance));
        ImGui::PopStyleColor();
        if (explanation && *explanation) ImGui::TextUnformatted(explanation);
        ImGui::EndTooltip();
    }
}

bool copyable_text(const char* text, const char* copy, const char* tooltip) {
    ImGui::PushFont(font(Font::mono));
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine({lo.x, hi.y}, {hi.x, hi.y}, color_u32(Token::accent, 0.7F));
        ImGui::SetTooltip("%s", tooltip ? tooltip : "Click to copy");
    }
    const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (clicked) ImGui::SetClipboardText(copy ? copy : text);
    return clicked;
}

bool hex_link(const std::uint64_t offset, const char* tooltip) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%06llX", static_cast<unsigned long long>(offset));
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::inferred));
    const bool clicked = copyable_text(buffer, buffer, tooltip ? tooltip : "Click to copy offset");
    ImGui::PopStyleColor();
    return clicked;
}

void highlighted_text(const std::string& text, const std::vector<std::uint32_t>& positions) {
    // Draw run by run so matched characters use the accent color.
    std::size_t next = 0, index = 0;
    bool first = true;
    const auto matches = [&](const std::size_t at) {
        return next < positions.size() && positions[next] == at;
    };
    while (index < text.size()) {
        const bool matched = matches(index);
        std::size_t end = index;
        while (end < text.size() && matches(end) == matched) {
            if (matched) ++next;
            ++end;
        }
        if (!first) ImGui::SameLine(0.0F, 0.0F);
        first = false;
        if (matched) ImGui::PushStyleColor(ImGuiCol_Text, color(Token::accent));
        ImGui::TextUnformatted(text.c_str() + index, text.c_str() + end);
        if (matched) ImGui::PopStyleColor();
        index = end;
    }
    if (text.empty()) ImGui::TextUnformatted("");
}

bool icon_row(const char* id, const char* icon, const Token icon_token, const char* label,
              const bool selected) {
    ImGui::PushID(id);
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns);
    ImGui::PopID();
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_Text, color(icon_token));
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    return clicked;
}

} // namespace rwsman::ui
