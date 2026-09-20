#include "csf/mission_scene.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace csf {
namespace {

std::string label(const Document& document, const Node& node) {
    if (!node.identifier_index) return {};
    const auto* value = document.identifier(*node.identifier_index);
    return value ? value->display_utf8() : std::string{};
}

CsfSourceId source(const Document& document, const Node& node) {
    const auto& entry = document.entries().at(node.entry_index);
    return {document.source_path(), node.entry_index, entry.source};
}

const Node* child(const Document& document, const Node& parent, const std::string_view name) {
    const auto found = std::ranges::find_if(
        parent.children, [&](const Node& value) { return label(document, value) == name; });
    return found == parent.children.end() ? nullptr : &*found;
}

const Node* descendant(const Document& document, const Node& parent, const std::string_view name) {
    if (label(document, parent) == name) return &parent;
    for (const auto& item : parent.children)
        if (const auto* found = descendant(document, item, name)) return found;
    return nullptr;
}

const Node* root_field(const Document& document, const std::string_view name) {
    for (const auto& root : document.roots())
        if (const auto* found = descendant(document, root, name)) return found;
    return nullptr;
}

std::optional<std::int32_t> integer(const Node* node) {
    if (!node) return std::nullopt;
    if (const auto* value = std::get_if<std::int32_t>(&node->scalar)) return *value;
    return std::nullopt;
}

std::optional<float> real(const Node* node) {
    if (!node) return std::nullopt;
    if (const auto* value = std::get_if<float>(&node->scalar)) return *value;
    return std::nullopt;
}

std::optional<std::string> string_value(const Document& document, const Node* node) {
    if (!node) return std::nullopt;
    if (const auto* index = std::get_if<std::uint32_t>(&node->scalar)) {
        if (const auto* value = document.string(*index)) return value->display_utf8();
    }
    return std::nullopt;
}

std::optional<Vec3> vec3(const Node* node) {
    if (!node || node->children.size() < 3) return std::nullopt;
    const auto x = real(&node->children[0]);
    const auto y = real(&node->children[1]);
    const auto z = real(&node->children[2]);
    if (!x || !y || !z || !std::isfinite(*x) || !std::isfinite(*y) || !std::isfinite(*z))
        return std::nullopt;
    return Vec3{*x, *y, *z};
}

enum class ExpectedField { integer, real, string, container, string_or_container };

bool has_expected_kind(const Node& node, const ExpectedField expected) {
    switch (expected) {
    case ExpectedField::integer:
        return std::holds_alternative<std::int32_t>(node.scalar);
    case ExpectedField::real:
        return std::holds_alternative<float>(node.scalar);
    case ExpectedField::string:
        return std::holds_alternative<std::uint32_t>(node.scalar);
    case ExpectedField::container:
        return std::holds_alternative<std::monostate>(node.scalar);
    case ExpectedField::string_or_container:
        return std::holds_alternative<std::uint32_t>(node.scalar) ||
               std::holds_alternative<std::monostate>(node.scalar);
    }
    return false;
}

void validate_fields(const Document& document, const Node& record,
                     const std::map<std::string, ExpectedField>& expected,
                     std::vector<RawField>& raw, std::vector<TypedDiagnostic>& diagnostics) {
    std::map<std::string, std::size_t> occurrences;
    for (const auto& item : record.children)
        ++occurrences[label(document, item)];
    std::map<std::string, std::size_t> seen;
    for (const auto& item : record.children) {
        const auto name = label(document, item);
        const auto specification = expected.find(name);
        const auto ordinal = seen[name]++;
        if (specification == expected.end() || ordinal != 0 ||
            !has_expected_kind(item, specification->second)) {
            raw.push_back({name, source(document, item)});
        }
        if (specification != expected.end() && ordinal == 0 && occurrences[name] > 1) {
            diagnostics.push_back(
                {Diagnostic::Severity::warning, source(document, item), "duplicate-typed-field",
                 "Field " + name + " occurs " + std::to_string(occurrences[name]) + " times"});
        }
        if (specification != expected.end() && !has_expected_kind(item, specification->second)) {
            diagnostics.push_back({Diagnostic::Severity::warning, source(document, item),
                                   "wrong-typed-field",
                                   "Field " + name + " has the wrong value kind"});
        }
    }
}

NavConnection connection(const Document& document, const Node& record,
                         const std::optional<std::int32_t> local_group) {
    NavConnection result;
    result.source = source(document, record);
    result.origin_group = integer(child(document, record, ".GRUPO_ORI"));
    result.destination_group = integer(child(document, record, ".GRUPO_DST"));
    if (!result.origin_group) result.origin_group = local_group;
    if (!result.destination_group) result.destination_group = local_group;
    result.origin_point = integer(child(document, record, ".PUNTO_ORI"));
    result.destination_point = integer(child(document, record, ".PUNTO_DST"));
    return result;
}

void collect_folders(const Document& document, const Node& node, const std::string& parent,
                     std::vector<FolderMembership>& output) {
    const auto name = string_value(document, child(document, node, ".NOMBRE")).value_or("");
    const auto path = parent.empty() ? name : (name.empty() ? parent : parent + "/" + name);
    if (const auto* elements = child(document, node, ".ELEMENTOS")) {
        FolderMembership folder{source(document, node), path, {}};
        for (const auto& item : elements->children)
            if (const auto value = integer(&item)) folder.element_ids.push_back(*value);
        output.push_back(std::move(folder));
    }
    for (const auto& item : node.children) {
        const auto item_label = label(document, item);
        if (!item.children.empty() && item_label != ".ELEMENTOS" && item_label != ".NOMBRE")
            collect_folders(document, item, path, output);
    }
}

std::string json_escape(const std::string_view value) {
    std::ostringstream out;
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"':
            out << "\\\"";
            break;
        case '\\':
            out << "\\\\";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (byte < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(byte)
                    << std::dec;
            else
                out << static_cast<char>(byte);
        }
    }
    return out.str();
}

