#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <fstream>

namespace rwsman::ui {
namespace {

Token token_for(const LogLevel level) {
    switch (level) {
    case LogLevel::info:
        return Token::text;
    case LogLevel::ok:
        return Token::ok;
    case LogLevel::warn:
        return Token::warn;
    case LogLevel::error:
        return Token::error;
    }
    return Token::text;
}

} // namespace

void draw_console(AppState& state) {
    static bool show_info = true, show_ok = true, show_warn = true, show_error = true;
    static bool follow = true;
    static std::uint64_t seen_revision = 0;

    if (icon_button("copy_all", icons::LC_COPY, "Copy all lines")) {
        ImGui::SetClipboardText(state.log.to_text().c_str());
        state.info("Copied console text to the clipboard");
    }
    ImGui::SameLine();
    if (icon_button("save_log", icons::LC_SAVE, "Save log to a new file next to the settings")) {
        // New file every time; never overwrites an earlier log.
        auto directory = state.config_dir.empty() ? std::filesystem::current_path() : state.config_dir;
        const auto stamp = format_log_time(std::chrono::system_clock::now());
        std::string name = "console-" + stamp + ".log";
        for (auto& c : name)
            if (c == ':') c = '-';
        auto path = directory / name;
        for (int suffix = 1; std::filesystem::exists(path); ++suffix)
            path = directory / (name.substr(0, name.size() - 4) + "-" + std::to_string(suffix) + ".log");
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        std::ofstream output(path);
        output << state.log.to_text();
        if (output)
            state.ok("Saved console log to " + path_utf8(path));
        else
            state.error("Could not save console log to " + path_utf8(path));
    }
    ImGui::SameLine();
    if (icon_button("clear_log", icons::LC_TRASH, "Clear")) state.log.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    const auto toggle = [&](const char* label, bool& flag, const Token token, const LogLevel level) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color(flag ? token : Token::text_dim));
        const std::string text = std::string(label) + " " + std::to_string(state.log.count(level));
        if (ImGui::SmallButton(text.c_str())) flag = !flag;
        ImGui::PopStyleColor();
    };
    toggle("info", show_info, Token::text, LogLevel::info);
    toggle("ok", show_ok, Token::ok, LogLevel::ok);
    toggle("warn", show_warn, Token::warn, LogLevel::warn);
    toggle("error", show_error, Token::error, LogLevel::error);
    ImGui::SameLine();
    ImGui::Checkbox("follow", &follow);

    ImGui::Separator();
    ImGui::BeginChild("##console_lines", {0.0F, 0.0F}, ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(font(Font::mono));
    const auto entries = state.log.snapshot();
    for (const auto& entry : entries) {
        const bool visible = (entry.level == LogLevel::info && show_info) ||
                             (entry.level == LogLevel::ok && show_ok) ||
                             (entry.level == LogLevel::warn && show_warn) ||
                             (entry.level == LogLevel::error && show_error);
        if (!visible) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::TextUnformatted(state.ui.deterministic ? "--:--:--" : format_log_time(entry.time).c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color(token_for(entry.level)));
        std::string level = log_level_name(entry.level);
        level.resize(5, ' ');
        ImGui::TextUnformatted(level.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(state.ui.shown(entry.message).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::SetClipboardText(entry.message.c_str());
    }
    ImGui::PopFont();
    const auto revision = state.log.revision();
    if (follow && revision != seen_revision) ImGui::SetScrollHereY(1.0F);
    seen_revision = revision;
    ImGui::EndChild();
}

} // namespace rwsman::ui
