#include "app_state.hpp"
#include "ui/ui.hpp"

#include "rws/decoded.hpp"
#include "rws/scene_export.hpp"

#include "ui/fonts.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace rwsman::ui {

void draw_hex(AppState& state, const std::uint64_t begin, const std::uint64_t size) {
    auto& document = *state.document;
    const auto bytes = document.bytes();
    const auto end = std::min<std::uint64_t>(begin + size, bytes.size());
    const auto shown_end = std::min<std::uint64_t>(end, begin + 4096);
    section("Payload bytes (editable, first 4096)");
    ImGui::BeginChild("hex", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(font(Font::mono));
    const auto highlight = state.ui.hex_highlight;
    const float cell_width = ImGui::CalcTextSize("FF").x + ImGui::GetStyle().FramePadding.x * 2.0F;
    const int rows = shown_end > begin ? static_cast<int>((shown_end - begin + 15) / 16) : 0;
    // Rows are 16-byte aligned to the payload start. Scroll to the row that holds
    // the start of the highlighted range (the clipper only submits visible rows).
    if (highlight && state.ui.hex_scroll_to_highlight) {
        if (highlight->first >= begin && highlight->first < shown_end) {
            const auto row = static_cast<float>((highlight->first - begin) / 16);
            const float target = row * ImGui::GetFrameHeightWithSpacing() - ImGui::GetWindowHeight() * 0.3F;
            ImGui::SetScrollY(std::max(target, 0.0F));
        }
        state.ui.hex_scroll_to_highlight = false;
    }
    ImGuiListClipper clipper;
    clipper.Begin(rows, ImGui::GetFrameHeightWithSpacing());
    while (clipper.Step())
        for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; ++line) {
            const std::uint64_t row = begin + static_cast<std::uint64_t>(line) * 16;
            char label[24];
            std::snprintf(label, sizeof(label), "%08llX", static_cast<unsigned long long>(row));
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, color(Token::inferred));
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0F, 12.0F * ui_scale());
            ImGui::PushID(line);
            for (std::uint64_t column = 0; column < 16 && row + column < shown_end; ++column) {
                const auto offset = row + column;
                const auto original = std::to_integer<unsigned char>(bytes[static_cast<std::size_t>(offset)]);
                auto value = static_cast<unsigned int>(original);
                const bool edited = state.range_dirty(offset, offset + 1);
                const bool highlighted = highlight && offset >= highlight->first && offset < highlight->second;
                ImGui::PushID(static_cast<int>(column));
                ImGui::SetNextItemWidth(cell_width);
                if (highlighted) ImGui::PushStyleColor(ImGuiCol_FrameBg, color(Token::accent_dim));
                if (edited) ImGui::PushStyleColor(ImGuiCol_Text, color(Token::dirty));
                // InputScalar has no "enter returns true" mode; apply the typed value when
                // the field is left (Enter, Tab, or a click elsewhere).
                ImGui::InputScalar("##byte", ImGuiDataType_U32, &value, nullptr, nullptr, "%02X",
                                   ImGuiInputTextFlags_CharsHexadecimal);
                if (ImGui::IsItemDeactivatedAfterEdit() && (value & 0xFFU) != original) {
                    document.set_byte(offset, static_cast<std::byte>(value & 0xFFU));
                    state.record_edit(offset, original, static_cast<std::uint8_t>(value & 0xFFU));
                }
                if (edited) ImGui::PopStyleColor();
                if (highlighted) ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && edited)
                    ImGui::SetTooltip("Edited: 0x%02X", static_cast<unsigned>(original));
                ImGui::PopID();
                if (column != 15) ImGui::SameLine(0.0F, column % 8 == 7 ? 18.0F * ui_scale() : -1.0F);
            }
            ImGui::PopID();
        }
    ImGui::PopFont();
    ImGui::EndChild();
}

void draw_hex_view(AppState& state) {
    auto& document = state.document;
    if (!document) return;
    const auto* selected_chunk =
        state.selected ? find_chunk(document->chunks(), *state.selected) : nullptr;
    const auto* selected_instance =
        state.selected ? find_instance(document->scene_instances(), *state.selected) : nullptr;
    if (selected_chunk) {
        ImGui::Text("%s (0x%08X)", rws::chunk_name(selected_chunk->type).data(),
                    selected_chunk->type);
        ImGui::Text("Header: 0x%llX   Payload: 0x%llX",
                    static_cast<unsigned long long>(selected_chunk->offset),
                    static_cast<unsigned long long>(selected_chunk->payload_offset));
        ImGui::Text("Declared: %u   Available: %llu   Library ID: 0x%08X",
                    selected_chunk->declared_size,
                    static_cast<unsigned long long>(selected_chunk->available_size),
                    selected_chunk->library_id);
        ImGui::Text("Vendor: %s (0x%06X)   Object ID: 0x%02X",
                    rws::chunk_vendor_name(rws::chunk_vendor_id(selected_chunk->type)).data(),
                    rws::chunk_vendor_id(selected_chunk->type),
                    rws::chunk_object_id(selected_chunk->type));
        const auto* owner = find_owning_object(document->chunks(), selected_chunk->offset);
        draw_typed_details(state, *selected_chunk, owner ? owner->type : 0);
        draw_hex(state, selected_chunk->payload_offset, selected_chunk->available_size);
    } else if (selected_instance) {
        ImGui::Text("CSF Scene Instance @ 0x%llX",
                    static_cast<unsigned long long>(selected_instance->offset));
        ImGui::Text("Prototype: %u   Instance ID: %u", selected_instance->prototype_id,
                    selected_instance->instance_id);
        ImGui::Text("Name: %s", selected_instance->prototype_name.empty()
                                    ? "(unnamed)"
                                    : selected_instance->prototype_name.c_str());
        ImGui::Text("Declared: %u   Physical: %llu", selected_instance->declared_size,
                    static_cast<unsigned long long>(selected_instance->physical_size));
        ImGui::Text("Position: %.6g, %.6g, %.6g", selected_instance->position.x,
                    selected_instance->position.y, selected_instance->position.z);
        ImGui::Text("Flags: 0x%08X (%s)", selected_instance->flags,
                    rws::scene_instance_flag_names(selected_instance->flags).c_str());
        ImGui::Text("Visibility distance: max %.6g, min %.6g, fade %.6g",
                    selected_instance->maximum_visibility_distance,
                    selected_instance->minimum_visibility_distance,
                    selected_instance->visibility_fade_range);
        ImGui::Text("Matrix flags: 0x%08X", selected_instance->matrix_flags);
        ImGui::Text("Rotation: [%.5g %.5g %.5g]", selected_instance->rotation[0],
                    selected_instance->rotation[1], selected_instance->rotation[2]);
        ImGui::Text("          [%.5g %.5g %.5g]", selected_instance->rotation[3],
                    selected_instance->rotation[4], selected_instance->rotation[5]);
        ImGui::Text("          [%.5g %.5g %.5g]", selected_instance->rotation[6],
                    selected_instance->rotation[7], selected_instance->rotation[8]);
        draw_hex(state, selected_instance->offset, selected_instance->physical_size);
    } else {
        ImGui::TextDisabled("Select a chunk or scene instance to inspect it.");
    }
}

} // namespace rwsman::ui
