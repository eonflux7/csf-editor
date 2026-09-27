// The Problems panel (docs/archive/editor/editor-ux-redesign.md, E7) and the Mission
// bar (B1): what is wrong with the mission, and how far along it is.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "authoring.hpp"
#include "commands.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>

namespace rwsman::ui {
namespace {

Token severity_token(const Problem::Severity severity) {
    return severity == Problem::Severity::error     ? Token::error
           : severity == Problem::Severity::warning ? Token::warn
                                                    : Token::text_dim;
}

const char* severity_icon(const Problem::Severity severity) {
    return severity == Problem::Severity::error     ? icons::LC_CIRCLE_ALERT
           : severity == Problem::Severity::warning ? icons::LC_TRIANGLE_ALERT
                                                    : icons::LC_INFO;
}

// Selects (or opens) what a problem is about.
void go_to_subject(AppState& state, const ProblemSubject& subject) {
    using Kind = ProblemSubject::Kind;
    switch (subject.kind) {
    case Kind::actor:
        return select_mission_record(state, {MissionRecordKey::Kind::actor, subject.id, 0}, true);
    case Kind::area:
        return select_mission_record(state, {MissionRecordKey::Kind::area, subject.id, 0}, true);
    case Kind::nav_group:
        return select_mission_record(state, {MissionRecordKey::Kind::nav_group, subject.id, 0}, true);
    case Kind::dummy:
        return select_mission_record(state, {MissionRecordKey::Kind::dummy, subject.id, 0}, true);
    case Kind::script:
        return open_flow_script(state, subject.text, subject.id);
    case Kind::placement:
        state.info("Placement " + subject.text + " is in the project's map; viewport editing of placements comes "
                   "in Phase 2");
        return;
    case Kind::none:
        return;
    }
}

// A filter toggle; it takes its colour only when on and something is counted.
bool chip(const char* label, bool& value, const Token token, const bool any = true) {
    const bool colored = value && any;
    ImGui::PushStyleColor(ImGuiCol_Button, colored ? color(token, 0.25F) : value ? color(Token::bg2) : transparent());
    ImGui::PushStyleColor(ImGuiCol_Text, colored ? color(token) : color(Token::text_dim));
    const bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor(2);
    if (clicked) value = !value;
    return clicked;
}

} // namespace

void draw_problems(AppState& state) {
    if (!state.mission.scene) {
        empty_state(icons::LC_CIRCLE_CHECK, "Open a mission or a project; its problems are listed here.");
        return;
    }
    static bool show_errors = true, show_warnings = true, show_notes = false, show_dismissed = false;
    const auto& problems = state.problems;
    const auto dismissed = [&](const Problem& problem) {
        return std::ranges::find(state.ui.dismissed_problems, problem.id) != state.ui.dismissed_problems.end();
    };
    const auto count = [&](const Problem::Severity severity) {
        return std::ranges::count_if(problems, [&](const Problem& p) { return p.severity == severity && !dismissed(p); });
    };
    const auto errors = count(Problem::Severity::error), warnings = count(Problem::Severity::warning),
               notes = count(Problem::Severity::note);
    chip((std::string(icons::LC_CIRCLE_ALERT) + " " + std::to_string(errors) + " errors").c_str(), show_errors,
         Token::error, errors > 0);
    ImGui::SameLine();
    chip((std::string(icons::LC_TRIANGLE_ALERT) + " " + std::to_string(warnings) + " warnings").c_str(), show_warnings,
         Token::warn, warnings > 0);
    ImGui::SameLine();
    chip((std::string(icons::LC_INFO) + " " + std::to_string(notes) + " notes").c_str(), show_notes, Token::text_dim);
    if (!state.ui.dismissed_problems.empty()) {
        ImGui::SameLine();
        chip(("dismissed (" + std::to_string(state.ui.dismissed_problems.size()) + ")").c_str(), show_dismissed,
             Token::text_dim);
    }
    help_marker("Everything the editor checks: the mission flow (events nobody raises, objectives nobody "
                "completes), heights against the terrain, the project file, zone shapes and unknown classes. "
                "Flow findings are evidence to check, not proof of what the game does.");

    std::size_t shown = 0;
    if (ImGui::BeginTable("##problems", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                               ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 40.0F * ui_scale());
        ImGui::TableSetupColumn("Problem", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 70.0F * ui_scale());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 118.0F * ui_scale());
        for (const auto& problem : problems) {
            const bool is_dismissed = dismissed(problem);
            if (is_dismissed && !show_dismissed) continue;
            if ((problem.severity == Problem::Severity::error && !show_errors) ||
                (problem.severity == Problem::Severity::warning && !show_warnings) ||
                (problem.severity == Problem::Severity::note && !show_notes))
                continue;
            ++shown;
            ImGui::PushID(problem.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, color(severity_token(problem.severity)));
            ImGui::TextUnformatted(severity_icon(problem.severity));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kind_color(problem.kind));
            ImGui::TextUnformatted(kind_icon(problem.kind));
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            const bool has_subject = problem.subject.kind != ProblemSubject::Kind::none;
            ImGui::PushStyleColor(ImGuiCol_Text, color(is_dismissed ? Token::text_dim : Token::text));
            if (ImGui::Selectable(problem.message.c_str(), false, ImGuiSelectableFlags_AllowOverlap) && has_subject)
                go_to_subject(state, problem.subject);
            ImGui::PopStyleColor();
            if (has_subject && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Click to go to it");
            ImGui::TableNextColumn();
            ImGui::PushFont(font(Font::caption));
            dim_text("%s", problem.source.c_str());
            ImGui::PopFont();
            ImGui::TableNextColumn();
            if (!problem.fix_command.empty()) {
                const auto* command = state.commands.find(problem.fix_command);
                ImGui::BeginDisabled(!command || (command->enabled && !command->enabled()));
                if (ImGui::SmallButton(problem.fix_label.c_str())) state.commands.run(problem.fix_command);
                ImGui::EndDisabled();
                ImGui::SameLine();
            }
            if (icon_button("##dismiss", is_dismissed ? icons::LC_UNDO_2 : icons::LC_X,
                            is_dismissed ? "Restore" : "Dismiss (for this session)")) {
                if (is_dismissed)
                    std::erase(state.ui.dismissed_problems, problem.id);
                else
                    state.ui.dismissed_problems.push_back(problem.id);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (shown == 0 && problems.empty()) empty_state(icons::LC_CIRCLE_CHECK, "No problems found.");
}

void draw_mission_bar(AppState& state) {
    const auto* viewport = ImGui::GetMainViewport();
    const float scale = ui_scale();
    const float height = ImGui::GetFrameHeight() + 8.0F * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0F * scale, 4.0F * scale});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color(Token::bg0));
    if (ImGui::BeginViewportSideBar("##mission_bar", const_cast<ImGuiViewport*>(viewport), ImGuiDir_Up, height,
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoTitleBar)) {
        const auto& scene = *state.mission.scene;
        // The project, and the slot its mission replaces.
        const std::string project = state.authoring.project && !state.authoring.project->name.empty()
                                        ? state.authoring.project->name
                                        : path_utf8(state.mission.graph->scene_path().stem());
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(font(Font::sans_bold));
        ImGui::TextUnformatted(project.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushFont(font(Font::caption));
        if (state.authoring.project || state.mission.project)
            dim_text("%s slot", path_utf8(state.mission.graph->scene_path().stem()).c_str());
        else
            dim_text("shipped mission, not saved as a project");
        ImGui::PopFont();

        // One step per stage of making a mission: a count and a status.
        const auto step = [&](const char* id, const char* icon, const std::string& label, const Token token,
                              const char* tooltip, const auto& action) {
            ImGui::SameLine(0.0F, 16.0F * scale);
            ImGui::PushStyleColor(ImGuiCol_Button, transparent());
            ImGui::PushStyleColor(ImGuiCol_Text, color(token));
            const bool clicked = ImGui::Button((std::string(icon) + " " + label + "##" + id).c_str());
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
            if (clicked) action();
        };
        const std::size_t cast = scene.actors().size();
        const auto* flow = mission_flow(state);
        const std::size_t objectives = flow ? flow->objectives().size() : 0;
        const bool success = flow && std::ranges::none_of(flow->findings(), [](const csf::FlowFinding& finding) {
                                         return finding.message.find("success") != std::string::npos;
                                     });
        const bool building = authoring_pending(state);
        if (state.authoring.project) {
            const auto findings = state.authoring.findings.size();
            step("map", icons::LC_MOUNTAIN, building ? "Map (building)" : findings ? "Map " + std::to_string(findings) : "Map",
                 building ? Token::inferred : findings ? Token::warn : Token::ok,
                 "The map built from the Blender terrain and the placed buildings and props. A number counts what "
                 "no longer stands on the ground (Height report).",
                 [&] { state.commands.run("mission.project_heights"); });
        }
        step("actors", icons::LC_USERS, "Actors " + std::to_string(cast), cast ? Token::text : Token::text_dim,
             "Characters, props and pickups (the Outliner lists them by kind).", [&] { show_panel(state, Panel::outliner); });
        step("zones", icons::LC_SQUARE_DASHED, "Zones " + std::to_string(scene.areas().size()),
             scene.areas().empty() ? Token::text_dim : Token::text, "Areas that scripts and objectives test.",
             [&] { show_panel(state, Panel::outliner); });
        step("objectives", icons::LC_FLAG, "Objectives " + std::to_string(objectives),
             objectives == 0 ? Token::warn : success ? Token::ok : Token::warn,
             objectives == 0 ? "No objectives yet: the mission cannot be won."
             : success       ? "Objectives, and the check that ends the mission in success."
                             : "Objectives are set up, but nothing ends the mission in success (see Problems).",
             [&] { show_panel(state, Panel::objectives); });
        step("intro", icons::LC_VIDEO, state.mission.cutscenes.empty() ? "Intro" : "Intro ✓",
             state.mission.cutscenes.empty() ? Token::text_dim : Token::ok,
             "The travelling-camera intro cutscene.", [&] { show_panel(state, Panel::timeline); });
        const auto errors = count_problems(state.problems, Problem::Severity::error);
        const auto warnings = count_problems(state.problems, Problem::Severity::warning);
        step("problems", errors ? icons::LC_CIRCLE_ALERT : warnings ? icons::LC_TRIANGLE_ALERT : icons::LC_CIRCLE_CHECK,
             errors + warnings ? std::to_string(errors + warnings) + " problems" : "No problems",
             errors ? Token::error : warnings ? Token::warn : Token::ok, "Everything the editor checks.",
             [&] { show_panel(state, Panel::problems); });

        // Right: save state and the build.
        const bool dirty = edits_unsaved(state);
        const char* save_label = dirty ? "Save" : "Saved";
        const float right = ImGui::GetWindowContentRegionMax().x;
        const float build_width = ImGui::CalcTextSize("Build...").x + ImGui::GetStyle().FramePadding.x * 2.0F +
                                  ImGui::CalcTextSize(icons::LC_HAMMER).x + 8.0F * scale;
        const float save_width = ImGui::CalcTextSize(save_label).x + ImGui::GetStyle().FramePadding.x * 2.0F +
                                 ImGui::CalcTextSize(icons::LC_SAVE).x + 8.0F * scale;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0F * scale, right - build_width - save_width - 8.0F * scale));
        ImGui::BeginDisabled(!dirty);
        if ((dirty ? primary_button((std::string(icons::LC_SAVE) + " " + save_label).c_str())
                   : secondary_button((std::string(icons::LC_SAVE) + " " + save_label).c_str())))
            state.commands.run("file.save");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (secondary_button((std::string(icons::LC_HAMMER) + " Build...").c_str())) show_panel(state, Panel::build);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace rwsman::ui
