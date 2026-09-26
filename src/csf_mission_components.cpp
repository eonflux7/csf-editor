#include "csf/mission_components.hpp"

#include "csf/authoring.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <stdexcept>

namespace csf {
namespace {

using Type = MissionRecordId::Type;

constexpr std::string_view list_header =
    "# rws-man components: each block is a component's operation lines (csf/mission_ops.hpp)\n";

std::optional<Type> type_named(const std::string_view name) {
    for (const auto type : {Type::actor, Type::navigation_group, Type::dummy, Type::area, Type::script,
                            Type::cutscene_script})
        if (name == record_type_name(type)) return type;
    return std::nullopt;
}

// Keys that hold the ID of a record the line makes.
struct IdKey {
    std::string_view op, key;
    Type type;
};
constexpr IdKey id_keys[] = {
    {"guard-patrol", "id", Type::actor},      {"guard-patrol", "route", Type::navigation_group},
    {"guard-patrol", "script", Type::script}, {"guard-idle", "id", Type::actor},
    {"guard-idle", "script", Type::script},   {"animal-patrol", "id", Type::actor},
    {"animal-patrol", "route", Type::navigation_group}, {"animal-patrol", "script", Type::script},
    {"cover-group", "id", Type::navigation_group}, {"walk-grid", "id", Type::navigation_group},
    {"objective", "script", Type::script},    {"objectives", "setup", Type::script},
    {"equipment", "script", Type::script},    {"tips", "script", Type::script},
    {"intro", "script", Type::script},        {"actor", "id", Type::actor},
    {"prop", "id", Type::actor},              {"nav-group", "id", Type::navigation_group},
    {"dummy", "id", Type::dummy},             {"area", "id", Type::area},
};

// Operations that make a component on their own, and those that collect
// lines for the one that ends them.
bool is_recipe(const std::string_view op) {
    return op == "guard-patrol" || op == "guard-idle" || op == "animal-patrol" || op == "cover-group" ||
           op == "walk-grid" || op == "tips" || op == "objectives" || op == "equipment" || op == "intro";
}
std::string_view collector_end(const std::string_view op) {
    if (op == "objective") return "objectives";
    if (op == "kit") return "equipment";
    if (op == "shot") return "intro";
    return {};
}

std::vector<OpLine> parse_lines(const std::string_view text) {
    std::vector<OpLine> lines;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto end = std::min(text.find('\n', begin), text.size());
        const auto raw = text.substr(begin, end - begin);
        begin = end + 1;
        const auto first = raw.find_first_not_of(" \t\r");
        if (first == std::string_view::npos || raw[first] == '#') continue;
        lines.push_back(parse_op_line(raw));
    }
    return lines;
}

std::string join(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) text += line + '\n';
    return text;
}

