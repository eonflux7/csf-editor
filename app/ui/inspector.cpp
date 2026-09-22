#include "app_state.hpp"
#include "ui/ui.hpp"

#include "app_util.hpp"
#include "commands.hpp"
#include "navigation.hpp"
#include "references.hpp"
#include "rws/decoded.hpp"
#include "rws/scene_export.hpp"
#include "rwsman/index_builders.hpp"
#include "rwsman/mission_lookup.hpp"
#include "ui/fonts.hpp"
#include "ui/icons.hpp"
#include "ui/property_grid.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace rwsman::ui {
namespace {

using Options = PropertyGrid::Options;

Options derived(const Provenance provenance, std::string evidence) {
    Options options;
    options.provenance = provenance;
    options.evidence = std::move(evidence);
    return options;
}

// Breadcrumb: "FR01.scn > Actors > Guard_01 > class 0x4A1 > Objetos.bdd#88".
void draw_breadcrumb(AppState& state, const SelectionRef& ref) {
    const auto segments = selection_breadcrumb(state, ref);
    if (segments.empty()) return;
    const float available = ImGui::GetContentRegionAvail().x;
    const float separator = ImGui::CalcTextSize(" > ").x;
    // Drop leading segments until the trail fits, keeping the last one.
    std::size_t first = 0;
    const auto total = [&](const std::size_t from) {
        float width = 0.0F;
        for (std::size_t i = from; i < segments.size(); ++i)
            width += ImGui::CalcTextSize(segments[i].label.c_str()).x + separator;
        return width;
    };
    while (first + 1 < segments.size() && total(first) + (first > 0 ? separator : 0.0F) > available) ++first;
    if (first > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::TextUnformatted("...");
        ImGui::PopStyleColor();
        ImGui::SameLine(0.0F, 4.0F);
    }
    for (std::size_t i = first; i < segments.size(); ++i) {
        const auto& segment = segments[i];
        if (i > first) {
            ImGui::SameLine(0.0F, 2.0F);
            ImGui::PushStyleColor(ImGuiCol_Text, color(Token::line));
            ImGui::TextUnformatted(">");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.0F, 2.0F);
        }
        const bool last = i + 1 == segments.size();
        const bool navigable = !segment.target.empty() && segment.target != ref;
        ImGui::PushStyleColor(ImGuiCol_Text, color(last ? Token::text : (navigable ? Token::inferred : Token::text_dim)));
        ImGui::PushFont(font(Font::mono));
        ImGui::TextUnformatted(segment.label.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        if (navigable && ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) navigate_to(state, segment.target);
        }
    }
}

void draw_header(AppState& state, const SelectionRef& ref, const int instance, const bool primary) {
    draw_breadcrumb(state, ref);
    ImGui::PushFont(font(Font::sans_bold));
    const auto title = selection_title(state, ref);
    ImGui::TextUnformatted(title.empty() ? "Nothing selected" : title.c_str());
    ImGui::PopFont();
    if (!ref.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::text_dim));
        ImGui::TextUnformatted(selection_kind_label(state, ref).c_str());
        ImGui::PopStyleColor();
        if (state.has_diagnostic(ref)) {
            ImGui::SameLine();
            token_text(Token::warn, "%s", icons::LC_TRIANGLE_ALERT);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("This selection carries a diagnostic; see the Diagnostics panel");
        }
        // Icon actions on the right: copy identity, pin, duplicate.
        const float button = ImGui::CalcTextSize(icons::LC_COPY).x + ImGui::GetStyle().FramePadding.x * 2.0F;
        const int count = primary ? 3 : 2;
        const float x = ImGui::GetWindowContentRegionMax().x - button * static_cast<float>(count) -
                        ImGui::GetStyle().ItemSpacing.x * static_cast<float>(count - 1) - 4.0F;
        ImGui::SameLine(std::max(x, ImGui::GetCursorPosX()));
        ImGui::PushID(instance);
        if (icon_button("copy_identity", icons::LC_COPY, "Copy identity (Ctrl+Shift+C)")) {
            const auto identity = selection_identity(state, ref);
            if (!identity.empty()) copy_to_clipboard(state, identity, "identity");
        }
        ImGui::SameLine();
        const bool pinned = primary && state.ui.inspector_pin.has_value();
        if (primary && icon_button("pin", pinned ? icons::LC_PIN_OFF : icons::LC_PIN,
                                   pinned ? "Unpin: follow the selection again" : "Pin: keep showing this selection",
                                   pinned)) {
            if (pinned)
                state.ui.inspector_pin.reset();
            else
                state.ui.inspector_pin = ref;
        }
        if (primary) ImGui::SameLine();
        if (icon_button("duplicate", icons::LC_EXTERNAL_LINK, "Open a second inspector on this selection")) {
            state.ui.extra_inspectors.push_back({state.ui.next_extra_inspector_id++, ref, true});
        }
        ImGui::PopID();
        const auto identity = selection_identity(state, ref);
        if (!identity.empty()) copyable_text(identity.c_str(), identity.c_str(), "Click to copy the full identity");
    }
}

