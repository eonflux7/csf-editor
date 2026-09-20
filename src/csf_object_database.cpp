#include "csf/object_database.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace csf {
namespace {

std::string label(const Document& document, const Node& node) {
    if (!node.identifier_index) return {};
    const auto* value = document.identifier(*node.identifier_index);
    return value ? value->display_utf8() : std::string{};
}
std::string normalized(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    value.erase(
        std::remove_if(value.begin(), value.end(),
                       [](const char c) { return c == '.' || c == '_' || c == '-' || c == ' '; }),
        value.end());
    return value;
}
CsfSourceId source(const Document& document, const Node& node) {
    const auto& entry = document.entries().at(node.entry_index);
    return {document.source_path(), node.entry_index, entry.source};
}
std::optional<std::int32_t> integer(const Node& node) {
    if (const auto* value = std::get_if<std::int32_t>(&node.scalar)) return *value;
    return std::nullopt;
}
std::optional<std::string> string_value(const Document& document, const Node& node) {
    if (const auto* index = std::get_if<std::uint32_t>(&node.scalar))
        if (const auto* value = document.string(*index)) return value->display_utf8();
    return std::nullopt;
}
const Node* field(const Document& document, const Node& record,
                  const std::set<std::string>& names) {
    for (const auto& child : record.children)
        if (names.contains(normalized(label(document, child)))) return &child;
    return nullptr;
}
std::optional<ObjectReference::Kind> reference_kind(const std::string& name) {
    const auto key = normalized(name);
    if (key.find("MODELOCOLISION") != std::string::npos ||
        key.find("COLLISIONMODEL") != std::string::npos || key == "CMO")
        return ObjectReference::Kind::collision_model;
    if (key.find("RAGDOLL") != std::string::npos) return ObjectReference::Kind::ragdoll;
    if (key.find("ANIM") != std::string::npos) return ObjectReference::Kind::animation;
    if (key.find("PHYSIC") != std::string::npos || key.find("PHYSICS") != std::string::npos ||
        key == "MODELFILE")
        return ObjectReference::Kind::physics_model;
    if (key.find("LOD") != std::string::npos) return ObjectReference::Kind::lod_model;
    if (key == "MODELO" || key == "MODEL" || key == "MODELFILEVISUAL" || key == "DFF")
        return ObjectReference::Kind::visual_model;
    return std::nullopt;
}
std::optional<ObjectReference::Kind> reference_kind(const std::string& name,
                                                    const std::string& path) {
    if (const auto explicit_kind = reference_kind(name)) return explicit_kind;
    auto extension = std::filesystem::path(path).extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto field_name = normalized(name);
    if (extension == ".cmo") return ObjectReference::Kind::collision_model;
    if (extension == ".anm") return ObjectReference::Kind::animation;
    if (extension == ".rws")
        return field_name.find("RAGDOLL") != std::string::npos ||
                       normalized(path).find("RAGDOLL") != std::string::npos
                   ? ObjectReference::Kind::ragdoll
                   : ObjectReference::Kind::physics_model;
    if (extension == ".dff" || extension == ".rpc")
        return field_name.find("LOD") != std::string::npos ? ObjectReference::Kind::lod_model
                                                           : ObjectReference::Kind::visual_model;
    return std::nullopt;
}
void collect_records(const Document& document, const Node& node,
                     std::vector<const Node*>& records) {
    bool identity{};
    bool reference{};
    bool named{};
    for (const auto& child : node.children) {
        const auto key = normalized(label(document, child));
        identity |= key == "CLASSID" || key == "IDCLASE" || key == "OBJECTID" || key == "ID";
        named |= key == "NOMBRE" || key == "NAME";
        const auto text = string_value(document, child);
        reference |=
            (text && reference_kind(key, *text).has_value()) || reference_kind(key).has_value();
    }
    if (identity && (reference || named))
        records.push_back(&node);
    else
        for (const auto& child : node.children)
            collect_records(document, child, records);
}

} // namespace