// Fills the record IDs the lines leave out with free ones, so running the
// lines again makes the same records.
std::vector<std::string> pin_ids(const MissionEditor& editor, std::vector<OpLine> lines) {
    std::map<Type, std::int32_t> next;
    const auto bump = [&](const Type type, const std::int32_t id) {
        const auto key = type == Type::cutscene_script ? Type::script : type;  // one ID space
        next[key] = std::max(next[key], id + 1);
    };
    for (const auto& record : editor.record_ids()) bump(record.type, record.id);
    bump(Type::script, editor.next_script_id(0) - 1);
    for (auto type : {Type::actor, Type::navigation_group, Type::dummy, Type::area, Type::script})
        next[type] = std::max(next[type], 1);
    const auto number = [](const std::string& text) {
        try {
            return std::stoi(text);
        } catch (const std::exception&) {
            throw std::invalid_argument("invalid ID '" + text + "'");
        }
    };
    const auto shots = std::ranges::count(lines, std::string("shot"), &OpLine::op);
    // IDs the lines give count as taken.
    for (const auto& line : lines) {
        for (const auto& key : id_keys)
            if (key.op == line.op)
                if (const auto* value = line.find(key.key)) bump(key.type, number(*value));
        if (line.op == "intro") {
            if (const auto* value = line.find("dummy")) bump(Type::dummy, number(*value) + static_cast<int>(shots) - 1);
            if (const auto* value = line.find("actor")) bump(Type::actor, number(*value) + 2 * static_cast<int>(shots) - 1);
            if (const auto* value = line.find("group"))
                bump(Type::navigation_group, number(*value) + static_cast<int>(shots) - 1);
        }
    }
    const auto take = [&](const Type type, const std::int32_t count) {
        const auto id = next[type];
        next[type] += count;
        return std::to_string(id);
    };
    std::vector<std::string> result;
    for (auto& line : lines) {
        for (const auto& key : id_keys)
            if (key.op == line.op && !line.find(key.key)) line.set(key.key, take(key.type, 1));
        if (line.op == "intro") {
            const auto n = static_cast<std::int32_t>(std::max<std::ptrdiff_t>(shots, 1));
            if (!line.find("dummy")) line.set("dummy", take(Type::dummy, n));
            if (!line.find("actor")) line.set("actor", take(Type::actor, 2 * n));
            if (!line.find("group")) line.set("group", take(Type::navigation_group, n));
            if (!line.find("cutscene")) {
                std::string ids;
                for (int k = 0; k < 4; ++k) ids += (k ? "," : "") + take(Type::script, 1);
                line.set("cutscene", ids);
            }
        }
        result.push_back(format_op_line(line));
    }
    return result;
}

std::vector<MissionRecordId> added_records(const std::vector<MissionRecordId>& before,
                                           const std::vector<MissionRecordId>& after) {
    std::vector<MissionRecordId> added;
    std::ranges::set_difference(after, before, std::back_inserter(added));
    return added;
}

EditResult run_lines(MissionEditor& editor, const std::vector<std::string>& lines, const MissionOpsOptions& options,
                     std::vector<std::string>& warnings) {
    const auto outcomes = run_mission_ops(editor, join(lines), options);
    if (outcomes.empty()) return {false, "The component has no operations", {}};
    for (const auto& outcome : outcomes)
        warnings.insert(warnings.end(), outcome.result.warnings.begin(), outcome.result.warnings.end());
    if (!outcomes.back().result.applied)
        return {false, outcomes.back().op + ": " + outcomes.back().result.message, {}};
    return {true, {}, {}};
}

std::optional<std::vector<MissionComponent>> load(const MissionEditor& editor, EditResult& failure) {
    std::string error;
    auto components = mission_components(editor, &error);
    if (!error.empty()) {
        failure = {false, "The component list does not parse: " + error, {}};
        return std::nullopt;
    }
    return components;
}

// Deletes whatever of the records still exists: actors first, so that
// nothing still stands on the groups.
EditResult delete_records(MissionEditor& editor, std::vector<MissionRecordId> records) {
    std::ranges::stable_sort(records, {}, [](const MissionRecordId& r) {
        switch (r.type) {
        case Type::actor: return 0;
        case Type::script:
        case Type::cutscene_script: return 1;
        default: return 2;
        }
    });
    for (const auto& record : records) {
        if (!editor.record_text(record)) continue;
        if (auto result = editor.delete_record(record, true); !result.applied) return result;
    }
    return {true, {}, {}};
}

