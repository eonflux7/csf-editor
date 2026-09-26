#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "file_dialogs.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <span>
#include <string>

namespace rwsman::ui {

void draw_preferences(AppState& state) {
    if (!state.ui.show_preferences) return;
    auto& settings = state.settings;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, {0.5F, 0.5F});
    ImGui::SetNextWindowSize({560.0F * ui_scale(), std::min(700.0F * ui_scale(), viewport->WorkSize.y * 0.9F)},
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preferences", &state.ui.show_preferences)) {
        ImGui::End();
        return;
    }
    const auto changed = [&] { state.settings_dirty = true; };

    section("Resource root");
    static std::array<char, 512> root_buffer{};
    static bool root_initialized = false;
    if (!root_initialized || ImGui::IsWindowAppearing()) {
        const auto text = path_utf8(settings.resource_root);
        std::strncpy(root_buffer.data(), text.c_str(), root_buffer.size() - 1);
        root_initialized = true;
    }
    ImGui::SetNextItemWidth(-90.0F * ui_scale());
    if (ImGui::InputTextWithHint("##root", "unpacked game folder or a package with Maps/", root_buffer.data(), root_buffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue) ||
        ImGui::IsItemDeactivatedAfterEdit()) {
        settings.resource_root = std::filesystem::path(std::u8string(root_buffer.begin(), root_buffer.begin() + static_cast<std::ptrdiff_t>(std::strlen(root_buffer.data()))));
        rescan_resource_root(state);
        changed();
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) request_file_dialog(state, DialogKind::resource_root);
    if (ImGui::IsWindowAppearing() || (file_dialog_open(state) == false && path_utf8(settings.resource_root) != root_buffer.data() && !ImGui::IsItemActive() && !ImGui::IsAnyItemActive())) {
        const auto text = path_utf8(settings.resource_root);
        std::strncpy(root_buffer.data(), text.c_str(), root_buffer.size() - 1);
    }
    dim_text("%zu missions found. The Missions tab and the start page list them.", state.discovered.size());

