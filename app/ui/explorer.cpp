#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "navigation.hpp"
#include "rwsman/index_builders.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rwsman::ui {
namespace {

void draw_chunk_icon(const std::uint32_t type) {
    auto* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 item_min = ImGui::GetItemRectMin();
    const ImVec2 item_max = ImGui::GetItemRectMax();
    const float size = std::min(12.0F, item_max.y - item_min.y - 4.0F);
    const float left = item_min.x + ImGui::GetTreeNodeToLabelSpacing() + 1.0F;
    const float top = item_min.y + (item_max.y - item_min.y - size) * 0.5F;
    const float right = left + size;
    const float bottom = top + size;
    const float middle_x = (left + right) * 0.5F;
    const float middle_y = (top + bottom) * 0.5F;
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    constexpr float stroke = 1.35F;

    switch (type) {
    case 0x01: // Struct: a data table.
        draw_list->AddRect({left, top}, {right, bottom}, color, 1.0F, 0, stroke);
        draw_list->AddLine({left + 3.0F, top}, {left + 3.0F, bottom}, color, stroke);
        draw_list->AddLine({left, top + 4.0F}, {right, top + 4.0F}, color, stroke);
        draw_list->AddLine({left, top + 8.0F}, {right, top + 8.0F}, color, stroke);
        break;
    case 0x03: // Extension: plug-in/puzzle piece.
        draw_list->AddRect({left + 1.0F, top + 3.0F}, {right - 1.0F, bottom - 1.0F}, color, 1.0F, 0,
                           stroke);
        draw_list->AddCircle({middle_x, top + 3.0F}, 2.2F, color, 8, stroke);
        break;
    case 0x06: // Texture: image frame.
        draw_list->AddRect({left, top + 1.0F}, {right, bottom - 1.0F}, color, 1.0F, 0, stroke);
        draw_list->AddCircleFilled({right - 3.0F, top + 4.0F}, 1.25F, color, 8);
        draw_list->AddTriangleFilled({left + 1.5F, bottom - 2.0F}, {left + 5.0F, top + 6.0F},
                                     {left + 8.0F, bottom - 2.0F}, color);
        break;
    case 0x07: // Material: shaded sphere.
        draw_list->AddCircle({middle_x, middle_y}, size * 0.46F, color, 16, stroke);
        draw_list->AddCircleFilled({middle_x - 2.0F, middle_y - 2.0F}, 2.0F, color, 10);
        break;
    case 0x08: // Material List: three swatches.
        for (int row = 0; row < 3; ++row) {
            const float y = top + 2.0F + row * 4.0F;
            draw_list->AddCircleFilled({left + 2.0F, y}, 1.3F, color, 8);
            draw_list->AddLine({left + 5.0F, y}, {right, y}, color, stroke);
        }
        break;
    case 0x09: // Atomic Sector: filled terrain facet.
        draw_list->AddTriangleFilled({middle_x, top}, {right, bottom}, {left, bottom}, color);
        draw_list->AddLine({middle_x, top}, {middle_x, bottom},
                           ImGui::GetColorU32(ImGuiCol_WindowBg), 1.0F);
        break;
    case 0x0B: // World: globe.
        draw_list->AddCircle({middle_x, middle_y}, size * 0.47F, color, 16, stroke);
        draw_list->AddLine({left + 1.0F, middle_y}, {right - 1.0F, middle_y}, color, stroke);
        draw_list->AddEllipse({middle_x, middle_y}, {size * 0.22F, size * 0.47F}, color, 0.0F, 12,
                              stroke);
        break;
    case 0x0E: // Frame List: coordinate frame.
        draw_list->AddCircleFilled({left + 3.0F, bottom - 3.0F}, 1.5F, color, 8);
        draw_list->AddLine({left + 3.0F, bottom - 3.0F}, {right, bottom - 3.0F}, color, stroke);
        draw_list->AddLine({left + 3.0F, bottom - 3.0F}, {left + 3.0F, top}, color, stroke);
        draw_list->AddLine({left + 3.0F, bottom - 3.0F}, {right - 2.0F, top + 2.0F}, color, stroke);
        break;
    case 0x0F: // Geometry: wireframe triangle.
        draw_list->AddTriangle({middle_x, top}, {right, bottom}, {left, bottom}, color, stroke);
        draw_list->AddLine({middle_x, top}, {middle_x, bottom}, color, stroke);
        break;
    case 0x10: // Clump: grouped overlapping objects.
        draw_list->AddRect({left, top + 3.0F}, {right - 3.0F, bottom}, color, 1.0F, 0, stroke);
        draw_list->AddRect({left + 3.0F, top}, {right, bottom - 3.0F}, color, 1.0F, 0, stroke);
        break;
    case 0x14: // Atomic: single solid object.
        draw_list->AddQuadFilled({middle_x, top}, {right, middle_y}, {middle_x, bottom},
                                 {left, middle_y}, color);
        break;
    default:
        draw_list->AddCircleFilled({middle_x, middle_y}, 2.0F, color, 8);
        break;
    }
}


void find_clump_size_range(const std::vector<rws::Chunk>& chunks, float& minimum, float& maximum) {
    for (const auto& chunk : chunks) {
        if (chunk.type == 0x10) {
            const float size = std::log1p(static_cast<float>(chunk.declared_size));
            minimum = std::min(minimum, size);
            maximum = std::max(maximum, size);
        }
        find_clump_size_range(chunk.children, minimum, maximum);
    }
}

float clump_size_fraction(const rws::Chunk& chunk, const float minimum, const float maximum) {
    const float denominator = maximum - minimum;
    return denominator > 0.0001F
               ? std::clamp((std::log1p(static_cast<float>(chunk.declared_size)) - minimum) / denominator,
                            0.0F, 1.0F)
               : 0.5F;
}

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

// ---------------------------------------------------------------------------
// Shared frame: search prompt, filter chips, counts.

struct Frame {
    std::string needle; // Lower-case search text.
    bool diagnostics_only{};
    bool dirty_only{};
    [[nodiscard]] bool active() const { return !needle.empty() || diagnostics_only || dirty_only; }
};

bool chip(const char* label, bool& value, const char* tooltip) {
    ImGui::PushStyleColor(ImGuiCol_Button, value ? color(Token::accent_dim) : transparent());
    ImGui::PushStyleColor(ImGuiCol_Text, color(value ? Token::accent : Token::text_dim));
    const bool clicked = ImGui::SmallButton(label);
    ImGui::PopStyleColor(2);
    if (clicked) value = !value;
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

// `dirty_filter` offers the "dirty" chip; only chunk trees carry byte edits.
Frame draw_frame_header(AppState& state, const char* hint, const std::string& counts,
                        const std::vector<std::pair<const char*, SymbolKind>>& kinds,
                        const bool dirty_filter = false) {
    auto& ui = state.ui;
    auto& query = ui.explorer_query[static_cast<std::size_t>(state.workspace)];
    search_input("##explorer_search", hint, query.data(), query.size());
    if (!kinds.empty()) {
        if (ImGui::SmallButton((std::string(icons::LC_FILTER) + " kind").c_str()))
            ImGui::OpenPopup("##explorer_kinds");
        if (ImGui::BeginPopup("##explorer_kinds")) {
            for (const auto& [label, kind] : kinds) {
                bool on = (ui.explorer_kind_mask >> static_cast<unsigned>(kind)) & 1U;
                if (ImGui::Checkbox(label, &on))
                    ui.explorer_kind_mask = on ? (ui.explorer_kind_mask | (1U << static_cast<unsigned>(kind)))
                                               : (ui.explorer_kind_mask & ~(1U << static_cast<unsigned>(kind)));
            }
            if (ImGui::SmallButton("all")) ui.explorer_kind_mask = ~0U;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
    }
    chip((std::string(icons::LC_TRIANGLE_ALERT) + " diagnostics").c_str(), ui.explorer_diagnostics_only,
         "Only rows that carry a diagnostic");
    if (dirty_filter) {
        ImGui::SameLine();
        chip((std::string(icons::LC_CIRCLE_DOT) + " dirty").c_str(), ui.explorer_dirty_only,
             "Only rows with unsaved byte edits");
    }
    dim_text("%s", counts.c_str());
    ImGui::Separator();
    Frame frame;
    frame.needle = lower_ascii(query.data());
    frame.diagnostics_only = ui.explorer_diagnostics_only;
    frame.dirty_only = dirty_filter && ui.explorer_dirty_only;
    return frame;
}

bool kind_enabled(const AppState& state, const SymbolKind kind) {
    return ((state.ui.explorer_kind_mask >> static_cast<unsigned>(kind)) & 1U) != 0;
}

// One selectable row: icon, label, dim detail, diagnostic and dirty markers,
// context menu. Returns true when clicked.
bool row(AppState& state, const SelectionRef& ref, const char* icon, const std::string& label,
         const std::string& detail, const bool diagnosed, const bool dirty, const float indent = 0.0F,
         const bool reveal = false) {
    const bool selected = state.selection == ref;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float right = start.x + ImGui::GetContentRegionAvail().x;
    ImGui::PushID(static_cast<int>(SelectionRefHash{}(ref) & 0x7FFFFFFF));
    const bool clicked = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
        navigate_to(state, ref, {.frame = false});
    if (ImGui::BeginPopupContextItem("##row_menu")) {
        draw_selection_context_menu(state);
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        const auto identity = selection_identity(state, ref);
        if (!identity.empty()) ImGui::SetTooltip("%s", identity.c_str());
    }
    ImGui::PopID();
    if (reveal && selected) ImGui::SetScrollHereY(0.5F);

    ImGui::SetCursorScreenPos({start.x + 4.0F * ui_scale() + indent, start.y});
    ImGui::PushStyleColor(ImGuiCol_Text, color(selected ? Token::accent : Token::text_dim));
    ImGui::TextUnformatted(icon);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextUnformatted(label.c_str());
    if (!detail.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::PushFont(font(Font::mono));
        ImGui::TextUnformatted(detail.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }
    float marker_x = right - 16.0F * ui_scale();
    if (diagnosed) {
        ImGui::SetCursorScreenPos({marker_x, start.y});
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::warn));
        ImGui::TextUnformatted(icons::LC_TRIANGLE_ALERT);
        ImGui::PopStyleColor();
        marker_x -= 16.0F * ui_scale();
    }
    if (dirty) {
        ImGui::SetCursorScreenPos({marker_x, start.y});
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::dirty));
        ImGui::TextUnformatted(icons::LC_CIRCLE_DOT);
        ImGui::PopStyleColor();
    }
    // Leave the cursor at the start of the next row.
    ImGui::SetCursorScreenPos({start.x, start.y + ImGui::GetTextLineHeightWithSpacing()});
    return clicked;
}

