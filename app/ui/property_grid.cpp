#include "ui/property_grid.hpp"

#include "navigation.hpp"
#include "rwsman/mission_lookup.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace rwsman::ui {
namespace {

bool angles_in_radians = false;

// Shortens `text` to fit `width` pixels in the current font. Paths keep their
// tail (the file name is what matters); other text keeps its head.
std::string elide(const std::string& text, const float width) {
    if (width <= 0.0F || ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const bool path = text.find_first_of("/\\") != std::string::npos;
    const std::string dots = "...";
    for (std::size_t keep = text.size(); keep > 1; --keep) {
        const std::string shown = path ? dots + text.substr(text.size() - keep) : text.substr(0, keep) + dots;
        if (ImGui::CalcTextSize(shown.c_str()).x <= width) return shown;
    }
    return dots;
}

std::string format_real(const double value) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

} // namespace

RawSlice raw_slice_for_entry(const csf::Document& document, const std::uint32_t entry) {
    RawSlice slice;
    slice.csf = &document;
    slice.entry = entry;
    const auto& entries = document.entries();
    if (entry < entries.size() && entries[entry].entry_index == entry) {
        slice.offset = entries[entry].source.offset;
        slice.size = entries[entry].source.size;
    } else {
        for (const auto& candidate : entries)
            if (candidate.entry_index == entry) {
                slice.offset = candidate.source.offset;
                slice.size = candidate.source.size;
                break;
            }
    }
    return slice;
}

RawSlice raw_slice_for_field(const csf::Document& document, const csf::Node& record,
                             const std::string_view field) {
    for (const auto& child : record.children) {
        if (!child.identifier_index) continue;
        const auto* identifier = document.identifier(*child.identifier_index);
        if (identifier && identifier->display_utf8() == field)
            return raw_slice_for_entry(document, child.entry_index);
    }
    return {};
}

void draw_hex_dump(const std::span<const std::byte> bytes, const std::uint64_t base_offset,
                   const std::size_t limit) {
    ImGui::PushFont(font(Font::mono));
    const auto shown = std::min(bytes.size(), limit);
    for (std::size_t row = 0; row < shown; row += 16) {
        char line[128];
        int used = std::snprintf(line, sizeof(line), "%08llX ",
                                 static_cast<unsigned long long>(base_offset + row));
        for (std::size_t i = 0; i < 16 && row + i < shown; ++i)
            used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used), " %02X",
                                  std::to_integer<unsigned>(bytes[row + i]));
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::raw));
        ImGui::TextUnformatted(line);
        ImGui::PopStyleColor();
    }
    if (bytes.size() > shown) {
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::Text("... %zu more bytes", bytes.size() - shown);
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
}

PropertyGrid::PropertyGrid(AppState& state, const char* id) : state_(state), id_(id) {
    open_ = ImGui::BeginTable(id, 3,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings |
                                  ImGuiTableFlags_PadOuterX);
    if (open_) {
        ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 84.0F * ui_scale());
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("meta", ImGuiTableColumnFlags_WidthFixed, 42.0F * ui_scale());
    }
}

PropertyGrid::~PropertyGrid() {
    if (open_) ImGui::EndTable();
}

void PropertyGrid::begin_row(const char* key) {
    key_ = key;
    ImGui::TableNextRow();
    ImGui::PushID(row_++);
    ImGui::TableSetColumnIndex(0);
    row_min_ = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::TextUnformatted(key);
    ImGui::PopStyleColor();
    ImGui::TableSetColumnIndex(1);
}