    section("Mission editing");
    // Paths edited as text keep a buffer while the field is active.
    const auto path_setting = [&](const char* id, const char* hint, std::filesystem::path& value,
                                  const DialogKind dialog) {
        static std::map<std::string, std::array<char, 512>> buffers;
        auto& buffer = buffers[id];
        const auto text = path_utf8(value);
        const auto input_id = std::string("##") + id;
        const bool editing = ImGui::GetActiveID() == ImGui::GetID(input_id.c_str());
        if (!editing) std::snprintf(buffer.data(), buffer.size(), "%s", text.c_str());
        ImGui::SetNextItemWidth(-90.0F * ui_scale());
        ImGui::InputTextWithHint(input_id.c_str(), hint, buffer.data(), buffer.size());
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            value = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(buffer.data()),
                                                        std::strlen(buffer.data())));
            changed();
        }
        ImGui::SameLine();
        if (ImGui::Button((std::string("Browse...##") + id).c_str())) request_file_dialog(state, dialog, value);
    };
    ImGui::TextUnformatted("Game installation");
    path_setting("game_root", "folder with maps/<Mission>.pak", settings.game_root, DialogKind::game_root);
    ImGui::TextUnformatted("Mission projects");
    path_setting("projects_root", path_utf8(state.config_dir / "projects").c_str(), settings.projects_root,
                 DialogKind::projects_root);
    ImGui::TextUnformatted("Blender");
    path_setting("blender", "blender (from PATH)", settings.blender, DialogKind::blender);
    dim_text("The export dialog finds the shipped archive in the game folder. New projects are "
             "created in the projects folder (empty: the per-user config folder). Edit in Blender runs "
             "Blender with the CSF add-on and the project set.");

    section("Appearance");
    // The slider edits a copy so the fonts are rebuilt once, on release. It follows
    // the setting otherwise (the zoom shortcuts change it too).
    static float pending_scale = 100.0F;
    const ImGuiID scale_slider = ImGui::GetID("UI scale");
    if (ImGui::GetActiveID() != scale_slider) pending_scale = settings.ui_scale * 100.0F;
    ImGui::SetNextItemWidth(220.0F * ui_scale());
    ImGui::SliderFloat("UI scale", &pending_scale, 80.0F, 200.0F, "%.0f%%");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        settings.ui_scale = pending_scale / 100.0F;
        settings.clamp();
        changed();
    }
    dim_text("Applied when you release the slider; Ctrl+= / Ctrl+- / Ctrl+0 zoom from anywhere. "
             "The OS display scale is applied on top of this.");
    const char* themes[] = {"dark", "high-contrast"};
    int theme_index = theme_name() == "high-contrast" ? 1 : 0;
    ImGui::SetNextItemWidth(220.0F * ui_scale());
    if (ImGui::Combo("Theme", &theme_index, themes, 2)) {
        set_theme(themes[theme_index]);
        apply_theme(ui_scale());
        settings.theme = themes[theme_index];
        changed();
    }
    if (ImGui::Checkbox("Color chunk tree by clump size", &settings.show_clump_colors)) changed();

    section("Viewport defaults");
    bool viewport_changed = false;
    ImGui::SetNextItemWidth(220.0F * ui_scale());
    viewport_changed |= ImGui::SliderFloat("Move speed", &settings.move_speed, 0.05F, 4.0F, "%.2fx", ImGuiSliderFlags_Logarithmic);
    viewport_changed |= ImGui::Checkbox("Invert Y (look and orbit)", &settings.invert_y);
    viewport_changed |= ImGui::Checkbox("Show stats HUD", &settings.show_hud);
    constexpr const char* styles[] = {"Textured", "Material index", "Material color", "Lightmap texture", "Base + lightmap", "Wireframe"};
    constexpr int style_values[] = {0, 1, 2, 5, 6, 7};
    int style_index = 0;
    for (int i = 0; i < 6; ++i)
        if (style_values[i] == settings.default_view_style) style_index = i;
    ImGui::SetNextItemWidth(220.0F * ui_scale());
    if (ImGui::Combo("Default shading", &style_index, styles, 6)) {
        settings.default_view_style = style_values[style_index];
        viewport_changed = true;
    }
    if (viewport_changed) {
        settings.clamp();
        apply_viewport_settings(state);
        changed();
    }

    section("Performance");
    // Choices, not free sliders: a cap is only useful at common refresh rates.
    const auto fps_combo = [&](const char* label, int& value, const std::span<const int> choices,
                               const char* none) {
        const auto name = [&](const int fps) { return fps == 0 ? std::string(none) : std::to_string(fps) + " fps"; };
        ImGui::SetNextItemWidth(220.0F * ui_scale());
        bool edited = false;
        if (ImGui::BeginCombo(label, name(value).c_str())) {
            for (const int choice : choices)
                if (ImGui::Selectable(name(choice).c_str(), choice == value)) {
                    value = choice;
                    edited = true;
                }
            ImGui::EndCombo();
        }
        return edited;
    };
    bool performance_changed = false;
    constexpr std::array<int, 8> foreground_choices{0, 30, 60, 90, 120, 144, 165, 240};
    constexpr std::array<int, 6> background_choices{0, 5, 10, 15, 30, 60};
    performance_changed |= fps_combo("Frame rate limit", settings.fps_limit, foreground_choices, "Display refresh");
    performance_changed |= fps_combo("When unfocused", settings.background_fps_limit, background_choices, "Same as focused");
    performance_changed |= ImGui::Checkbox("Pause redraws when idle", &settings.idle_redraw);
    dim_text("Stops drawing while nothing moves; input, camera motion, and playing animations "
             "resume it at once.");
    performance_changed |= ImGui::Checkbox("Frame timings in the stats HUD", &settings.show_frame_stats);
    if (performance_changed) {
        settings.clamp();
        apply_viewport_settings(state);
        changed();
    }

    section("Exports");
    int policy = settings.export_policy == ExportPolicy::new_files_only ? 0 : 1;
    if (ImGui::RadioButton("New files only (default)", &policy, 0)) {
        settings.export_policy = ExportPolicy::new_files_only;
        changed();
    }
    dim_text("An export never replaces an existing file: it writes a numbered new file instead.");
    if (ImGui::RadioButton("Confirm before overwriting", &policy, 1)) {
        settings.export_policy = ExportPolicy::confirm_overwrite;
        changed();
    }
    dim_text("Writes to the usual name and asks first when it already exists.");

    section("Settings file");
    ImGui::PushFont(font(Font::mono));
    ImGui::TextUnformatted(path_utf8(state.config_dir / "settings.ini").c_str());
    ImGui::PopFont();
    if (ImGui::Button("Reset all settings")) {
        const auto root = settings.resource_root;
        settings = Settings{};
        settings.resource_root = root;
        set_theme(settings.theme);
        apply_theme(ui_scale());
        apply_viewport_settings(state);
        changed();
        state.info("Settings reset to defaults (resource root kept)");
    }
    ImGui::End();
}

void draw_overwrite_dialog(AppState& state) {
    if (!state.pending_overwrite) return;
    ImGui::OpenPopup("Replace existing file?");
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({viewport->GetCenter().x, viewport->GetCenter().y}, ImGuiCond_Always, {0.5F, 0.5F});
    if (ImGui::BeginPopupModal("Replace existing file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushFont(font(Font::mono));
        ImGui::TextUnformatted(path_utf8(state.pending_overwrite->path).c_str());
        ImGui::PopFont();
        dim_text("already exists. The export policy asks before replacing it.");
        ImGui::Spacing();
        if (ImGui::Button("Replace")) {
            auto action = std::move(state.pending_overwrite->overwrite);
            state.pending_overwrite.reset();
            ImGui::CloseCurrentPopup();
            if (action) action();
        } else {
            ImGui::SameLine();
            if (ImGui::Button("Write a new file instead") && state.pending_overwrite) {
                auto action = std::move(state.pending_overwrite->write_unique);
                state.pending_overwrite.reset();
                ImGui::CloseCurrentPopup();
                if (action) action();
            } else {
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    state.pending_overwrite.reset();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace rwsman::ui
