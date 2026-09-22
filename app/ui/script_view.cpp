#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "csf/source_text.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"
#include "rwsman/index_builders.hpp"
#include "rwsman/mission_lookup.hpp"

#include <imgui.h>

#include <string>
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

// Editable source of one script, kept while the user types. It reloads when
// another script is selected or the editor changes underneath it.
struct ScriptSource {
    std::filesystem::path file;
    std::int32_t script{};
    std::uint64_t revision{~std::uint64_t{}};
    std::string text;
    bool modified{};
    std::vector<std::string> messages;
    bool error{};
};

int resize_callback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::string*>(data->UserData);
        text->resize(static_cast<std::size_t>(data->BufTextLen));
        data->Buf = text->data();
    }
    return 0;
}

std::optional<std::size_t> editor_file(const AppState& state, const std::filesystem::path& path) {
    const auto& editor = *state.mission.editor;
    return editor.find_file(path.lexically_relative(editor.package_root()));
}

// Keeps the script with `id` selected once the views include the edit.
void reselect_script(AppState& state, const std::size_t document, const std::int32_t id) {
    state.mission.pending_script = std::pair{document, id};
}

void report(ScriptSource& source, const csf::EditResult& result) {
    source.messages = result.warnings;
    source.error = !result.applied && !result.message.starts_with("No change");
    if (source.error) source.messages.insert(source.messages.begin(), result.message);
}

void draw_script_source_editor(AppState& state, const std::size_t document, const csf::ProgramScript& script,
                               const std::filesystem::path& path) {
    static ScriptSource source;
    if (!mission_editable(state)) return;
    auto& editor = *state.mission.editor;
    const auto file = editor_file(state, path);
    if (!file) return;
    ImGui::PushStyleColor(ImGuiCol_Header, transparent());
    const bool open = ImGui::CollapsingHeader("Edit script source", ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor();
    if (!open) return;
    const bool other = source.file != path || source.script != script.id;
    if (other || (!source.modified && source.revision != editor.revision())) {
        source.file = path;
        source.script = script.id;
        source.revision = editor.revision();
        source.text = editor.script_text(*file, script.id).value_or("");
        source.modified = false;
        if (other) source.messages.clear();
    }
    dim_text("Recompilable CSFFBS text: one instruction per line inside { }; (TAG value) operands; "
             "2.0 is a real, 2 an integer. Apply checks it against every shipped script and this mission.");
    // Flags are edited through the text so that everything goes through one path.
    const auto set_flag = [&](const char* flag, const bool value) {
        try {
            auto tree = csf::parse_source_value(source.text);
            if (auto* flags = tree.child(".FLAGS"))
                if (auto* node = flags->child(flag)) node->set_int(value ? 1 : 0);
            const auto result = editor.set_script_text(*file, script.id, csf::to_source_text(tree) + "\n");
            report(source, result);
            if (result.applied) {
                apply_mission_edit(state, result);
                reselect_script(state, document, script.id);
            }
        } catch (const std::exception& error) {
            source.messages = {error.what()};
            source.error = true;
        }
    };
    bool enabled = script.flags.enabled.value_or(false), trigger = script.flags.trigger.value_or(false);
    ImGui::BeginDisabled(source.modified);
    if (ImGui::Checkbox("Enabled", &enabled)) set_flag(".ENABLED", enabled);
    ImGui::SameLine();
    if (ImGui::Checkbox("Trigger (global, started by events)", &trigger)) set_flag(".TRIGGER", trigger);
    ImGui::EndDisabled();
    const float height = std::max(ImGui::GetContentRegionAvail().y * 0.55F, 240.0F * ui_scale());
    if (ImGui::InputTextMultiline("##script_source", source.text.data(), source.text.capacity() + 1,
                                  {-FLT_MIN, height},
                                  ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
                                  resize_callback, &source.text))
        source.modified = true;
    const bool apply_key = ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl &&
                           ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    ImGui::BeginDisabled(!source.modified);
    if (ImGui::Button("Apply (Ctrl+Enter)") || (apply_key && source.modified)) {
        const auto result = editor.set_script_text(*file, script.id, source.text);
        report(source, result);
        if (result.applied) {
            source.modified = false;
            std::int32_t id = script.id;
            try {
                if (const auto* value = csf::parse_source_value(source.text).child(".ID"))
                    id = value->as_int().value_or(id);
            } catch (...) {
            }
            apply_mission_edit(state, result);
            reselect_script(state, document, id);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        source.revision = ~std::uint64_t{};
        source.modified = false;
        source.messages.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("New script")) {
        std::int32_t id{};
        const auto result =
            editor.add_script(*file, csf::MissionEditor::script_template(editor.next_script_id(*file), "NEW_SCRIPT"), &id);
        report(source, result);
        if (apply_mission_edit(state, result)) reselect_script(state, document, id);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete script")) {
        const auto result = editor.delete_script(*file, script.id, ImGui::GetIO().KeyShift);
        report(source, result);
        if (apply_mission_edit(state, result)) state.mission.pending_script = std::pair{document, std::int32_t{-1}};
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refuses while actors run it or operands name it; hold Shift to force");
    for (const auto& message : source.messages)
        token_text(source.error ? Token::error : Token::warn, "%s", message.c_str());
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
                    draw_script_source_editor(state, selected_program_document, script, path);
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
