#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "navigation.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "rwsman/index_builders.hpp"
#include "rwsman/mission_lookup.hpp"

#include <imgui.h>

#include <type_traits>
#include <variant>

namespace rwsman::ui {

namespace {

void draw_program_operand(const csf::ProgramOperand& operand) {
    ImGui::PushID(static_cast<int>(operand.source.entry_index));
    ImGui::TextDisabled("%s%s%s", operand.tag.c_str(), operand.tag.empty() ? "" : ": ",
                        program_operand_text(operand).c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Entry %u | offset 0x%llX\n%s", operand.source.entry_index,
                          static_cast<unsigned long long>(operand.source.range.offset),
                          path_utf8(operand.source.file).c_str());
    if (!operand.children.empty()) {
        ImGui::Indent();
        for (const auto& child : operand.children) draw_program_operand(child);
        ImGui::Unindent();
    }
    ImGui::PopID();
}

void draw_program_instruction_list(AppState& state, const std::size_t document, const std::size_t script,
                                   const std::vector<csf::ProgramInstruction>& instructions, const char* heading) {
    if (instructions.empty()) return;
    section(heading);
    for (const auto& row : csf::program_structure(instructions)) {
        const auto& instruction = *row.instruction;
        ImGui::PushID(static_cast<int>(instruction.source.entry_index));
        ImGui::Indent(static_cast<float>(row.depth) * 18.0F);
        const bool comment = instruction.opcode == "COMENTARIO" ||
                             instruction.opcode == "COMENTARIO_ACCION";
        const bool focused = state.ui.script_focus_entry && *state.ui.script_focus_entry == instruction.source.entry_index;
        const bool selected = state.selection.kind == SelectionRef::Kind::program_instruction &&
                              state.selection.c == instruction.source.entry_index;
        // The opcode line is the row's click target: it selects the instruction.
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##instruction", selected || focused, ImGuiSelectableFlags_AllowOverlap, {0.0F, 0.0F}))
            navigate_to(state, SelectionRef::program_instruction(document, script, instruction.source.entry_index),
                        {.frame = false});
        if (focused && state.ui.script_scroll_pending) {
            ImGui::SetScrollHereY(0.25F);
            state.ui.script_scroll_pending = false;
        }
        ImGui::SetCursorScreenPos(start);
        if (comment) ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        else ImGui::PushFont(font(Font::sans_bold));
        ImGui::Text("%s", instruction.opcode.c_str());
        if (comment) ImGui::PopStyleColor();
        else ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("entry %u | 0x%llX", instruction.source.entry_index,
                            static_cast<unsigned long long>(instruction.source.range.offset));
        if (!instruction.operands.empty()) {
            ImGui::Indent();
            for (const auto& operand : instruction.operands) draw_program_operand(operand);
            ImGui::Unindent();
        }
        ImGui::Unindent(static_cast<float>(row.depth) * 18.0F);
        ImGui::PopID();
    }
}

} // namespace

void draw_script_view(AppState& state) {
    auto& mission = state.mission;
    auto& workspace = state.workspace;
    auto& geometry_preview = state.preview;
    auto& selected_program_document = state.selected_program_document;
    auto& selected_program_script = state.selected_program_script;
    ImGui::PushFont(font(Font::mono));
                if (selected_program_document < mission.programs.size() &&
                    selected_program_script <
                        mission.programs[selected_program_document].second.scripts().size()) {
                    const auto& [path, program] = mission.programs[selected_program_document];
                    const auto& script = program.scripts()[selected_program_script];
                    ImGui::PushStyleColor(ImGuiCol_Header, transparent());
                    const bool resources_open = ImGui::CollapsingHeader("Declared resources and globals");
                    ImGui::PopStyleColor();
                    if (resources_open) {
                        for (const auto& list : program.resources())
                            ImGui::BulletText("%s: %zu value%s [entry %u]", list.name.c_str(),
                                              list.values.size(), list.values.size() == 1 ? "" : "s",
                                              list.source.entry_index);
                        if (!program.global_variables().empty()) {
                            ImGui::SeparatorText("Global variables");
                            for (const auto& variable : program.global_variables())
                                ImGui::BulletText("%d %s %s%s = %s", variable.id,
                                                  variable.type.c_str(), variable.name.c_str(),
                                                  variable.is_array ? "[]" : "",
                                                  program_operand_text(variable.initial_value).c_str());
                        }
                    }
                    ImGui::Text("%s", script.name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("ID %d | %s", script.id,
                                        path_utf8(path.filename()).c_str());
                    ImGui::Text("Folder: %s", script.folder.empty() ? "(root)" : script.folder.c_str());
                    ImGui::TextDisabled("source entry %u | offset 0x%llX", script.source.entry_index,
                                        static_cast<unsigned long long>(script.source.range.offset));
                    if (selected_program_document < mission.program_documents.size() &&
                        [] {
                            ImGui::PushStyleColor(ImGuiCol_Header, transparent());
                            const bool open = ImGui::CollapsingHeader("Raw CSFFBS source");
                            ImGui::PopStyleColor();
                            return open;
                        }()) {
                        const auto& raw_document =
                            mission.program_documents[selected_program_document];
                        if (const auto* raw =
                                find_csf_node(raw_document.roots(), script.source.entry_index))
                            draw_csf_subtree(raw_document, *raw);
                    }
                    ImGui::Text("Trigger %s | enabled %s | valid %s",
                                script.flags.trigger ? (*script.flags.trigger ? "yes" : "no") : "?",
                                script.flags.enabled ? (*script.flags.enabled ? "yes" : "no") : "?",
                                script.flags.valid ? (*script.flags.valid ? "yes" : "no") : "?");
                    if (!script.local_variables.empty()) {
                        ImGui::SeparatorText("Local variables");
                        if (ImGui::BeginTable("program_variables", 6,
                                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                                  ImGuiTableFlags_ScrollY)) {
                            ImGui::TableSetupColumn("ID");
                            ImGui::TableSetupColumn("Name");
                            ImGui::TableSetupColumn("Type");
                            ImGui::TableSetupColumn("Array");
                            ImGui::TableSetupColumn("Initial");
                            ImGui::TableSetupColumn("Source");
                            ImGui::TableHeadersRow();
                            for (const auto& variable : script.local_variables) {
                                ImGui::TableNextRow();
                                ImGui::TableNextColumn(); ImGui::Text("%d", variable.id);
                                ImGui::TableNextColumn(); ImGui::TextUnformatted(variable.name.c_str());
                                ImGui::TableNextColumn(); ImGui::TextUnformatted(variable.type.c_str());
                                ImGui::TableNextColumn(); ImGui::TextUnformatted(variable.is_array ? "yes" : "no");
                                ImGui::TableNextColumn(); ImGui::TextUnformatted(program_operand_text(variable.initial_value).c_str());
                                ImGui::TableNextColumn(); ImGui::Text("entry %u", variable.source.entry_index);
                            }
                            ImGui::EndTable();
                        }
                    }
                    if (!script.events.empty()) {
                        ImGui::SeparatorText("Events");
                        for (const auto& event : script.events)
                            ImGui::BulletText("%s  [entry %u]", event.name.c_str(),
                                              event.source.entry_index);
                    }
                    draw_program_instruction_list(state, selected_program_document, selected_program_script, script.conditions, "Conditions");
                    draw_program_instruction_list(state, selected_program_document, selected_program_script, script.actions, "Actions");
                    ImGui::SeparatorText("Typed references");
                    bool any_reference{};
                    for (const auto& reference : mission.program_references.references()) {
                        if (reference.source.file != path || reference.owner_script != script.id)
                            continue;
                        any_reference = true;
                        ImGui::PushID(static_cast<int>(reference.source.entry_index));
                        ImGui::Text("%s %s -> %s",
                                    csf::program_reference_kind_name(reference.kind),
                                    reference.display_value.c_str(),
                                    csf::program_reference_status_name(reference.status));
                        if (reference.targets.size() == 1 && mission.scene &&
                            reference.targets[0].file == mission.scene->source_path()) {
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Select mission object")) {
                                geometry_preview.select_mission_entry(
                                    reference.targets[0].entry_index);
                                workspace = Workspace::mission;
                            }
                        }
                        ImGui::PopID();
                    }
                    if (!any_reference) ImGui::TextDisabled("No typed reference operands");
                } else {
                    ImGui::TextDisabled("Select a script from the outline.");
                }
    ImGui::PopFont();
}

} // namespace rwsman::ui
