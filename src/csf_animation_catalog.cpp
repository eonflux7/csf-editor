#include "csf/animation_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace csf {
namespace {

std::string label(const Document& d, const Node& n) {
    if (!n.identifier_index) return {};
    const auto* v = d.identifier(*n.identifier_index);
    return v ? v->display_utf8() : std::string{};
}
std::string norm(std::string v) {
    std::ranges::transform(v, v.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](char c) { return c == '.' || c == '_' || c == '-' || c == ' '; }),
            v.end());
    return v;
}
CsfSourceId source(const Document& d, const Node& n) {
    const auto& e = d.entries().at(n.entry_index);
    return {d.source_path(), n.entry_index, e.source};
}
std::optional<std::string> text(const Document& d, const Node& n) {
    if (const auto* i = std::get_if<std::uint32_t>(&n.scalar))
        if (const auto* v = d.string(*i)) return v->display_utf8();
    return std::nullopt;
}
std::optional<float> number(const Node& n) {
    if (const auto* v = std::get_if<float>(&n.scalar)) return *v;
    if (const auto* v = std::get_if<std::int32_t>(&n.scalar)) return static_cast<float>(*v);
    return std::nullopt;
}
bool has_anm(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value.ends_with(".anm");
}
bool contains_anm(const Document& d, const Node& n) {
    if (const auto value = text(d, n); value && has_anm(*value)) return true;
    return std::ranges::any_of(n.children,
                               [&](const auto& child) { return contains_anm(d, child); });
}
void collect_records(const Document& d, const Node& n, std::vector<const Node*>& out) {
    const bool named = std::ranges::any_of(n.children, [&](const auto& child) {
        const auto key = norm(label(d, child));
        return key == "NAME" || key == "NOMBRE" || key == "ANIMNAME";
    });
    if (named && contains_anm(d, n))
        out.push_back(&n);
    else
        for (const auto& c : n.children)
            collect_records(d, c, out);
}
void strings_recursive(const Document& d, const Node& n,
                       std::vector<std::pair<const Node*, std::string>>& out) {
    if (auto v = text(d, n)) out.emplace_back(&n, *v);
    for (const auto& c : n.children)
        strings_recursive(d, c, out);
}
const Node* find(const Document& d, const Node& n, const std::set<std::string>& keys) {
    for (const auto& c : n.children)
        if (keys.contains(norm(label(d, c)))) return &c;
    return nullptr;
}
std::optional<Vec3> vector(const Document& d, const Node& n, const std::string& prefix) {
    Vec3 v{};
    bool any{};
    for (const auto& c : n.children) {
        const auto k = norm(label(d, c));
        const auto value = number(c);
        if (!value) continue;
        if (k == prefix + "X") {
            v.x = *value;
            any = true;
        } else if (k == prefix + "Y") {
            v.y = *value;
            any = true;
        } else if (k == prefix + "Z") {
            v.z = *value;
            any = true;
        }
    }
    return any ? std::optional(v) : std::nullopt;
}
CutsceneActionKind classify(const std::string& name) {
    const auto k = norm(name);
    if (k == "CUTSCENEEXE") return CutsceneActionKind::sequence;
    if (k.find("ANIM") != std::string::npos) return CutsceneActionKind::animation;
    if (k.find("FOV") != std::string::npos) return CutsceneActionKind::fov;
    if (k.find("CAM") != std::string::npos || k.find("DUMMY") != std::string::npos)
        return CutsceneActionKind::camera;
    if (k.find("SOUND") != std::string::npos || k.find("SONIDO") != std::string::npos ||
        k.find("AUDIO") != std::string::npos)
        return CutsceneActionKind::sound;
    if (k.find("PAUSE") != std::string::npos || k.find("PAUSA") != std::string::npos)
        return CutsceneActionKind::pause;
    if (k.find("WAIT") != std::string::npos || k.find("ESPERA") != std::string::npos ||
        k.find("CONDITION") != std::string::npos)
        return CutsceneActionKind::wait;
    if (k == "CONTINUE" || k.find("IF") != std::string::npos ||
        k.find("BRANCH") != std::string::npos || k.find("GOTO") != std::string::npos)
        return CutsceneActionKind::branch;
    if (k.find("ACTOR") != std::string::npos || k.find("PERSONAJE") != std::string::npos)
        return CutsceneActionKind::actor;
    if (k.find("INIT") != std::string::npos || k.find("START") != std::string::npos ||
        k.find("INICIO") != std::string::npos)
        return CutsceneActionKind::init;
    if (k.find("END") != std::string::npos || k.find("FIN") != std::string::npos ||
        k.find("COMPLETE") != std::string::npos)
        return CutsceneActionKind::end;
    return CutsceneActionKind::unknown;
}
void collect_sound_events(const Document& d, const Node& n, std::vector<AnimationSoundEvent>& out) {
    const Node* id{};
    const Node* time{};
    for (const auto& child : n.children) {
        const auto key = norm(label(d, child));
        if (key == "SNDID" || key == "SOUNDID" || key == "SONIDOID")
            id = &child;
        else if (key == "SNDTIME" || key == "SOUNDTIME")
            time = &child;
    }
    if (id) {
        std::string logical;
        if (auto text_value = text(d, *id))
            logical = *text_value;
        else if (const auto number_value = number(*id))
            logical = std::to_string(static_cast<std::int32_t>(*number_value));
        if (!logical.empty())
            out.push_back({source(d, n), time ? number(*time) : std::nullopt, std::move(logical)});
    }
    for (const auto& child : n.children)
        collect_sound_events(d, child, out);
}
std::optional<std::string> first_text(const Document& d, const Node& n) {
    if (auto value = text(d, n)) return value;
    for (const auto& child : n.children)
        if (auto value = first_text(d, child)) return value;
    return std::nullopt;
}
std::optional<float> first_number(const Node& n) {
    if (auto value = number(n)) return value;
    for (const auto& child : n.children)
        if (auto value = first_number(child)) return value;
    return std::nullopt;
}
std::optional<float> labeled_number(const Document& d, const Node& n,
                                    const std::set<std::string>& keys) {
    for (const auto& child : n.children) {
        if (keys.contains(norm(label(d, child))))
            if (auto value = number(child)) return value;
        if (auto value = labeled_number(d, child, keys)) return value;
    }
    return std::nullopt;
}
void collect_script_nodes(const Document& d, const Node& n, std::vector<const Node*>& out) {
    if (find(d, n, {"NOMBRE", "NAME"}) && find(d, n, {"ACCIONES", "ACTIONS"}))
        out.push_back(&n);
    else
        for (const auto& child : n.children)
            collect_script_nodes(d, child, out);
}
std::optional<std::int32_t> integer_value(const Node& n) {
    if (const auto* value = std::get_if<std::int32_t>(&n.scalar)) return *value;
    return std::nullopt;
}
std::optional<std::int32_t> tagged_integer(const Document& d, const Node& n,
                                           const std::set<std::string>& keys) {
    if (!n.children.empty() && keys.contains(norm(first_text(d, n.children.front()).value_or(""))))
        for (const auto& child : n.children)
            if (const auto value = integer_value(child)) return value;
    for (const auto& child : n.children)
        if (const auto value = tagged_integer(d, child, keys)) return value;
    return std::nullopt;
}
bool contains_tag(const Document& d, const Node& n, const std::set<std::string>& keys) {
    if (const auto value = text(d, n); value && keys.contains(norm(*value))) return true;
    return std::ranges::any_of(n.children,
                               [&](const auto& child) { return contains_tag(d, child, keys); });
}
} // namespace