void draw_chunk(AppState& state, const rws::Chunk& chunk) {
    auto& document = *state.document;
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##chunk_identity"); grid) {
            grid.text("type", rws::chunk_name(chunk.type), derived(Provenance::proven, "RenderWare chunk type table"));
            char type[16];
            std::snprintf(type, sizeof(type), "0x%08X", chunk.type);
            grid.text("type ID", type, derived(Provenance::proven, "Chunk header word"));
            if (const auto name = state.display_names.find(chunk.offset); name != state.display_names.end())
                grid.text("name", name->second, derived(Provenance::inferred, "Resolved from the chunk's plug-in data or CSF placements; not part of the chunk header"));
            grid.offset("offset", chunk.offset);
            grid.offset("payload", chunk.payload_offset);
            grid.integer("declared size", chunk.declared_size, derived(Provenance::proven, "Chunk header size field"));
            grid.integer("available", static_cast<std::int64_t>(chunk.available_size),
                         derived(chunk.truncated ? Provenance::diagnosed : Provenance::proven,
                                 chunk.truncated ? "The file ends before the declared size: the chunk is truncated"
                                                 : "Bytes present in the file"));
            const auto version = rws::decode_library_id(chunk.library_id);
            char text[64];
            std::snprintf(text, sizeof(text), "%u.%u.%u.%u build %u", version.major, version.minor,
                          version.revision, version.binary, version.build);
            grid.text("RenderWare", text, derived(Provenance::proven, "Library ID stamp in the chunk header"));
            std::snprintf(text, sizeof(text), "%s (0x%06X)", rws::chunk_vendor_name(rws::chunk_vendor_id(chunk.type)).data(),
                          rws::chunk_vendor_id(chunk.type));
            grid.text("vendor", text, derived(Provenance::proven, "Vendor bits of the chunk type"));
        }
        end_section();
    }
    if (begin_section(state, "Decoded", true)) {
        const auto* owner = find_owning_object(document.chunks(), chunk.offset);
        ImGui::PushFont(font(Font::mono));
        ImGui::PushTextWrapPos(0.0F); // Long decoded lines wrap at the panel edge.
        draw_typed_details(state, chunk, owner ? owner->type : 0);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        end_section();
    }
    if (begin_section(state, "Raw", false)) {
        const auto bytes = document.bytes();
        if (chunk.payload_offset < bytes.size())
            draw_hex_dump(bytes.subspan(static_cast<std::size_t>(chunk.payload_offset),
                                        static_cast<std::size_t>(std::min<std::uint64_t>(
                                            chunk.available_size, bytes.size() - chunk.payload_offset))),
                          chunk.payload_offset, 96);
        if (ImGui::SmallButton("Open in Hex workspace")) state.commands.run("nav.show_in_hex");
        end_section();
    }
}

