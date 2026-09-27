// The authoring project's lifecycle (docs/archive/editor/editor-ux-redesign.md, E8,
// E9, E15): the New project wizard, and the Build panel's archives, test
// install (deploy and roll back) and playtest log.
#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "authoring.hpp"
#include "file_dialogs.hpp"
#include "mission_editing.hpp"
#include "project_actions.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace rwsman::ui {
namespace {

std::string text_of(const std::span<const char> buffer) { return {buffer.data(), strnlen(buffer.data(), buffer.size())}; }

std::filesystem::path utf8_path(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

void copy_text(std::span<char> buffer, const std::string& text) {
    std::fill(buffer.begin(), buffer.end(), '\0');
    text.copy(buffer.data(), std::min(text.size(), buffer.size() - 1));
}

// A path with a Choose... button beside it.
void path_row(AppState& state, const char* id, const std::filesystem::path& value, const char* missing,
              const DialogKind dialog, const std::filesystem::path& start = {}) {
    ImGui::PushID(id);
    if (value.empty()) token_text(Token::warn, "%s", missing);
    else dim_text("%s", state.ui.shown(path_utf8(value)).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Choose...")) request_file_dialog(state, dialog, start);
    ImGui::PopID();
}

const csf::ProjectPlaytest* last_playtest(const csf::AuthoringProject& project, const std::string& build) {
    for (auto it = project.playtests.rbegin(); it != project.playtests.rend(); ++it)
        if (it->build == build) return &*it;
    return nullptr;
}

void archives_card(AppState& state, csf::AuthoringProject& project) {
    CardOptions options{icons::LC_PACKAGE};
    options.help = "Both archives the game loads: the mission (maps/<Mission>.pak) rebuilt from the untouched one, "
                   "and GlobalEK.pak with the project's texts. Each build goes to its own dist/<build> folder.";
    if (!begin_card("##archives", "Archives", options)) return;
    std::vector<std::filesystem::path> archives{project.slot.archive};
    if (project.texts && !project.strings.empty()) archives.push_back(project.texts->archive);
    if (begin_properties("##originals")) {
        for (const auto& archive : archives) {
            property_row(("Untouched " + path_utf8(archive.filename())).c_str(),
                         "The shipped archive each build starts from: found in the test install's first "
                         "deployment backup, or chosen here.");
            const auto found = original_archive(state, archive);
            ImGui::PushID(path_utf8(archive).c_str());
            if (found) dim_text("%s", state.ui.shown(path_utf8(*found)).c_str());
            else token_text(Token::warn, "not found");
            ImGui::SameLine();
            if (ImGui::SmallButton("Choose...")) {
                state.ui.original_archive_for = archive;
                request_file_dialog(state, DialogKind::original_archive);
            }
            ImGui::PopID();
        }
        end_properties();
    }
    const bool busy = authoring_pending(state);
    ImGui::BeginDisabled(busy);
    if (primary_button((std::string(icons::LC_PACKAGE) + " Build archives").c_str())) build_project_archives(state);
    ImGui::EndDisabled();
    if (edits_unsaved(state)) {
        ImGui::SameLine();
        dim_text("saves first");
    }
    const auto builds = csf::archive_builds(project);
    if (state.ui.selected_build.empty() && !builds.empty()) state.ui.selected_build = builds.front();
    if (builds.empty()) dim_text("No builds yet.");
    for (std::size_t i = 0; i < builds.size() && i < 8; ++i) {
        const auto& build = builds[i];
        const auto* playtest = last_playtest(project, build);
        ImGui::PushID(build.c_str());
        const auto label = build + (i == 0 ? "  (latest)" : "");
        if (ImGui::Selectable(label.c_str(), state.ui.selected_build == build, ImGuiSelectableFlags_AllowOverlap))
            state.ui.selected_build = build;
        if (playtest) {
            ImGui::SameLine();
            token_text(playtest->worked ? Token::ok : Token::error, "%s %s",
                       playtest->worked ? icons::LC_CIRCLE_CHECK : icons::LC_CIRCLE_ALERT,
                       playtest->worked ? "worked" : "failed");
        }
        ImGui::PopID();
    }
    if (!builds.empty() && secondary_button((std::string(icons::LC_FOLDER_OPEN) + " Open build folder").c_str()))
        open_folder(project.directory / "dist" / state.ui.selected_build);
    end_card();
}

void install_card(AppState& state, csf::AuthoringProject& project) {
    CardOptions options{icons::LC_ROCKET};
    options.help = "A copy of the game to try builds in. Deploying replaces the slot's archive (and GlobalEK.pak) "
                   "there and keeps what it replaced; Roll back puts it back.";
    if (!begin_card("##install", "Test install", options)) return;
    path_row(state, "test_install", project.local.test_install, "not set", DialogKind::test_install,
             state.settings.game_root);
    const auto& build = state.ui.selected_build;
    ImGui::BeginDisabled(build.empty() || project.local.test_install.empty());
    if (primary_button((std::string(icons::LC_ROCKET) + " Deploy " + (build.empty() ? "" : build)).c_str()))
        state.ui.confirm_deploy = build;
    ImGui::EndDisabled();
    if (!project.local.deployments.empty()) {
        ImGui::Spacing();
        dim_text("Deployments, newest first");
    }
    for (auto it = project.local.deployments.rbegin(); it != project.local.deployments.rend(); ++it) {
        const auto deployment = *it;  // rolling back may reorder nothing, but copy to be safe
        const auto current = cached_deployment_state(state, deployment);
        ImGui::PushID(path_utf8(deployment.manifest).c_str());
        ImGui::TextUnformatted((deployment.build + "  " + path_utf8(deployment.archive)).c_str());
        ImGui::SameLine();
        token_text(current == csf::DeploymentState::active ? Token::ok : Token::text_dim, "%s",
                   csf::deployment_state_name(current));
        if (current == csf::DeploymentState::active) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Roll back")) roll_back_project_build(state, deployment.build);
        }
        ImGui::PopID();
    }
    end_card();
}

void playtest_card(AppState& state, csf::AuthoringProject& project) {
    CardOptions options{icons::LC_LIST_CHECKS};
    options.help = "What happened when you played a build: kept in the project with the build it was.";
    if (!begin_card("##playtests", "Playtest log", options)) return;
    for (auto it = project.playtests.rbegin(); it != project.playtests.rend(); ++it) {
        token_text(it->worked ? Token::ok : Token::error, "%s", it->worked ? icons::LC_CIRCLE_CHECK : icons::LC_CIRCLE_ALERT);
        ImGui::SameLine();
        ImGui::TextUnformatted(it->build.c_str());
        if (!it->note.empty()) {
            ImGui::SameLine();
            dim_text("%s", it->note.c_str());
        }
    }
    const auto& build = state.ui.selected_build;
    ImGui::BeginDisabled(build.empty());
    // Left-aligned: toasts cover the panel's right edge.
    ImGui::Checkbox("Worked", &state.ui.playtest_worked);
    ImGui::SameLine();
    if (secondary_button(("Record for " + build + "##record").c_str())) {
        add_playtest(state, build, state.ui.playtest_worked, text_of(state.ui.playtest_note));
        copy_text(state.ui.playtest_note, "");
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##playtest_note", "what you saw", state.ui.playtest_note.data(), state.ui.playtest_note.size());
    ImGui::EndDisabled();
    end_card();
}

} // namespace

void draw_project_pipeline(AppState& state) {
    auto* project = state.authoring.project.get();
    if (!project) return;
    archives_card(state, *project);
    install_card(state, *project);
    playtest_card(state, *project);
}

void draw_project_dialogs(AppState& state) {
    auto& ui = state.ui;
    if (!ui.confirm_deploy.empty() && state.authoring.project) {
        ImGui::OpenPopup("Deploy to the test install");
        if (ImGui::BeginPopupModal("Deploy to the test install", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const auto& project = *state.authoring.project;
            std::string archives = path_utf8(project.slot.archive);
            if (project.texts && std::filesystem::exists(project.directory / "dist" / ui.confirm_deploy /
                                                         project.texts->archive.filename()))
                archives += " and " + path_utf8(project.texts->archive);
            ImGui::Text("Build %s replaces %s", ui.confirm_deploy.c_str(), archives.c_str());
            ImGui::Text("in %s.", ui.shown(path_utf8(project.local.test_install)).c_str());
            dim_text("What it replaces is backed up there; Roll back restores it.");
            ImGui::Spacing();
            if (primary_button("Deploy")) {
                const auto build = std::exchange(ui.confirm_deploy, {});
                ImGui::CloseCurrentPopup();
                deploy_project_build(state, build);
            }
            ImGui::SameLine();
            if (secondary_button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                ui.confirm_deploy.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    if (!ui.show_new_project) return;
    ImGui::OpenPopup("New project");
    ImGui::SetNextWindowSize({560.0F * ui_scale(), 0.0F}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New project", &ui.show_new_project)) return;
    if (ImGui::IsWindowAppearing()) {
        if (!ui.new_project_corpus[0]) copy_text(ui.new_project_corpus, path_utf8(state.settings.resource_root));
        if (!ui.new_project_name[0]) copy_text(ui.new_project_name, "Checkpoint");
    }
    const auto corpus = utf8_path(text_of(ui.new_project_corpus));
    const auto& slots = project_slots(state, corpus);
    if (ui.new_project_slot.empty() && !slots.empty()) {
        const auto convoy = std::ranges::find(slots, std::string("Convoy"), &csf::MissionSlot::mission);
        ui.new_project_slot = (convoy != slots.end() ? *convoy : slots.front()).mission;
    }
    const auto name = text_of(ui.new_project_name);
    const auto default_folder = projects_folder(state) / utf8_path(name);
    dim_text("A mission of your own in the slot of a shipped one: the game loads it in that mission's place.");
    if (begin_properties("##new_project")) {
        property_row("Name");
        ImGui::InputText("##new_project_name", ui.new_project_name.data(), ui.new_project_name.size());
        property_row("Game files", "The unpacked game: one folder per mission (the resource root).");
        ImGui::InputText("##new_project_corpus", ui.new_project_corpus.data(), ui.new_project_corpus.size());
        property_row("Slot", "The shipped mission it replaces: its map is the start, its environment and "
                             "databases stay.");
        if (ImGui::BeginCombo("##new_project_slot", ui.new_project_slot.empty() ? "(none found)" : ui.new_project_slot.c_str())) {
            for (const auto& slot : slots)
                if (ImGui::Selectable((slot.mission + "  " + path_utf8(slot.visual_map.parent_path().filename())).c_str(),
                                      slot.mission == ui.new_project_slot))
                    ui.new_project_slot = slot.mission;
            ImGui::EndCombo();
        } else {
            name_last_item("##new_project_slot");
        }
        property_row("Terrain", "A flat square of the slot's ground to start from; shape it in Blender.");
        ImGui::Checkbox("##new_project_terrain", &ui.new_project_terrain);
        if (ui.new_project_terrain) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(160.0F * ui_scale());
            ImGui::DragFloat("##new_project_size", &ui.new_project_size, 100.0F, 1000.0F, 40000.0F, "%.0f cm a side");
        }
        property_row("Folder", "Where the project is created; it must be new or empty.");
        ImGui::InputTextWithHint("##new_project_folder", path_utf8(default_folder).c_str(), ui.new_project_folder.data(),
                                 ui.new_project_folder.size());
        end_properties();
    }
    const bool ready = !name.empty() && !ui.new_project_slot.empty();
    ImGui::BeginDisabled(!ready);
    if (primary_button("Create")) {
        const auto typed = text_of(ui.new_project_folder);
        const auto folder = typed.empty() ? default_folder : utf8_path(typed);
        if (create_project(state, folder, name, ui.new_project_slot, corpus,
                           ui.new_project_terrain ? ui.new_project_size : 0.0F)) {
            ui.show_new_project = false;
            copy_text(ui.new_project_folder, "");
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (secondary_button("Cancel")) {
        ui.show_new_project = false;
        ImGui::CloseCurrentPopup();
    }
    if (slots.empty()) {
        ImGui::SameLine();
        token_text(Token::warn, "No mission slots in the game files");
    }
    ImGui::EndPopup();
}

} // namespace rwsman::ui
