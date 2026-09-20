#pragma once

#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"

#include <optional>
#include <string>
#include <vector>

namespace csf {

struct ObjectReference {
    enum class Kind { visual_model, lod_model, collision_model, physics_model, ragdoll, animation };
    Kind kind{Kind::visual_model};
    std::string path;
    CsfSourceId source;
    std::string field;
};

struct ObjectDefinition {
    CsfSourceId source;
    std::optional<std::int32_t> class_id;
    std::optional<std::int32_t> id;
    std::optional<std::string> name;
    std::vector<ObjectReference> references;
    std::vector<RawField> unknown_fields;
};

class ObjectDatabase {
public:
    [[nodiscard]] static ObjectDatabase project(const Document& document);
    [[nodiscard]] const std::vector<ObjectDefinition>& definitions() const noexcept {
        return definitions_;
    }
    [[nodiscard]] const std::vector<TypedDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] std::vector<const ObjectDefinition*> find_class(std::int32_t class_id) const;

private:
    std::vector<ObjectDefinition> definitions_;
    std::vector<TypedDiagnostic> diagnostics_;
};

struct AssociationEvidence {
    std::string rule;
    CsfSourceId source;
    std::string field;
    Resolution resolution;
    std::optional<std::filesystem::path> resolved_path;
};

struct ActorAssociation {
    CsfSourceId actor;
    std::optional<std::int32_t> class_id;
    std::vector<const ObjectDefinition*> definitions;
    std::vector<AssociationEvidence> visual_models;
    std::vector<AssociationEvidence> lod_models;
    std::vector<AssociationEvidence> collision_models;
    std::vector<AssociationEvidence> physics_models;
    std::vector<AssociationEvidence> ragdolls;
    std::vector<AssociationEvidence> animations;
    std::vector<std::string> diagnostics;
};

[[nodiscard]] std::vector<ActorAssociation>
associate_actors(const MissionScene& scene, const ObjectDatabase& objects,
                 const ResourceIndex& resources,
                 std::optional<std::size_t> preferred_root = std::nullopt);
[[nodiscard]] const char* object_reference_kind_name(ObjectReference::Kind) noexcept;

} // namespace csf