void draw_instance(AppState& state, const rws::SceneInstance& instance) {
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##instance_identity"); grid) {
            grid.text("name", instance.prototype_name, derived(instance.prototype_name.empty() ? Provenance::unknown : Provenance::proven, "CSF scene instance prototype table"));
            grid.integer("prototype", instance.prototype_id, derived(Provenance::proven, "Scene instance record"));
            grid.integer("instance", instance.instance_id, derived(Provenance::proven, "Scene instance record"));
            grid.offset("offset", instance.offset);
            grid.integer("declared size", instance.declared_size, derived(Provenance::proven, "Record header"));
            grid.integer("physical size", static_cast<std::int64_t>(instance.physical_size), derived(Provenance::proven, "Bytes present"));
        }
        end_section();
    }
    if (begin_section(state, "Transform", true)) {
        if (PropertyGrid grid(state, "##instance_transform"); grid) {
            grid.vec3("position", instance.position.x, instance.position.y, instance.position.z, derived(Provenance::proven, "Scene instance record"));
            for (int row = 0; row < 3; ++row) {
                const char* names[] = {"rotation row 0", "rotation row 1", "rotation row 2"};
                grid.vec3(names[row], instance.rotation[static_cast<std::size_t>(row * 3)],
                          instance.rotation[static_cast<std::size_t>(row * 3 + 1)],
                          instance.rotation[static_cast<std::size_t>(row * 3 + 2)], derived(Provenance::proven, "Scene instance record"));
            }
        }
        end_section();
    }
    if (begin_section(state, "Flags and visibility", true)) {
        if (PropertyGrid grid(state, "##instance_flags"); grid) {
            grid.custom("flags", [&] {
                char text[24];
                std::snprintf(text, sizeof(text), "0x%08X", instance.flags);
                copyable_text(text);
                ImGui::SameLine();
                dim_text("%s", rws::scene_instance_flag_names(instance.flags).c_str());
            }, derived(Provenance::proven, "Named bits from the recovered flag table"));
            grid.integer("matrix flags", instance.matrix_flags, derived(Provenance::unknown, "Preserved; meaning not decoded"));
            grid.real("max distance", instance.maximum_visibility_distance, derived(Provenance::proven, "Scene instance record"));
            grid.real("min distance", instance.minimum_visibility_distance, derived(Provenance::proven, "Scene instance record"));
            grid.real("fade range", instance.visibility_fade_range, derived(Provenance::proven, "Scene instance record"));
        }
        end_section();
    }
    if (begin_section(state, "Raw", false)) {
        const auto bytes = state.document->bytes();
        if (instance.offset < bytes.size())
            draw_hex_dump(bytes.subspan(static_cast<std::size_t>(instance.offset),
                                        static_cast<std::size_t>(std::min<std::uint64_t>(instance.physical_size, bytes.size() - instance.offset))),
                          instance.offset, 96);
        if (ImGui::SmallButton("Open in Hex workspace")) state.commands.run("nav.show_in_hex");
        end_section();
    }
}

