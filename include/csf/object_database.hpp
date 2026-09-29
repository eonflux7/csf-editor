#pragma once

#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"

#include <cstdint>
#include <map>
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
    std::optional<std::string> type;  // .TIPO: ALEMAN, DECORATIVO, ITEM_ARMA, ...
    std::vector<std::int32_t> weapon_ids;
    // .ANIMACIONES: the class's own clips, as (.TIPO, Anims.bdd .ID): a
    // vehicle's seats (SUBIR_PUESTO_0, REPOSO_PUESTO_1, ...), an archetype's
    // overrides.
    std::vector<std::pair<std::string, std::int32_t>> animations;
    // Player classes: the first-person hands (.MODELO, when .MODELO_TERCERA
    // names the body, which `references` then holds as the visual model).
    std::optional<std::string> first_person_model;
    std::vector<ObjectReference> references;
    std::vector<RawField> unknown_fields;
};

struct WeaponDefinition {
    CsfSourceId source;
    std::optional<std::int32_t> id;
    std::optional<std::string> name;
    std::optional<std::string> type;  // .TIPO: RIFLE, SMG, PISTOLA, Granada, Desarmado, ...
    std::optional<std::string> first_person_model;
    std::optional<std::string> third_person_model;
    std::optional<std::string> hand;
    // .ANIMACIONES_PLAYER: the first-person clips, as (.TIPO, Anims.bdd .ID):
    // ESTATICO, AVANZANDO, DISPARO, RECARGAR, SACAR, ...
    std::vector<std::pair<std::string, std::int32_t>> player_animations;
    // Sonidos.bdd IDs: .WAVE_DISPARO (a shot heard from others),
    // .WAVE_DISPARO_PLAYER (the player's own), .WAVE_DISPARO_SIN_BALAS (empty),
    // .WAVE_RECARGA (reload).
    std::optional<std::int32_t> shot_sound, player_shot_sound, empty_sound, reload_sound;
};

// A Sonidos.bdd record: the waves one sound picks from, and how it plays.
struct SoundDefinition {
    CsfSourceId source;
    std::int32_t id{};
    std::string name;
    std::vector<std::string> files;  // .FILE: variants, one picked per play
    std::optional<float> volume;     // .VOLUMEN, 0-100
    bool loop{};                     // .LOOP
    bool positional{};               // .ES3D
    std::optional<float> radius;     // .RADIO, game units (cm)
};

// A Materiales.bdd record: a surface type (the name the map's materials give
// as their surface) and the Sonidos.bdd IDs of stepping on it and hitting it.
struct SurfaceDefinition {
    std::int32_t id{};
    std::string name;                        // .NOMBRE_TIPO: Piedra, Madera, ...
    std::optional<std::int32_t> step_sound;  // .SONIDO_PISADA
    std::optional<std::int32_t> impact_sound;  // .SONIDO_IMPACTO
};
[[nodiscard]] std::vector<SurfaceDefinition> project_surfaces(const Document& document);

class SoundDatabase {
public:
    [[nodiscard]] static SoundDatabase project(const Document& document);
    [[nodiscard]] const std::vector<SoundDefinition>& definitions() const noexcept {
        return definitions_;
    }
    [[nodiscard]] const SoundDefinition* find_id(std::int32_t id) const noexcept;

private:
    std::vector<SoundDefinition> definitions_;
};

class WeaponDatabase {
public:
    [[nodiscard]] static WeaponDatabase project(const Document& document);
    [[nodiscard]] const std::vector<WeaponDefinition>& definitions() const noexcept {
        return definitions_;
    }
    [[nodiscard]] const WeaponDefinition* find_id(std::int32_t id) const noexcept;

private:
    std::vector<WeaponDefinition> definitions_;
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

// Every record of a BDD that has an .ID and a .NOMBRE, by ID (Efectos.bdd,
// or any table the typed databases above don't read).
[[nodiscard]] std::map<std::int32_t, std::string> record_names(const Document& document);

} // namespace csf
