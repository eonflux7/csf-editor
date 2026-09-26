#include "app_state.hpp"
#include "ui/ui.hpp"

#include "navigation.hpp"
#include "rwsman/fuzzy.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace rwsman::ui {
namespace {

struct Row {
    const Command* command{};
    const SearchEntry* entry{};
    int score{};
    std::vector<std::uint32_t> positions;
};

struct Cache {
    std::string query;
    UiState::PaletteMode mode{UiState::PaletteMode::closed};
    std::size_t commands{};
    std::uint64_t symbols_generation{}; // Rows point into the search index.
    std::vector<Row> rows;
} cache;

const char* icon_for(const SymbolKind kind) {
    switch (kind) {
    case SymbolKind::actor:
        return icons::LC_USER;
    case SymbolKind::navigation_group:
        return icons::LC_ROUTE;
    case SymbolKind::navigation_point:
        return icons::LC_MAP_PIN;
    case SymbolKind::dummy:
        return icons::LC_TARGET;
    case SymbolKind::area:
        return icons::LC_SQUARE;
    case SymbolKind::light:
        return icons::LC_LIGHTBULB;
    case SymbolKind::effect:
        return icons::LC_SPARKLES;
    case SymbolKind::scene_object:
    case SymbolKind::animation:
        return icons::LC_FILM;
    case SymbolKind::script:
        return icons::LC_FILE_CODE;
    case SymbolKind::variable:
        return icons::LC_BRACES;
    case SymbolKind::class_record:
        return icons::LC_DATABASE;
    case SymbolKind::chunk:
        return icons::LC_BOX;
    case SymbolKind::instance:
        return icons::LC_CUBOID;
    case SymbolKind::resource:
        return icons::LC_FILE;
    case SymbolKind::count:
        break;
    }
    return icons::LC_DOT;
}

void compute(AppState& state, const std::string& query, const UiState::PaletteMode mode) {
    cache.rows.clear();
    const bool symbol_query = !query.empty() && (query.front() == '#' || query.starts_with("0x") ||
                                                 query.starts_with("0X") ||
                                                 std::isdigit(static_cast<unsigned char>(query.front())));
    if (mode == UiState::PaletteMode::commands && !symbol_query) {
        for (const auto& command : state.commands.commands()) {
            if (!command.in_palette) continue;
            const auto by_label = fuzzy_match(query, command.label);
            const auto by_keywords =
                command.keywords.empty() || by_label ? std::nullopt
                                                     : fuzzy_match(query, command.keywords);
            if (!by_label && !by_keywords) continue;
            Row row;
            row.command = &command;
            row.score = by_label ? by_label->score * 2 + 20 : by_keywords->score;
            if (by_label) row.positions = by_label->positions;
            // Commands that cannot run right now sort below the ones that can.
            if (command.enabled && !command.enabled()) row.score -= 200;
            cache.rows.push_back(std::move(row));
        }
        if (query.empty()) {
            // No query: keep registration order, grouped by category.
            std::ranges::stable_sort(cache.rows, [](const Row& a, const Row& b) {
                return (a.score >= -100) > (b.score >= -100);
            });
        }
    }
    if (!query.empty() || mode == UiState::PaletteMode::go_to) {
        // Mission mode looks for gameplay records first; Inspect keeps file order.
        const bool gameplay_first = mode_of(state.workspace) != Mode::inspect;
        for (auto& result : state.search_index.query(query, 60, gameplay_first)) {
            Row row;
            row.entry = result.entry;
            row.score = result.score;
            row.positions = std::move(result.label_positions);
            cache.rows.push_back(std::move(row));
        }
    }
    if (!query.empty())
        std::ranges::stable_sort(cache.rows, [](const Row& a, const Row& b) { return a.score > b.score; });
    if (cache.rows.size() > 80) cache.rows.resize(80);
}

void close(AppState& state) {
    state.ui.palette = UiState::PaletteMode::closed;
    state.ui.palette_query.fill('\0');
    state.ui.palette_cursor = 0;
    cache = {};
}

void activate(AppState& state, const Row& row) {
    if (row.command) {
        const std::string id = row.command->id;
        close(state);
        state.commands.run(id);
    } else if (row.entry) {
        const auto target = row.entry->target;
        close(state);
        navigate_to(state, target);
    }
}

} // namespace