void draw_script(AppState& state, const SelectionRef& ref) {
    const auto& [path, program] = state.mission.programs[ref.a];
    const auto& script = program.scripts()[ref.b];
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##script_identity"); grid) {
            grid.text("name", script.name, derived(Provenance::proven, "Script .NOMBRE"));
            grid.integer("ID", script.id, derived(Provenance::proven, "Script .ID"));
            grid.text("folder", script.folder.empty() ? "(root)" : script.folder, derived(Provenance::proven, "Folder path in the script file"));
            grid.text("file", path_utf8(path.filename()), derived(Provenance::proven, "Program document"));
            grid.offset("offset", script.source.range.offset);
            const auto flag = [&](const char* key, const std::optional<bool>& value) {
                if (value) grid.boolean(key, *value, derived(Provenance::proven, "Parsed script flag"));
                else grid.text(key, "unknown", derived(Provenance::unknown, "Flag not present in the file"));
            };
            flag("trigger", script.flags.trigger);
            flag("enabled", script.flags.enabled);
            flag("valid", script.flags.valid);
            grid.integer("conditions", static_cast<std::int64_t>(script.conditions.size()), derived(Provenance::proven, "Count"));
            grid.integer("actions", static_cast<std::int64_t>(script.actions.size()), derived(Provenance::proven, "Count"));
        }
        end_section();
    }
    if (!script.local_variables.empty() && begin_section(state, "Variables", false)) {
        if (PropertyGrid grid(state, "##script_variables"); grid)
            for (const auto& variable : script.local_variables) {
                const auto key = variable.type + " " + std::to_string(variable.id);
                grid.text(key.c_str(), variable.name + (variable.is_array ? "[]" : ""), derived(Provenance::proven, "Local variable declaration"));
            }
        end_section();
    }
    if (!script.events.empty() && begin_section(state, "Events", false)) {
        if (PropertyGrid grid(state, "##script_events"); grid)
            for (const auto& event : script.events)
                grid.text(("entry " + std::to_string(event.source.entry_index)).c_str(), event.name, derived(Provenance::proven, "Event declaration"));
        end_section();
    }
    const auto references = collect_references(state, ref);
    if (!references.empty() && begin_section(state, "References from this script", true)) {
        int shown = 0;
        if (PropertyGrid grid(state, "##script_refs"); grid)
            for (const auto& row : references) {
                if (++shown > 30) break;
                Options options = derived(row.provenance, row.evidence);
                grid.link(row.label.c_str(), row.detail, row.target, options);
            }
        if (references.size() > 30) dim_text("%zu more in the References panel", references.size() - 30);
        end_section();
    }
    if (ref.a < state.mission.program_documents.size()) {
        const auto& document = state.mission.program_documents[ref.a];
        if (const auto* node = find_csf_node(document.roots(), script.source.entry_index);
            node && begin_section(state, "Raw", false)) {
            draw_csf_subtree(document, *node);
            end_section();
        }
    }
}

// `document` is the index of the program the instruction belongs to.
void draw_operand_rows(AppState& state, PropertyGrid& grid, const std::size_t document,
                       const csf::ProgramOperand& operand, const std::string& prefix,
                       const std::vector<const csf::ProgramReference*>& references) {
    const std::string key = prefix + (operand.tag.empty() ? "(value)" : operand.tag);
    const auto match = std::ranges::find_if(references, [&](const csf::ProgramReference* reference) {
        return reference->source.entry_index == operand.source.entry_index;
    });
    Options options = derived(Provenance::proven, "Operand of the instruction");
    if (document < state.mission.program_documents.size())
        options.raw = raw_slice_for_entry(state.mission.program_documents[document], operand.source.entry_index);
    SelectionRef target;
    if (match != references.end()) {
        options = derived(provenance_of((*match)->status), std::string("Resolver: ") + csf::program_reference_status_name((*match)->status) + ((*match)->detail.empty() ? "" : " - " + (*match)->detail));
        if ((*match)->targets.size() == 1 && state.mission.scene && (*match)->targets[0].file == state.mission.scene->source_path())
            target = SelectionRef::mission_entry((*match)->targets[0].entry_index);
        else if ((*match)->targets.size() == 1)
            target = program_entry_ref(state, (*match)->targets[0].file, (*match)->targets[0].entry_index);
        options.raw = {};
    }
    const auto value = program_operand_text(operand);
    if (!target.empty())
        grid.link(key.c_str(), value, target, options);
    else
        grid.text(key.c_str(), value, options);
    for (const auto& child : operand.children)
        draw_operand_rows(state, grid, document, child, prefix + "  ", references);
}

