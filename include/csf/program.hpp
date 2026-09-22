#pragma once

#include "csf/animation_catalog.hpp"
#include "csf/mission_scene.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace csf {

struct ScriptFlags {
    std::optional<bool> trigger;
    std::optional<bool> enabled;
    std::optional<bool> valid;
};

struct ProgramOperand {
    CsfSourceId source;
    std::string tag;
    std::variant<std::monostate, std::int32_t, float, std::string> value;
    std::vector<ProgramOperand> children;
    std::vector<RawField> unknown_fields;
};

struct ProgramEvent {
    CsfSourceId source;
    std::string name;
};

struct ProgramInstruction {
    CsfSourceId source;
    std::string opcode;
    std::vector<ProgramOperand> operands;
};

struct ProgramVariable {
    CsfSourceId source;
    std::int32_t id{};
    std::string type;
    std::string name;
    bool is_array{};
    ProgramOperand initial_value;
};

struct ProgramScript {
    CsfSourceId source;
    std::int32_t id{};
    std::string name;
    std::string folder;
    ScriptFlags flags;
    std::vector<ProgramVariable> local_variables;
    std::vector<ProgramEvent> events;
    std::vector<ProgramInstruction> conditions;
    std::vector<ProgramInstruction> actions;
};

struct ProgramResourceList {
    CsfSourceId source;
    std::string name;
    std::vector<std::variant<std::int32_t, float, std::string>> values;
};

struct ProgramStructureRow {
    const ProgramInstruction* instruction{};
    std::size_t depth{};
    bool closes_scope{};
    bool opens_scope{};
};

class ProgramDocument {
public:
    [[nodiscard]] static ProgramDocument project(const Document&);

    [[nodiscard]] const std::filesystem::path& source_path() const noexcept { return source_path_; }
    [[nodiscard]] const std::vector<ProgramResourceList>& resources() const noexcept {
        return resources_;
    }
    [[nodiscard]] const std::vector<ProgramVariable>& global_variables() const noexcept {
        return globals_;
    }
    [[nodiscard]] const std::vector<ProgramScript>& scripts() const noexcept { return scripts_; }
    [[nodiscard]] const std::vector<TypedDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] const ProgramScript* find_script(std::int32_t id) const noexcept;

private:
    std::filesystem::path source_path_;
    std::vector<ProgramResourceList> resources_;
    std::vector<ProgramVariable> globals_;
    std::vector<ProgramScript> scripts_;
    std::vector<TypedDiagnostic> diagnostics_;
};

enum class ProgramReferenceStatus { resolved, candidate, missing, ambiguous, unclassified };
enum class ProgramReferenceKind {
    actor,
    dummy,
    area,
    navigation_point,
    script,
    trigger,
    event,
    animation,
    object_class,
    effect_class,
    weapon_class,
    sound,
    variable,
    array,
    unknown
};

struct ProgramReference {
    CsfSourceId source;
    std::int32_t owner_script{};
    std::string tag;
    ProgramReferenceKind kind{ProgramReferenceKind::unknown};
    ProgramReferenceStatus status{ProgramReferenceStatus::unclassified};
    std::string display_value;
    std::vector<CsfSourceId> targets;
    std::string detail;
};

class ProgramReferenceIndex {
public:
    void add_program(const ProgramDocument&, const MissionScene* = nullptr,
                     const AnimationCatalog* = nullptr);
    [[nodiscard]] const std::vector<ProgramReference>& references() const noexcept {
        return references_;
    }
    [[nodiscard]] std::vector<const ProgramReference*> uses(const CsfSourceId&) const;

private:
    struct ScriptDefinition {
        std::int32_t id{};
        bool trigger{};
        CsfSourceId source;
    };
    std::vector<ProgramReference> references_;
    std::vector<ScriptDefinition> script_definitions_;
};

[[nodiscard]] std::vector<ProgramStructureRow>
program_structure(const std::vector<ProgramInstruction>&,
                  std::vector<TypedDiagnostic>* diagnostics = nullptr);
[[nodiscard]] std::string program_json(const ProgramDocument&);
[[nodiscard]] const char* program_reference_status_name(ProgramReferenceStatus) noexcept;
[[nodiscard]] const char* program_reference_kind_name(ProgramReferenceKind) noexcept;

} // namespace csf
