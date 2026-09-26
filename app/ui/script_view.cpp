#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "csf/mission_components.hpp"
#include "csf/mission_recipes.hpp"
#include "csf/script_signatures.hpp"
#include "csf/source_text.hpp"
#include "ui/icons.hpp"
#include "ui/script_editor.hpp"
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
    // The text's own check, redone when it changes: a syntax error's line and
    // what the signature table has never seen.
    std::string checked;
    std::optional<std::size_t> error_line;
    std::string error_message;
    std::vector<std::string> advisories;
};

void check_source(ScriptSource& source) {
    if (source.checked == source.text) return;
    source.checked = source.text;
    source.error_line.reset();
    source.error_message.clear();
    source.advisories.clear();
    try {
        const auto tree = csf::parse_source_value(source.text);
        for (const auto& finding : csf::check_script_against_signatures(tree))
            source.advisories.push_back(finding.message);
    } catch (const csf::SourceTextError& error) {
        source.error_line = error.line();
        source.error_message = error.what();
    } catch (const std::exception& error) {
        source.error_message = error.what();
    }
}

// Ctrl+click on an operand: the record, script or group it names.
void follow_operand(AppState& state, const OperandAt& operand) {
    const auto number = [&](const std::size_t k) -> std::optional<std::int32_t> {
        if (k >= operand.values.size()) return std::nullopt;
        try {
            return std::stoi(operand.values[k]);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    using Kind = MissionRecordKey::Kind;
    std::optional<MissionRecordKey> key;
    if (operand.tag == "BICHO" && number(0)) key = MissionRecordKey{Kind::actor, *number(0), 0};
    else if (operand.tag == "ZONA" && number(0)) key = MissionRecordKey{Kind::area, *number(0), 0};
    else if (operand.tag == "DUMMY" && number(0)) key = MissionRecordKey{Kind::dummy, *number(0), 0};
    else if (operand.tag == "GRUPO_PATHPOINT" && number(0)) key = MissionRecordKey{Kind::nav_group, *number(0), 0};
    else if (operand.tag == "PATHPOINT" && number(0) && number(1)) key = MissionRecordKey{Kind::nav_point, *number(0), *number(1)};
    if (key) {
        if (!mission_record_entry(*state.mission.scene, *key)) return state.warn(operand.tag + " " + operand.values[0] + " is not in the mission");
        state.workspace = Workspace::mission;
        select_mission_record(state, *key, true);
        return;
    }
    if ((operand.tag == "SCRIPT" || operand.tag == "TRIGGER") && number(0)) {
        for (std::size_t document = 0; document < state.mission.programs.size(); ++document) {
            const auto& scripts = state.mission.programs[document].second.scripts();
            for (std::size_t i = 0; i < scripts.size(); ++i)
                if (scripts[i].id == *number(0)) {
                    state.selected_program_document = document;
                    state.selected_program_script = i;
                    state.selection = {SelectionRef::Kind::program_script, document, i};
                    return;
                }
        }
        return state.warn("Script " + operand.values[0] + " is not in the mission");
    }
    state.info("Ctrl+click follows actors, zones, markers, routes and scripts; " + operand.tag + " names none of them");
}

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
    // A script a recipe made is its component's: read-only here until detached (S1).
    const csf::MissionComponent* owner = nullptr;
    const bool in_cutscene = lower_ascii(path_utf8(path.extension())) == ".csc";
    for (const auto& component : mission_component_list(state))
        if (std::ranges::find(component.owns, csf::MissionRecordId{in_cutscene ? csf::MissionRecordId::Type::cutscene_script
                                                                                 : csf::MissionRecordId::Type::script,
                                                                      script.id}) != component.owns.end())
            owner = &component;
    if (owner) {
        token_text(Token::inferred, "%s Made by %s: edit it on its card, or detach it to edit this text.",
                   icons::LC_COMPONENT, csf::component_title(*owner).c_str());
        const auto id = owner->id;
        if (ImGui::SmallButton("Detach and edit")) {
            apply_mission_edit(state, csf::detach_component(editor, id));
            reselect_script(state, document, script.id);
        }
    }
    check_source(source);
    const float height = std::max(ImGui::GetContentRegionAvail().y * 0.55F, 240.0F * ui_scale());
    const auto edited = script_code_editor("##script_source", source.text, {-FLT_MIN, height}, owner != nullptr,
                                           source.error_line);
    if (edited.changed) source.modified = true;
    const bool apply_key = ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl &&
                           ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (edited.ctrl_clicked) follow_operand(state, *edited.ctrl_clicked);
    // What the current line's opcode takes, and the text's own problems.
    if (const auto hint = signature_hint(line_opcode(edited.cursor_line)); !hint.empty()) dim_text("%s", hint.c_str());
    else dim_text("Tab completes opcodes and operand tags; Ctrl+click an operand to go to it.");
    if (!source.error_message.empty()) token_text(Token::error, "%s", source.error_message.c_str());
    for (std::size_t i = 0; i < source.advisories.size() && i < 4; ++i)
        token_text(Token::warn, "%s", source.advisories[i].c_str());
    ImGui::BeginDisabled(!source.modified || owner);
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
    // A new script already listening to an event (S4).
    if (ImGui::Button("New script...")) ImGui::OpenPopup("##new_script");
    if (ImGui::BeginPopup("##new_script")) {
        static std::array<char, 64> name{"NEW_SCRIPT"};
        static int event = 0;
        static bool global = true;
        std::vector<std::string> events{"START_GAME", "INIT", "BICHO_ENT_ZONA", "MORIBUNDO", "MUERTO", "EVT_GHOST_USADO",
                                        "IA_CHANGE_STATE"};
        // Events the mission's scripts raise.
        for (const auto& [path_value, program] : state.mission.programs)
            for (const auto& other : program.scripts())
                for (const auto& instruction : other.actions)
                    if (instruction.opcode == "SEND_EVENT")
                        for (const auto& operand : instruction.operands)
                            if (const auto* raised = std::get_if<std::string>(&operand.value);
                                operand.tag == "EVENT" && raised && std::ranges::find(events, *raised) == events.end())
                                events.push_back(*raised);
        ImGui::InputText("Name", name.data(), name.size());
        std::vector<const char*> labels;
        for (const auto& value : events) labels.push_back(value.c_str());
        event = std::min(event, static_cast<int>(labels.size()) - 1);
        ImGui::Combo("Listens to", &event, labels.data(), static_cast<int>(labels.size()));
        ImGui::Checkbox("Global (a trigger); off: an actor script", &global);
        if (ImGui::Button("Create")) {
            std::int32_t id{};
            const auto text = csf::script_text(editor.next_script_id(*file), name.data(), global ? 1 : 0,
                                               {events[static_cast<std::size_t>(event)]}, {});
            const auto result = editor.add_script(*file, text, &id);
            report(source, result);
            if (apply_mission_edit(state, result)) reselect_script(state, document, id);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(owner != nullptr);
    if (ImGui::Button("Delete script")) {
        const auto result = editor.delete_script(*file, script.id, ImGui::GetIO().KeyShift);
        report(source, result);
        if (apply_mission_edit(state, result)) state.mission.pending_script = std::pair{document, std::int32_t{-1}};
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
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