void draw_instruction(AppState& state, const SelectionRef& ref) {
    const auto& script = state.mission.programs[ref.a].second.scripts()[ref.b];
    const csf::ProgramInstruction* instruction = nullptr;
    for (const auto* list : {&script.conditions, &script.actions})
        for (const auto& candidate : *list)
            if (candidate.source.entry_index == ref.c) instruction = &candidate;
    if (!instruction) {
        dim_text("This instruction is no longer in the loaded script.");
        return;
    }
    if (begin_section(state, "Instruction", true)) {
        if (PropertyGrid grid(state, "##instruction"); grid) {
            grid.text("opcode", instruction->opcode, derived(Provenance::proven, "Instruction .TIPO"));
            grid.integer("entry", instruction->source.entry_index, derived(Provenance::proven, "CSFFBS entry"));
            grid.offset("offset", instruction->source.range.offset);
            grid.link("script", script.name, SelectionRef::program_script(ref.a, ref.b), derived(Provenance::proven, "Owning script"));
        }
        end_section();
    }
    if (begin_section(state, "Operands", true)) {
        std::vector<const csf::ProgramReference*> references;
        for (const auto& reference : state.mission.program_references.references())
            if (reference.source.file == state.mission.programs[ref.a].first && reference.owner_script == script.id)
                references.push_back(&reference);
        if (PropertyGrid grid(state, "##operands"); grid)
            for (const auto& operand : instruction->operands)
                draw_operand_rows(state, grid, ref.a, operand, "", references);
        end_section();
    }
    if (ref.a < state.mission.program_documents.size()) {
        const auto& document = state.mission.program_documents[ref.a];
        if (const auto* node = find_csf_node(document.roots(), ref.c); node && begin_section(state, "Raw", false)) {
            const auto slice = raw_slice_for_entry(document, ref.c);
            if (slice.valid() && slice.offset < document.bytes().size())
                draw_hex_dump(document.bytes().subspan(static_cast<std::size_t>(slice.offset), static_cast<std::size_t>(std::min<std::uint64_t>(slice.size, document.bytes().size() - slice.offset))), slice.offset, 64);
            draw_csf_subtree(document, *node);
            end_section();
        }
    }
}

