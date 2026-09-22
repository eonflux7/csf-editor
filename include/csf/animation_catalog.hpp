#pragma once

#include "csf/document.hpp"
#include "csf/mission.hpp"
#include "csf/mission_scene.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace csf {

struct AnimationSoundEvent {
    CsfSourceId source;
    std::optional<float> time;
    std::string logical_id;
};

struct AnimationVariant {
    CsfSourceId source;
    std::string field;
    std::string reference;
    std::optional<Resolution> resolution;
    std::vector<AnimationSoundEvent> sounds;
};

struct AnimationRecord {
    CsfSourceId source;
    std::optional<std::int32_t> id;
    std::string logical_name;
    std::vector<AnimationVariant> variants;
    std::optional<bool> loop;
    std::optional<float> blend_in;
    std::optional<float> velocity_scalar;
    std::optional<float> translation_scalar;
    std::optional<float> rotation_scalar;
    std::optional<Vec3> velocity;
    std::optional<Vec3> translation;
    std::optional<Vec3> rotation;
    std::vector<std::string> model_context;
    std::vector<std::string> item_context;
    std::vector<std::string> hand_context;
    std::optional<std::string> model3d_item;
    std::optional<std::string> mano_item;
    std::optional<std::int32_t> num_anims_ps2;
    std::optional<std::int32_t> num_anims_xbox;
    std::optional<std::int32_t> sound_pc;
    std::optional<std::int32_t> sound_ps2;
    std::optional<std::int32_t> sound_xbox;
    // Compatibility aggregate. Each event is also retained on its owning file variant.
    std::vector<AnimationSoundEvent> sounds;
    std::vector<RawField> unknown_fields;
};

class AnimationCatalog {
public:
    [[nodiscard]] static AnimationCatalog
    project(const Document&, const ResourceIndex* = nullptr,
            std::optional<std::size_t> preferred_root = std::nullopt);
    [[nodiscard]] const std::vector<AnimationRecord>& records() const noexcept { return records_; }
    [[nodiscard]] const std::vector<TypedDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }
    [[nodiscard]] std::vector<const AnimationRecord*> compatible(std::string_view model,
                                                                 std::string_view item = {},
                                                                 std::string_view hand = {}) const;
    [[nodiscard]] const AnimationRecord* find_id(std::int32_t id) const noexcept;

private:
    std::vector<AnimationRecord> records_;
    std::vector<TypedDiagnostic> diagnostics_;
};

struct ScriptAnimationUse {
    CsfSourceId source;
    std::int32_t script_id{};
    std::string script_name;
    std::string opcode;
    std::int32_t animation_id{};
    bool targets_this{};
    std::optional<std::int32_t> actor_id;
};

class ScriptAnimationIndex {
public:
    void add_document(const Document&);
    [[nodiscard]] const std::vector<ScriptAnimationUse>& uses() const noexcept { return uses_; }
    [[nodiscard]] std::vector<const ScriptAnimationUse*>
    for_actor(std::span<const std::int32_t> script_ids, std::optional<std::int32_t> actor_id) const;

private:
    std::vector<ScriptAnimationUse> uses_;
};

enum class CutsceneActionKind {
    animation,
    camera,
    fov,
    sound,
    pause,
    wait,
    branch,
    sequence,
    actor,
    init,
    end,
    unknown
};

struct CutsceneAction {
    CsfSourceId source;
    CutsceneActionKind kind{CutsceneActionKind::unknown};
    std::string opcode;
    std::string reference;
    std::optional<float> explicit_time;
    std::optional<float> duration;
    std::optional<float> numeric_value;
};

struct CutsceneBlock {
    std::uint32_t index{};
    std::vector<std::size_t> action_indices;
    std::vector<std::uint32_t> successors;
    bool runtime_wait{};
    bool conditional{};
};

struct CutsceneScript {
    CsfSourceId source;
    std::string name;
    std::vector<CutsceneAction> actions;
    std::vector<CutsceneBlock> blocks;
};

class CutsceneTimeline {
public:
    [[nodiscard]] static CutsceneTimeline project(const Document&);
    [[nodiscard]] const std::vector<CutsceneScript>& scripts() const noexcept { return scripts_; }
    [[nodiscard]] const std::vector<TypedDiagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

private:
    std::vector<CutsceneScript> scripts_;
    std::vector<TypedDiagnostic> diagnostics_;
};

[[nodiscard]] const char* cutscene_action_kind_name(CutsceneActionKind) noexcept;

} // namespace csf
