#include "csf/mission_ops.hpp"

#include "csf/mission_recipes.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace csf {
namespace {

struct Line {
    std::string op;
    std::map<std::string, std::string> values;
    mutable std::set<std::string> used;

    [[nodiscard]] bool has(const std::string& key) const { return values.contains(key); }
    [[nodiscard]] const std::string& text(const std::string& key) const {
        const auto found = values.find(key);
        if (found == values.end()) throw std::invalid_argument("missing " + key + "=");
        used.insert(key);
        return found->second;
    }
    [[nodiscard]] std::string text_or(const std::string& key, std::string fallback) const {
        return has(key) ? text(key) : std::move(fallback);
    }
};

Line parse_line(const std::string_view text) {
    auto parsed = parse_op_line(text);
    Line result;
    result.op = std::move(parsed.op);
    for (auto& [key, value] : parsed.values)
        if (!result.values.emplace(key, std::move(value)).second) throw std::invalid_argument("repeated " + key + "=");
    return result;
}

template <typename T>
T number(const std::string& text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
        throw std::invalid_argument("invalid number '" + text + "'");
    return value;
}

std::vector<std::string> split(const std::string& text, const char separator) {
    std::vector<std::string> parts;
    if (text.empty()) return parts;
    std::size_t begin = 0;
    for (;;) {
        const auto end = text.find(separator, begin);
        parts.push_back(text.substr(begin, end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return parts;
}

std::vector<float> floats(const std::string& text) {
    std::vector<float> values;
    for (const auto& part : split(text, ',')) values.push_back(number<float>(part));
    return values;
}

Vec3 vec3(const std::string& text) {
    const auto v = floats(text);
    if (v.size() != 3) throw std::invalid_argument("expected x,y,z, got '" + text + "'");
    return {v[0], v[1], v[2]};
}

std::vector<Vec3> vec3_list(const std::string& text) {
    std::vector<Vec3> values;
    for (const auto& part : split(text, ';')) values.push_back(vec3(part));
    return values;
}

std::vector<NavPointSpec> nav_points(const std::string& text) { return parse_op_points(text); }

std::pair<std::int32_t, std::int32_t> cell(const std::string& text) {
    const auto slash = text.find('/');
    if (slash == std::string::npos) throw std::invalid_argument("expected <group>/<point>, got '" + text + "'");
    return {number<std::int32_t>(text.substr(0, slash)), number<std::int32_t>(text.substr(slash + 1))};
}

std::optional<std::int32_t> optional_id(const Line& line, const std::string& key = "id") {
    if (!line.has(key)) return std::nullopt;
    return number<std::int32_t>(line.text(key));
}

ActorSpec actor_spec(const Line& line, const bool with_position = true) {
    ActorSpec spec;
    spec.id = optional_id(line);
    spec.name = line.text("name");
    spec.class_id = number<std::int32_t>(line.text("class"));
    if (with_position) spec.placement.position = vec3(line.text("pos"));
    spec.placement.heading_degrees = number<float>(line.text_or("heading", "0"));
    spec.placement.pitch_degrees = number<float>(line.text_or("pitch", "0"));
    if (line.has("portrait")) spec.portrait = line.text("portrait");
    return spec;
}

// start=<event>: a behaviour's script waits for this event instead of INIT.
std::string start_event(const Line& line) {
    auto event = line.text_or("start", "");
    if (event == default_start_event) event.clear();
    if (event.find_first_of("() \t") != std::string::npos) throw std::invalid_argument("start= is one event name");
    return event;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("cannot read " + path.generic_string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Lines that collect records for a later line (objective -> objectives, kit
// -> equipment).
struct Pending {
    std::vector<Objective> objectives;
    std::vector<Kit> kits;
    std::vector<CameraShot> shots;
};

EditResult run_line(MissionEditor& editor, const Line& line, const MissionOpsOptions& options, Pending& pending) {
    const auto& op = line.op;
    const auto path = [&](const std::string& key) { return options.base_directory / line.text(key); };
    if (op == "new-mission") return editor.new_mission();
    if (op == "import-class") return editor.import_class(path("donor"), number<std::int32_t>(line.text("class")));
    if (op == "import-anim") return editor.import_animation(path("donor"), number<std::int32_t>(line.text("anim")));
    if (op == "actor" || op == "prop") {
        auto spec = actor_spec(line);
        if (op == "actor") {
            if (line.has("cell")) spec.cell = cell(line.text("cell"));
            if (line.has("scripts"))
                for (const auto& id : split(line.text("scripts"), ',')) spec.scripts.push_back(number<std::int32_t>(id));
        }
        return editor.add_actor_record(spec);
    }
    if (op == "nav-group") {
        const auto points = nav_points(line.text("points"));
        std::vector<std::pair<std::int32_t, std::int32_t>> links;
        const auto kind = line.text_or("links", "");
        const auto n = static_cast<std::int32_t>(points.size());
        if (kind == "loop" || kind == "chain") {
            for (std::int32_t k = 1; k < n; ++k) links.emplace_back(k, k + 1);
            if (kind == "loop" && n > 2) links.emplace_back(n, 1);
        } else {
            for (const auto& pair : split(kind, ',')) {
                const auto dash = pair.find('-');
                if (dash == std::string::npos) throw std::invalid_argument("expected <a>-<b> links, got '" + pair + "'");
                links.emplace_back(number<std::int32_t>(pair.substr(0, dash)), number<std::int32_t>(pair.substr(dash + 1)));
            }
        }
        return editor.add_navigation_group(line.text("name"), number<std::int32_t>(line.text("type")), points, links,
                                           optional_id(line));
    }
    if (op == "link") {
        const auto [from_group, from_point] = cell(line.text("from"));
        const auto [to_group, to_point] = cell(line.text("to"));
        return editor.connect_navigation_points(from_group, from_point, to_group, to_point);
    }
    if (op == "link-nearest")
        return link_to_nearest(editor, number<std::int32_t>(line.text("group")), number<std::int32_t>(line.text("target")));
    if (op == "dummy")
        return editor.add_dummy(line.text("name"), vec3(line.text("pos")), number<float>(line.text_or("rot", "0")),
                                number<float>(line.text_or("pitch", "0")), optional_id(line));
    if (op == "area")
        return editor.add_area(line.text("name"), number<float>(line.text("height")), vec3_list(line.text("points")),
                               optional_id(line));
    if (op == "scale-class")
        return editor.add_scaled_class(number<std::int32_t>(line.text("class")), number<float>(line.text("scale")));
    if (op == "look")
        return editor.set_actor_look(number<std::int32_t>(line.text("actor")), number<std::int32_t>(line.text("class")));
    if (op == "player") return editor.set_player_actor(number<std::int32_t>(line.text("actor")));
    if (op == "script" || op == "cutscene-script") {
        const auto kind = op == "script" ? MissionFileKind::mission_script : MissionFileKind::cutscene_script;
        const auto file = editor.file_of_kind(kind);
        if (!file) throw std::invalid_argument("the mission has no such program");
        return editor.add_script(*file, read_text(path("file")));
    }
    if (op == "guard-patrol" || op == "animal-patrol") {
        RouteSpec route{optional_id(line, "route"), line.text("route-name"), nav_points(line.text("points")),
                        line.text_or("loop", "1") != "0"};
        ScriptSpec script{optional_id(line, "script"), line.text("script-name")};
        if (op == "guard-patrol") {
            GuardPatrol recipe{actor_spec(line, false), std::move(route), number<float>(line.text_or("pause", "3")),
                               optional_id(line, "cover"), std::move(script), start_event(line)};
            return add_guard_patrol(editor, std::move(recipe));
        }
        AnimalPatrol recipe{actor_spec(line), std::move(route), number<std::int32_t>(line.text("walk")), std::move(script),
                            start_event(line)};
        return add_animal_patrol(editor, std::move(recipe));
    }
    if (op == "guard-idle") {
        GuardIdle recipe{actor_spec(line), {}, optional_id(line, "cover"),
                         {optional_id(line, "script"), line.text("script-name")}, start_event(line)};
        for (const auto& step : split(line.text("loop"), ',')) {
            const auto colon = step.find(':');
            IdleStep idle{number<std::int32_t>(step.substr(0, colon)), std::nullopt};
            if (colon != std::string::npos) {
                const auto range = step.substr(colon + 1);
                const auto dash = range.find('-');
                if (dash == std::string::npos) throw std::invalid_argument("expected <anim>:<min>-<max>");
                idle.random_cycles = std::pair{number<float>(range.substr(0, dash)), number<float>(range.substr(dash + 1))};
            }
            recipe.loop.push_back(idle);
        }
        return add_guard_idle(editor, std::move(recipe));
    }
    if (op == "cover-group")
        return add_cover_group(editor, {optional_id(line), line.text("name"), nav_points(line.text("points"))});
    if (op == "walk-grid") {
        WalkGrid grid;
        grid.id = optional_id(line);
        grid.name = line.text_or("name", "MALLA");
        grid.spacing = number<float>(line.text("spacing"));
        const auto range = [&](const std::string& key, float& low, float& high) {
            const auto text = line.text(key);
            const auto dots = text.find("..");
            if (dots == std::string::npos) throw std::invalid_argument("expected " + key + "=<min>..<max>");
            low = number<float>(text.substr(0, dots));
            high = number<float>(text.substr(dots + 2));
        };
        range("x", grid.min_x, grid.max_x);
        range("z", grid.min_z, grid.max_z);
        grid.stagger = line.text_or("stagger", "1") != "0";
        for (const auto& box : split(line.text_or("exclude", ""), ';')) {
            const auto v = floats(box);
            if (v.size() != 4) throw std::invalid_argument("expected exclude=x0,z0,x1,z1");
            grid.excluded_boxes.push_back({v[0], v[1], v[2], v[3]});
        }
        for (const auto& circle : split(line.text_or("avoid", ""), ';')) {
            const auto v = floats(circle);
            if (v.size() != 3) throw std::invalid_argument("expected avoid=x,z,r");
            grid.avoided_circles.push_back({v[0], v[1], v[2]});
        }
        if (!options.ground) throw std::invalid_argument("walk-grid needs ground heights (csf-mod mission-ops --ground)");
        grid.ground = options.ground;
        return add_walk_grid(editor, grid);
    }
    if (op == "objective") {
        Objective objective;
        objective.number = number<std::int32_t>(line.text("n"));
        objective.secondary = line.text_or("secondary", "0") != "0";
        const auto kind = line.text("kind");
        if (kind == "zone") objective.kind = Objective::Kind::enter_zone;
        else if (kind == "kill") objective.kind = Objective::Kind::kill_actor;
        else if (kind == "use") objective.kind = Objective::Kind::use_object;
        else throw std::invalid_argument("kind= is zone, kill or use");
        objective.target = number<std::int32_t>(line.text("target"));
        objective.label = line.text("label");
        objective.done = line.text("done");
        objective.prompt = line.text_or("prompt", "");
        objective.script = {optional_id(line, "script"), line.text_or("script-name", "")};
        pending.objectives.push_back(std::move(objective));
        return {true, "Objective " + std::to_string(pending.objectives.back().number) + " noted", {}};
    }
    if (op == "objectives") {
        Objectives recipe{std::move(pending.objectives), {optional_id(line, "setup"), line.text_or("setup-name", "")},
                          line.text_or("success", "g014"), number<float>(line.text_or("pause", "4"))};
        pending.objectives.clear();
        return add_objectives(editor, recipe);
    }
    if (op == "kit") {
        Kit kit;
        kit.actor = number<std::int32_t>(line.text("actor"));
        // <class>[@<ammo>/<ammo>],...
        for (const auto& weapon : split(line.text("weapons"), ',')) {
            const auto at = weapon.find('@');
            Kit::Weapon entry{number<std::int32_t>(weapon.substr(0, at)), std::nullopt};
            if (at != std::string::npos) {
                const auto ammo = weapon.substr(at + 1);
                const auto slash = ammo.find('/');
                if (slash == std::string::npos) throw std::invalid_argument("expected <class>@<ammo>/<ammo>");
                entry.ammunition = std::pair{number<float>(ammo.substr(0, slash)), number<float>(ammo.substr(slash + 1))};
            }
            kit.weapons.push_back(entry);
        }
        if (line.has("select")) kit.selected = number<std::int32_t>(line.text("select"));
        if (line.has("disguise")) kit.disguise = number<std::int32_t>(line.text("disguise"));
        pending.kits.push_back(std::move(kit));
        return {true, "Kit for actor " + std::to_string(pending.kits.back().actor) + " noted", {}};
    }
    if (op == "equipment") {
        Equipment recipe{std::move(pending.kits), {optional_id(line, "script"), line.text_or("script-name", "")}};
        pending.kits.clear();
        return add_equipment(editor, recipe);
    }
    if (op == "shot") {
        CameraShot shot;
        shot.camera = vec3(line.text("camera"));
        shot.camera_end = vec3(line.text_or("end", line.text("camera")));
        shot.target = vec3(line.text("target"));
        shot.seconds = number<float>(line.text_or("seconds", "4"));
        if (line.has("aim")) {
            const auto v = floats(line.text("aim"));
            if (v.size() != 2) throw std::invalid_argument("expected aim=<rotation>,<pitch>");
            shot.aim = std::pair{v[0], v[1]};
        }
        if (line.has("heading")) shot.heading = number<float>(line.text("heading"));
        if (line.has("speed")) shot.speed = number<float>(line.text("speed"));
        pending.shots.push_back(shot);
        return {true, "Shot " + std::to_string(pending.shots.size()) + " noted", {}};
    }
    if (op == "intro") {
        IntroCutscene recipe;
        recipe.shots = std::move(pending.shots);
        pending.shots.clear();
        recipe.camera_class = number<std::int32_t>(line.text_or("class", "197"));
        recipe.send_init = line.text_or("send-init", "1") != "0";
        recipe.first_dummy = optional_id(line, "dummy");
        recipe.first_actor = optional_id(line, "actor");
        recipe.first_group = optional_id(line, "group");
        recipe.intro = {optional_id(line, "script"), line.text_or("script-name", "CUTSCENE_INICIO")};
        if (line.has("cutscene")) {
            const auto ids = split(line.text("cutscene"), ',');
            if (ids.size() != 4) throw std::invalid_argument("expected cutscene=<main>,<init>,<end>,<camera>");
            for (std::size_t k = 0; k < 4; ++k) recipe.cutscene_ids[k] = number<std::int32_t>(ids[k]);
        }
        recipe.cutscene_name = line.text_or("cutscene-name", "CUT_INICIO");
        recipe.zone = optional_id(line, "zone");
        recipe.setup = {optional_id(line, "setup"), line.text_or("setup-name", "")};
        recipe.arm_event = line.text_or("arm", "");
        return add_intro_cutscene(editor, recipe);
    }
    if (op == "trigger") {
        OpLine parsed{op, {}};
        for (const auto& [key, value] : line.values) parsed.values.emplace_back(key, line.text(key));
        return add_trigger(editor, parse_trigger(parsed));
    }
    if (op == "tips") {
        Tips recipe{split(line.text("tips"), ','), {0.115F, 0.25F}, {optional_id(line, "script"), line.text_or("script-name", "")}};
        if (line.has("pos")) {
            const auto v = floats(line.text("pos"));
            if (v.size() != 2) throw std::invalid_argument("expected pos=<x>,<y>");
            recipe.position = {v[0], v[1]};
        }
        return add_tips(editor, recipe);
    }
    throw std::invalid_argument("unknown operation '" + op + "'");
}

} // namespace

const std::string* OpLine::find(const std::string_view key) const {
    const auto found = std::ranges::find(values, key, &std::pair<std::string, std::string>::first);
    return found == values.end() ? nullptr : &found->second;
}

std::string OpLine::get(const std::string_view key, std::string fallback) const {
    const auto* value = find(key);
    return value ? *value : std::move(fallback);
}

void OpLine::set(const std::string_view key, std::string value) {
    const auto found = std::ranges::find(values, key, &std::pair<std::string, std::string>::first);
    if (found != values.end()) found->second = std::move(value);
    else values.emplace_back(std::string(key), std::move(value));
}

void OpLine::erase(const std::string_view key) {
    std::erase_if(values, [&](const auto& pair) { return pair.first == key; });
}

OpLine parse_op_line(const std::string_view line) {
    OpLine result;
    std::size_t i = 0;
    const auto skip = [&] {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r' || line[i] == '\n')) ++i;
    };
    skip();
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') result.op.push_back(line[i++]);
    for (skip(); i < line.size(); skip()) {
        std::string key;
        while (i < line.size() && line[i] != '=' && line[i] != ' ') key.push_back(line[i++]);
        if (i >= line.size() || line[i] != '=') throw std::invalid_argument("expected key=value, got '" + key + "'");
        ++i;
        std::string value;
        if (i < line.size() && line[i] == '"') {
            for (++i; i < line.size() && line[i] != '"'; ++i) {
                if (line[i] == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) ++i;
                value.push_back(line[i]);
            }
            if (i >= line.size()) throw std::invalid_argument("unterminated string");
            ++i;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') value.push_back(line[i++]);
        }
        result.values.emplace_back(std::move(key), std::move(value));
    }
    return result;
}

std::string format_op_line(const OpLine& line) {
    std::string text = line.op;
    for (const auto& [key, value] : line.values) {
        text += ' ' + key + '=';
        if (!value.empty() && value.find_first_of(" \t\"\\") == std::string::npos) {
            text += value;
            continue;
        }
        text += '"';
        for (const char c : value) {
            if (c == '"' || c == '\\') text += '\\';
            text += c;
        }
        text += '"';
    }
    return text;
}

std::string op_number(const float value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), end};
}

std::string op_vec3(const Vec3 value) {
    return op_number(value.x) + ',' + op_number(value.y) + ',' + op_number(value.z);
}

std::string op_points(const std::vector<NavPointSpec>& points) {
    std::string text;
    for (const auto& point : points) {
        if (!text.empty()) text += ';';
        text += op_vec3(point.position);
        if (point.rotation_radians != 0.0F) text += ',' + op_number(point.rotation_radians);
    }
    return text;
}

std::vector<NavPointSpec> parse_op_points(const std::string_view text) {
    std::vector<NavPointSpec> points;
    for (const auto& part : split(std::string(text), ';')) {
        const auto v = floats(part);
        if (v.size() != 3 && v.size() != 4) throw std::invalid_argument("expected x,y,z[,rotation], got '" + part + "'");
        points.push_back({{v[0], v[1], v[2]}, v.size() == 4 ? v[3] : 0.0F, 0.0F});
    }
    return points;
}

std::vector<MissionOpOutcome> run_mission_ops(MissionEditor& editor, const std::string_view text,
                                              const MissionOpsOptions& options) {
    std::vector<MissionOpOutcome> outcomes;
    Pending pending;
    std::size_t number_of_line = 0;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto end = std::min(text.find('\n', begin), text.size());
        const auto raw = text.substr(begin, end - begin);
        begin = end + 1;
        ++number_of_line;
        const auto first = raw.find_first_not_of(" \t\r");
        if (first == std::string_view::npos || raw[first] == '#') continue;
        MissionOpOutcome outcome{number_of_line, {}, {}};
        try {
            const auto line = parse_line(raw);
            outcome.op = line.op;
            outcome.result = run_line(editor, line, options, pending);
            for (const auto& [key, value] : line.values)
                if (!line.used.contains(key) && outcome.result.applied)
                    outcome.result.warnings.push_back("ignored " + key + "=");
        } catch (const std::exception& error) {
            outcome.result = {false, error.what(), {}};
        }
        const bool applied = outcome.result.applied;
        outcomes.push_back(std::move(outcome));
        if (!applied) break;
    }
    if (!outcomes.empty() && outcomes.back().result.applied &&
        (!pending.objectives.empty() || !pending.kits.empty() || !pending.shots.empty()))
        outcomes.push_back({number_of_line, "end",
                            {false, "objective, kit or shot lines were not followed by their objectives, equipment or "
                                    "intro line", {}}});
    return outcomes;
}

} // namespace csf

namespace csf {
namespace {

void set_id(OpLine& line, const std::string_view key, const std::optional<std::int32_t> id) {
    if (id) line.set(key, std::to_string(*id));
}

void set_text(OpLine& line, const std::string_view key, const std::string& value) {
    if (!value.empty()) line.set(key, value);
}

// id= name= class= [pos=] [heading=] [pitch=] [portrait=]
void set_actor(OpLine& line, const ActorSpec& actor, const bool with_position) {
    set_id(line, "id", actor.id);
    line.set("name", actor.name);
    line.set("class", std::to_string(actor.class_id));
    if (with_position) line.set("pos", op_vec3(actor.placement.position));
    if (actor.placement.heading_degrees != 0.0F) line.set("heading", op_number(actor.placement.heading_degrees));
    if (actor.placement.pitch_degrees != 0.0F) line.set("pitch", op_number(actor.placement.pitch_degrees));
    if (actor.portrait) line.set("portrait", *actor.portrait);
}

void set_route(OpLine& line, const RouteSpec& route) {
    set_id(line, "route", route.id);
    line.set("route-name", route.name);
    line.set("points", op_points(route.points));
}

void set_script(OpLine& line, const ScriptSpec& script, const std::string_view id_key = "script",
                const std::string_view name_key = "script-name") {
    set_id(line, id_key, script.id);
    set_text(line, name_key, script.name);
}

std::string lines(const std::vector<OpLine>& values) {
    std::string text;
    for (const auto& value : values) text += format_op_line(value) + '\n';
    return text;
}

} // namespace

std::string ops_text(const GuardPatrol& recipe) {
    OpLine line{"guard-patrol", {}};
    set_actor(line, recipe.actor, false);
    set_route(line, recipe.route);
    line.set("pause", op_number(recipe.pause_seconds));
    set_id(line, "cover", recipe.cover_group);
    set_script(line, recipe.script);
    if (!recipe.route.loop) line.set("loop", "0");
    if (recipe.start_event != default_start_event) set_text(line, "start", recipe.start_event);
    return lines({line});
}

std::string ops_text(const GuardIdle& recipe) {
    OpLine line{"guard-idle", {}};
    set_actor(line, recipe.actor, true);
    set_id(line, "cover", recipe.cover_group);
    set_script(line, recipe.script);
    std::string loop;
    for (const auto& step : recipe.loop) {
        if (!loop.empty()) loop += ',';
        loop += std::to_string(step.animation);
        if (step.random_cycles)
            loop += ':' + op_number(step.random_cycles->first) + '-' + op_number(step.random_cycles->second);
    }
    line.set("loop", loop);
    if (recipe.start_event != default_start_event) set_text(line, "start", recipe.start_event);
    return lines({line});
}

std::string ops_text(const AnimalPatrol& recipe) {
    OpLine line{"animal-patrol", {}};
    set_actor(line, recipe.actor, true);
    set_route(line, recipe.route);
    line.set("walk", std::to_string(recipe.walk_animation));
    set_script(line, recipe.script);
    if (!recipe.route.loop) line.set("loop", "0");
    if (recipe.start_event != default_start_event) set_text(line, "start", recipe.start_event);
    return lines({line});
}

std::string ops_text(const CoverGroup& recipe) {
    OpLine line{"cover-group", {}};
    set_id(line, "id", recipe.id);
    line.set("name", recipe.name);
    line.set("points", op_points(recipe.points));
    return lines({line});
}

std::string ops_text(const WalkGrid& recipe) {
    OpLine line{"walk-grid", {}};
    set_id(line, "id", recipe.id);
    line.set("name", recipe.name);
    line.set("spacing", op_number(recipe.spacing));
    line.set("x", op_number(recipe.min_x) + ".." + op_number(recipe.max_x));
    line.set("z", op_number(recipe.min_z) + ".." + op_number(recipe.max_z));
    if (!recipe.stagger) line.set("stagger", "0");
    std::string boxes, circles;
    for (const auto& box : recipe.excluded_boxes)
        boxes += (boxes.empty() ? "" : ";") + op_number(box[0]) + ',' + op_number(box[1]) + ',' + op_number(box[2]) + ',' +
                 op_number(box[3]);
    for (const auto& circle : recipe.avoided_circles)
        circles += (circles.empty() ? "" : ";") + op_number(circle[0]) + ',' + op_number(circle[1]) + ',' +
                   op_number(circle[2]);
    set_text(line, "exclude", boxes);
    set_text(line, "avoid", circles);
    return lines({line});
}

std::string ops_text(const Objectives& recipe) {
    std::vector<OpLine> values;
    for (const auto& objective : recipe.objectives) {
        OpLine line{"objective", {}};
        line.set("n", std::to_string(objective.number));
        line.set("kind", objective.kind == Objective::Kind::enter_zone  ? "zone"
                         : objective.kind == Objective::Kind::kill_actor ? "kill"
                                                                         : "use");
        line.set("target", std::to_string(objective.target));
        line.set("label", objective.label);
        line.set("done", objective.done);
        set_text(line, "prompt", objective.prompt);
        if (objective.secondary) line.set("secondary", "1");
        set_script(line, objective.script);
        values.push_back(std::move(line));
    }
    OpLine end{"objectives", {}};
    set_script(end, recipe.setup, "setup", "setup-name");
    end.set("success", recipe.success_message);
    end.set("pause", op_number(recipe.success_pause));
    values.push_back(std::move(end));
    return lines(values);
}

std::string ops_text(const Equipment& recipe) {
    std::vector<OpLine> values;
    for (const auto& kit : recipe.kits) {
        OpLine line{"kit", {}};
        line.set("actor", std::to_string(kit.actor));
        std::string weapons;
        for (const auto& weapon : kit.weapons) {
            if (!weapons.empty()) weapons += ',';
            weapons += std::to_string(weapon.weapon_class);
            if (weapon.ammunition)
                weapons += '@' + op_number(weapon.ammunition->first) + '/' + op_number(weapon.ammunition->second);
        }
        line.set("weapons", weapons);
        set_id(line, "select", kit.selected);
        set_id(line, "disguise", kit.disguise);
        values.push_back(std::move(line));
    }
    OpLine end{"equipment", {}};
    set_script(end, recipe.script);
    values.push_back(std::move(end));
    return lines(values);
}

std::string ops_text(const Tips& recipe) {
    OpLine line{"tips", {}};
    std::string tips;
    for (const auto& tip : recipe.tips) tips += (tips.empty() ? "" : ",") + tip;
    line.set("tips", tips);
    line.set("pos", op_number(recipe.position.first) + ',' + op_number(recipe.position.second));
    set_script(line, recipe.script);
    return lines({line});
}

std::string ops_text(const IntroCutscene& recipe) {
    std::vector<OpLine> values;
    for (const auto& shot : recipe.shots) {
        OpLine line{"shot", {}};
        line.set("camera", op_vec3(shot.camera));
        line.set("end", op_vec3(shot.camera_end));
        line.set("target", op_vec3(shot.target));
        line.set("seconds", op_number(shot.seconds));
        if (shot.aim) line.set("aim", op_number(shot.aim->first) + ',' + op_number(shot.aim->second));
        if (shot.heading) line.set("heading", op_number(*shot.heading));
        if (shot.speed) line.set("speed", op_number(*shot.speed));
        values.push_back(std::move(line));
    }
    OpLine end{"intro", {}};
    end.set("class", std::to_string(recipe.camera_class));
    if (!recipe.send_init) end.set("send-init", "0");
    set_id(end, "dummy", recipe.first_dummy);
    set_id(end, "actor", recipe.first_actor);
    set_id(end, "group", recipe.first_group);
    set_script(end, recipe.intro);
    if (std::ranges::all_of(recipe.cutscene_ids, [](const auto& id) { return id.has_value(); }))
        end.set("cutscene", std::to_string(*recipe.cutscene_ids[0]) + ',' + std::to_string(*recipe.cutscene_ids[1]) + ',' +
                                std::to_string(*recipe.cutscene_ids[2]) + ',' + std::to_string(*recipe.cutscene_ids[3]));
    end.set("cutscene-name", recipe.cutscene_name);
    set_id(end, "zone", recipe.zone);
    set_id(end, "setup", recipe.setup.id);
    if (!recipe.setup.name.empty()) end.set("setup-name", recipe.setup.name);
    if (!recipe.arm_event.empty()) end.set("arm", recipe.arm_event);
    values.push_back(std::move(end));
    return lines(values);
}

} // namespace csf

namespace csf {
namespace {

constexpr std::pair<Trigger::When, const char*> when_names[]{
    {Trigger::When::mission_start, "start"}, {Trigger::When::enter_zone, "zone"}, {Trigger::When::actor_killed, "killed"},
    {Trigger::When::object_used, "used"},    {Trigger::When::event, "event"},     {Trigger::When::timer, "timer"},
    {Trigger::When::alerted, "alerted"},     {Trigger::When::body_found, "body-found"}};

std::int32_t integer_value(const std::string& text) {
    std::int32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("invalid number '" + text + "'");
    return value;
}

} // namespace

const char* trigger_when_name(const Trigger::When when) noexcept {
    for (const auto& [value, name] : when_names)
        if (value == when) return name;
    return "?";
}

std::string trigger_action_text(const TriggerAction& action) {
    using Kind = TriggerAction::Kind;
    const auto n = std::to_string(action.number);
    switch (action.kind) {
    case Kind::complete_objective: return "complete:" + n;
    case Kind::message: return "message:" + action.text;
    case Kind::raise_event: return "raise:" + action.text;
    case Kind::alarm: return "alarm:" + n;
    case Kind::ai_alert: return "alert:" + n + ":" + action.text;
    case Kind::ai_combat: return "combat:" + n + ":" + action.text;
    case Kind::enable_ghost: return "ghost-on:" + n;
    case Kind::disable_ghost: return "ghost-off:" + n;
    case Kind::mission_success: return "success";
    }
    return {};
}

Trigger parse_trigger(const OpLine& line) {
    using Kind = TriggerAction::Kind;
    Trigger trigger;
    const auto id = [&](const std::string_view key) -> std::optional<std::int32_t> {
        if (const auto* value = line.find(key)) return integer_value(*value);
        return std::nullopt;
    };
    trigger.script = {id("script"), line.get("name")};
    trigger.setup = {id("setup"), line.get("setup-name")};
    const auto when = line.get("when", "start");
    const auto found = std::ranges::find(when_names, when, [](const auto& pair) { return std::string(pair.second); });
    if (found == std::end(when_names))
        throw std::invalid_argument("when= is start, zone, killed, used, event, timer, alerted or body-found");
    trigger.when = found->first;
    trigger.target = id("target").value_or(0);
    trigger.event = line.get("event");
    for (const auto& actor : split(line.get("watch"), ',')) trigger.watch.push_back(integer_value(actor));
    if (const auto* seconds = line.find("seconds")) {
        float value{};
        const auto [end, error] = std::from_chars(seconds->data(), seconds->data() + seconds->size(), value);
        if (error != std::errc{} || end != seconds->data() + seconds->size()) throw std::invalid_argument("invalid seconds=");
        trigger.seconds = value;
    }
    if (const auto* condition = line.find("if")) {
        const auto colon = condition->find(':');
        const auto state = colon == std::string::npos ? std::string() : condition->substr(colon + 1);
        if (state != "done" && state != "open") throw std::invalid_argument("if= is <objective>:done or <objective>:open");
        trigger.if_objective = std::pair{integer_value(condition->substr(0, colon)), state == "done"};
    }
    for (const auto& item : split(line.get("do"), ';')) {
        const auto parts = split(item, ':');
        const auto part = [&](const std::size_t k) -> const std::string& {
            if (k >= parts.size()) throw std::invalid_argument("incomplete action '" + item + "'");
            return parts[k];
        };
        TriggerAction action;
        const auto& verb = part(0);
        if (verb == "complete") action = {Kind::complete_objective, integer_value(part(1)), {}};
        else if (verb == "message") action = {Kind::message, 0, part(1)};
        else if (verb == "raise") action = {Kind::raise_event, 0, part(1)};
        else if (verb == "alarm") action = {Kind::alarm, integer_value(part(1)), {}};
        else if (verb == "alert") action = {Kind::ai_alert, integer_value(part(1)), part(2)};
        else if (verb == "combat") action = {Kind::ai_combat, integer_value(part(1)), part(2)};
        else if (verb == "ghost-on") action = {Kind::enable_ghost, integer_value(part(1)), {}};
        else if (verb == "ghost-off") action = {Kind::disable_ghost, integer_value(part(1)), {}};
        else if (verb == "success") action = {Kind::mission_success, 0, {}};
        else throw std::invalid_argument("unknown action '" + verb + "'");
        trigger.actions.push_back(std::move(action));
    }
    return trigger;
}

std::string ops_text(const Trigger& recipe) {
    OpLine line{"trigger", {}};
    set_id(line, "script", recipe.script.id);
    line.set("name", recipe.script.name);
    if (trigger_needs_setup(recipe)) {
        set_id(line, "setup", recipe.setup.id);
        set_text(line, "setup-name", recipe.setup.name);
    }
    line.set("when", trigger_when_name(recipe.when));
    if (recipe.when == Trigger::When::enter_zone || recipe.when == Trigger::When::actor_killed ||
        recipe.when == Trigger::When::object_used)
        line.set("target", std::to_string(recipe.target));
    if (recipe.when == Trigger::When::event) line.set("event", recipe.event);
    if (recipe.when == Trigger::When::timer) line.set("seconds", op_number(recipe.seconds));
    if (recipe.when == Trigger::When::body_found) {
        std::string watch;
        for (const auto actor : recipe.watch) watch += (watch.empty() ? "" : ",") + std::to_string(actor);
        line.set("watch", watch);
    }
    if (recipe.if_objective)
        line.set("if", std::to_string(recipe.if_objective->first) + (recipe.if_objective->second ? ":done" : ":open"));
    std::string actions;
    for (const auto& action : recipe.actions) actions += (actions.empty() ? "" : ";") + trigger_action_text(action);
    line.set("do", actions);
    return lines({line});
}

} // namespace csf