void draw_database_record(AppState& state, const SelectionRef& ref) {
    const auto entry = static_cast<std::uint32_t>(ref.a);
    const csf::ObjectDefinition* definition = nullptr;
    if (state.mission.objects)
        for (const auto& candidate : state.mission.objects->definitions())
            if (candidate.source.entry_index == entry && path_utf8(candidate.source.file) == ref.path) definition = &candidate;
    if (definition) {
        if (begin_section(state, "Identity", true)) {
            if (PropertyGrid grid(state, "##class_identity"); grid) {
                if (definition->name) grid.text("name", *definition->name, derived(Provenance::proven, "Record .NOMBRE"));
                if (definition->class_id) grid.integer("class", *definition->class_id, derived(Provenance::proven, "Record .CLASSID"));
                if (definition->id) grid.integer("ID", *definition->id, derived(Provenance::proven, "Record .ID"));
                grid.text("file", path_utf8(definition->source.file.filename()), derived(Provenance::proven, "Object database"));
                grid.integer("entry", entry, derived(Provenance::proven, "CSFFBS entry"));
                grid.offset("offset", definition->source.range.offset);
                if (!definition->weapon_ids.empty()) {
                    std::string ids;
                    for (const auto id : definition->weapon_ids) ids += (ids.empty() ? "" : ", ") + std::to_string(id);
                    grid.text("weapon IDs", ids, derived(Provenance::proven, "Record weapon list"));
                }
            }
            end_section();
        }
        if (!definition->references.empty() && begin_section(state, "Resources", true)) {
            if (PropertyGrid grid(state, "##class_resources"); grid)
                for (const auto& reference : definition->references) {
                    Options options = derived(Provenance::unknown, "No mission resource index");
                    SelectionRef target;
                    if (state.mission.graph) {
                        const auto resolution = state.mission.graph->index().resolve(reference.path);
                        options = derived(provenance_of(resolution.status), std::string("Resolver: ") + csf::resolution_status_name(resolution.status) + " (" + reference.field + ")");
                        if (resolution.candidate_indices.size() == 1)
                            target = SelectionRef::resource_path(path_utf8(state.mission.graph->index().resources()[resolution.candidate_indices.front()].path));
                    }
                    grid.link(csf::object_reference_kind_name(reference.kind), reference.path, target, options);
                }
            end_section();
        }
        const auto references = collect_references(state, ref);
        if (!references.empty() && begin_section(state, "Actors of this class", true)) {
            if (PropertyGrid grid(state, "##class_actors"); grid) {
                int shown = 0;
                for (const auto& row : references) {
                    if (++shown > 40) break;
                    grid.link(row.label.c_str(), row.detail, row.target, derived(row.provenance, row.evidence));
                }
            }
            if (references.size() > 40) dim_text("%zu more in the References panel", references.size() - 40);
            end_section();
        }
        return;
    }
    if (state.mission.animations)
        for (const auto& record : state.mission.animations->records())
            if (record.source.entry_index == entry && path_utf8(record.source.file) == ref.path) {
                if (begin_section(state, "Identity", true)) {
                    if (PropertyGrid grid(state, "##anim_identity"); grid) {
                        grid.text("name", record.logical_name, derived(Provenance::proven, "Anims.bdd record"));
                        if (record.id) grid.integer("ID", *record.id, derived(Provenance::proven, "Anims.bdd record"));
                        if (record.loop) grid.boolean("loop", *record.loop, derived(Provenance::proven, "Anims.bdd record"));
                        if (record.blend_in) grid.real("blend in", *record.blend_in, derived(Provenance::proven, "Anims.bdd record"));
                        grid.offset("offset", record.source.range.offset);
                    }
                    end_section();
                }
                if (begin_section(state, "Variants", true)) {
                    if (PropertyGrid grid(state, "##anim_variants"); grid)
                        for (const auto& variant : record.variants) {
                            Options options = derived(Provenance::unknown, "Not resolved");
                            SelectionRef target;
                            if (variant.resolution) {
                                options = derived(provenance_of(variant.resolution->status), std::string("Resolver: ") + csf::resolution_status_name(variant.resolution->status));
                                if (variant.resolution->candidate_indices.size() == 1 && state.mission.graph)
                                    target = SelectionRef::resource_path(path_utf8(state.mission.graph->index().resources()[variant.resolution->candidate_indices.front()].path));
                            }
                            grid.link(variant.field.c_str(), variant.reference, target, options);
                        }
                    end_section();
                }
                return;
            }
    dim_text("This database record is no longer loaded.");
}

void draw_resource(AppState& state, const SelectionRef& ref) {
    const csf::ResourceNode* node = nullptr;
    if (state.mission.graph)
        for (const auto& candidate : state.mission.graph->nodes()) {
            const auto path = candidate.resolved_path.empty() ? std::filesystem::path(candidate.original_reference) : candidate.resolved_path;
            if (path_utf8(path) == ref.path) node = &candidate;
        }
    if (begin_section(state, "Identity", true)) {
        if (PropertyGrid grid(state, "##resource"); grid) {
            grid.text("path", ref.path, derived(Provenance::proven, "Mission resource graph"));
            if (node) {
                grid.text("kind", csf::resource_kind_name(node->kind), derived(Provenance::inferred, "Classified from the file name and referencing field"));
                const char* state_name = node->state == csf::LoadState::available ? "available" : node->state == csf::LoadState::missing ? "missing" : node->state == csf::LoadState::ambiguous ? "ambiguous" : node->state == csf::LoadState::rejected ? "rejected" : "metadata only";
                const auto provenance = node->state == csf::LoadState::available ? Provenance::proven : node->state == csf::LoadState::metadata_only ? Provenance::inferred : Provenance::diagnosed;
                grid.text("state", state_name, derived(provenance, "Resolver load state"));
                grid.text("referenced as", node->original_reference, derived(Provenance::proven, "Text in the referencing file"));
                grid.integer("size", static_cast<std::int64_t>(node->size), derived(Provenance::proven, "File size"));
                if (node->content_hash) {
                    char text[24];
                    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(*node->content_hash));
                    grid.text("content hash", text, derived(Provenance::proven, "Hash of the file bytes"));
                }
            }
        }
        end_section();
    }
    if (node && node->state == csf::LoadState::ambiguous && begin_section(state, "Candidates", true)) {
        if (PropertyGrid grid(state, "##candidates"); grid)
            for (const auto& candidate : node->candidates)
                grid.text("candidate", path_utf8(candidate), derived(Provenance::diagnosed, "Several files match; none was chosen"));
        end_section();
    }
    if (state.mission.graph && begin_section(state, "Used by", true)) {
        const auto uses = state.mission.graph->uses(std::filesystem::path(ref.path));
        if (uses.empty()) dim_text("No dependency edge references this file.");
        if (PropertyGrid grid(state, "##resource_uses"); grid) {
            int shown = 0;
            for (const auto* edge : uses) {
                if (++shown > 30) break;
                grid.text(csf::dependency_kind_name(edge->kind), path_utf8(edge->evidence.file.filename()), derived(provenance_of(edge->status), std::string("Resolver: ") + csf::resolution_status_name(edge->status)));
            }
        }
        end_section();
    }
    if (node)
        for (const auto& diagnostic : node->diagnostics)
            token_text(diagnostic.severity == csf::MissionDiagnostic::Severity::error ? Token::error : Token::warn, "%s %s", diagnostic.code.c_str(), diagnostic.message.c_str());
}

