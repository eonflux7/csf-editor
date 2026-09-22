#include "references.hpp"

#include "app_util.hpp"
#include "rwsman/mission_lookup.hpp"

#include <algorithm>

namespace rwsman {

ui::Provenance provenance_of(const csf::ProgramReferenceStatus status) {
    switch (status) {
    case csf::ProgramReferenceStatus::resolved:
        return ui::Provenance::proven;
    case csf::ProgramReferenceStatus::candidate:
        return ui::Provenance::inferred;
    case csf::ProgramReferenceStatus::missing:
    case csf::ProgramReferenceStatus::ambiguous:
        return ui::Provenance::diagnosed;
    case csf::ProgramReferenceStatus::unclassified:
        return ui::Provenance::unknown;
    }
    return ui::Provenance::unknown;
}

ui::Provenance provenance_of(const csf::ResolutionStatus status) {
    switch (status) {
    case csf::ResolutionStatus::explicit_path:
    case csf::ResolutionStatus::exact:
        return ui::Provenance::proven;
    case csf::ResolutionStatus::case_mismatch:
    case csf::ResolutionStatus::mapped_dff_to_rpc:
    case csf::ResolutionStatus::shared_root:
    case csf::ResolutionStatus::shared_root_case_mismatch:
        return ui::Provenance::inferred;
    case csf::ResolutionStatus::ambiguous:
    case csf::ResolutionStatus::missing:
    case csf::ResolutionStatus::outside_root:
        return ui::Provenance::diagnosed;
    }
    return ui::Provenance::unknown;
}

std::optional<std::pair<std::size_t, std::size_t>>
find_script_by_id(const AppState& state, const std::filesystem::path& file, const std::int32_t id) {
    for (std::size_t document = 0; document < state.mission.programs.size(); ++document) {
        if (state.mission.programs[document].first != file) continue;
        const auto& scripts = state.mission.programs[document].second.scripts();
        for (std::size_t script = 0; script < scripts.size(); ++script)
            if (scripts[script].id == id) return std::pair{document, script};
    }
    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>>
find_script_containing(const AppState& state, const std::filesystem::path& file,
                       const std::uint32_t entry) {
    for (std::size_t document = 0; document < state.mission.programs.size(); ++document) {
        if (state.mission.programs[document].first != file) continue;
        const auto& scripts = state.mission.programs[document].second.scripts();
        std::optional<std::size_t> owner;
        for (std::size_t script = 0; script < scripts.size(); ++script)
            if (scripts[script].source.entry_index <= entry) owner = script;
        if (owner) return std::pair{document, *owner};
    }
    return std::nullopt;
}

namespace {

bool operand_has_entry(const csf::ProgramOperand& operand, const std::uint32_t entry) {
    if (operand.source.entry_index == entry) return true;
    return std::ranges::any_of(operand.children, [&](const auto& child) {
        return operand_has_entry(child, entry);
    });
}

} // namespace

SelectionRef program_entry_ref(const AppState& state, const std::filesystem::path& file,
                               const std::uint32_t entry) {
    const auto script = find_script_containing(state, file, entry);
    if (!script) return {};
    const auto& owner = state.mission.programs[script->first].second.scripts()[script->second];
    for (const auto* list : {&owner.conditions, &owner.actions})
        for (const auto& instruction : *list)
            if (instruction.source.entry_index == entry ||
                std::ranges::any_of(instruction.operands, [&](const auto& operand) {
                    return operand_has_entry(operand, entry);
                }))
                return SelectionRef::program_instruction(script->first, script->second,
                                                         instruction.source.entry_index);
    return SelectionRef::program_script(script->first, script->second);
}

std::vector<ReferenceRow> collect_references(const AppState& state, const SelectionRef& ref) {
    using Kind = SelectionRef::Kind;
    std::vector<ReferenceRow> rows;
    const auto& mission = state.mission;

    if (ref.kind == Kind::mission_entry && mission.scene) {
        std::string kind, label;
        const auto* source = find_mission_source(*mission.scene, static_cast<std::uint32_t>(ref.a),
                                                 kind, label);
        if (!source) return rows;
        for (const auto* use : mission.program_references.uses(*source)) {
            ReferenceRow row;
            row.group = "Script uses";
            const auto script = find_script_by_id(state, use->source.file, use->owner_script);
            std::string script_name = "script " + std::to_string(use->owner_script);
            if (script)
                script_name = mission.programs[script->first].second.scripts()[script->second].name +
                              " [" + std::to_string(use->owner_script) + "]";
            row.label = script_name;
            row.detail = std::string(csf::program_reference_kind_name(use->kind)) + " " +
                         use->display_value + " · entry " + std::to_string(use->source.entry_index);
            if (script)
                row.target = SelectionRef::program_instruction(script->first, script->second,
                                                               use->source.entry_index);
            row.provenance = provenance_of(use->status);
            row.evidence = std::string("Resolver status: ") +
                           csf::program_reference_status_name(use->status) +
                           (use->detail.empty() ? "" : " - " + use->detail);
            rows.push_back(std::move(row));
        }
        if (mission.symbols) {
            std::vector<const csf::SymbolSite*> sites;
            const auto append = [&](std::string_view symbol) {
                const auto found = mission.symbols->exact(symbol);
                sites.insert(sites.end(), found.begin(), found.end());
            };
            if (const auto actor = std::ranges::find_if(mission.scene->actors(),
                                                        [&](const auto& value) {
                                                            return value.source.entry_index == source->entry_index;
                                                        });
                actor != mission.scene->actors().end() && actor->class_id)
                append("class:" + std::to_string(*actor->class_id));
            if (const auto effect = std::ranges::find_if(mission.scene->effects(),
                                                         [&](const auto& value) {
                                                             return value.source.entry_index == source->entry_index;
                                                         });
                effect != mission.scene->effects().end() && effect->class_id)
                append("class:" + std::to_string(*effect->class_id));
            if (const auto dummy = std::ranges::find_if(mission.scene->dummies(),
                                                        [&](const auto& value) {
                                                            return value.source.entry_index == source->entry_index;
                                                        });
                dummy != mission.scene->dummies().end() && dummy->id)
                append("dummy:" + std::to_string(*dummy->id));
            if (!label.empty()) append(label);
            for (const auto* site : sites) {
                if (site->source.file == source->file &&
                    site->source.entry_index == source->entry_index)
                    continue;
                ReferenceRow row;
                row.group = "Definitions and exact uses";
                row.label = std::string(csf::symbol_role_name(site->role)) + " / " +
                            csf::symbol_category_name(site->category);
                row.detail = path_utf8(site->source.file.filename()) + " : entry " +
                             std::to_string(site->source.entry_index);
                if (site->source.file == mission.scene->source_path())
                    row.target = SelectionRef::mission_entry(site->source.entry_index);
                else
                    row.target = program_entry_ref(state, site->source.file, site->source.entry_index);
                row.provenance = site->role == csf::SymbolRole::candidate ? ui::Provenance::inferred
                                                                          : ui::Provenance::proven;
                row.evidence = "Exact symbol match in the mission's typed documents";
                rows.push_back(std::move(row));
            }
        }
    } else if ((ref.kind == Kind::program_script || ref.kind == Kind::program_instruction) &&
               ref.a < mission.programs.size()) {
        const auto& [path, program] = mission.programs[ref.a];
        if (ref.b >= program.scripts().size()) return rows;
        const auto& script = program.scripts()[ref.b];
        for (const auto& reference : mission.program_references.references()) {
            if (reference.source.file != path || reference.owner_script != script.id) continue;
            ReferenceRow row;
            row.group = "References from this script";
            row.label = std::string(csf::program_reference_kind_name(reference.kind)) + " " +
                        reference.display_value;
            row.detail = std::string(csf::program_reference_status_name(reference.status)) +
                         " · entry " + std::to_string(reference.source.entry_index);
            if (reference.targets.size() == 1 && mission.scene &&
                reference.targets[0].file == mission.scene->source_path())
                row.target = SelectionRef::mission_entry(reference.targets[0].entry_index);
            row.provenance = provenance_of(reference.status);
            row.evidence = reference.detail;
            rows.push_back(std::move(row));
        }
    } else if (ref.kind == Kind::database_record && mission.objects && mission.scene) {
        for (const auto& definition : mission.objects->definitions()) {
            if (definition.source.entry_index != ref.a || path_utf8(definition.source.file) != ref.path ||
                !definition.class_id)
                continue;
            for (const auto& actor : mission.scene->actors()) {
                if (actor.class_id != definition.class_id) continue;
                ReferenceRow row;
                row.group = "Actors of this class";
                row.label = actor.name.value_or("(unnamed)");
                row.detail = "ID " + std::to_string(actor.id.value_or(-1)) + " · entry " +
                             std::to_string(actor.source.entry_index);
                row.target = SelectionRef::mission_entry(actor.source.entry_index);
                row.provenance = ui::Provenance::proven;
                row.evidence = "Scene actor class ID equals this record's class ID";
                rows.push_back(std::move(row));
            }
        }
    }
    return rows;
}

} // namespace rwsman