void json_source(std::ostringstream& out, const CsfSourceId& value) {
    out << "{\"file\":\"" << json_escape(value.file.generic_string())
        << "\",\"entry\":" << value.entry_index << ",\"offset\":" << value.range.offset
        << ",\"size\":" << value.range.size << '}';
}

template <class T>
void json_number(std::ostringstream& out, const T value) {
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) {
            out << "null";
            return;
        }
    }
    out << value;
}
template <class T>
void json_optional_number(std::ostringstream& out, const std::optional<T>& value) {
    if (value)
        json_number(out, *value);
    else
        out << "null";
}
void json_optional_string(std::ostringstream& out, const std::optional<std::string>& value) {
    if (value)
        out << '"' << json_escape(*value) << '"';
    else
        out << "null";
}
void json_vec(std::ostringstream& out, const std::optional<Vec3>& value) {
    if (value && std::isfinite(value->x) && std::isfinite(value->y) && std::isfinite(value->z))
        out << '[' << value->x << ',' << value->y << ',' << value->z << ']';
    else
        out << "null";
}
void json_raw_fields(std::ostringstream& out, const std::vector<RawField>& fields) {
    out << '[';
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index) out << ',';
        out << "{\"name\":\"" << json_escape(fields[index].name) << "\",\"source\":";
        json_source(out, fields[index].source);
        out << '}';
    }
    out << ']';
}

} // namespace

float mission_actor_angle_radians(const float degrees) noexcept {
    return degrees * 0.01745329251994329577F;
}

const NavPoint* MissionScene::navigation_point(const std::int32_t group_id,
                                               const std::int32_t point_id) const noexcept {
    const NavGroup* owner{};
    for (const auto& group : navigation_)
        if (group.id == group_id) {
            if (owner) return nullptr;
            owner = &group;
        }
    if (!owner) return nullptr;
    const NavPoint* result{};
    for (const auto& point : owner->points)
        if (point.id == point_id) {
            if (result) return nullptr;
            result = &point;
        }
    return result;
}

std::optional<Vec3>
MissionScene::actor_spawn_position(const MissionActor& actor) const noexcept {
    if (actor.group && actor.cell && *actor.group >= 0 && *actor.cell >= 0)
        if (const auto* point = navigation_point(*actor.group, *actor.cell); point && point->position)
            return point->position;
    return actor.position;
}