AnimationCatalog AnimationCatalog::project(const Document& document, const ResourceIndex* resources,
                                           const std::optional<std::size_t> preferred) {
    AnimationCatalog result;
    std::vector<const Node*> records;
    for (const auto& root : document.roots())
        collect_records(document, root, records);
    for (const auto* node : records) {
        AnimationRecord record;
        record.source = source(document, *node);
        if (const auto* id = find(document, *node, {"ID"})) record.id = integer_value(*id);
        if (const auto* name = find(document, *node, {"NAME", "NOMBRE", "ANIMNAME"})) {
            if (auto value = text(document, *name))
                record.logical_name = *value;
            else
                record.logical_name = label(document, *name);
        }
        if (record.logical_name.empty())
            record.logical_name = "animation@" + std::to_string(node->entry_index);
        record.velocity = vector(document, *node, "VELOCITY");
        if (!record.velocity) record.velocity = vector(document, *node, "VELOCIDAD");
        record.translation = vector(document, *node, "TRANSLATION");
        record.rotation = vector(document, *node, "ROTATION");
        for (const auto& child : node->children) {
            const auto name = label(document, child), key = norm(name);
            if (const auto text_value = text(document, child)) {
                if (has_anm(*text_value)) {
                    AnimationVariant variant{source(document, child), name, *text_value,
                                             std::nullopt};
                    if (resources) variant.resolution = resources->resolve(*text_value, preferred);
                    record.variants.push_back(std::move(variant));
                } else if ((key.find("ITEM") != std::string::npos ||
                            key.find("OBJETO") != std::string::npos ||
                            key.find("ARMA") != std::string::npos) &&
                           !text_value->empty())
                    record.item_context.push_back(*text_value);
                else if ((key.find("MODEL") != std::string::npos ||
                          key.find("MODELO") != std::string::npos) &&
                         !text_value->empty())
                    record.model_context.push_back(*text_value);
                else if (key.find("HAND") != std::string::npos ||
                         key.find("MANO") != std::string::npos) {
                    if (!text_value->empty()) record.hand_context.push_back(*text_value);
                } else if (key.find("SOUND") != std::string::npos ||
                           key.find("SONIDO") != std::string::npos)
                    record.sounds.push_back({source(document, child), std::nullopt, *text_value});
                else
                    record.unknown_fields.push_back({name, source(document, child)});
            } else if (const auto number_value = number(child)) {
                if (key.find("LOOP") != std::string::npos || key.find("BUCLE") != std::string::npos)
                    record.loop = *number_value != 0;
                else if (key.find("BLEND") != std::string::npos)
                    record.blend_in = number_value;
                else if (key == "VEL" || key == "VELOCITY" || key == "VELOCIDAD")
                    record.velocity_scalar = number_value;
                else if (key == "TRASLACION" || key == "TRANSLATION")
                    record.translation_scalar = number_value;
                else if (key == "ROTACION" || key == "ROTATION")
                    record.rotation_scalar = number_value;
                else
                    record.unknown_fields.push_back({name, source(document, child)});
            } else
                record.unknown_fields.push_back({name, source(document, child)});
        }
        std::vector<std::pair<const Node*, std::string>> nested;
        strings_recursive(document, *node, nested);
        for (const auto& [item, value] : nested) {
            const auto key = norm(label(document, *item));
            if (has_anm(value) && std::ranges::none_of(record.variants, [&](const auto& variant) {
                    return variant.source.entry_index == item->entry_index;
                })) {
                AnimationVariant variant{source(document, *item), label(document, *item), value,
                                         std::nullopt};
                if (resources) variant.resolution = resources->resolve(value, preferred);
                record.variants.push_back(std::move(variant));
            } else if ((key.find("ITEM") != std::string::npos ||
                        key.find("OBJETO") != std::string::npos ||
                        key.find("ARMA") != std::string::npos) &&
                       !value.empty() &&
                       std::ranges::find(record.item_context, value) == record.item_context.end())
                record.item_context.push_back(value);
            else if ((key.find("MODEL") != std::string::npos ||
                      key.find("MODELO") != std::string::npos) &&
                     !value.empty() &&
                     std::ranges::find(record.model_context, value) == record.model_context.end())
                record.model_context.push_back(value);
            else if ((key.find("HAND") != std::string::npos ||
                      key.find("MANO") != std::string::npos) &&
                     !value.empty() &&
                     std::ranges::find(record.hand_context, value) == record.hand_context.end())
                record.hand_context.push_back(value);
        }
        record.sounds.clear();
        collect_sound_events(document, *node, record.sounds);
        if (record.variants.empty())
            result.diagnostics_.push_back({Diagnostic::Severity::warning, record.source,
                                           "animation-without-file",
                                           "Animation record has no .anm file variant"});
        result.records_.push_back(std::move(record));
    }
    return result;
}

