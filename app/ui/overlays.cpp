#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "mission_loader.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>

namespace rwsman::ui {

void draw_toasts(AppState& state) {
    auto& toasts = state.toasts;
    if (toasts.empty()) return;
    const auto* viewport = ImGui::GetMainViewport();
    const float width = 380.0F * ui_scale();
    float y = viewport->WorkPos.y + viewport->WorkSize.y - 12.0F;
    const double dt = static_cast<double>(ImGui::GetIO().DeltaTime);
    for (std::size_t index = toasts.size(); index-- > 0;) {
        auto& toast = toasts[index];
        // Toasts stay while hovered, and errors stay longer.
        const double lifetime = toast.level == LogLevel::error ? 9.0 : 5.0;
        const float fade = static_cast<float>(std::clamp((lifetime - toast.age) / 0.6, 0.0, 1.0));
        ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x - 12.0F, y}, ImGuiCond_Always, {1.0F, 1.0F});
        ImGui::SetNextWindowSize({width, 0.0F});
        ImGui::SetNextWindowBgAlpha(0.94F * fade);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(fade, 0.05F));
        ImGui::PushStyleColor(ImGuiCol_Border, color(toast.level == LogLevel::error  ? Token::error
                                                    : toast.level == LogLevel::warn ? Token::warn
                                                    : toast.level == LogLevel::ok   ? Token::ok
                                                                                     : Token::line));
        const std::string name = "##toast" + std::to_string(reinterpret_cast<std::uintptr_t>(&toast) ^ index);
        bool hovered = false;
        if (ImGui::Begin(("toast_" + std::to_string(index)).c_str(), nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoNav)) {
            ImGui::PushStyleColor(ImGuiCol_Text, color(toast.level == LogLevel::error  ? Token::error
                                                       : toast.level == LogLevel::warn ? Token::warn
                                                       : toast.level == LogLevel::ok   ? Token::ok
                                                                                        : Token::text_dim));
            ImGui::TextUnformatted(toast.level == LogLevel::error  ? icons::LC_CIRCLE_ALERT
                                   : toast.level == LogLevel::warn ? icons::LC_TRIANGLE_ALERT
                                   : toast.level == LogLevel::ok   ? icons::LC_CIRCLE_CHECK
                                                                    : icons::LC_INFO);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushTextWrapPos(width - 24.0F);
            ImGui::TextUnformatted(toast.message.c_str());
            ImGui::PopTextWrapPos();
            if (!toast.folder.empty()) {
                if (ImGui::SmallButton((std::string(icons::LC_FOLDER_OPEN) + " Open folder").c_str())) {
                    open_folder(toast.folder);
                    toast.age = lifetime;
                }
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Dismiss")) toast.age = lifetime;
            hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);
            y -= ImGui::GetWindowHeight() + 6.0F;
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        (void)name;
        if (!hovered) toast.age += dt;
    }
    std::erase_if(toasts, [](const Toast& toast) {
        return toast.age >= (toast.level == LogLevel::error ? 9.0 : 5.0);
    });
}

void draw_load_overlay(AppState& state) {
    if (!state.loader) return;
    const auto progress = state.loader->progress();
    if (!progress.active) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
                             viewport->WorkPos.y + viewport->WorkSize.y * 0.4F},
                            ImGuiCond_Always, {0.5F, 0.5F});
    ImGui::SetNextWindowSize({460.0F * ui_scale(), 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14.0F, 12.0F});
    ImGui::PushStyleColor(ImGuiCol_Border, color(Token::accent, 0.6F));
    if (ImGui::Begin("##load_overlay", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
        section("Loading mission");
        ImGui::PushFont(font(Font::mono));
        ImGui::TextUnformatted(path_utf8(progress.input.filename()).c_str());
        ImGui::PopFont();
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%d/%d  %s", std::max(progress.stage_index, 1),
                      progress.stage_count, progress.stage.c_str());
        ImGui::ProgressBar(std::clamp(progress.fraction, 0.0F, 1.0F), {-1.0F, 0.0F}, overlay);
        dim_text("The window stays responsive. The current mission is kept until the new one finishes.");
        ImGui::BeginDisabled(progress.cancel_requested);
        if (ImGui::Button(progress.cancel_requested ? "Cancelling..." : "Cancel")) cancel_mission_load(state);
        ImGui::EndDisabled();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

} // namespace rwsman::ui