void draw_document_section(AppState& state) {
    if (!state.document) return;
    if (!begin_section(state, "Document", false)) return;
    if (PropertyGrid grid(state, "##document"); grid) {
        grid.text("file", path_utf8(state.document->source_path().filename()), derived(Provenance::proven, "Loaded file"));
        grid.integer("bytes", static_cast<std::int64_t>(state.document->bytes().size()), derived(Provenance::proven, "File size"));
        grid.integer("diagnostics", static_cast<std::int64_t>(state.document->diagnostics().size()), derived(state.document->diagnostics().empty() ? Provenance::proven : Provenance::diagnosed, "Parser diagnostics for this file"));
        grid.text("collision", state.collision_status, derived(Provenance::inferred, "Companion _col file pairing"));
    }
    end_section();
}

// Draws the selection body. `primary` is false in duplicated inspector windows.
void draw_body(AppState& state, const SelectionRef& ref, const bool primary) {
    using Kind = SelectionRef::Kind;
    // Sections remember their open state per selection kind, keyed off state.selection.
    const SelectionRef saved = state.selection;
    if (!primary) state.selection = ref;
    switch (ref.kind) {
    case Kind::none:
        dim_text("Nothing selected. Click an object in the viewport or Explorer, or press Ctrl+P to go to anything.");
        break;
    case Kind::mission_entry:
        if (state.mission.scene) {
            draw_mission_record(state, static_cast<std::uint32_t>(ref.a));
            if (primary || state.workspace == Workspace::animation) {
                const auto association = std::ranges::find_if(state.mission.actor_associations, [&](const auto& value) {
                    return value.actor.entry_index == ref.a;
                });
                if (association != state.mission.actor_associations.end()) {
                    if (state.workspace == Workspace::mission && state.mission.animations &&
                        ImGui::Button("Open actor in Animation tab", {-1.0F, 0.0F}))
                        state.workspace = Workspace::animation;
                    if (state.workspace == Workspace::animation && state.mission.animations && state.mission.graph &&
                        begin_section(state, "Animation playback", true)) {
                        draw_actor_animation(state, *association, static_cast<std::uint32_t>(ref.a));
                        end_section();
                    }
                }
                if (state.workspace == Workspace::animation) {
                    std::string kind, label;
                    if (find_mission_source(*state.mission.scene, static_cast<std::uint32_t>(ref.a), kind, label) && kind == "Dummy") {
                        const auto dummy = std::ranges::find_if(state.mission.scene->dummies(), [&](const auto& value) {
                            return value.source.entry_index == ref.a;
                        });
                        if (dummy != state.mission.scene->dummies().end() && begin_section(state, "Cutscene camera references", true)) {
                            std::size_t uses{};
                            for (const auto& [path, timeline] : state.mission.cutscenes)
                                for (const auto& script : timeline.scripts())
                                    for (std::size_t index = 0; index < script.actions.size(); ++index) {
                                        const auto& action = script.actions[index];
                                        if (action.kind != csf::CutsceneActionKind::camera || !action.numeric_value ||
                                            static_cast<std::int32_t>(*action.numeric_value) != dummy->id)
                                            continue;
                                        ++uses;
                                        ImGui::BulletText("%s / %s", path_utf8(path.filename()).c_str(), script.name.c_str());
                                        ImGui::SameLine();
                                        dim_text("action %zu, %s, entry %u", index, action.opcode.c_str(), action.source.entry_index);
                                    }
                            if (uses == 0) dim_text("Not referenced by a recognized camera action.");
                            end_section();
                        }
                    }
                }
            }
        }
        break;
    case Kind::chunk:
        if (state.document)
            if (const auto* chunk = find_chunk(state.document->chunks(), ref.a)) draw_chunk(state, *chunk);
        break;
    case Kind::scene_instance:
        if (state.document)
            if (const auto* instance = find_instance(state.document->scene_instances(), ref.a)) {
                draw_map_instance_edit_section(state, *instance);
                draw_instance(state, *instance);
            }
        break;
    case Kind::program_script:
        if (ref.a < state.mission.programs.size() && ref.b < state.mission.programs[ref.a].second.scripts().size())
            draw_script(state, ref);
        break;
    case Kind::program_instruction:
        if (ref.a < state.mission.programs.size() && ref.b < state.mission.programs[ref.a].second.scripts().size())
            draw_instruction(state, ref);
        break;
    case Kind::database_record:
        draw_database_record(state, ref);
        break;
    case Kind::resource_path:
        draw_resource(state, ref);
        break;
    }
    if (primary) draw_document_section(state);
    state.selection = saved;
}

} // namespace