std::vector<const AnimationRecord*>
AnimationCatalog::compatible(const std::string_view model, const std::string_view item,
                             const std::string_view hand) const {
    const auto key = [](const std::string_view value) {
        const std::filesystem::path path{value};
        return norm(path.stem().string().empty() ? std::string(value) : path.stem().string());
    };
    const auto match = [&](const std::vector<std::string>& values, const std::string_view query) {
        return values.empty() || query.empty() ||
               std::ranges::any_of(values, [&](const auto& v) { return key(v) == key(query); });
    };
    std::vector<const AnimationRecord*> out;
    for (const auto& r : records_)
        if (match(r.model_context, model) && match(r.item_context, item) &&
            match(r.hand_context, hand))
            out.push_back(&r);
    return out;
}

const AnimationRecord* AnimationCatalog::find_id(const std::int32_t id) const noexcept {
    const auto found =
        std::ranges::find_if(records_, [&](const auto& record) { return record.id == id; });
    return found == records_.end() ? nullptr : &*found;
}

void ScriptAnimationIndex::add_document(const Document& document) {
    std::vector<const Node*> scripts;
    for (const auto& root : document.roots())
        collect_script_nodes(document, root, scripts);
    for (const auto* script : scripts) {
        const auto* id_node = find(document, *script, {"ID"});
        const auto script_id = id_node ? integer_value(*id_node) : std::nullopt;
        if (!script_id) continue;
        std::string script_name = "script@" + std::to_string(script->entry_index);
        if (const auto* name = find(document, *script, {"NOMBRE", "NAME"}))
            script_name = text(document, *name).value_or(script_name);
        const auto* actions = find(document, *script, {"ACCIONES", "ACTIONS"});
        if (!actions) continue;
        for (const auto& action : actions->children) {
            const auto opcode = first_text(document, action);
            if (!opcode || norm(*opcode).find("ANM") == std::string::npos) continue;
            const auto animation_id = tagged_integer(document, action, {"ANMBDD"});
            if (!animation_id) continue;
            ScriptAnimationUse use{source(document, action),
                                   *script_id,
                                   script_name,
                                   *opcode,
                                   *animation_id,
                                   contains_tag(document, action, {"THIS"}),
                                   tagged_integer(document, action, {"BICHO", "ACTOR"})};
            uses_.push_back(std::move(use));
        }
    }
}