// Cross-group links between the records and groups outside them that the
// lines do not make themselves (another component's link-nearest to one of
// these groups, or a link made by hand): deleting the groups drops them, so
// they are made again afterwards.
std::vector<std::array<std::int32_t, 4>> outside_links(const MissionEditor& editor, const std::vector<MissionRecordId>& owns,
                                                       const std::vector<OpLine>& lines) {
    std::set<std::int32_t> groups;
    for (const auto& record : owns)
        if (record.type == Type::navigation_group) groups.insert(record.id);
    std::set<std::pair<std::int32_t, std::int32_t>> made;
    for (const auto& line : lines)
        if (line.op == "link-nearest") try {
                made.emplace(std::stoi(line.get("group")), std::stoi(line.get("target")));
            } catch (const std::exception&) {
            }
    std::vector<std::array<std::int32_t, 4>> links;
    for (const auto& link : editor.scene().cross_group_connections()) {
        if (!link.origin_group || !link.origin_point || !link.destination_group || !link.destination_point) continue;
        const auto a = *link.origin_group, b = *link.destination_group;
        if (groups.contains(a) == groups.contains(b)) continue;
        if (made.contains({a, b}) || made.contains({b, a})) continue;
        links.push_back({a, *link.origin_point, b, *link.destination_point});
    }
    return links;
}

// Writes the list; an unchanged list is not a failure.
EditResult save_list(MissionEditor& editor, const std::vector<MissionComponent>& components, std::string label = "Edit components") {
    const auto text = format_components(components);
    if (text == editor.components_text()) return {true, {}, {}};
    return editor.set_components_text(text, std::move(label));
}

} // namespace

const char* record_type_name(const MissionRecordId::Type type) noexcept {
    switch (type) {
    case Type::actor: return "actor";
    case Type::navigation_group: return "group";
    case Type::dummy: return "dummy";
    case Type::area: return "area";
    case Type::script: return "script";
    case Type::cutscene_script: return "cutscene";
    }
    return "record";
}

std::string MissionComponent::op() const {
    if (lines.empty()) return {};
    const auto begin = lines.front().find_first_not_of(" \t");
    const auto end = lines.front().find_first_of(" \t", begin);
    return lines.front().substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::vector<MissionComponent> parse_components(const std::string_view text) {
    std::vector<MissionComponent> components;
    std::size_t number = 0;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto end = std::min(text.find('\n', begin), text.size());
        auto raw = text.substr(begin, end - begin);
        begin = end + 1;
        ++number;
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        const auto first = raw.find_first_not_of(" \t");
        if (first == std::string_view::npos || raw[first] == '#') continue;
        const auto where = "line " + std::to_string(number) + ": ";
        if (first == 0) {
            const auto line = parse_op_line(raw);
            if (line.op != "component") throw std::invalid_argument(where + "expected a component line");
            MissionComponent component;
            try {
                component.id = std::stoi(line.get("id"));
            } catch (const std::exception&) {
                throw std::invalid_argument(where + "the component has no id=");
            }
            component.fingerprint = line.get("fingerprint");
            const auto owns = line.get("owns");
            for (std::size_t at = 0; at < owns.size();) {
                const auto comma = std::min(owns.find(',', at), owns.size());
                const auto item = owns.substr(at, comma - at);
                at = comma + 1;
                const auto colon = item.find(':');
                const auto type = type_named(item.substr(0, colon));
                if (colon == std::string::npos || !type) throw std::invalid_argument(where + "bad owned record '" + item + "'");
                try {
                    component.owns.push_back({*type, std::stoi(item.substr(colon + 1))});
                } catch (const std::exception&) {
                    throw std::invalid_argument(where + "bad owned record '" + item + "'");
                }
            }
            if (std::ranges::find(components, component.id, &MissionComponent::id) != components.end())
                throw std::invalid_argument(where + "component " + std::to_string(component.id) + " is listed twice");
            components.push_back(std::move(component));
            continue;
        }
        if (components.empty()) throw std::invalid_argument(where + "an operation line before any component");
        components.back().lines.emplace_back(raw.substr(first));
    }
    return components;
}

std::string format_components(const std::vector<MissionComponent>& components) {
    if (components.empty()) return {};
    std::string text(list_header);
    for (const auto& component : components) {
        std::string owns;
        for (const auto& record : component.owns)
            owns += (owns.empty() ? "" : ",") + std::string(record_type_name(record.type)) + ':' + std::to_string(record.id);
        text += "component id=" + std::to_string(component.id) + " owns=" + (owns.empty() ? "\"\"" : owns) +
                " fingerprint=" + (component.fingerprint.empty() ? "\"\"" : component.fingerprint) + '\n';
        for (const auto& line : component.lines) text += "  " + line + '\n';
    }
    return text;
}