MissionScene MissionScene::project(const Document& document) {
    MissionScene scene;
    scene.source_path_ = document.source_path();
    if (document.state() == ParseState::non_csffbs) {
        const CsfSourceId location{document.source_path(), 0, document.header().source};
        scene.diagnostics_.push_back(
            {Diagnostic::Severity::error, location, "document-not-projectable",
             "SCN typed projection requires a structurally valid CSFFBS document"});
        return scene;
    }
    for (const auto& diagnostic : document.diagnostics()) {
        const CsfSourceId location{
            document.source_path(), diagnostic.entry_index.value_or(0), {diagnostic.offset, 0}};
        scene.diagnostics_.push_back(
            {diagnostic.severity, location, "document-diagnostic", diagnostic.message});
    }
    auto scalar = [&](const std::string_view name) { return integer(root_field(document, name)); };
    scene.player_.active_player = scalar(".PLAYER");
    scene.player_.commando_start = scalar(".INICIO_COMMANDO");
    scene.player_.sniper_start = scalar(".INICIO_SNIPER");
    scene.player_.spy_start = scalar(".INICIO_SPY");

    if (const auto* world = root_field(document, ".MUNDOVIS")) {
        for (const auto& item : world->children) {
            EnvironmentField field;
            field.name = label(document, item);
            field.source = source(document, item);
            if (const auto* integer_value = std::get_if<std::int32_t>(&item.scalar))
                field.value = *integer_value;
            else if (const auto* real_value = std::get_if<float>(&item.scalar))
                field.value = *real_value;
            else if (const auto text_value = string_value(document, &item))
                field.value = *text_value;
            else
                continue;
            scene.environment_.push_back(std::move(field));
        }
    }

    if (const auto* actors = root_field(document, ".BICHOS")) {
        for (const auto& record : actors->children) {
            MissionActor actor;
            actor.source = source(document, record);
            actor.name = string_value(document, child(document, record, ".NOMBRE"));
            actor.id = integer(child(document, record, ".ID"));
            actor.class_id = integer(child(document, record, ".CLASSID"));
            actor.position = vec3(child(document, record, ".POS"));
            actor.heading = real(child(document, record, ".ANGULO"));
            actor.pitch = real(child(document, record, ".ANGULO_X"));
            if (const auto* scripts = child(document, record, ".SCRIPT")) {
                actor.script = string_value(document, scripts);
                if (const auto value = integer(scripts)) actor.script_ids.push_back(*value);
                for (const auto& item : scripts->children)
                    if (const auto value = integer(&item)) actor.script_ids.push_back(*value);
            }
            actor.collision = integer(child(document, record, ".COLISION"));
            if (const auto flags = integer(child(document, record, ".FLAGS")))
                actor.flags = static_cast<std::uint32_t>(*flags);
            actor.secondary_explosion =
                integer(child(document, record, ".SEGUNDA_EXPLOSION"));
            actor.faction = string_value(document, child(document, record, ".BANDO"));
            actor.portrait = string_value(document, child(document, record, ".PORTRAIT"));
            if (const auto* animations = child(document, record, ".ANIMACIONES"))
                for (const auto& item : animations->children)
                    actor.animations.push_back(
                        {integer(child(document, item, ".ID")),
                         string_value(document, child(document, item, ".TIPO"))});
            if (const auto* box = child(document, record, ".DOOR_BOX");
                box && box->children.size() >= 2) {
                const auto first = vec3(&box->children[0]);
                const auto second = vec3(&box->children[1]);
                if (first && second) actor.door_box = std::array<Vec3, 2>{*first, *second};
            }
            if (const auto* cell = child(document, record, ".CELDA")) {
                actor.group = integer(child(document, *cell, ".GRUPO"));
                actor.cell = integer(child(document, *cell, ".PUNTO"));
            }
            validate_fields(document, record,
                            {{".NOMBRE", ExpectedField::string},
                             {".ID", ExpectedField::integer},
                             {".CLASSID", ExpectedField::integer},
                             {".POS", ExpectedField::container},
                             {".ANGULO", ExpectedField::real},
                             {".ANGULO_X", ExpectedField::real},
                             {".SCRIPT", ExpectedField::string_or_container},
                             {".COLISION", ExpectedField::integer},
                             {".FLAGS", ExpectedField::integer},
                             {".SEGUNDA_EXPLOSION", ExpectedField::integer},
                             {".BANDO", ExpectedField::string},
                             {".PORTRAIT", ExpectedField::string},
                             {".ANIMACIONES", ExpectedField::container},
                             {".DOOR_BOX", ExpectedField::container},
                             {".CELDA", ExpectedField::container}},
                            actor.unknown_fields, scene.diagnostics_);
            if (!actor.position)
                scene.diagnostics_.push_back({Diagnostic::Severity::warning, actor.source,
                                              "actor-position",
                                              "Actor has no finite three-real .POS value"});
            scene.actors_.push_back(std::move(actor));
        }
    }

    if (const auto* effects = root_field(document, ".EFECTOS")) {
        for (const auto& record : effects->children) {
            MissionEffect effect{source(document, record)};
            effect.id = integer(child(document, record, ".ID"));
            effect.name = string_value(document, child(document, record, ".NOMBRE"));
            effect.class_id = integer(child(document, record, ".CLASSID"));
            effect.dummy_id = integer(child(document, record, ".DUMMY"));
            effect.priority = integer(child(document, record, ".PRIORITY"));
            effect.share_group = integer(child(document, record, ".SHARE_GROUP"));
            validate_fields(document, record,
                            {{".ID", ExpectedField::integer},
                             {".NOMBRE", ExpectedField::string},
                             {".CLASSID", ExpectedField::integer},
                             {".DUMMY", ExpectedField::integer},
                             {".PRIORITY", ExpectedField::integer},
                             {".SHARE_GROUP", ExpectedField::integer}},
                            effect.unknown_fields, scene.diagnostics_);
            scene.effects_.push_back(std::move(effect));
        }
    }

    if (const auto* nav = root_field(document, ".MALLA_NAVEGACION")) {
        if (const auto* groups = child(document, *nav, ".GRUPOS")) {
            for (const auto& record : groups->children) {
                NavGroup group;
                group.source = source(document, record);
                group.id = integer(child(document, record, ".ID"));
                group.name = string_value(document, child(document, record, ".NOMBRE"));
                group.type = integer(child(document, record, ".TIPO"));
                if (const auto* points = child(document, record, ".PUNTOS")) {
                    for (const auto& item : points->children) {
                        NavPoint point;
                        point.source = source(document, item);
                        point.group_id = group.id;
                        point.id = integer(child(document, item, ".ID"));
                        point.name = string_value(document, child(document, item, ".NOMBRE"));
                        point.position = vec3(child(document, item, ".POS"));
                        point.heading = real(child(document, item, ".ROT"));
                        point.pitch = real(child(document, item, ".ROT_X"));
                        validate_fields(document, item,
                                        {{".ID", ExpectedField::integer},
                                         {".NOMBRE", ExpectedField::string},
                                         {".POS", ExpectedField::container},
                                         {".ROT", ExpectedField::real},
                                         {".ROT_X", ExpectedField::real}},
                                        point.unknown_fields, scene.diagnostics_);
                        group.points.push_back(std::move(point));
                    }
                }
                if (const auto* links = child(document, record, ".CONEXIONES"))
                    for (const auto& item : links->children)
                        group.connections.push_back(connection(document, item, group.id));
                validate_fields(document, record,
                                {{".ID", ExpectedField::integer},
                                 {".NOMBRE", ExpectedField::string},
                                 {".TIPO", ExpectedField::integer},
                                 {".PUNTOS", ExpectedField::container},
                                 {".CONEXIONES", ExpectedField::container}},
                                group.unknown_fields, scene.diagnostics_);
                scene.navigation_.push_back(std::move(group));
            }
        }
        for (const auto& item : nav->children) {
            if (label(document, item) == ".GRUPOS") continue;
            if (label(document, item).find("CONEX") != std::string::npos)
                for (const auto& record : item.children)
                    scene.cross_group_.push_back(connection(document, record, std::nullopt));
        }
    }

    if (const auto* mesh = root_field(document, ".MALLA_DUMMIES")) {
        if (const auto* records = child(document, *mesh, ".DUMMIES")) {
            for (const auto& record : records->children) {
                MissionDummy dummy{source(document, record)};
                dummy.id = integer(child(document, record, ".ID"));
                dummy.name = string_value(document, child(document, record, ".NOMBRE"));
                dummy.position = vec3(child(document, record, ".POS"));
                dummy.heading = real(child(document, record, ".ROT"));
                dummy.pitch = real(child(document, record, ".ROT_X"));
                validate_fields(document, record,
                                {{".ID", ExpectedField::integer},
                                 {".NOMBRE", ExpectedField::string},
                                 {".POS", ExpectedField::container},
                                 {".ROT", ExpectedField::real},
                                 {".ROT_X", ExpectedField::real}},
                                dummy.unknown_fields, scene.diagnostics_);
                scene.dummies_.push_back(std::move(dummy));
            }
        }
        if (const auto* folders = child(document, *mesh, ".CARPETAS"))
            collect_folders(document, *folders, "", scene.folders_);
    }

    if (const auto* mesh = root_field(document, ".MALLA_AREAS")) {
        if (const auto* records = child(document, *mesh, ".AREAS")) {
            for (const auto& record : records->children) {
                MissionArea area{source(document, record)};
                area.id = integer(child(document, record, ".ID"));
                area.name = string_value(document, child(document, record, ".NOMBRE"));
                area.flags = integer(child(document, record, ".FLAGS"));
                area.occlusion = integer(child(document, record, ".OCLUSION"));
                area.height = real(child(document, record, ".HEIGHT"));
                area.reverb = integer(child(document, record, ".REVERB"));
                area.limit_reverb = integer(child(document, record, ".LIMITREVERB"));
                if (const auto* points = child(document, record, ".PUNTOS"))
                    for (const auto& item : points->children)
                        if (const auto value = vec3(child(document, item, ".POS")))
                            area.points.push_back(*value);
                validate_fields(document, record,
                                {{".ID", ExpectedField::integer},
                                 {".NOMBRE", ExpectedField::string},
                                 {".FLAGS", ExpectedField::integer},
                                 {".OCLUSION", ExpectedField::integer},
                                 {".HEIGHT", ExpectedField::real},
                                 {".REVERB", ExpectedField::integer},
                                 {".LIMITREVERB", ExpectedField::integer},
                                 {".PUNTOS", ExpectedField::container}},
                                area.unknown_fields, scene.diagnostics_);
                scene.areas_.push_back(std::move(area));
            }
        }
    }

    if (const auto* mesh = root_field(document, ".MALLA_LUCES")) {
        if (const auto* records = child(document, *mesh, ".LIGHTS")) {
            for (const auto& record : records->children) {
                MissionLight light{source(document, record)};
                light.id = integer(child(document, record, ".ID"));
                light.name = string_value(document, child(document, record, ".NOMBRE"));
                light.position = vec3(child(document, record, ".POS"));
                if (const auto color = integer(child(document, record, ".COLOR")))
                    light.color = static_cast<std::uint32_t>(*color);
                light.modulate = integer(child(document, record, ".MODULATE"));
                light.radius = real(child(document, record, ".RADIO"));
                validate_fields(document, record,
                                {{".ID", ExpectedField::integer},
                                 {".NOMBRE", ExpectedField::string},
                                 {".POS", ExpectedField::container},
                                 {".COLOR", ExpectedField::integer},
                                 {".MODULATE", ExpectedField::integer},
                                 {".RADIO", ExpectedField::real}},
                                light.unknown_fields, scene.diagnostics_);
                scene.lights_.push_back(std::move(light));
            }
        }
        if (const auto* folders = child(document, *mesh, ".CARPETAS"))
            collect_folders(document, *folders, "", scene.folders_);
    }

    auto& stats = scene.navigation_stats_;
    stats.groups = scene.navigation_.size();
    using Key = std::pair<std::int32_t, std::int32_t>;
    std::map<std::int32_t, std::size_t> group_counts;
    std::map<Key, std::size_t> point_counts;
    for (const auto& group : scene.navigation_) {
        if (group.id && ++group_counts[*group.id] > 1) ++stats.duplicate_group_ids;
        stats.points += group.points.size();
        for (const auto& point : group.points)
            if (point.group_id && point.id && ++point_counts[{*point.group_id, *point.id}] > 1)
                ++stats.duplicate_point_ids;
    }
    std::vector<NavConnection*> links;
    for (auto& group : scene.navigation_)
        for (auto& link : group.connections)
            links.push_back(&link);
    for (auto& link : scene.cross_group_)
        links.push_back(&link);
    stats.connections = links.size();
    std::map<Key, std::vector<Key>> adjacency;
    for (const auto& [key, count] : point_counts)
        if (count == 1 && group_counts[key.first] == 1) adjacency[key];
    for (auto* link : links) {
        if (!link->origin_group || !link->origin_point || !link->destination_group ||
            !link->destination_point)
            link->invalid_reason = "missing-identity";
        else {
            const Key origin{*link->origin_group, *link->origin_point};
            const Key destination{*link->destination_group, *link->destination_point};
            if (group_counts[origin.first] != 1 || group_counts[destination.first] != 1 ||
                point_counts[origin] != 1 || point_counts[destination] != 1)
                link->invalid_reason =
                    (group_counts[origin.first] > 1 || group_counts[destination.first] > 1 ||
                     point_counts[origin] > 1 || point_counts[destination] > 1)
                        ? "ambiguous-identity"
                        : "missing-endpoint";
        }
        link->valid = link->invalid_reason.empty();
        if (!link->valid) {
            ++stats.invalid_connections;
            continue;
        }
        const Key a{*link->origin_group, *link->origin_point},
            b{*link->destination_group, *link->destination_point};
        adjacency[a].push_back(b);
        adjacency[b].push_back(a);
    }
    std::set<Key> visited;
    for (const auto& [point, neighbors] : adjacency) {
        if (neighbors.empty()) ++stats.orphan_points;
        if (visited.contains(point)) continue;
        ++stats.connected_components;
        std::queue<Key> queue;
        queue.push(point);
        visited.insert(point);
        while (!queue.empty()) {
            const auto here = queue.front();
            queue.pop();
            for (const auto next : adjacency[here])
                if (visited.insert(next).second) queue.push(next);
        }
    }
    if (stats.invalid_connections)
        scene.diagnostics_.push_back(
            {Diagnostic::Severity::warning,
             CsfSourceId{document.source_path(), 0, document.header().source},
             "invalid-navigation-reference",
             std::to_string(stats.invalid_connections) +
                 " navigation connection(s) reference missing groups or points"});
    std::map<std::int32_t, std::size_t> dummy_counts;
    for (const auto& dummy : scene.dummies_)
        if (dummy.id) ++dummy_counts[*dummy.id];
    for (const auto& effect : scene.effects_)
        if (!effect.dummy_id || dummy_counts[*effect.dummy_id] != 1)
            scene.diagnostics_.push_back(
                {Diagnostic::Severity::warning, effect.source, "invalid-effect-dummy",
                 !effect.dummy_id
                     ? "Effect has no dummy placement reference"
                     : "Effect dummy ID " + std::to_string(*effect.dummy_id) +
                           (dummy_counts[*effect.dummy_id] == 0 ? " is missing" : " is ambiguous")});
    auto non_finite = [&](const CsfSourceId& where, const std::string& field,
                          const std::optional<float> value) {
        if (value && !std::isfinite(*value))
            scene.diagnostics_.push_back({Diagnostic::Severity::warning, where, "non-finite-number",
                                          field + " is non-finite and is exported as null"});
    };
    for (const auto& field : scene.environment_)
        if (const auto* value = std::get_if<float>(&field.value); value && !std::isfinite(*value))
            scene.diagnostics_.push_back({Diagnostic::Severity::warning, field.source,
                                          "non-finite-number",
                                          field.name + " is non-finite and is exported as null"});
    for (const auto& actor : scene.actors_) {
        non_finite(actor.source, ".ANGULO", actor.heading);
        non_finite(actor.source, ".ANGULO_X", actor.pitch);
    }
    for (const auto& group : scene.navigation_)
        for (const auto& point : group.points) {
            non_finite(point.source, ".ROT", point.heading);
            non_finite(point.source, ".ROT_X", point.pitch);
        }
    for (const auto& value : scene.dummies_) {
        non_finite(value.source, ".ROT", value.heading);
        non_finite(value.source, ".ROT_X", value.pitch);
    }
    for (const auto& value : scene.areas_)
        non_finite(value.source, ".HEIGHT", value.height);
    for (const auto& value : scene.lights_)
        non_finite(value.source, ".RADIO", value.radius);
    return scene;
}

