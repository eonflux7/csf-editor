#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "navigation.hpp"
#include "references.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace rwsman::ui {
namespace {

Token token_for(const DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::note:
        return Token::text_dim;
    case DiagnosticSeverity::warning:
        return Token::warn;
    case DiagnosticSeverity::error:
        return Token::error;
    }
    return Token::warn;
}

const char* icon_for(const DiagnosticSeverity severity) {
    switch (severity) {
    case DiagnosticSeverity::note:
        return icons::LC_INFO;
    case DiagnosticSeverity::warning:
        return icons::LC_TRIANGLE_ALERT;
    case DiagnosticSeverity::error:
        return icons::LC_CIRCLE_ALERT;
    }
    return icons::LC_INFO;
}

} // namespace

void draw_diagnostics(AppState& state) {
    static DiagnosticFilter filter;
    static std::array<char, 128> text{};
    static DiagnosticColumn column = DiagnosticColumn::severity;
    static bool ascending = true;

    std::size_t counts[3]{};
    std::set<std::string> sources;
    for (const auto& row : state.diagnostics) {
        ++counts[static_cast<std::size_t>(row.severity)];
        sources.insert(row.source);
    }
    const auto toggle = [&](const char* label, bool& flag, const DiagnosticSeverity severity) {
        ImGui::PushStyleColor(ImGuiCol_Text, color(flag ? token_for(severity) : Token::text_dim));
        const std::string caption = std::string(icon_for(severity)) + " " +
                                    std::to_string(counts[static_cast<std::size_t>(severity)]) + " " + label;
        if (ImGui::SmallButton(caption.c_str())) flag = !flag;
        ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    toggle("errors", filter.errors, DiagnosticSeverity::error);
    toggle("warnings", filter.warnings, DiagnosticSeverity::warning);
    toggle("notes", filter.notes, DiagnosticSeverity::note);
    ImGui::SetNextItemWidth(130.0F * ui_scale());
    if (ImGui::BeginCombo("##diag_source", filter.source.empty() ? "all sources" : filter.source.c_str())) {
        if (ImGui::Selectable("all sources", filter.source.empty())) filter.source.clear();
        for (const auto& source : sources)
            if (ImGui::Selectable(source.c_str(), filter.source == source)) filter.source = source;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (search_input("##diag_text", "filter message, file, code...", text.data(), text.size()))
        filter.text = text.data();

    if (state.diagnostics.empty()) {
        dim_text("No diagnostics. Parser, mission, resource, and navigation findings appear here.");
        return;
    }
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Hideable |
                                      ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##diagnostics", 6, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Sev", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 44.0F * ui_scale(),
                            static_cast<ImGuiID>(DiagnosticColumn::severity));
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 80.0F * ui_scale(),
                            static_cast<ImGuiID>(DiagnosticColumn::source));
    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthFixed, 120.0F * ui_scale(),
                            static_cast<ImGuiID>(DiagnosticColumn::file));
    ImGui::TableSetupColumn("Entry", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending,
                            60.0F * ui_scale(), static_cast<ImGuiID>(DiagnosticColumn::entry));
    ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80.0F * ui_scale(),
                            static_cast<ImGuiID>(DiagnosticColumn::offset));
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 0.0F,
                            static_cast<ImGuiID>(DiagnosticColumn::message));
    ImGui::TableHeadersRow();
    if (auto* sort = ImGui::TableGetSortSpecs(); sort && sort->SpecsCount > 0) {
        column = static_cast<DiagnosticColumn>(sort->Specs[0].ColumnUserID);
        ascending = sort->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
    }
    const auto rows = select_diagnostics(state.diagnostics, filter, column, ascending);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& row = state.diagnostics[rows[static_cast<std::size_t>(i)]];
            ImGui::TableNextRow();
            ImGui::PushID(i);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, color(token_for(row.severity)));
            const bool clicked = ImGui::Selectable(icon_for(row.severity), false,
                                                   ImGuiSelectableFlags_SpanAllColumns |
                                                       ImGuiSelectableFlags_AllowOverlap);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", diagnostic_severity_name(row.severity),
                                                            row.target.empty() ? "Not linked to a selectable object"
                                                                               : "Click to select the source");
            if (clicked && !row.target.empty()) navigate_to(state, row.target);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.source.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.file.c_str());
            ImGui::TableNextColumn();
            ImGui::PushFont(font(Font::mono));
            if (row.entry) ImGui::Text("%u", *row.entry);
            ImGui::TableNextColumn();
            if (row.offset) ImGui::Text("0x%06llX", static_cast<unsigned long long>(*row.offset));
            ImGui::PopFont();
            ImGui::TableNextColumn();
            if (!row.code.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
                ImGui::TextUnformatted(row.code.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(row.message.c_str());
            ImGui::PopID();
        }
    ImGui::EndTable();
}

void draw_references(AppState& state) {
    if (state.selection.empty()) {
        dim_text("Select an actor, dummy, script, or class to see what uses it.");
        return;
    }
    const auto rows = collect_references(state, state.selection);
    ImGui::TextUnformatted(selection_title(state, state.selection).c_str());
    ImGui::SameLine();
    dim_text("%zu reference%s", rows.size(), rows.size() == 1 ? "" : "s");
    if (rows.empty()) {
        dim_text("Nothing references this object in the loaded scripts and databases.");
        return;
    }
    if (!ImGui::BeginTable("##references", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Group", ImGuiTableColumnFlags_WidthFixed, 190.0F * ui_scale());
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 240.0F * ui_scale());
    ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int id = 0;
    for (const auto& row : rows) {
        ImGui::TableNextRow();
        ImGui::PushID(id++);
        ImGui::TableNextColumn();
        badge(row.provenance, row.evidence.c_str());
        ImGui::SameLine();
        const bool clicked = ImGui::Selectable(row.group.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
        if (clicked && !row.target.empty()) navigate_to(state, row.target);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.label.c_str());
        ImGui::TableNextColumn();
        ImGui::PushFont(font(Font::mono));
        ImGui::TextUnformatted(row.detail.c_str());
        ImGui::PopFont();
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace rwsman::ui