void draw_inspector(AppState& state) {
    const SelectionRef ref = state.ui.inspector_pin ? *state.ui.inspector_pin : state.selection;
    draw_header(state, ref, 0, true);
    ImGui::Separator();
    if (state.ui.inspector_pin) {
        ImGui::PushStyleColor(ImGuiCol_Text, color(Token::accent));
        ImGui::Text("%s pinned", icons::LC_PIN);
        ImGui::PopStyleColor();
    }
    // A pinned selection is drawn under its own identity so its sections do not
    // depend on what is currently selected.
    if (state.ui.inspector_pin) {
        const SelectionRef live = state.selection;
        state.selection = ref;
        draw_body(state, ref, true);
        state.selection = live;
    } else {
        draw_body(state, ref, true);
    }
}

void draw_extra_inspectors(AppState& state) {
    // The duplicate button appends to extra_inspectors, so work on copies by index: a
    // reference into the vector would dangle once it reallocates. New windows draw next frame.
    const auto count = state.ui.extra_inspectors.size();
    for (std::size_t i = 0; i < count && i < state.ui.extra_inspectors.size(); ++i) {
        const auto extra = state.ui.extra_inspectors[i];
        if (!extra.open) continue;
        const auto title = selection_title(state, extra.ref);
        const auto name = "Inspector: " + (title.empty() ? std::string("(empty)") : title) + "###extra_inspector_" + std::to_string(extra.id);
        ImGui::SetNextWindowSize({360.0F * ui_scale(), 480.0F * ui_scale()}, ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin(name.c_str(), &open)) {
            draw_header(state, extra.ref, extra.id, false);
            ImGui::Separator();
            draw_body(state, extra.ref, false);
        }
        ImGui::End();
        // A link may have loaded another document, which clears the list.
        if (!open && i < state.ui.extra_inspectors.size()) state.ui.extra_inspectors[i].open = false;
    }
    std::erase_if(state.ui.extra_inspectors, [](const auto& extra) { return !extra.open; });
}

} // namespace rwsman::ui