void draw_palette(AppState& state) {
    if (state.ui.palette == UiState::PaletteMode::closed) return;
    const auto mode = state.ui.palette;
    const auto* viewport = ImGui::GetMainViewport();
    const float width = std::min(viewport->WorkSize.x - 32.0F, 620.0F * ui_scale());
    ImGui::SetNextWindowPos({viewport->WorkPos.x + viewport->WorkSize.x * 0.5F,
                             viewport->WorkPos.y + viewport->WorkSize.y * 0.14F},
                            ImGuiCond_Always, {0.5F, 0.0F});
    ImGui::SetNextWindowSize({width, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0F * ui_scale(), 8.0F * ui_scale()});
    ImGui::PushStyleColor(ImGuiCol_Border, color(Token::accent, 0.5F) );
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoScrollbar;
    if (!ImGui::Begin("##palette", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        return;
    }
    if (state.ui.palette_just_opened) {
        ImGui::SetKeyboardFocusHere();
        ImGui::SetWindowFocus();
        // Opening starts with an empty query, unless a caller pre-filled one.
        if (!state.ui.palette_prefilled) state.ui.palette_query.fill('\0');
        state.ui.palette_prefilled = false;
        state.ui.palette_cursor = 0;
        cache = {};
    }
    section(mode == UiState::PaletteMode::commands ? "Command palette" : "Go to");
    const bool edited = search_input(
        "##palette_query",
        mode == UiState::PaletteMode::commands
            ? "command, actor, script, class, 0xOFFSET, #ENTRY..."
            : "actor, script, chunk, class, 0xOFFSET, #ENTRY...",
        state.ui.palette_query.data(), state.ui.palette_query.size());
    const std::string query = state.ui.palette_query.data();
    if (cache.mode != mode || cache.query != query || cache.commands != state.commands.commands().size() ||
        cache.symbols_generation != state.search_index.generation() || state.ui.palette_just_opened) {
        compute(state, query, mode);
        cache.query = query;
        cache.mode = mode;
        cache.commands = state.commands.commands().size();
        cache.symbols_generation = state.search_index.generation();
        if (edited || state.ui.palette_just_opened) state.ui.palette_cursor = 0;
    }
    state.ui.palette_just_opened = false;

    const int count = static_cast<int>(cache.rows.size());
    bool keyboard_moved = false;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && count > 0) {
        state.ui.palette_cursor = (state.ui.palette_cursor + 1) % count;
        keyboard_moved = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && count > 0) {
        state.ui.palette_cursor = (state.ui.palette_cursor + count - 1) % count;
        keyboard_moved = true;
    }
    state.ui.palette_cursor = std::clamp(state.ui.palette_cursor, 0, std::max(count - 1, 0));

    const float row_height = ImGui::GetTextLineHeightWithSpacing() + 2.0F;
    const float list_height = std::min(static_cast<float>(std::max(count, 1)), 12.0F) * row_height + 4.0F;
    ImGui::Spacing();
    std::optional<int> activated;
    if (ImGui::BeginChild("##palette_results", {0.0F, list_height}, ImGuiChildFlags_None)) {
        if (count == 0)
            dim_text(query.empty() ? "Type to search commands and symbols." : "No matches.");
        for (int i = 0; i < count; ++i) {
            const auto& row = cache.rows[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const bool selected = i == state.ui.palette_cursor;
            const ImVec2 start = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, {0.0F, row_height - 2.0F}))
                activated = i;
            // Hover moves the cursor only when the mouse moves, so a still pointer
            // does not fight the arrow keys.
            const auto delta = ImGui::GetIO().MouseDelta;
            if (ImGui::IsItemHovered() && (delta.x != 0.0F || delta.y != 0.0F))
                state.ui.palette_cursor = i;
            if (selected && keyboard_moved) ImGui::SetScrollHereY();
            const float right = start.x + ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorScreenPos({start.x + 4.0F * ui_scale(), start.y});
            const bool disabled = row.command && row.command->enabled && !row.command->enabled();
            if (disabled) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, color(selected ? Token::accent : Token::text_dim));
            ImGui::TextUnformatted(row.command ? icons::LC_COMMAND : icon_for(row.entry->kind));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            const std::string& label = row.command ? row.command->label : row.entry->label;
            highlighted_text(label, row.positions);
            std::string secondary, tail;
            if (row.command) {
                secondary = row.command->category;
                tail = row.command->shortcut;
            } else {
                secondary = row.entry->detail;
                tail = symbol_kind_name(row.entry->kind);
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
            ImGui::PushFont(font(Font::mono));
            ImGui::TextUnformatted(secondary.c_str());
            if (!tail.empty()) {
                const float tail_width = ImGui::CalcTextSize(tail.c_str()).x;
                ImGui::SameLine();
                ImGui::SetCursorScreenPos({std::max(right - tail_width - 6.0F * ui_scale(),
                                                    ImGui::GetCursorScreenPos().x + 8.0F * ui_scale()),
                                           start.y});
                ImGui::TextUnformatted(tail.c_str());
            }
            ImGui::PopFont();
            ImGui::PopStyleColor();
            if (disabled) ImGui::EndDisabled();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
        if (count > 0) activated = state.ui.palette_cursor;
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape);
    const bool lost_focus = !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                            !state.ui.palette_just_opened;
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (activated && *activated < count) {
        const Row row = cache.rows[static_cast<std::size_t>(*activated)];
        activate(state, row);
    } else if (escape || lost_focus) {
        close(state);
    }
}

} // namespace rwsman::ui