// A group header with a dim count; returns whether the group is open.
bool group(const char* label, const std::size_t count, const bool default_open = true) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (default_open) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    ImGui::PushFont(font(Font::sans_bold));
    const bool open = ImGui::TreeNodeEx(label, flags);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::Text("(%zu)", count);
    ImGui::PopStyleColor();
    return open;
}

// ---------------------------------------------------------------------------
// Chunk tree (Scene / Geometry / Hex workspaces).

struct ChunkTreeContext {
    const std::vector<std::uint64_t>* reveal_path{};
    bool reveal{};
    bool colors{};
    float minimum{}, maximum{};
};

void draw_chunk_tree(AppState& state, const std::vector<rws::Chunk>& chunks,
                     const ChunkTreeContext& context) {
    for (const auto& chunk : chunks) {
        const bool has_children = !chunk.children.empty();
        const bool selected = state.selection == SelectionRef::chunk(chunk.offset);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;
        if (context.reveal && context.reveal_path &&
            std::ranges::find(*context.reveal_path, chunk.offset) != context.reveal_path->end())
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);

        Token token = Token::text;
        std::optional<ImVec4> tint;
        if (chunk.truncated)
            token = Token::error;
        else if (chunk.type == 0x10 && context.colors)
            tint = heat(clump_size_fraction(chunk, context.minimum, context.maximum));
        ImGui::PushStyleColor(ImGuiCol_Text, tint ? *tint : color(token));

        // Leave room for the icon that draw_chunk_icon paints over the label's start.
        const auto pad = static_cast<std::size_t>(std::ceil((14.0F * ui_scale() + 4.0F) / std::max(ImGui::CalcTextSize(" ").x, 1.0F)));
        std::string label = std::string(pad, ' ') + std::string(rws::chunk_name(chunk.type));
        if (const auto name = state.display_names.find(chunk.offset); name != state.display_names.end())
            label += "  \"" + name->second + "\"";
        ImGui::PushID(static_cast<int>(chunk.offset & 0x7FFFFFFF));
        const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(chunk.offset + 1)),
                                            flags, "%s", label.c_str());
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right);
        draw_chunk_icon(chunk.type);
        ImGui::PopStyleColor();
        if (clicked && !selected) navigate_to(state, SelectionRef::chunk(chunk.offset), {.frame = false});
        if (ImGui::BeginPopupContextItem("##chunk_menu")) {
            draw_selection_context_menu(state);
            ImGui::EndPopup();
        }
        if (context.reveal && selected) ImGui::SetScrollHereY(0.5F);
        ImGui::PopID();

        // Dim offset, size, and markers after the label.
        ImGui::SameLine();
        char detail[64];
        std::snprintf(detail, sizeof(detail), "@0x%llX (%u)", static_cast<unsigned long long>(chunk.offset),
                      chunk.declared_size);
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::PushFont(font(Font::mono));
        ImGui::TextUnformatted(detail);
        ImGui::PopFont();
        ImGui::PopStyleColor();
        const auto end = chunk.offset + 12 + chunk.available_size;
        if (state.range_has_diagnostic(chunk.offset, chunk.offset + 1)) {
            ImGui::SameLine();
            token_text(Token::warn, "%s", icons::LC_TRIANGLE_ALERT);
        }
        if (state.range_dirty(chunk.offset, end)) {
            ImGui::SameLine();
            token_text(Token::dirty, "%s", icons::LC_CIRCLE_DOT);
        }
        if (has_children && open) {
            draw_chunk_tree(state, chunk.children, context);
            ImGui::TreePop();
        }
    }
}