void PropertyGrid::end_row(const Options& options) {
    const ImVec2 row_max{ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x,
                         row_min_.y + ImGui::GetTextLineHeightWithSpacing()};
    const bool value_hovered = ImGui::IsMouseHoveringRect(row_min_, row_max);
    ImGui::TableSetColumnIndex(2);
    if (options.show_badge) {
        badge(options.provenance, options.evidence.c_str());
        ImGui::SameLine(0.0F, 4.0F * ui_scale());
    }
    bool raw_open = false;
    if (options.raw.valid()) {
        const ImGuiID storage_id = ImGui::GetID("raw");
        auto* storage = ImGui::GetStateStorage();
        raw_open = storage->GetBool(storage_id, false);
        ImGui::PushStyleColor(ImGuiCol_Text, color(raw_open ? Token::raw : Token::text_dim));
        if (ImGui::SmallButton(icons::LC_BINARY)) storage->SetBool(storage_id, raw_open = !raw_open);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Show the raw bytes behind this value");
    } else if (value_hovered) {
        // Read-only marker; the guarded-authoring editor attaches here (Phase 06).
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::line));
        ImGui::TextUnformatted(icons::LC_LOCK);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Read-only. Editing arrives with guarded authoring.");
    }
    if (raw_open) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        const auto& raw = options.raw;
        const auto bytes = raw.csf ? raw.csf->bytes() : raw.rws->bytes();
        if (raw.offset < bytes.size()) {
            const auto size = std::min<std::uint64_t>(raw.size, bytes.size() - raw.offset);
            ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
            const auto file = raw.csf ? raw.csf->source_path() : raw.rws->source_path();
            ImGui::PushFont(font(Font::mono));
            if (raw.entry)
                ImGui::Text("entry %u @0x%llX, %llu bytes", *raw.entry,
                            static_cast<unsigned long long>(raw.offset),
                            static_cast<unsigned long long>(size));
            else
                ImGui::Text("@0x%llX, %llu bytes", static_cast<unsigned long long>(raw.offset),
                            static_cast<unsigned long long>(size));
            ImGui::PopFont();
            ImGui::PopStyleColor();
            (void)file;
            draw_hex_dump(bytes.subspan(static_cast<std::size_t>(raw.offset), static_cast<std::size_t>(size)),
                          raw.offset, 64);
        }
    }
    ImGui::PopID();
}

void PropertyGrid::value_text(const std::string& shown, const std::string& copy, const Options&) {
    // The value cell never wraps: long values are elided and copy in full.
    ImGui::PushFont(font(Font::mono));
    const auto fitted = elide(shown, ImGui::GetContentRegionAvail().x);
    ImGui::PopFont();
    copyable_text(fitted.c_str(), copy.c_str(), fitted == shown ? "Click to copy" : shown.c_str());
}

void PropertyGrid::text(const char* key, const std::string_view value, const Options& options) {
    begin_row(key);
    const std::string shown = value.empty() ? std::string("-") : std::string(value);
    value_text(shown, std::string(value), options);
    end_row(options);
}

void PropertyGrid::integer(const char* key, const std::int64_t value, const Options& options) {
    begin_row(key);
    const ImGuiID storage_id = ImGui::GetID("hex");
    auto* storage = ImGui::GetStateStorage();
    const bool as_hex = storage->GetBool(storage_id, false);
    char decimal[32], hexadecimal[32];
    std::snprintf(decimal, sizeof(decimal), "%lld", static_cast<long long>(value));
    std::snprintf(hexadecimal, sizeof(hexadecimal), "0x%llX",
                  static_cast<unsigned long long>(static_cast<std::uint64_t>(value)));
    value_text(as_hex ? hexadecimal : decimal, as_hex ? hexadecimal : decimal, options);
    if (ImGui::BeginPopupContextItem("##int_menu")) {
        if (ImGui::MenuItem("Copy decimal")) ImGui::SetClipboardText(decimal);
        if (ImGui::MenuItem("Copy hex")) ImGui::SetClipboardText(hexadecimal);
        if (ImGui::MenuItem(as_hex ? "Show as decimal" : "Show as hex")) storage->SetBool(storage_id, !as_hex);
        ImGui::EndPopup();
    }
    end_row(options);
}

void PropertyGrid::real(const char* key, const double value, const Options& options) {
    begin_row(key);
    const auto text = format_real(value);
    value_text(text, text, options);
    end_row(options);
}

void PropertyGrid::boolean(const char* key, const bool value, const Options& options) {
    begin_row(key);
    value_text(value ? "yes" : "no", value ? "true" : "false", options);
    end_row(options);
}