void MissionSymbolIndex::add_scene(const MissionScene& scene) {
    auto add = [&](const std::optional<std::string>& name, const SymbolCategory category,
                   const CsfSourceId& source, const std::string& field) {
        if (name) sites_.push_back({*name, category, SymbolRole::definition, source, field});
    };
    for (const auto& value : scene.actors()) {
        add(value.name, SymbolCategory::actor, value.source, ".NOMBRE");
        if (value.script)
            sites_.push_back({*value.script, SymbolCategory::script, SymbolRole::typed_reference,
                              value.source, ".SCRIPT"});
        for (const auto id : value.script_ids)
            sites_.push_back({"script:" + std::to_string(id), SymbolCategory::script,
                              SymbolRole::typed_reference, value.source, ".SCRIPT"});
        if (value.class_id)
            sites_.push_back({"class:" + std::to_string(*value.class_id),
                              SymbolCategory::database_record, SymbolRole::typed_reference,
                              value.source, ".CLASSID"});
    }
    for (const auto& group : scene.navigation()) {
        add(group.name, SymbolCategory::navigation_group, group.source, ".NOMBRE");
        for (const auto& point : group.points)
            add(point.name, SymbolCategory::navigation_point, point.source, ".NOMBRE");
    }
    for (const auto& value : scene.dummies())
        add(value.name, SymbolCategory::dummy, value.source, ".NOMBRE");
    for (const auto& value : scene.areas())
        add(value.name, SymbolCategory::area, value.source, ".NOMBRE");
    for (const auto& value : scene.lights())
        add(value.name, SymbolCategory::light, value.source, ".NOMBRE");
    for (const auto& value : scene.effects()) {
        add(value.name, SymbolCategory::effect, value.source, ".NOMBRE");
        if (value.class_id)
            sites_.push_back({"class:" + std::to_string(*value.class_id), SymbolCategory::effect,
                              SymbolRole::typed_reference, value.source, ".CLASSID"});
        if (value.dummy_id)
            sites_.push_back({"dummy:" + std::to_string(*value.dummy_id),
                              SymbolCategory::dummy, SymbolRole::typed_reference, value.source,
                              ".DUMMY"});
    }
}

