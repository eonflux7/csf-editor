#include "rwsman/index_builders.hpp"

#include "rws/decoded.hpp"

#include <sstream>
#include <type_traits>
#include <variant>

namespace rwsman {
namespace {

std::string number(const std::optional<std::int32_t>& value) {
    return value ? std::to_string(*value) : "?";
}

std::string file_name(const std::filesystem::path& path) {
    const auto value = path.filename().generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string path_text(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::size_t add_entry(SearchIndex& index, const SymbolKind kind, std::string label, std::string detail,
                      const csf::CsfSourceId& source, std::string haystack = {},
                      const std::optional<std::size_t> parent = std::nullopt) {
    SearchEntry entry;
    entry.kind = kind;
    entry.label = std::move(label);
    entry.detail = std::move(detail);
    entry.haystack = std::move(haystack);
    entry.target = SelectionRef::mission_entry(source.entry_index);
    entry.offset = source.range.offset;
    entry.id = source.entry_index;
    entry.parent = parent;
    const auto position = index.size();
    index.add(std::move(entry));
    return position;
}

std::string operand_haystack(const csf::ProgramOperand& operand) {
    std::string text = operand.tag + " " + program_operand_text(operand);
    for (const auto& child : operand.children) text += " " + operand_haystack(child);
    return text;
}

} // namespace

std::string program_operand_text(const csf::ProgramOperand& operand) {
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>) return {};
            else if constexpr (std::is_same_v<T, std::string>) return value;
            else return std::to_string(value);
        },
        operand.value);
}

void index_chunks(SearchIndex& index, const std::vector<rws::Chunk>& chunks,
                  const ChunkDisplayNames& names) {
    for (const auto& chunk : chunks) {
        SearchEntry entry;
        entry.kind = SymbolKind::chunk;
        const auto name = names.find(chunk.offset);
        entry.label = std::string(rws::chunk_name(chunk.type));
        if (name != names.end()) entry.label += " \"" + name->second + "\"";
        char buffer[48];
        std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(chunk.offset));
        entry.detail = buffer;
        entry.target = SelectionRef::chunk(chunk.offset);
        entry.offset = chunk.offset;
        entry.end_offset = chunk.offset + 12 + chunk.available_size;
        index.add(std::move(entry));
        index_chunks(index, chunk.children, names);
    }
}

void index_scene_instances(SearchIndex& index, const std::span<const rws::SceneInstance> instances) {
    for (const auto& instance : instances) {
        SearchEntry entry;
        entry.kind = SymbolKind::instance;
        entry.label = instance.prototype_name.empty()
                          ? "Prototype " + std::to_string(instance.prototype_id)
                          : instance.prototype_name;
        entry.detail = "instance " + std::to_string(instance.instance_id);
        entry.target = SelectionRef::scene_instance(instance.offset);
        entry.offset = instance.offset;
        entry.end_offset = instance.offset + instance.physical_size;
        entry.id = instance.instance_id;
        index.add(std::move(entry));
    }
}