bool chunk_path_offsets(const std::vector<rws::Chunk>& chunks, const std::uint64_t offset,
                        std::vector<std::uint64_t>& path) {
    for (const auto& chunk : chunks) {
        path.push_back(chunk.offset);
        if (chunk.offset == offset || chunk_path_offsets(chunk.children, offset, path)) return true;
        path.pop_back();
    }
    return false;
}

void draw_legend(AppState& state) {
    if (ImGui::Checkbox("size colors", &state.settings.show_clump_colors)) state.settings_dirty = true;
    if (!state.settings.show_clump_colors) return;
    ImGui::SameLine();
    // Gradient bar: small clump -> large clump.
    const ImVec2 lo = ImGui::GetCursorScreenPos();
    const float width = 60.0F * ui_scale(), height = ImGui::GetTextLineHeight() * 0.6F;
    auto* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilledMultiColor({lo.x, lo.y + 3.0F}, {lo.x + width, lo.y + 3.0F + height},
                                       ImGui::ColorConvertFloat4ToU32(heat(0.0F)),
                                       ImGui::ColorConvertFloat4ToU32(heat(1.0F)),
                                       ImGui::ColorConvertFloat4ToU32(heat(1.0F)),
                                       ImGui::ColorConvertFloat4ToU32(heat(0.0F)));
    ImGui::Dummy({width, height + 6.0F});
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Clump color: declared size, log scale\n(dim = small, accent = large)");
}