void MissionSymbolIndex::add_document(const Document& document) {
    auto extension = document.source_path().extension().string();
    std::ranges::transform(extension, extension.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    auto filename = document.source_path().filename().string();
    std::ranges::transform(filename, filename.begin(), [](const unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    std::function<void(const Node&, std::string_view)> visit = [&](const Node& node,
                                                                   const std::string_view context) {
        const auto node_label = label(document, node);
        const auto next_context = node_label.empty() ? context : std::string_view(node_label);
        if (const auto name = string_value(document, child(document, node, ".NOMBRE"));
            name && !name->empty()) {
            if (extension == ".bdd") {
                sites_.push_back({*name, SymbolCategory::database_record, SymbolRole::definition,
                                  source(document, node), ".NOMBRE"});
                auto class_id = integer(child(document, node, ".CLASSID"));
                if (!class_id && filename == "objetos.bdd")
                    class_id = integer(child(document, node, ".ID"));
                if (class_id)
                    sites_.push_back({"class:" + std::to_string(*class_id),
                                      SymbolCategory::database_record, SymbolRole::definition,
                                      source(document, node), ".CLASSID"});
            } else if ((extension == ".gsc" || extension == ".csc") &&
                       (context.find("SCRIPT") != std::string_view::npos ||
                        node_label.find("SCRIPT") != std::string::npos)) {
                sites_.push_back({*name, SymbolCategory::script, SymbolRole::definition,
                                  source(document, node), ".NOMBRE"});
            }
        }
        if (const auto value = string_value(document, &node); value && !value->empty())
            sites_.push_back({*value, SymbolCategory::unknown, SymbolRole::candidate,
                              source(document, node), node_label});
        for (const auto& item : node.children)
            visit(item, next_context);
    };
    for (const auto& root : document.roots())
        visit(root, {});
}

std::vector<const SymbolSite*> MissionSymbolIndex::exact(const std::string_view symbol) const {
    std::vector<const SymbolSite*> result;
    for (const auto& site : sites_)
        if (site.symbol == symbol) result.push_back(&site);
    return result;
}

std::string mission_scene_json(const MissionScene& scene) {
    std::ostringstream out;
    out << std::setprecision(9) << "{\"schema\":\"csf-mission-scene-1\",\"source\":\""
        << json_escape(scene.source_path().generic_string()) << "\"";
    out << ",\"player\":{\"active\":";
    json_optional_number(out, scene.player().active_player);
    out << ",\"commando_start\":";
    json_optional_number(out, scene.player().commando_start);
    out << ",\"sniper_start\":";
    json_optional_number(out, scene.player().sniper_start);
    out << ",\"spy_start\":";
    json_optional_number(out, scene.player().spy_start);
    out << '}';
    out << ",\"environment\":[";
    bool comma = false;
    for (const auto& field : scene.environment()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"name\":\"" << json_escape(field.name) << "\",\"source\":";
        json_source(out, field.source);
        out << ",\"value\":";
        if (const auto* integer_value = std::get_if<std::int32_t>(&field.value))
            out << *integer_value;
        else if (const auto* real_value = std::get_if<float>(&field.value))
            json_number(out, *real_value);
        else
            out << '"' << json_escape(std::get<std::string>(field.value)) << '"';
        out << '}';
    }
    out << ']';
    out << ",\"actors\":[";
    comma = false;
    for (const auto& value : scene.actors()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"source\":";
        json_source(out, value.source);
        out << ",\"name\":";
        json_optional_string(out, value.name);
        out << ",\"id\":";
        json_optional_number(out, value.id);
        out << ",\"class_id\":";
        json_optional_number(out, value.class_id);
        out << ",\"position\":";
        json_vec(out, value.position);
        out << ",\"heading\":";
        json_optional_number(out, value.heading);
        out << ",\"pitch\":";
        json_optional_number(out, value.pitch);
        out << ",\"script\":";
        json_optional_string(out, value.script);
        out << ",\"script_ids\":[";
        for (std::size_t i = 0; i < value.script_ids.size(); ++i) {
            if (i) out << ',';
            out << value.script_ids[i];
        }
        out << "],\"group\":";
        json_optional_number(out, value.group);
        out << ",\"cell\":";
        json_optional_number(out, value.cell);
        out << ",\"collision\":";
        json_optional_number(out, value.collision);
        out << ",\"flags\":";
        json_optional_number(out, value.flags);
        out << ",\"secondary_explosion\":";
        json_optional_number(out, value.secondary_explosion);
        out << ",\"faction\":";
        json_optional_string(out, value.faction);
        out << ",\"portrait\":";
        json_optional_string(out, value.portrait);
        out << ",\"animations\":[";
        for (std::size_t i = 0; i < value.animations.size(); ++i) {
            if (i) out << ',';
            out << "{\"id\":";
            json_optional_number(out, value.animations[i].id);
            out << ",\"type\":";
            json_optional_string(out, value.animations[i].type);
            out << '}';
        }
        out << "],\"door_box\":";
        if (value.door_box) {
            out << "[[" << (*value.door_box)[0].x << ',' << (*value.door_box)[0].y << ','
                << (*value.door_box)[0].z << "],[" << (*value.door_box)[1].x << ','
                << (*value.door_box)[1].y << ',' << (*value.door_box)[1].z << "]]";
        } else
            out << "null";
        out << ",\"unknown_fields\":";
        json_raw_fields(out, value.unknown_fields);
        out << '}';
    }
    out << "],\"navigation\":{\"stats\":{\"groups\":" << scene.navigation_stats().groups
        << ",\"points\":" << scene.navigation_stats().points
        << ",\"connections\":" << scene.navigation_stats().connections
        << ",\"components\":" << scene.navigation_stats().connected_components
        << ",\"orphans\":" << scene.navigation_stats().orphan_points
        << ",\"invalid\":" << scene.navigation_stats().invalid_connections
        << ",\"duplicate_group_ids\":" << scene.navigation_stats().duplicate_group_ids
        << ",\"duplicate_point_ids\":" << scene.navigation_stats().duplicate_point_ids
        << "},\"groups\":[";
    comma = false;
    for (const auto& group : scene.navigation()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"source\":";
        json_source(out, group.source);
        out << ",\"id\":";
        json_optional_number(out, group.id);
        out << ",\"name\":";
        json_optional_string(out, group.name);
        out << ",\"type\":";
        json_optional_number(out, group.type);
        out << ",\"points\":[";
        bool inner = false;
        for (const auto& point : group.points) {
            if (inner) out << ',';
            inner = true;
            out << "{\"source\":";
            json_source(out, point.source);
            out << ",\"group_id\":";
            json_optional_number(out, point.group_id);
            out << ",\"id\":";
            json_optional_number(out, point.id);
            out << ",\"name\":";
            json_optional_string(out, point.name);
            out << ",\"position\":";
            json_vec(out, point.position);
            out << ",\"heading\":";
            json_optional_number(out, point.heading);
            out << ",\"pitch\":";
            json_optional_number(out, point.pitch);
            out << ",\"unknown_fields\":";
            json_raw_fields(out, point.unknown_fields);
            out << '}';
        }
        out << "],\"unknown_fields\":";
        json_raw_fields(out, group.unknown_fields);
        out << ",\"connections\":[";
        inner = false;
        for (const auto& link : group.connections) {
            if (inner) out << ',';
            inner = true;
            out << "{\"source\":";
            json_source(out, link.source);
            out << ",\"origin_group\":";
            json_optional_number(out, link.origin_group);
            out << ",\"origin_point\":";
            json_optional_number(out, link.origin_point);
            out << ",\"destination_group\":";
            json_optional_number(out, link.destination_group);
            out << ",\"destination_point\":";
            json_optional_number(out, link.destination_point);
            out << ",\"valid\":" << (link.valid ? "true" : "false") << ",\"invalid_reason\":\""
                << json_escape(link.invalid_reason) << "\"}";
        }
        out << "]}";
    }
    out << "],\"cross_group_connections\":[";
    comma = false;
    for (const auto& link : scene.cross_group_connections()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"source\":";
        json_source(out, link.source);
        out << ",\"origin_group\":";
        json_optional_number(out, link.origin_group);
        out << ",\"origin_point\":";
        json_optional_number(out, link.origin_point);
        out << ",\"destination_group\":";
        json_optional_number(out, link.destination_group);
        out << ",\"destination_point\":";
        json_optional_number(out, link.destination_point);
        out << ",\"valid\":" << (link.valid ? "true" : "false") << ",\"invalid_reason\":\""
            << json_escape(link.invalid_reason) << "\"}";
    }
    out << "]}";
    out << ",\"dummies\":[";
    comma = false;
    for (const auto& value : scene.dummies()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"source\":";
        json_source(out, value.source);
        out << ",\"name\":";
        json_optional_string(out, value.name);
        out << ",\"id\":";
        json_optional_number(out, value.id);
        out << ",\"position\":";
        json_vec(out, value.position);
        out << ",\"heading\":";
        json_optional_number(out, value.heading);
        out << ",\"pitch\":";
        json_optional_number(out, value.pitch);
        out << ",\"unknown_fields\":";
        json_raw_fields(out, value.unknown_fields);
        out << '}';
    }
    out << "],\"areas\":[";
    comma = false;
    for (const auto& value : scene.areas()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"source\":";
        json_source(out, value.source);
        out << ",\"name\":";
        json_optional_string(out, value.name);
        out << ",\"id\":";
        json_optional_number(out, value.id);
        out << ",\"flags\":";
        json_optional_number(out, value.flags);
        out << ",\"occlusion\":";
        json_optional_number(out, value.occlusion);
        out << ",\"height\":";
        json_optional_number(out, value.height);
        out << ",\"reverb\":";
        json_optional_number(out, value.reverb);
        out << ",\"limit_reverb\":";
        json_optional_number(out, value.limit_reverb);
        out << ",\"points\":[";
        for (std::size_t i = 0; i < value.points.size(); ++i) {
            if (i) out << ',';
            out << '[';
            json_number(out, value.points[i].x);
            out << ',';
            json_number(out, value.points[i].y);
            out << ',';
            json_number(out, value.points[i].z);
            out << ']';
        }
        out << "],\"unknown_fields\":";
        json_raw_fields(out, value.unknown_fields);
        out << '}';
    }
    out << "],\"lights\":[";
    comma = false;
    for (const auto& value : scene.lights()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"source\":";
        json_source(out, value.source);
        out << ",\"name\":";
        json_optional_string(out, value.name);
        out << ",\"id\":";
        json_optional_number(out, value.id);
        out << ",\"position\":";
        json_vec(out, value.position);
        out << ",\"color\":";
        json_optional_number(out, value.color);
        out << ",\"modulate\":";
        json_optional_number(out, value.modulate);
        out << ",\"radius\":";
        json_optional_number(out, value.radius);
        out << ",\"unknown_fields\":";
        json_raw_fields(out, value.unknown_fields);
        out << '}';
    }
    out << "],\"effects\":[";
    comma = false;
    for (const auto& value : scene.effects()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"source\":";
        json_source(out, value.source);
        out << ",\"id\":";
        json_optional_number(out, value.id);
        out << ",\"name\":";
        json_optional_string(out, value.name);
        out << ",\"class_id\":";
        json_optional_number(out, value.class_id);
        out << ",\"dummy_id\":";
        json_optional_number(out, value.dummy_id);
        out << ",\"priority\":";
        json_optional_number(out, value.priority);
        out << ",\"share_group\":";
        json_optional_number(out, value.share_group);
        out << ",\"unknown_fields\":";
        json_raw_fields(out, value.unknown_fields);
        out << '}';
    }
    out << "],\"folders\":[";
    comma = false;
    for (const auto& value : scene.folders()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"source\":";
        json_source(out, value.source);
        out << ",\"path\":\"" << json_escape(value.path) << "\",\"element_ids\":[";
        for (std::size_t i = 0; i < value.element_ids.size(); ++i) {
            if (i) out << ',';
            out << value.element_ids[i];
        }
        out << "]}";
    }
    out << "],\"diagnostics\":[";
    comma = false;
    for (const auto& value : scene.diagnostics()) {
        if (comma) out << ',';
        comma = true;
        out << "{\"severity\":\""
            << (value.severity == Diagnostic::Severity::error ? "error" : "warning")
            << "\",\"code\":\"" << json_escape(value.code) << "\",\"message\":\""
            << json_escape(value.message) << "\",\"source\":";
        json_source(out, value.source);
        out << '}';
    }
    out << "]}\n";
    return out.str();
}

const char* symbol_category_name(const SymbolCategory value) noexcept {
    switch (value) {
    case SymbolCategory::actor:
        return "actor";
    case SymbolCategory::navigation_group:
        return "navigation-group";
    case SymbolCategory::navigation_point:
        return "navigation-point";
    case SymbolCategory::dummy:
        return "dummy";
    case SymbolCategory::area:
        return "area";
    case SymbolCategory::light:
        return "light";
    case SymbolCategory::effect:
        return "effect";
    case SymbolCategory::script:
        return "script";
    case SymbolCategory::database_record:
        return "database-record";
    case SymbolCategory::unknown:
        return "unknown";
    }
    return "unknown";
}
const char* symbol_role_name(const SymbolRole value) noexcept {
    switch (value) {
    case SymbolRole::definition:
        return "definition";
    case SymbolRole::typed_reference:
        return "typed-reference";
    case SymbolRole::candidate:
        return "candidate";
    }
    return "candidate";
}

} // namespace csf