std::vector<MissionComponent> mission_components(const MissionEditor& editor, std::string* error) {
    try {
        return parse_components(editor.components_text());
    } catch (const std::exception& failure) {
        if (error) *error = failure.what();
        return {};
    }
}

std::optional<MissionComponent> component_owning(const MissionEditor& editor, const MissionRecordId record) {
    for (auto& component : mission_components(editor))
        if (std::ranges::find(component.owns, record) != component.owns.end()) return std::move(component);
    return std::nullopt;
}

std::string component_kind_title(const std::string_view op) {
    if (op == "guard-patrol") return "Guard patrol";
    if (op == "guard-idle") return "Guard at a post";
    if (op == "animal-patrol") return "Animal patrol";
    if (op == "cover-group") return "Cover group";
    if (op == "walk-grid") return "Walk grid";
    if (op == "objective" || op == "objectives") return "Objectives";
    if (op == "kit" || op == "equipment") return "Equipment";
    if (op == "tips") return "Tips";
    if (op == "shot" || op == "intro") return "Intro cutscene";
    return std::string(op);
}

std::string component_title(const MissionComponent& component) {
    auto title = component_kind_title(component.op());
    if (component.lines.empty()) return title;
    try {
        const auto line = parse_op_line(component.lines.front());
        if (const auto* name = line.find("name")) return title + ' ' + *name;
    } catch (const std::exception&) {
    }
    return title;
}

std::string component_fingerprint(const MissionEditor& editor, const MissionComponent& component) {
    std::string text;
    for (const auto& record : component.owns) {
        text += std::string(record_type_name(record.type)) + ':' + std::to_string(record.id) + '\n';
        text += editor.record_text(record).value_or("(missing)");
        text += '\n';
    }
    std::set<std::pair<std::int32_t, std::int32_t>> made;
    for (const auto& value : component.lines) try {
            if (const auto line = parse_op_line(value); line.op == "link-nearest")
                made.emplace(std::stoi(line.get("group")), std::stoi(line.get("target")));
        } catch (const std::exception&) {
        }
    for (const auto& link : editor.scene().cross_group_connections())
        if (link.origin_group && link.destination_group &&
            (made.contains({*link.origin_group, *link.destination_group}) ||
             made.contains({*link.destination_group, *link.origin_group})))
            text += "link:" + std::to_string(*link.origin_group) + '/' + std::to_string(link.origin_point.value_or(-1)) +
                    '-' + std::to_string(*link.destination_group) + '/' +
                    std::to_string(link.destination_point.value_or(-1)) + '\n';
    return sha256(std::as_bytes(std::span(text))).substr(0, 16);
}

ComponentState component_state(const MissionEditor& editor, const MissionComponent& component) {
    return component_fingerprint(editor, component) == component.fingerprint ? ComponentState::clean
                                                                                  : ComponentState::modified;
}

EditResult add_component(MissionEditor& editor, const std::string_view text, const MissionOpsOptions& options,
                         std::int32_t* new_id) {
    EditResult failure;
    auto components = load(editor, failure);
    if (!components) return failure;
    std::vector<std::string> lines;
    try {
        lines = pin_ids(editor, parse_lines(text));
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    }
    if (lines.empty()) return {false, "The component has no operations", {}};
    MissionComponent component;
    component.lines = lines;
    component.id = 1;
    for (const auto& other : *components) component.id = std::max(component.id, other.id + 1);
    const auto title = component_title(component);
    std::vector<std::string> warnings;
    auto result = editor.batch("Add " + title, [&] {
        const auto before = editor.record_ids();
        if (auto ran = run_lines(editor, lines, options, warnings); !ran.applied) return ran;
        component.owns = added_records(before, editor.record_ids());
        component.fingerprint = component_fingerprint(editor, component);
        components->push_back(component);
        if (auto saved = save_list(editor, *components); !saved.applied) return saved;
        return EditResult{true, "Added " + title, warnings};
    });
    if (result.applied && new_id) *new_id = component.id;
    return result;
}