void draw_chunk_explorer(AppState& state, const Frame& frame) {
    auto& document = *state.document;
    if (frame.active()) {
        // Search results are a flat list from the index; the tree is for browsing.
        const auto kinds = {SymbolKind::chunk, SymbolKind::instance};
        std::size_t shown = 0;
        for (const auto kind : kinds)
            for (const auto index : state.search_index.indices_of(kind)) {
                const auto& entry = state.search_index.entries()[index];
                if (!state.search_index.contains(index, frame.needle)) continue;
                const bool diagnosed = entry.kind == SymbolKind::chunk && entry.offset &&
                                       state.range_has_diagnostic(*entry.offset, *entry.offset + 1);
                const bool dirty = entry.offset && entry.end_offset &&
                                   state.range_dirty(*entry.offset, *entry.end_offset);
                if (frame.diagnostics_only && !diagnosed) continue;
                if (frame.dirty_only && !dirty) continue;
                if (++shown > 400) break;
                row(state, entry.target, icon_for(entry.kind), entry.label, entry.detail, diagnosed, dirty);
            }
        if (shown > 400) dim_text("Showing the first 400 matches. Refine the search.");
        if (shown == 0) dim_text("No chunk matches.");
        return;
    }
    draw_legend(state);
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();
    find_clump_size_range(document.chunks(), minimum, maximum);
    static SelectionRef last_reveal;
    const bool reveal = state.selection != last_reveal;
    last_reveal = state.selection;
    std::vector<std::uint64_t> reveal_path;
    if (reveal && state.selection.kind == SelectionRef::Kind::chunk)
        chunk_path_offsets(document.chunks(), state.selection.a, reveal_path);
    ChunkTreeContext context;
    context.reveal = reveal;
    context.reveal_path = &reveal_path;
    context.colors = state.settings.show_clump_colors;
    context.minimum = minimum;
    context.maximum = maximum;
    draw_chunk_tree(state, document.chunks(), context);

    if (!document.scene_instances().empty()) {
        const auto& instances = state.search_index.indices_of(SymbolKind::instance);
        if (group("CSF Scene Instances", instances.size(), false)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(instances.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& entry = state.search_index.entries()[instances[static_cast<std::size_t>(i)]];
                    row(state, entry.target, icon_for(entry.kind), entry.label, entry.detail, false,
                        entry.offset && entry.end_offset && state.range_dirty(*entry.offset, *entry.end_offset),
                        0.0F, reveal);
                }
            ImGui::TreePop();
        }
    }
}

// ---------------------------------------------------------------------------
// Mission explorer.

// Rows of one kind that pass the frame's filters.
std::vector<std::size_t> filtered(const AppState& state, const SymbolKind kind, const Frame& frame) {
    std::vector<std::size_t> result;
    if (!kind_enabled(state, kind)) return result;
    for (const auto index : state.search_index.indices_of(kind)) {
        if (!state.search_index.contains(index, frame.needle)) continue;
        if (frame.diagnostics_only && !state.has_diagnostic(state.search_index.entries()[index].target))
            continue;
        result.push_back(index);
    }
    return result;
}

void list_rows(AppState& state, const std::vector<std::size_t>& indices, const bool reveal) {
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(indices.size()), ImGui::GetTextLineHeightWithSpacing());
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto& entry = state.search_index.entries()[indices[static_cast<std::size_t>(i)]];
            row(state, entry.target, icon_for(entry.kind), entry.label, entry.detail,
                state.has_diagnostic(entry.target), false, 0.0F, reveal);
        }
}

void draw_simple_group(AppState& state, const char* title, const std::vector<SymbolKind>& kinds,
                       const Frame& frame, const bool reveal, const bool default_open = true) {
    std::vector<std::size_t> rows;
    for (const auto kind : kinds) {
        const auto part = filtered(state, kind, frame);
        rows.insert(rows.end(), part.begin(), part.end());
    }
    if (rows.empty() && frame.active()) return;
    std::size_t total = 0;
    for (const auto kind : kinds) total += state.search_index.indices_of(kind).size();
    if (total == 0) return;
    if (group(title, rows.size(), default_open || frame.active())) {
        list_rows(state, rows, reveal);
        ImGui::TreePop();
    }
}