std::vector<const ScriptAnimationUse*>
ScriptAnimationIndex::for_actor(const std::span<const std::int32_t> script_ids,
                                const std::optional<std::int32_t> actor_id) const {
    std::vector<const ScriptAnimationUse*> result;
    for (const auto& use : uses_) {
        const bool owned =
            std::ranges::find(script_ids, use.script_id) != script_ids.end() && use.targets_this;
        const bool explicit_target = actor_id && use.actor_id == actor_id;
        if (owned || explicit_target) result.push_back(&use);
    }
    return result;
}

CutsceneTimeline CutsceneTimeline::project(const Document& document) {
    CutsceneTimeline result;
    std::vector<const Node*> script_nodes;
    for (const auto& root : document.roots())
        collect_script_nodes(document, root, script_nodes);
    std::set<std::string> names;
    for (const auto* node : script_nodes) {
        CutsceneScript script;
        script.source = source(document, *node);
        if (const auto* name = find(document, *node, {"NOMBRE", "NAME"}))
            script.name =
                text(document, *name).value_or("script@" + std::to_string(node->entry_index));
        if (script.name.empty()) script.name = "script@" + std::to_string(node->entry_index);
        const auto* actions = find(document, *node, {"ACCIONES", "ACTIONS"});
        if (!actions) continue;
        for (const auto& action_node : actions->children) {
            const auto opcode = first_text(document, action_node);
            if (!opcode) continue;
            CutsceneAction action;
            action.source = source(document, action_node);
            action.opcode = *opcode;
            action.kind = classify(*opcode);
            std::vector<std::pair<const Node*, std::string>> values;
            strings_recursive(document, action_node, values);
            std::string parameter;
            for (const auto& [value_node, value] : values)
                if (value != *opcode && parameter.empty()) parameter = value;
            if (const auto numeric = first_number(action_node)) {
                if (action.kind == CutsceneActionKind::pause)
                    action.duration = numeric;
                else
                    action.numeric_value = numeric;
                action.explicit_time = labeled_number(document, action_node, {"TIME", "TIEMPO"});
                if (!parameter.empty())
                    action.reference = parameter + "=" + std::to_string(*numeric);
            } else if (!parameter.empty()) {
                action.reference = parameter;
            }
            script.actions.push_back(std::move(action));
        }
        if (!names.insert(script.name).second)
            result.diagnostics_.push_back(
                {Diagnostic::Severity::warning, script.source, "duplicate-cutscene-script",
                 "Duplicate cutscene script name; source identity remains unique"});
        CutsceneBlock block;
        block.index = 0;
        for (std::size_t i = 0; i < script.actions.size(); ++i) {
            block.action_indices.push_back(i);
            const auto kind = script.actions[i].kind;
            if (kind == CutsceneActionKind::wait || kind == CutsceneActionKind::branch) {
                block.runtime_wait = kind == CutsceneActionKind::wait;
                block.conditional = kind == CutsceneActionKind::branch;
                if (i + 1 < script.actions.size()) block.successors.push_back(block.index + 1);
                script.blocks.push_back(std::move(block));
                block = {};
                block.index = static_cast<std::uint32_t>(script.blocks.size());
            }
        }
        if (!block.action_indices.empty()) script.blocks.push_back(std::move(block));
        for (std::size_t i = 0; i + 1 < script.blocks.size(); ++i)
            if (script.blocks[i].successors.empty())
                script.blocks[i].successors.push_back(static_cast<std::uint32_t>(i + 1));
        result.scripts_.push_back(std::move(script));
    }
    return result;
}

const char* cutscene_action_kind_name(const CutsceneActionKind kind) noexcept {
    switch (kind) {
    case CutsceneActionKind::animation:
        return "animation";
    case CutsceneActionKind::camera:
        return "camera";
    case CutsceneActionKind::fov:
        return "fov";
    case CutsceneActionKind::sound:
        return "sound";
    case CutsceneActionKind::pause:
        return "pause";
    case CutsceneActionKind::wait:
        return "runtime-wait";
    case CutsceneActionKind::branch:
        return "branch";
    case CutsceneActionKind::sequence:
        return "sequence";
    case CutsceneActionKind::actor:
        return "actor";
    case CutsceneActionKind::init:
        return "init";
    case CutsceneActionKind::end:
        return "end";
    default:
        return "unknown";
    }
}

} // namespace csf