EditResult update_component(MissionEditor& editor, const std::int32_t id, const std::string_view text,
                            const MissionOpsOptions& options) {
    EditResult failure;
    auto components = load(editor, failure);
    if (!components) return failure;
    const auto found = std::ranges::find(*components, id, &MissionComponent::id);
    if (found == components->end()) return {false, "Component " + std::to_string(id) + " does not exist", {}};
    std::vector<OpLine> parsed;
    try {
        parsed = parse_lines(text);
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    }
    if (parsed.empty()) return {false, "The component has no operations", {}};
    const auto title = component_title(*found);
    std::vector<std::string> warnings;
    const auto links = outside_links(editor, found->owns, parsed);
    return editor.replace_in_place("Edit " + title, [&] {
        if (auto deleted = delete_records(editor, found->owns); !deleted.applied) return deleted;
        const auto before = editor.record_ids();
        std::vector<std::string> lines;
        try {
            lines = pin_ids(editor, parsed);
        } catch (const std::exception& error) {
            return EditResult{false, error.what(), {}};
        }
        if (auto ran = run_lines(editor, lines, options, warnings); !ran.applied) return ran;
        for (const auto& [a, a_point, b, b_point] : links)
            (void)editor.connect_navigation_points(a, a_point, b, b_point);  // not when the point is gone
        found->lines = std::move(lines);
        found->owns = added_records(before, editor.record_ids());
        found->fingerprint = component_fingerprint(editor, *found);
        if (auto saved = save_list(editor, *components); !saved.applied) return saved;
        return EditResult{true, "Edited " + title, warnings};
    });
}

EditResult regenerate_component(MissionEditor& editor, const std::int32_t id, const MissionOpsOptions& options) {
    for (const auto& component : mission_components(editor))
        if (component.id == id) return update_component(editor, id, join(component.lines), options);
    return {false, "Component " + std::to_string(id) + " does not exist", {}};
}

EditResult delete_component(MissionEditor& editor, const std::int32_t id) {
    EditResult failure;
    auto components = load(editor, failure);
    if (!components) return failure;
    const auto found = std::ranges::find(*components, id, &MissionComponent::id);
    if (found == components->end()) return {false, "Component " + std::to_string(id) + " does not exist", {}};
    const auto title = component_title(*found);
    const auto owns = found->owns;
    components->erase(found);
    return editor.batch("Delete " + title, [&] {
        if (auto deleted = delete_records(editor, owns); !deleted.applied) return deleted;
        if (auto saved = save_list(editor, *components); !saved.applied) return saved;
        return EditResult{true, "Deleted " + title, {}};
    });
}

EditResult detach_component(MissionEditor& editor, const std::int32_t id) {
    EditResult failure;
    auto components = load(editor, failure);
    if (!components) return failure;
    const auto found = std::ranges::find(*components, id, &MissionComponent::id);
    if (found == components->end()) return {false, "Component " + std::to_string(id) + " does not exist", {}};
    const auto title = component_title(*found);
    components->erase(found);
    return editor.set_components_text(format_components(*components), "Detach " + title);
}