void draw_navigation_group(AppState& state, const Frame& frame, const bool reveal) {
    const auto& groups = state.search_index.indices_of(SymbolKind::navigation_group);
    if (groups.empty() || !kind_enabled(state, SymbolKind::navigation_group)) return;
    // Children (points) per group, from the parent links recorded at index time.
    std::unordered_map<std::size_t, std::vector<std::size_t>> children;
    for (const auto index : state.search_index.indices_of(SymbolKind::navigation_point))
        if (const auto parent = state.search_index.entries()[index].parent)
            children[*parent].push_back(index);
    struct Visible {
        std::size_t index;
        std::vector<std::size_t> points;
    };
    std::vector<Visible> visible;
    std::size_t point_total = state.search_index.indices_of(SymbolKind::navigation_point).size();
    for (const auto index : groups) {
        Visible entry{index, {}};
        const bool group_match = state.search_index.contains(index, frame.needle) &&
                                 (!frame.diagnostics_only ||
                                  state.has_diagnostic(state.search_index.entries()[index].target));
        for (const auto point : children[index]) {
            if (!state.search_index.contains(point, frame.needle)) continue;
            if (frame.diagnostics_only && !state.has_diagnostic(state.search_index.entries()[point].target))
                continue;
            entry.points.push_back(point);
        }
        if (group_match || !entry.points.empty()) {
            // A matching group shows all of its points only when nothing narrows them.
            if (group_match && frame.needle.empty() && !frame.diagnostics_only) entry.points = children[index];
            visible.push_back(std::move(entry));
        }
    }
    if (visible.empty() && frame.active()) return;
    if (!group("Navigation", visible.size(), frame.active())) return;
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::Text("%zu groups, %zu points", groups.size(), point_total);
    ImGui::PopStyleColor();
    for (const auto& entry : visible) {
        const auto& group_entry = state.search_index.entries()[entry.index];
        const bool selected = state.selection == group_entry.target;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entry.points.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;
        if (frame.active()) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        ImGui::PushID(static_cast<int>(entry.index));
        const std::string text = std::string(icons::LC_ROUTE) + "  " + group_entry.label;
        const bool open = ImGui::TreeNodeEx("##nav_group", flags, "%s", text.c_str());
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
            navigate_to(state, group_entry.target, {.frame = false});
        if (ImGui::BeginPopupContextItem("##nav_menu")) {
            if (!selected) navigate_to(state, group_entry.target, {.frame = false});
            draw_selection_context_menu(state);
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::PushFont(font(Font::mono));
        ImGui::TextUnformatted(group_entry.detail.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        if (open) {
            for (const auto point : entry.points) {
                const auto& point_entry = state.search_index.entries()[point];
                row(state, point_entry.target, icon_for(point_entry.kind), point_entry.label,
                    point_entry.detail, state.has_diagnostic(point_entry.target), false,
                    12.0F * ui_scale(), reveal);
            }
            ImGui::TreePop();
        }
    }
    ImGui::TreePop();
}

void draw_resources(AppState& state, const Frame& frame) {
    const auto& graph = state.mission.graph;
    if (!graph || !kind_enabled(state, SymbolKind::resource)) return;
    const auto& resources = state.search_index.indices_of(SymbolKind::resource);
    const auto& nodes = graph->nodes();
    if (resources.size() != nodes.size()) return;
    struct Bucket {
        const char* title;
        Provenance provenance;
        std::vector<std::size_t> rows;
    };
    Bucket buckets[] = {{"Resolved", Provenance::proven, {}},
                        {"Ambiguous", Provenance::diagnosed, {}},
                        {"Missing", Provenance::diagnosed, {}}};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!state.search_index.contains(resources[i], frame.needle)) continue;
        switch (nodes[i].state) {
        case csf::LoadState::available:
        case csf::LoadState::metadata_only:
            // Resolved rows carry no diagnostic marker.
            if (!frame.diagnostics_only) buckets[0].rows.push_back(i);
            break;
        case csf::LoadState::ambiguous:
            buckets[1].rows.push_back(i);
            break;
        case csf::LoadState::missing:
        case csf::LoadState::rejected:
            buckets[2].rows.push_back(i);
            break;
        }
    }
    std::size_t total = 0;
    for (const auto& bucket : buckets) total += bucket.rows.size();
    if (total == 0 && frame.active()) return;
    if (!group("Resources", total, frame.active())) return;
    for (const auto& bucket : buckets) {
        if (bucket.rows.empty()) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, color(provenance_token(bucket.provenance)));
        ImGui::Text("%s (%zu)", bucket.title, bucket.rows.size());
        ImGui::PopStyleColor();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(bucket.rows.size()), ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& entry =
                    state.search_index.entries()[resources[bucket.rows[static_cast<std::size_t>(i)]]];
                row(state, entry.target, icon_for(entry.kind), entry.label, entry.detail,
                    bucket.provenance == Provenance::diagnosed, false, 8.0F * ui_scale());
            }
    }
    ImGui::TreePop();
}

void draw_folders(AppState& state);
void draw_animation_catalog(AppState& state, const Frame& frame);
void draw_cutscenes(AppState& state);
void draw_cutscene_cameras(AppState& state);