void index_mission(SearchIndex& index, const MissionIndexInputs& inputs) {
    if (const auto* scene = inputs.scene) {
        for (const auto& actor : scene->actors()) {
            std::string scripts = actor.script.value_or("");
            for (const auto id : actor.script_ids) {
                if (!scripts.empty()) scripts += ',';
                scripts += std::to_string(id);
            }
            add_entry(index, SymbolKind::actor, actor.name.value_or("(unnamed)"),
                      "class " + number(actor.class_id) + ", ID " + number(actor.id) + ", group " +
                          number(actor.group) + (scripts.empty() ? "" : ", scripts " + scripts),
                      actor.source, actor.faction.value_or(""));
        }
        for (const auto& group : scene->navigation()) {
            const auto group_index = add_entry(
                index, SymbolKind::navigation_group, group.name.value_or("(unnamed)"),
                "ID " + number(group.id) + ", " + std::to_string(group.points.size()) + " points",
                group.source);
            for (const auto& point : group.points)
                add_entry(index, SymbolKind::navigation_point, point.name.value_or("(unnamed)"),
                          number(point.group_id) + ":" + number(point.id), point.source, {},
                          group_index);
        }
        for (const auto& value : scene->dummies())
            add_entry(index, SymbolKind::dummy, value.name.value_or("(unnamed)"),
                      "ID " + number(value.id), value.source);
        for (const auto& value : scene->areas())
            add_entry(index, SymbolKind::area, value.name.value_or("(unnamed)"),
                      "ID " + number(value.id), value.source);
        for (const auto& value : scene->lights())
            add_entry(index, SymbolKind::light, value.name.value_or("(unnamed)"),
                      "ID " + number(value.id), value.source);
        for (const auto& value : scene->effects())
            add_entry(index, SymbolKind::effect, value.name.value_or("(unnamed)"),
                      "ID " + number(value.id) + ", class " + number(value.class_id),
                      value.source);
        for (const auto& value : scene->scene_objects())
            add_entry(index, SymbolKind::scene_object, value.id.value_or("(unnamed)"),
                      "animation " + number(value.animation_id), value.source);
    }

    if (inputs.programs) {
        for (std::size_t document_index = 0; document_index < inputs.programs->size();
             ++document_index) {
            const auto& [path, program] = (*inputs.programs)[document_index];
            const auto file = file_name(path);
            const auto add_variable = [&](const csf::ProgramVariable& variable,
                                          const std::size_t script_index,
                                          const std::string& owner) {
                SearchEntry entry;
                entry.kind = SymbolKind::variable;
                entry.label = variable.name;
                entry.detail = variable.type + " " + std::to_string(variable.id) + " in " + owner;
                entry.target = SelectionRef::program_script(document_index, script_index);
                entry.offset = variable.source.range.offset;
                entry.id = variable.source.entry_index;
                index.add(std::move(entry));
            };
            for (const auto& variable : program.global_variables()) add_variable(variable, 0, file);
            for (std::size_t script_index = 0; script_index < program.scripts().size();
                 ++script_index) {
                const auto& script = program.scripts()[script_index];
                std::string haystack = script.folder;
                for (const auto& variable : script.local_variables) {
                    haystack += " " + variable.name + " " + variable.type;
                    add_variable(variable, script_index, script.name);
                }
                for (const auto& event : script.events) haystack += " " + event.name;
                for (const auto* list : {&script.conditions, &script.actions})
                    for (const auto& instruction : *list) {
                        haystack += " " + instruction.opcode;
                        for (const auto& operand : instruction.operands)
                            haystack += " " + operand_haystack(operand);
                    }
                SearchEntry entry;
                entry.kind = SymbolKind::script;
                entry.label = script.name;
                entry.detail = (script.folder.empty() ? "(root)" : script.folder) + " · ID " +
                               std::to_string(script.id) + " · " + file;
                entry.haystack = std::move(haystack);
                entry.group = script.folder;
                entry.target = SelectionRef::program_script(document_index, script_index);
                entry.offset = script.source.range.offset;
                entry.id = script.source.entry_index;
                index.add(std::move(entry));
            }
        }
    }

    if (inputs.objects)
        for (const auto& definition : inputs.objects->definitions()) {
            SearchEntry entry;
            entry.kind = SymbolKind::class_record;
            entry.label = definition.name.value_or("class " + number(definition.class_id));
            entry.detail = "class " + number(definition.class_id) + ", ID " + number(definition.id) +
                           " · " + file_name(definition.source.file);
            entry.target = SelectionRef::database_record(path_text(definition.source.file),
                                                         definition.source.entry_index);
            entry.offset = definition.source.range.offset;
            entry.id = definition.source.entry_index;
            index.add(std::move(entry));
        }

    if (inputs.animations)
        for (const auto& record : inputs.animations->records()) {
            SearchEntry entry;
            entry.kind = SymbolKind::animation;
            entry.label = record.logical_name;
            entry.detail = std::to_string(record.variants.size()) + " variants · " +
                           file_name(record.source.file);
            entry.target =
                SelectionRef::database_record(path_text(record.source.file), record.source.entry_index);
            entry.offset = record.source.range.offset;
            entry.id = record.source.entry_index;
            index.add(std::move(entry));
        }

    if (inputs.graph)
        for (const auto& node : inputs.graph->nodes()) {
            const auto path = node.resolved_path.empty() ? std::filesystem::path(node.original_reference)
                                                         : node.resolved_path;
            SearchEntry entry;
            entry.kind = SymbolKind::resource;
            entry.label = file_name(path);
            entry.detail = std::string(csf::resource_kind_name(node.kind));
            entry.haystack = node.original_reference;
            entry.target = SelectionRef::resource_path(path_text(path));
            index.add(std::move(entry));
        }
}

} // namespace rwsman