ObjectDatabase ObjectDatabase::project(const Document& document) {
    ObjectDatabase result;
    std::vector<const Node*> records;
    for (const auto& root : document.roots())
        collect_records(document, root, records);
    std::map<std::int32_t, CsfSourceId> seen_classes;
    for (const auto* record : records) {
        ObjectDefinition definition;
        definition.source = source(document, *record);
        if (const auto* value = field(document, *record, {"CLASSID", "IDCLASE"}))
            definition.class_id = integer(*value);
        if (!definition.class_id)
            if (const auto* value = field(document, *record, {"ID"}))
                definition.class_id = integer(*value);
        if (const auto* value = field(document, *record, {"ID", "OBJECTID"}))
            definition.id = integer(*value);
        if (const auto* value = field(document, *record, {"NOMBRE", "NAME"}))
            definition.name = string_value(document, *value);
        for (const auto& child : record->children) {
            const auto name = label(document, child);
            const auto value = string_value(document, child);
            const auto kind = value ? reference_kind(name, *value) : reference_kind(name);
            if (kind) {
                if (value && !value->empty())
                    definition.references.push_back({*kind, *value, source(document, child), name});
                else
                    definition.unknown_fields.push_back({name, source(document, child)});
            } else if (!std::set<std::string>{"CLASSID", "IDCLASE", "ID", "OBJECTID", "NOMBRE",
                                              "NAME"}
                            .contains(normalized(name)))
                definition.unknown_fields.push_back({name, source(document, child)});
        }
        if (definition.class_id) {
            const auto [found, inserted] =
                seen_classes.emplace(*definition.class_id, definition.source);
            if (!inserted)
                result.diagnostics_.push_back(
                    {Diagnostic::Severity::warning, definition.source, "duplicate-object-class",
                     "Duplicate Objetos.bdd class " + std::to_string(*definition.class_id) +
                         "; both definitions remain visible"});
        }
        result.definitions_.push_back(std::move(definition));
    }
    return result;
}

std::vector<const ObjectDefinition*> ObjectDatabase::find_class(const std::int32_t class_id) const {
    std::vector<const ObjectDefinition*> result;
    for (const auto& definition : definitions_)
        if (definition.class_id == class_id) result.push_back(&definition);
    return result;
}

std::vector<ActorAssociation> associate_actors(const MissionScene& scene,
                                               const ObjectDatabase& objects,
                                               const ResourceIndex& resources,
                                               const std::optional<std::size_t> preferred_root) {
    std::vector<ActorAssociation> result;
    for (const auto& actor : scene.actors()) {
        ActorAssociation association;
        association.actor = actor.source;
        association.class_id = actor.class_id;
        if (!actor.class_id)
            association.diagnostics.emplace_back("Actor has no class ID");
        else
            association.definitions = objects.find_class(*actor.class_id);
        if (actor.class_id && association.definitions.empty())
            association.diagnostics.emplace_back("No Objetos.bdd definition for class " +
                                                 std::to_string(*actor.class_id));
        if (association.definitions.size() > 1)
            association.diagnostics.emplace_back(
                "Multiple Objetos.bdd definitions match this class; none was selected implicitly");
        for (const auto* definition : association.definitions)
            for (const auto& reference : definition->references) {
                auto resolution = resources.resolve(reference.path, preferred_root);
                AssociationEvidence evidence{"Objetos.bdd " + reference.field, reference.source,
                                             reference.field, resolution, std::nullopt};
                if (resolution.candidate_indices.size() == 1 &&
                    resolution.status != ResolutionStatus::ambiguous)
                    evidence.resolved_path =
                        resources.resources()[resolution.candidate_indices.front()].path;
                auto* destination = [&]() -> std::vector<AssociationEvidence>* {
                    switch (reference.kind) {
                    case ObjectReference::Kind::visual_model:
                        return &association.visual_models;
                    case ObjectReference::Kind::lod_model:
                        return &association.lod_models;
                    case ObjectReference::Kind::collision_model:
                        return &association.collision_models;
                    case ObjectReference::Kind::physics_model:
                        return &association.physics_models;
                    case ObjectReference::Kind::ragdoll:
                        return &association.ragdolls;
                    case ObjectReference::Kind::animation:
                        return &association.animations;
                    }
                    return &association.visual_models;
                }();
                destination->push_back(std::move(evidence));
            }
        result.push_back(std::move(association));
    }
    return result;
}

const char* object_reference_kind_name(const ObjectReference::Kind kind) noexcept {
    switch (kind) {
    case ObjectReference::Kind::visual_model:
        return "visual";
    case ObjectReference::Kind::lod_model:
        return "lod";
    case ObjectReference::Kind::collision_model:
        return "collision";
    case ObjectReference::Kind::physics_model:
        return "physics";
    case ObjectReference::Kind::ragdoll:
        return "ragdoll";
    case ObjectReference::Kind::animation:
        return "animation";
    }
    return "unknown";
}

} // namespace csf
