#include "csf/mission_ops.hpp"

#include "csf/mission_recipes.hpp"

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

Line parse_line(const std::string_view line) {
    Line result;
    std::size_t i = 0;
    const auto skip = [&] {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    };
    skip();
    while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') result.op.push_back(line[i++]);
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
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') value.push_back(line[i++]);
        }
        if (!result.values.emplace(key, value).second) throw std::invalid_argument("repeated " + key + "=");
    }
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

std::vector<NavPointSpec> nav_points(const std::string& text) {
    std::vector<NavPointSpec> points;
    for (const auto& part : split(text, ';')) {
        const auto v = floats(part);
        if (v.size() != 3 && v.size() != 4) throw std::invalid_argument("expected x,y,z[,rotation], got '" + part + "'");
        points.push_back({{v[0], v[1], v[2]}, v.size() == 4 ? v[3] : 0.0F, 0.0F});
    }
    return points;
}

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
                               optional_id(line, "cover"), std::move(script)};
            return add_guard_patrol(editor, std::move(recipe));
        }
        AnimalPatrol recipe{actor_spec(line), std::move(route), number<std::int32_t>(line.text("walk")), std::move(script)};
        return add_animal_patrol(editor, std::move(recipe));
    }
    if (op == "guard-idle") {
        GuardIdle recipe{actor_spec(line), {}, optional_id(line, "cover"),
                         {optional_id(line, "script"), line.text("script-name")}};
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
        return add_intro_cutscene(editor, recipe);
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
