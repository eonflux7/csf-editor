#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "authoring.hpp"
#include "navigation.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace rwsman::ui {
namespace {

// A clickable status segment. Returns true when clicked.
bool segment(const std::string& text, const Token token, const char* tooltip = nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, color(token));
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine({lo.x, hi.y}, {hi.x, hi.y}, color_u32(Token::accent, 0.6F));
        if (tooltip) ImGui::SetTooltip("%s", tooltip);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

void separator() {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::line));
    ImGui::TextUnformatted("|");
    ImGui::PopStyleColor();
    ImGui::SameLine();
}

} // namespace

void draw_status_bar(AppState& state) {
    auto* viewport = ImGui::GetMainViewport();
    const float height = ImGui::GetFrameHeight();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, color(Token::bg0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0F * ui_scale(), 2.0F * ui_scale()});
    if (ImGui::BeginViewportSideBar("##status_bar", viewport, ImGuiDir_Down, height,
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoDecoration)) {
        const auto& document = state.document;
        const bool mission_dirty = edits_unsaved(state);
        if (mode_of(state.workspace) != Mode::inspect && state.mission.graph) {
            // Mission and Script modes (B6): save state, map build, problems,
            // selection, the latest message.
            if (segment(std::string(icons::LC_CIRCLE_DOT) + (mission_dirty ? " Unsaved changes" : " Saved"),
                        mission_dirty ? Token::dirty : Token::ok,
                        mission_dirty ? "Ctrl+S saves the project" : "Everything is saved"))
                if (mission_dirty) state.commands.run("file.save");
            if (state.authoring.project) {
                separator();
                const auto findings = state.authoring.findings.size();
                const bool building = authoring_pending(state);
                if (segment(std::string(icons::LC_MOUNTAIN) +
                                (building ? " Building the map..."
                                 : findings ? " Map: " + std::to_string(findings) + " off the ground"
                                            : " Map built"),
                            building ? Token::inferred : findings ? Token::warn : Token::text_dim,
                            "The map from the Blender terrain and the placements; click for the height report"))
                    state.commands.run("mission.project_heights");
                separator();
                const auto& sent = state.authoring.last_send;
                if (segment(std::string(icons::LC_MOUNTAIN_SNOW) + (sent.empty() ? " Blender" : " Blender sent " + sent),
                            sent.empty() ? Token::text_dim : Token::ok,
                            "Edit in Blender; Send there (CSF tab) rebuilds the map here"))
                    state.commands.run("build.edit_in_blender");
            }
            separator();
            const auto errors = count_problems(state.problems, Problem::Severity::error);
            const auto warnings = count_problems(state.problems, Problem::Severity::warning);
            if (segment(std::string(errors ? icons::LC_CIRCLE_ALERT : warnings ? icons::LC_TRIANGLE_ALERT
                                                                               : icons::LC_CIRCLE_CHECK) +
                            " " + std::to_string(errors) + " / " + std::to_string(warnings),
                        errors ? Token::error : warnings ? Token::warn : Token::text_dim,
                        "Errors / warnings: open Problems"))
                state.commands.run("mission.problems");
            separator();
            if (state.selection.empty()) {
                segment("Nothing selected", Token::text_dim);
            } else if (segment(selection_kind_label(state, state.selection) + " " +
                                   selection_title(state, state.selection),
                               Token::text, "Show Properties")) {
                state.commands.run("view.panel.properties");
            }
        } else {
            // Inspect mode: the file, the workspace, the selection with its
            // offset, changed files, the resource root and diagnostics.
            ImGui::PushFont(font(Font::mono));
            std::string name = "no document";
            if (state.mission.graph)
                name = path_utf8(state.mission.graph->scene_path().filename());
            else if (document)
                name = path_utf8(document->source_path().filename());
            const bool dirty = (document && document->dirty()) || mission_dirty;
            if (segment(std::string(icons::LC_CIRCLE_DOT) + " " + name,
                        dirty ? Token::dirty : (document ? Token::ok : Token::text_dim),
                        mission_dirty ? "Unsaved mission edits (Ctrl+S saves the project)"
                        : dirty       ? "Unsaved byte edits"
                                      : "Toggle the left panels"))
                state.commands.run("view.toggle_explorer");
            separator();
            segment(workspace_name(state.workspace), Token::text_dim);
            separator();
            if (state.selection.empty()) {
                segment("sel -", Token::text_dim);
            } else {
                const auto offset = selection_offset(state, state.selection);
                char at[32]{};
                if (offset) std::snprintf(at, sizeof(at), " @0x%llX", static_cast<unsigned long long>(*offset));
                const auto text = "sel " + selection_kind_label(state, state.selection) + " " +
                                  selection_title(state, state.selection) + at;
                if (segment(text, Token::text, "Show the Inspector; Ctrl+Shift+C copies the identity")) {
                    state.settings.show_inspector = true;
                    state.settings_dirty = true;
                }
            }
            separator();
            std::size_t changed = document && document->dirty() ? 1 : 0;
            if (state.mission.editor) changed += state.mission.editor->modified_files().size();
            if (segment(std::to_string(changed) + (mission_dirty ? " changed *" : " changed"),
                        changed || mission_dirty ? Token::dirty : Token::text_dim,
                        "Files that differ from the source; open the Changes panel"))
                state.commands.run("view.panel.changes");
            separator();
            const auto root =
                state.settings.resource_root.empty() ? std::string("unset") : state.ui.shown(path_utf8(state.settings.resource_root));
            if (segment("root " + root, Token::text_dim, "Open Preferences to set the resource root"))
                state.ui.show_preferences = true;
            if (const auto warnings = state.diagnostic_problem_count(); warnings > 0) {
                separator();
                if (segment(std::string(icons::LC_TRIANGLE_ALERT) + " " + std::to_string(warnings), Token::warn,
                            "Open Diagnostics"))
                    state.commands.run("view.panel.diagnostics");
            }
            ImGui::PopFont();
        }
        // Latest log line fills the remaining width; frame rate sits at the right edge.
        char fps[24];
        // Between idle frames the rate means nothing; say the loop is waiting instead.
        if (state.ui.deterministic)
            std::snprintf(fps, sizeof(fps), "- fps");
        else if (state.frame_stats.idle)
            std::snprintf(fps, sizeof(fps), "idle");
        else
            std::snprintf(fps, sizeof(fps), "%.0f fps", state.frame_stats.fps);
        const float fps_width = ImGui::CalcTextSize(fps).x;
        const auto latest = state.log.latest();
        if (latest) {
            separator();
            const float available = ImGui::GetContentRegionAvail().x - fps_width - 24.0F;
            if (available > 40.0F) {
                std::string message = state.ui.shown(latest->message);
                const float per_char = ImGui::CalcTextSize("M").x;
                const auto limit = static_cast<std::size_t>(available / per_char);
                if (message.size() > limit && limit > 3) message = message.substr(0, limit - 3) + "...";
                const Token token = latest->level == LogLevel::error  ? Token::error
                                    : latest->level == LogLevel::warn ? Token::warn
                                    : latest->level == LogLevel::ok   ? Token::ok
                                                                      : Token::text_dim;
                if (segment(message, token, latest->message.c_str())) state.commands.run("view.panel.console");
            }
        }
        ImGui::SameLine(ImGui::GetWindowWidth() - fps_width - ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::TextUnformatted(fps);
        ImGui::PopStyleColor();
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace rwsman::ui