void draw_mission_explorer(AppState& state, const Frame& frame) {
    const bool animation = state.workspace == Workspace::animation;
    static SelectionRef last_reveal;
    const bool reveal = state.selection != last_reveal;
    last_reveal = state.selection;

    draw_simple_group(state, "Actors", {SymbolKind::actor}, frame, reveal);
    if (animation && !frame.active()) draw_cutscene_cameras(state);
    if (!animation) {
        draw_navigation_group(state, frame, reveal);
        draw_simple_group(state, "Spatial", {SymbolKind::dummy, SymbolKind::area, SymbolKind::light}, frame,
                          reveal, false);
        draw_simple_group(state, "Effects", {SymbolKind::effect}, frame, reveal, false);
    }
    draw_simple_group(state, "Scene-object animations", {SymbolKind::scene_object}, frame, reveal, false);
    // Folders and cutscenes are structured subtrees that the search and filter chips
    // do not apply to; they stay out of the way while filtering.
    if (!animation) {
        if (!frame.active()) draw_folders(state);
        draw_simple_group(state, "Classes", {SymbolKind::class_record}, frame, reveal, false);
        if (!frame.diagnostics_only) draw_animation_catalog(state, frame);
    }
    if (!frame.active()) draw_cutscenes(state);
    if (!animation) draw_resources(state, frame);
}