std::vector<MissionOpOutcome> run_component_ops(MissionEditor& editor, const std::string_view text,
                                                const MissionOpsOptions& options) {
    std::vector<MissionOpOutcome> outcomes;
    std::vector<std::string> unit;  // collected lines of the component being read
    std::string unit_end;
    std::size_t number = 0;
    const auto finish = [&](MissionOpOutcome outcome) {
        const bool applied = outcome.result.applied;
        outcomes.push_back(std::move(outcome));
        return applied;
    };
    for (std::size_t begin = 0; begin < text.size();) {
        const auto end = std::min(text.find('\n', begin), text.size());
        const auto raw = text.substr(begin, end - begin);
        begin = end + 1;
        ++number;
        const auto first = raw.find_first_not_of(" \t\r");
        if (first == std::string_view::npos || raw[first] == '#') continue;
        MissionOpOutcome outcome{number, {}, {}};
        OpLine line;
        try {
            line = parse_op_line(raw);
        } catch (const std::exception& error) {
            outcome.result = {false, error.what(), {}};
            finish(std::move(outcome));
            return outcomes;
        }
        outcome.op = line.op;
        std::string line_text(raw.substr(first));
        while (!line_text.empty() && (line_text.back() == '\r' || line_text.back() == ' ')) line_text.pop_back();
        if (const auto ends = collector_end(line.op); !ends.empty() || !unit.empty()) {
            if (!ends.empty()) unit_end = ends;
            unit.push_back(line_text);
            if (line.op != unit_end) {
                outcome.result = {true, line.op + " noted", {}};
                finish(std::move(outcome));
                continue;
            }
            outcome.result = add_component(editor, join(unit), options);
            unit.clear();
            unit_end.clear();
            if (!finish(std::move(outcome))) return outcomes;
            continue;
        }
        if (is_recipe(line.op)) {
            outcome.result = add_component(editor, line_text, options);
            if (!finish(std::move(outcome))) return outcomes;
            continue;
        }
        if (line.op == "link-nearest") {
            std::optional<MissionComponent> owner;
            try {
                owner = component_owning(editor, {Type::navigation_group, std::stoi(line.get("group"))});
            } catch (const std::exception&) {
            }
            if (owner) {
                outcome.result = editor.batch("Link " + component_title(*owner), [&] {
                    auto components = mission_components(editor);
                    const auto found = std::ranges::find(components, owner->id, &MissionComponent::id);
                    std::vector<std::string> warnings;
                    if (auto ran = run_lines(editor, {line_text}, options, warnings); !ran.applied) return ran;
                    found->lines.push_back(line_text);
                    found->fingerprint = component_fingerprint(editor, *found);
                    if (auto saved = save_list(editor, components); !saved.applied) return saved;
                    return EditResult{true, "Linked " + component_title(*owner), warnings};
                });
                if (!finish(std::move(outcome))) return outcomes;
                continue;
            }
        }
        auto ran = run_mission_ops(editor, line_text, options);
        outcome.result = ran.empty() ? EditResult{false, "nothing to run", {}} : std::move(ran.back().result);
        if (!finish(std::move(outcome))) return outcomes;
    }
    if (!unit.empty())
        outcomes.push_back({number, "end", {false, "objective, kit or shot lines were not followed by their "
                                                   "objectives, equipment or intro line", {}}});
    return outcomes;
}

std::vector<std::int32_t> components_that_drift(MissionEditor& editor, const MissionOpsOptions& options) {
    std::vector<std::int32_t> drift;
    for (const auto& component : mission_components(editor)) {
        std::vector<std::pair<bool, std::vector<std::byte>>> before;
        for (const auto& file : editor.files()) before.emplace_back(file.present, file.present ? file.bytes() : std::vector<std::byte>{});
        bool same = false;
        (void)editor.batch("Check", [&] {
            auto result = regenerate_component(editor, component.id, options);
            if (!result.applied) return result;
            const auto& files = editor.files();
            same = files.size() == before.size();
            for (std::size_t i = 0; same && i < files.size(); ++i)
                same = files[i].present == before[i].first && (!files[i].present || files[i].bytes() == before[i].second);
            return EditResult{false, "Checked", {}};  // puts everything back
        });
        if (!same) drift.push_back(component.id);
    }
    return drift;
}

} // namespace csf
