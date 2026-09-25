#include "app_state.hpp"
#include "ui/ui.hpp"
#include "ui/fonts.hpp"

#include "authoring.hpp"
#include "mission_editing.hpp"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace rwsman::ui {

// The authoring project's height report: placements and anchored actors whose
// height rule resolves more than 1 cm from where they stand.
void draw_height_report(AppState& state) {
    auto& session = state.authoring;
    if (!session.show_heights || !session.project) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, {0.5F, 0.5F});
    ImGui::SetNextWindowSize({520.0F * ui_scale(), 360.0F * ui_scale()}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Height report", &session.show_heights)) {
        ImGui::End();
        return;
    }
    const bool pending = authoring_pending(state);
    if (pending) ImGui::TextDisabled("Rebuilding and checking...");
    else if (session.findings.empty()) ImGui::TextUnformatted("Everything stands where its height rule says.");
    else ImGui::Text("%zu placements and actors are off their height rules.", session.findings.size());

    ImGui::BeginDisabled(pending || session.findings.empty());
    if (ImGui::Button("Resnap all")) resnap_authoring_heights(state);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(pending);
    if (ImGui::Button("Check again")) rebuild_authoring_map(state, false);
    ImGui::EndDisabled();

    if (!session.findings.empty() &&
        ImGui::BeginTable("heights", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Subject");
        ImGui::TableSetupColumn("Stored");
        ImGui::TableSetupColumn("Resolved");
        ImGui::TableSetupColumn("Change");
        ImGui::TableHeadersRow();
        for (const auto& finding : session.findings) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool actor = finding.subject == csf::HeightFinding::Subject::actor;
            const auto label = (actor ? "actor " : "") + finding.id;
            if (actor) {
                // Actors are scene records: select one to see it in the viewport.
                if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns))
                    select_mission_record(state, {MissionRecordKey::Kind::actor, finding.actor_id, 0}, true);
            } else {
                ImGui::TextUnformatted(label.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", finding.position.y);
            ImGui::TableNextColumn();
            if (finding.resolved) {
                ImGui::Text("%.1f", *finding.resolved);
                ImGui::TableNextColumn();
                ImGui::Text("%+.1f cm", *finding.resolved - finding.position.y);
            } else {
                ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", finding.problem.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace rwsman::ui
