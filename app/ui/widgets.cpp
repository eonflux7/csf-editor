#include "ui/widgets.hpp"

#include "ui/fonts.hpp"
#include "ui/icons.hpp"

#include <imgui_internal.h>

#include <array>
#include <cctype>
#include <cstdarg>
#include <algorithm>
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
    // "<icon>##<id>": UI scripts address the button by its ID ("Properties::pick_support").
    std::string_view name(id);
    if (name.starts_with("##")) name.remove_prefix(2);
    const bool clicked = ImGui::Button((std::string(icon) + "##" + std::string(name)).c_str());
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
    // Not SpanAllColumns: Home lays its lists out as columns of one table row,
    // and a spanning row would highlight (and take clicks) across all of them.
    const bool clicked = ImGui::Selectable("##row", selected);
    ImGui::PopID();
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_Text, color(icon_token));
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    return clicked;
}

bool begin_card(const char* id, const char* title, const CardOptions& options) {
    const float scale = ui_scale();
    auto* storage = ImGui::GetStateStorage();
    const ImGuiID open_id = ImGui::GetID((std::string(id) + "#open").c_str());
    bool open = !options.collapsible || storage->GetBool(open_id, options.default_open);

    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0F * scale, 8.0F * scale});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, color(Token::bg2, 0.55F));
    ImGui::PushStyleColor(ImGuiCol_Border, color(Token::line, 0.7F));
    const bool visible = ImGui::BeginChild(id, {0.0F, 0.0F},
                                           ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding |
                                               ImGuiChildFlags_Borders,
                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    // Header: the whole row folds the card.
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetTextLineHeight();
    if (options.collapsible) {
        // Named after the title, so UI scripts can fold a card ("Properties::Advanced").
        if (ImGui::InvisibleButton((std::string("##") + title).c_str(), {std::max(width - 24.0F * scale, 1.0F), height})) {
            open = !open;
            storage->SetBool(open_id, open);
        }
        ImGui::SetCursorScreenPos(start);
    }
    if (options.icon) {
        ImGui::PushStyleColor(ImGuiCol_Text, options.icon_color.w > 0.0F ? options.icon_color : color(Token::text_dim));
        ImGui::TextUnformatted(options.icon);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::PushFont(font(Font::sans_bold));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (options.subtitle && *options.subtitle) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::TextUnformatted(options.subtitle);
        ImGui::PopStyleColor();
    }
    if (options.help) help_marker(options.help);
    if (options.collapsible) {
        ImGui::SameLine(width - 6.0F * scale);
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::TextUnformatted(open ? icons::LC_CHEVRON_DOWN : icons::LC_CHEVRON_RIGHT);
        ImGui::PopStyleColor();
    }
    if (!visible || !open) {
        ImGui::EndChild();
        ImGui::Spacing();
        return false;
    }
    ImGui::Spacing();
    return true;
}

void end_card() {
    ImGui::EndChild();
    ImGui::Spacing();
}

bool primary_button(const char* label, const ImVec2 size) {
    const auto accent = color(Token::accent);
    ImGui::PushStyleColor(ImGuiCol_Button, accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(Token::accent, 0.85F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color(Token::accent, 0.7F));
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::bg0));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

bool secondary_button(const char* label, const ImVec2 size) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
    ImGui::PushStyleColor(ImGuiCol_Border, color(Token::line));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return clicked;
}

bool danger_button(const char* label, const ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color(Token::error, 0.75F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, color(Token::error));
    const bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(2);
    return clicked;
}

bool chip(const char* label, const bool active) {
    const float scale = ui_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0F * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8.0F * scale, 2.0F * scale});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
    ImGui::PushStyleColor(ImGuiCol_Border, color(active ? Token::accent : Token::line));
    ImGui::PushStyleColor(ImGuiCol_Button, active ? color(Token::accent, 0.35F) : transparent());
    ImGui::PushStyleColor(ImGuiCol_Text, color(active ? Token::text : Token::text_dim));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    return clicked;
}

bool empty_state(const char* icon, const char* message, const char* action) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    if (icon) {
        ImGui::TextUnformatted(icon);
        ImGui::SameLine();
    }
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextUnformatted(message);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    if (!action) return false;
    ImGui::Spacing();
    return primary_button(action);
}

void help_marker(const char* text) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::TextUnformatted(icons::LC_CIRCLE_HELP);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0F);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

bool begin_properties(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, 104.0F * ui_scale());
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void property_row(const char* label, const char* help) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (help) help_marker(help);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void end_properties() { ImGui::EndTable(); }

} // namespace rwsman::ui

namespace rwsman::ui {
namespace {

// The field being edited and its working value; one at a time.
struct ActiveField {
    ImGuiID id{};
    std::array<char, 1024> text{};
    float number{};
    int integer{};
};
ActiveField active_field;

bool editing(const ImGuiID id) { return ImGui::GetActiveID() == id && active_field.id == id; }

} // namespace

bool edit_text_value(const char* id, const std::string& value, std::string& edited, const char* hint) {
    const auto key = ImGui::GetID(id);
    std::array<char, 1024> shown{};
    const bool active = editing(key);
    char* data = active ? active_field.text.data() : shown.data();
    if (!active) value.copy(shown.data(), std::min(value.size(), shown.size() - 1));
    if (hint) ImGui::InputTextWithHint(id, hint, data, shown.size());
    else ImGui::InputText(id, data, shown.size());
    if (ImGui::IsItemActivated()) {
        active_field.id = key;
        active_field.text = shown;
    }
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    edited = data;
    active_field.id = 0;
    return edited != value;
}

bool edit_float_value(const char* id, const float value, float& edited, const float speed, const char* format) {
    const auto key = ImGui::GetID(id);
    const bool active = editing(key);
    float shown = active ? active_field.number : value;
    ImGui::DragFloat(id, &shown, speed, 0.0F, 0.0F, format);
    if (ImGui::IsItemActivated() && !active) active_field = {key, {}, value, 0};
    if (ImGui::GetActiveID() == key) active_field.number = shown;
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    edited = shown;
    active_field.id = 0;
    return edited != value;
}

bool edit_int_value(const char* id, const int value, int& edited) {
    const auto key = ImGui::GetID(id);
    const bool active = editing(key);
    int shown = active ? active_field.integer : value;
    ImGui::InputInt(id, &shown, 0, 0);
    if (ImGui::IsItemActivated() && !active) active_field = {key, {}, 0, value};
    if (ImGui::GetActiveID() == key) active_field.integer = shown;
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    edited = shown;
    active_field.id = 0;
    return edited != value;
}

} // namespace rwsman::ui

namespace rwsman::ui {

void name_last_item(const char* label) {
#ifdef IMGUI_ENABLE_TEST_ENGINE
    ImGuiContext& g = *GImGui;
    IMGUI_TEST_ENGINE_ITEM_INFO(g.LastItemData.ID, label, g.LastItemData.StatusFlags);
#else
    (void)label;
#endif
}

} // namespace rwsman::ui