void PropertyGrid::vec3(const char* key, const double x, const double y, const double z,
                        const Options& options) {
    begin_row(key);
    const std::string parts[3] = {format_real(x), format_real(y), format_real(z)};
    const std::string all = parts[0] + ", " + parts[1] + ", " + parts[2];
    static const char* const axes[3] = {"x", "y", "z"};
    // Stack the components when they do not fit on one line.
    ImGui::PushFont(font(Font::mono));
    const float needed = ImGui::CalcTextSize(parts[0].c_str()).x + ImGui::CalcTextSize(parts[1].c_str()).x +
                         ImGui::CalcTextSize(parts[2].c_str()).x + 16.0F * ui_scale();
    ImGui::PopFont();
    const bool stacked = needed > ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        if (stacked) {
            ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
            ImGui::TextUnformatted(axes[i]);
            ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        copyable_text(parts[i].c_str(), parts[i].c_str(), "Click to copy this component; right-click for more");
        if (ImGui::BeginPopupContextItem("##vec3_menu")) {
            for (int axis = 0; axis < 3; ++axis)
                if (ImGui::MenuItem((std::string("Copy ") + axes[axis]).c_str())) ImGui::SetClipboardText(parts[axis].c_str());
            if (ImGui::MenuItem("Copy all")) ImGui::SetClipboardText(all.c_str());
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (i != 2 && !stacked) ImGui::SameLine(0.0F, 8.0F * ui_scale());
    }
    end_row(options);
}

void PropertyGrid::angle(const char* key, const double value, const bool degrees, const Options& options) {
    constexpr double pi = 3.14159265358979323846;
    begin_row(key);
    const double as_degrees = degrees ? value : value * 180.0 / pi;
    const double as_radians = degrees ? value * pi / 180.0 : value;
    const auto shown = angles_in_radians ? format_real(as_radians) + " rad" : format_real(as_degrees) + " deg";
    value_text(shown, angles_in_radians ? format_real(as_radians) : format_real(as_degrees), options);
    if (ImGui::BeginPopupContextItem("##angle_menu")) {
        if (ImGui::MenuItem("Copy degrees")) ImGui::SetClipboardText(format_real(as_degrees).c_str());
        if (ImGui::MenuItem("Copy radians")) ImGui::SetClipboardText(format_real(as_radians).c_str());
        if (ImGui::MenuItem(angles_in_radians ? "Show angles in degrees" : "Show angles in radians"))
            angles_in_radians = !angles_in_radians;
        ImGui::EndPopup();
    }
    end_row(options);
}

void PropertyGrid::offset(const char* key, const std::uint64_t value, const Options& options) {
    begin_row(key);
    hex_link(value);
    end_row(options);
}

void PropertyGrid::link(const char* key, const std::string_view label, const SelectionRef& target,
                        const Options& options) {
    begin_row(key);
    ImGui::PushStyleColor(ImGuiCol_Text, color(target.empty() ? Token::text : Token::inferred));
    ImGui::PushFont(font(Font::mono));
    const std::string text = elide(std::string(label), ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(text.empty() ? "-" : text.c_str());
    ImGui::PopFont();
    ImGui::PopStyleColor();
    if (!target.empty() && ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Go to %s", selection_title(state_, target).c_str());
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine({lo.x, hi.y}, {hi.x, hi.y}, color_u32(Token::inferred, 0.7F));
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) navigate_to(state_, target);
    }
    end_row(options);
}

bool begin_section(AppState& state, const char* title, const bool default_open) {
    // Open state is remembered per selection kind for the session.
    static std::map<std::string, bool> remembered;
    const std::string key = std::string(selection_kind_name(state.selection.kind)) + "/" + title;
    const auto found = remembered.find(key);
    const bool want_open = found == remembered.end() ? default_open : found->second;
    ImGui::SetNextItemOpen(want_open, ImGuiCond_Always);
    ImGui::Spacing();
    ImGui::PushFont(font(Font::sans_bold));
    ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
    ImGui::PushStyleColor(ImGuiCol_Header, transparent());
    // Upper-case label like the other section headers.
    std::string label(title);
    for (auto& c : label) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const bool open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoAutoOpenOnLog,
                                        "%s", label.c_str());
    ImGui::PopStyleColor(2);
    ImGui::PopFont();
    remembered[key] = open;
    return open;
}

void end_section() {
    ImGui::TreePop();
}

} // namespace rwsman::ui