void draw_script_explorer(AppState& state, const Frame& frame) {
    const auto& programs = state.mission.programs;
    const auto& scripts = state.search_index.indices_of(SymbolKind::script);
    static SelectionRef last_reveal;
    const bool reveal = state.selection != last_reveal;
    last_reveal = state.selection;
    for (std::size_t document = 0; document < programs.size(); ++document) {
        std::vector<std::size_t> rows;
        for (const auto index : scripts) {
            const auto& entry = state.search_index.entries()[index];
            if (entry.target.a != document || !state.search_index.contains(index, frame.needle)) continue;
            if (frame.diagnostics_only && !state.has_diagnostic(entry.target)) continue;
            rows.push_back(index);
        }
        if (rows.empty() && frame.active()) continue;
        ImGui::PushID(static_cast<int>(document));
        if (group(path_utf8(programs[document].first.filename()).c_str(), rows.size(), true)) {
            std::string previous_folder = "\x01";
            for (const auto index : rows) {
                const auto& entry = state.search_index.entries()[index];
                if (entry.group != previous_folder) {
                    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
                    ImGui::PushFont(font(Font::sans_bold));
                    ImGui::TextUnformatted(entry.group.empty() ? "(root)" : entry.group.c_str());
                    ImGui::PopFont();
                    ImGui::PopStyleColor();
                    previous_folder = entry.group;
                }
                const bool selected = (state.selection.kind == SelectionRef::Kind::program_script ||
                                       state.selection.kind == SelectionRef::Kind::program_instruction) &&
                                      state.selection.a == entry.target.a && state.selection.b == entry.target.b;
                // Compare on the script, not the instruction focus.
                ImGui::PushID(static_cast<int>(index));
                const ImVec2 start = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##script", selected)) navigate_to(state, entry.target, {.frame = false});
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !selected)
                    navigate_to(state, entry.target, {.frame = false});
                if (ImGui::BeginPopupContextItem("##script_menu")) {
                    draw_selection_context_menu(state);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                if (reveal && selected) ImGui::SetScrollHereY(0.5F);
                ImGui::SetCursorScreenPos({start.x + 12.0F * ui_scale(), start.y});
                ImGui::PushStyleColor(ImGuiCol_Text, color(selected ? Token::accent : Token::text_dim));
                ImGui::TextUnformatted(icons::LC_FILE_CODE);
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextUnformatted(entry.label.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
                ImGui::PushFont(font(Font::mono));
                ImGui::Text("[%llu]", static_cast<unsigned long long>(entry.id.value_or(0)));
                ImGui::PopFont();
                ImGui::PopStyleColor();
                if (state.has_diagnostic(entry.target)) {
                    ImGui::SameLine();
                    token_text(Token::warn, "%s", icons::LC_TRIANGLE_ALERT);
                }
                ImGui::SetCursorScreenPos({start.x, start.y + ImGui::GetTextLineHeightWithSpacing()});
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------
// Mission subtrees kept from the original explorer: they are structured
// (folders, catalog records, cutscene blocks) rather than flat lists.

void draw_cutscene_cameras(AppState& state) {
    auto& mission = state.mission;
    auto& geometry_preview = state.preview;
    const auto selected_entry = geometry_preview.selected_mission_entry();
    if (ImGui::TreeNodeEx("Cutscene camera dummies", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::set<std::uint32_t> shown;
        for (const auto& [path, timeline] : mission.cutscenes)
            for (const auto& script : timeline.scripts())
                for (const auto& action : script.actions) {
                    if (action.kind != csf::CutsceneActionKind::camera || !action.numeric_value)
                        continue;
                    const auto dummy =
                        std::ranges::find_if(mission.scene->dummies(), [&](const auto& value) {
                            return value.id == static_cast<std::int32_t>(*action.numeric_value);
                        });
                    if (dummy == mission.scene->dummies().end() ||
                        !shown.insert(dummy->source.entry_index).second)
                        continue;
                    const auto text = dummy->name.value_or("(unnamed camera)") + " [dummy " +
                                      std::to_string(dummy->id.value_or(-1)) + ", " + script.name +
                                      "]";
                    ImGui::PushID(static_cast<int>(dummy->source.entry_index));
                    if (ImGui::Selectable(text.c_str(),
                                          selected_entry == dummy->source.entry_index))
                        geometry_preview.select_mission_entry(dummy->source.entry_index);
                    ImGui::PopID();
                }
        if (shown.empty()) ImGui::TextDisabled("No camera actions resolve to SCN dummies");
        ImGui::TreePop();
    }
}

void draw_folders(AppState& state) {
    auto& mission = state.mission;
    auto& geometry_preview = state.preview;
    auto& workspace = state.workspace;
    const auto selected_entry = geometry_preview.selected_mission_entry();
    if (workspace == Workspace::mission && !mission.scene->folders().empty() &&
        ImGui::TreeNode("Folders")) {
        for (const auto& folder : mission.scene->folders()) {
            std::vector<std::uint32_t> entries;
            for (const auto id : folder.element_ids) {
                const auto dummy = std::ranges::find_if(
                    mission.scene->dummies(), [&](const auto& value) { return value.id == id; });
                if (dummy != mission.scene->dummies().end())
                    entries.push_back(dummy->source.entry_index);
                const auto light = std::ranges::find_if(
                    mission.scene->lights(), [&](const auto& value) { return value.id == id; });
                if (light != mission.scene->lights().end())
                    entries.push_back(light->source.entry_index);
            }
            bool visible = std::ranges::all_of(entries, [&](const auto entry) {
                return geometry_preview.mission_entry_visible(entry);
            });
            ImGui::PushID(static_cast<int>(folder.source.entry_index));
            if (ImGui::Checkbox("##visible", &visible))
                geometry_preview.set_mission_entries_visible(entries, visible);
            ImGui::SameLine();
            const auto label = (folder.path.empty() ? "(root)" : folder.path) + " (" +
                               std::to_string(folder.element_ids.size()) + ")";
            if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                for (const auto id : folder.element_ids) {
                    const auto dummy =
                        std::ranges::find_if(mission.scene->dummies(),
                                             [&](const auto& value) { return value.id == id; });
                    const auto light = std::ranges::find_if(
                        mission.scene->lights(), [&](const auto& value) { return value.id == id; });
                    const csf::CsfSourceId* source = nullptr;
                    std::string text = "Missing element " + std::to_string(id);
                    if (dummy != mission.scene->dummies().end()) {
                        source = &dummy->source;
                        text = "Dummy: " + dummy->name.value_or("(unnamed)");
                    } else if (light != mission.scene->lights().end()) {
                        source = &light->source;
                        text = "Light: " + light->name.value_or("(unnamed)");
                    }
                    if (source &&
                        ImGui::Selectable(text.c_str(), selected_entry == source->entry_index))
                        geometry_preview.select_mission_entry(source->entry_index);
                    else if (!source)
                        ImGui::TextDisabled("%s", text.c_str());
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void draw_animation_catalog(AppState& state, const Frame& frame) {
    auto& mission = state.mission;
    auto& workspace = state.workspace;
    const auto matches = [&](const std::string& text) {
        return frame.needle.empty() || lower_ascii(text).find(frame.needle) != std::string::npos;
    };
    if (workspace == Workspace::mission && mission.animations &&
        ImGui::TreeNode("Animation catalog")) {
        ImGui::TextDisabled("%zu logical records", mission.animations->records().size());
        for (const auto& animation : mission.animations->records())
            if (matches(animation.logical_name)) {
                ImGui::PushID(static_cast<int>(animation.source.entry_index));
                if (ImGui::TreeNode(animation.logical_name.c_str())) {
                    ImGui::TextDisabled("source entry %u | %zu variant%s",
                                        animation.source.entry_index, animation.variants.size(),
                                        animation.variants.size() == 1 ? "" : "s");
                    if (animation.loop) ImGui::Text("Loop: %s", *animation.loop ? "yes" : "no");
                    if (animation.blend_in) ImGui::Text("Blend in: %.6g", *animation.blend_in);
                    if (animation.velocity_scalar)
                        ImGui::Text("Velocity: %.6g", *animation.velocity_scalar);
                    if (animation.translation_scalar)
                        ImGui::Text("Translation: %.6g", *animation.translation_scalar);
                    if (animation.rotation_scalar)
                        ImGui::Text("Rotation: %.6g", *animation.rotation_scalar);
                    for (const auto& variant : animation.variants) {
                        ImGui::BulletText("%s", variant.reference.c_str());
                        if (variant.resolution) {
                            ImGui::Indent();
                            ImGui::TextDisabled(
                                "%s", csf::resolution_status_name(variant.resolution->status));
                            ImGui::Unindent();
                        }
                    }
                    for (const auto& sound : animation.sounds)
                        ImGui::BulletText(
                            "Sound %s%s", sound.logical_id.c_str(),
                            sound.time ? (" @ " + std::to_string(*sound.time) + " s").c_str() : "");
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        ImGui::TreePop();
    }
}

void draw_cutscenes(AppState& state) {
    auto& mission = state.mission;
    auto& geometry_preview = state.preview;
    if (!mission.cutscenes.empty() && ImGui::TreeNode("Cutscenes / control flow")) {
        for (const auto& [path, timeline] : mission.cutscenes)
            if (ImGui::TreeNode(path_utf8(path.filename()).c_str())) {
                for (const auto& script : timeline.scripts())
                    if (ImGui::TreeNode(&script, "%s [%zu blocks]", script.name.c_str(),
                                        script.blocks.size())) {
                        for (const auto& block : script.blocks) {
                            ImGui::Text("Block %u%s%s", block.index,
                                        block.conditional ? " | branch" : "",
                                        block.runtime_wait ? " | runtime wait" : "");
                            ImGui::Indent();
                            for (auto index : block.action_indices) {
                                const auto& action = script.actions[index];
                                ImGui::BulletText("%s: %s",
                                                  csf::cutscene_action_kind_name(action.kind),
                                                  action.opcode.c_str());
                                if (!action.reference.empty()) {
                                    ImGui::SameLine();
                                    ImGui::TextDisabled("%s", action.reference.c_str());
                                }
                                ImGui::Indent();
                                ImGui::TextDisabled("source entry %u", action.source.entry_index);
                                ImGui::Unindent();
                                if (action.kind == csf::CutsceneActionKind::camera &&
                                    action.numeric_value) {
                                    const auto dummy = std::ranges::find_if(
                                        mission.scene->dummies(), [&](const auto& value) {
                                            return value.id &&
                                                   *value.id == static_cast<std::int32_t>(
                                                                    *action.numeric_value);
                                        });
                                    if (dummy != mission.scene->dummies().end()) {
                                        ImGui::SameLine();
                                        ImGui::PushID(&action);
                                        if (ImGui::SmallButton("Preview dummy"))
                                            geometry_preview.select_mission_entry(
                                                dummy->source.entry_index);
                                        ImGui::PopID();
                                    }
                                }
                                if (action.kind == csf::CutsceneActionKind::fov &&
                                    action.numeric_value) {
                                    ImGui::SameLine();
                                    ImGui::Text("FOV %.4g", *action.numeric_value);
                                }
                                if (action.kind == csf::CutsceneActionKind::animation &&
                                    mission.animations) {
                                    auto reference = action.reference;
                                    std::ranges::transform(
                                        reference, reference.begin(), [](unsigned char c) {
                                            return static_cast<char>(std::toupper(c));
                                        });
                                    const auto logical = std::ranges::find_if(
                                        mission.animations->records(), [&](const auto& record) {
                                            auto name = record.logical_name;
                                            std::ranges::transform(
                                                name, name.begin(), [](unsigned char c) {
                                                    return static_cast<char>(std::toupper(c));
                                                });
                                            return !name.empty() &&
                                                   reference.find(name) != std::string::npos;
                                        });
                                    if (logical != mission.animations->records().end()) {
                                        ImGui::Indent();
                                        ImGui::Text("Catalog: %s [entry %u]",
                                                    logical->logical_name.c_str(),
                                                    logical->source.entry_index);
                                        for (const auto& variant : logical->variants)
                                            ImGui::TextDisabled("%s", variant.reference.c_str());
                                        ImGui::Unindent();
                                    }
                                }
                            }
                            ImGui::Unindent();
                        }
                        ImGui::TreePop();
                    }
                ImGui::TreePop();
            }
        ImGui::TreePop();
    }
}

} // namespace

void draw_explorer(AppState& state) {
    const auto workspace = state.workspace;
    if (workspace == Workspace::script) {
        std::size_t script_count{};
        for (const auto& [path, program] : state.mission.programs) {
            (void)path;
            script_count += program.scripts().size();
        }
        char counts[128];
        std::snprintf(counts, sizeof(counts), "%zu documents | %zu scripts | %zu references",
                      state.mission.programs.size(), script_count,
                      state.mission.program_references.references().size());
        const auto frame = draw_frame_header(state, "script, folder, opcode...", counts, {});
        draw_script_explorer(state, frame);
    } else if ((workspace == Workspace::mission || workspace == Workspace::animation) && state.mission.scene) {
        const auto& scene = *state.mission.scene;
        char counts[160];
        std::snprintf(counts, sizeof(counts), "%zu actors | %zu nav points | %zu effects | %zu diagnostics",
                      scene.actors().size(), scene.navigation_stats().points, scene.effects().size(),
                      state.diagnostic_problem_count());
        const auto frame = draw_frame_header(
            state, "name, class, 0xID...", counts,
            {{"Actors", SymbolKind::actor},
             {"Navigation", SymbolKind::navigation_group},
             {"Dummies", SymbolKind::dummy},
             {"Areas", SymbolKind::area},
             {"Lights", SymbolKind::light},
             {"Effects", SymbolKind::effect},
             {"Classes", SymbolKind::class_record},
             {"Resources", SymbolKind::resource}});
        draw_mission_explorer(state, frame);
    } else if (state.document) {
        char counts[128];
        std::snprintf(counts, sizeof(counts), "%zu chunks | %zu instances | %zu diagnostics",
                      state.document->chunks().size(), state.document->scene_instances().size(),
                      state.diagnostic_problem_count());
        const auto frame = draw_frame_header(state, "chunk, name, 0xOFFSET...", counts, {}, true);
        draw_chunk_explorer(state, frame);
    }
}

} // namespace rwsman::ui
