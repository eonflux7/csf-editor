#include "ui_script_runner.hpp"

#include "app_actions.hpp"
#include "app_util.hpp"
#include "authoring.hpp"
#include "mission_authoring.hpp"
#include "mission_editing.hpp"
#include "navigation.hpp"
#include "screenshot.hpp"
#include "state_snapshot.hpp"
#include "ui_automation.hpp"
#include "ui/theme.hpp"
#include "viewport_tools.hpp"

#include "rws/texture_image.hpp"

#include <GLFW/glfw3.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace rwsman {
namespace {

// Frames a widget step waits for its target (after a command opens a panel,
// the panel's widgets exist from the next frame on).
constexpr int retry_limit = 60;
// Idle frames every step waits for after background work finishes.
constexpr int settle_frames = 3;

std::filesystem::path utf8_path(const std::string& text) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::optional<float> parse_float(const std::string& text) {
    char* end = nullptr;
    const float value = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return std::nullopt;
    return value;
}

// "~/" and $NAME or ${NAME} environment variables (the UI tests find their
// fixture through $RWSMAN_UI_FIXTURE).
std::string expand_home(const std::string& text) {
    std::string result;
    if (text.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        result = home ? std::string(home) : std::string("~");
        result += '/';
    }
    for (std::size_t i = result.empty() ? 0 : 2; i < text.size(); ++i) {
        if (text[i] != '$' || i + 1 >= text.size()) {
            result += text[i];
            continue;
        }
        const bool braced = text[i + 1] == '{';
        std::size_t end = i + (braced ? 2 : 1);
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) ++end;
        const auto name = text.substr(i + (braced ? 2 : 1), end - i - (braced ? 2 : 1));
        if (braced && (end >= text.size() || text[end] != '}')) {
            result += text[i];
            continue;
        }
        const char* value = std::getenv(name.c_str());
        result += value ? value : "";
        i = braced ? end : end - 1;
    }
    return result;
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

} // namespace

UiScriptRunner::UiScriptRunner(std::vector<UiScriptStep> steps, UiScriptOptions options)
    : steps_(std::move(steps)), options_(std::move(options)) {}

bool UiScriptRunner::busy(const AppState& state) const {
    // An edit is applied between frames, and the mission views follow it at the
    // start of a later frame; the preview may be mid-drag.
    const bool views_behind =
        state.mission.editor && state.mission.editor->revision() != state.mission.applied_revision;
    return mission_load_active(state) || authoring_pending(state) || state.dialog != nullptr || views_behind ||
           state.preview.edit_drag_pending() || state.ui.focus_panel_frames > 0;
}

std::filesystem::path UiScriptRunner::output_path(const std::string& name) const {
    auto path = utf8_path(expand_home(name));
    if (path.is_relative() && !options_.output_directory.empty()) path = options_.output_directory / path;
    return path;
}

void UiScriptRunner::fail(const AppState& state, const std::string& message) {
    failed_ = true;
    const auto where = options_.name + ":" + (current_ ? std::to_string(current_->line) : std::string("?"));
    std::fprintf(stderr, "%s: %s%s%s\n", where.c_str(), current_ ? format_ui_script_step(*current_).c_str() : "",
                 current_ ? ": " : "", message.c_str());
    if (monkey_)
        for (const auto& action : monkey_->recent) std::fprintf(stderr, "%s:   after: %s\n", where.c_str(), action.c_str());
    const auto snapshot = snapshot_state(state);
    auto stem = utf8_path(options_.name).stem().string();
    if (stem.empty()) stem = "ui-script";
    const auto report = output_path(stem + ".failure.json");
    if (write_text(report, state_snapshot_json(snapshot)))
        std::fprintf(stderr, "%s: state written to %s\n", where.c_str(), path_utf8(report).c_str());
}

void UiScriptRunner::before_frame(AppState& state, UiAutomation& automation) {
    // Fixed time steps keep animations, double clicks and toasts reproducible.
    ImGui::GetIO().DeltaTime = 1.0F / 60.0F;
    // Input from the previous step is still being sent, or was sent this frame:
    // the next step waits until ImGui has handled it and the app settled.
    if (automation.before_new_frame()) idle_frames_ = 0;
    if (done() || capture_ != Capture::none) return;
    // A workspace shown for the first time builds its dock layout, and tab
    // bars size themselves over the next frames.
    if (last_workspace_ != state.workspace) {
        last_workspace_ = state.workspace;
        wait_frames_ = std::max(wait_frames_, 8);
    }
    if (busy(state)) {
        idle_frames_ = 0;
        return;
    }
    if (++idle_frames_ < settle_frames) return;
    if (wait_frames_ > 0) {
        --wait_frames_;
        return;
    }
    if (next_ >= steps_.size()) return;
    current_ = &steps_[next_];
    run_step(state, automation, *current_);
    if (failed_) return;
    if (retry_frames_ == 0) ++next_;
}

void UiScriptRunner::run_step(AppState& state, UiAutomation& automation, const UiScriptStep& step) {
    const auto& op = step.op;
    const auto& args = step.args;
    // $UI_OUTPUT is the output directory (where `copy` puts its copies).
    const auto resolve = [this](std::string text) {
        if (text.starts_with("$UI_OUTPUT")) text = path_utf8(options_.output_directory) + text.substr(10);
        auto path = utf8_path(expand_home(text));
        if (path.is_relative() && !options_.base_directory.empty()) path = options_.base_directory / path;
        return path;
    };
    // Keys held during a click or drag: "Shift", "Ctrl", "Shift+Ctrl".
    const auto modifiers = [](const std::string& text) -> std::optional<ImGuiKeyChord> {
        const auto parsed = parse_shortcut(text + "+A");  // a shortcut with a dummy key
        if (!parsed) return std::nullopt;
        return (parsed->ctrl ? ImGuiMod_Ctrl : 0) | (parsed->shift ? ImGuiMod_Shift : 0) |
               (parsed->alt ? ImGuiMod_Alt : 0);
    };
    // Widget steps retry until the target shows up.
    const auto find_item = [&](const std::string& text) -> const UiItem* {
        const auto target = parse_ui_target(text);
        const auto found = automation.find(target);
        if (!found.empty()) {
            retry_frames_ = 0;
            return found.front();
        }
        // Scroll the named window every other frame: a scroll shows the next frame.
        if (retry_frames_ % 2 == 1) automation.scroll_towards(target);
        if (++retry_frames_ >= retry_limit) {
            retry_frames_ = 0;
            fail(state, "no visible widget matches '" + text + "'");
        }
        return nullptr;
    };

    if (op == "require") {
        const char* value = std::getenv(args[0].c_str());
        if (!value || !*value) {
            std::fprintf(stderr, "%s: skipped: %s is not set\n", options_.name.c_str(), args[0].c_str());
            skipped_ = true;
        }
    } else if (op == "require-release") {
#ifndef NDEBUG
        std::fprintf(stderr, "%s: skipped: a Debug build\n", options_.name.c_str());
        skipped_ = true;
#endif
    } else if (op == "expect-at-most") {
        const auto snapshot = snapshot_state(state);
        const auto found = snapshot.find(args[0]);
        if (found == snapshot.end()) return fail(state, "no state key '" + args[0] + "'");
        const auto actual = parse_float(found->second), limit = parse_float(args[1]);
        if (!limit) return fail(state, "'" + args[1] + "' is not a number");
        if (!actual) return fail(state, args[0] + " is '" + found->second + "', not a number");
        std::fprintf(stderr, "%s: %s = %s (at most %s)\n", options_.name.c_str(), args[0].c_str(),
                     found->second.c_str(), args[1].c_str());
        if (*actual > *limit) fail(state, args[0] + " is " + found->second + ", over " + args[1]);
    } else if (op == "open") {
        open_path(state, resolve(args[0]));
    } else if (op == "copy") {
        // A scratch copy of a project or mission, so a script can edit and save it.
        const auto source = resolve(args[0]);
        const auto target = output_path(args[1]);
        std::error_code error;
        std::filesystem::remove_all(target, error);
        std::filesystem::create_directories(target, error);
        std::filesystem::copy(source, target, std::filesystem::copy_options::recursive, error);
        if (error) return fail(state, "cannot copy " + path_utf8(source) + ": " + error.message());
    } else if (op == "open-project") {
        open_mission_project(state, resolve(args[0]));
    } else if (op == "wait") {
        if (args[0] == "idle" && args.size() == 1) {
            // Background work often starts a frame or two after the step that
            // caused it; give it time to show up before the next idle check.
            wait_frames_ = settle_frames;
            idle_frames_ = 0;
        } else if (args[0] == "frames" && args.size() == 2) {
            wait_frames_ = std::max(0, std::atoi(args[1].c_str()));
        } else {
            fail(state, "expected 'wait idle' or 'wait frames <n>'");
        }
    } else if (op == "command" || op == "undo" || op == "redo") {
        const std::string id = op == "command" ? args[0] : "edit." + op;
        if (!state.commands.find(id)) return fail(state, "unknown command '" + id + "'");
        if (!state.commands.run(id)) return fail(state, "command '" + id + "' is disabled");
    } else if (op == "goto") {
        auto text = args[0];
        std::string kind;
        if (const auto colon = text.find(':'); colon != std::string::npos) {
            kind = text.substr(0, colon);
            text = text.substr(colon + 1);
        }
        for (const auto& result : state.search_index.query(text, 100))
            if (kind.empty() || kind == symbol_kind_name(result.entry->kind)) {
                navigate_to(state, result.entry->target);
                return;
            }
        fail(state, "nothing matches '" + args[0] + "'");
    } else if (op == "palette" || op == "goto-palette") {
        state.ui.palette = op == "goto-palette" ? UiState::PaletteMode::go_to : UiState::PaletteMode::commands;
        state.ui.palette_just_opened = true;
        state.ui.palette_prefilled = true;
        std::snprintf(state.ui.palette_query.data(), state.ui.palette_query.size(), "%s", args[0].c_str());
    } else if (op == "click" || op == "double-click" || op == "hover") {
        const auto held = args.size() > 1 ? modifiers(args[1]) : ImGuiKeyChord{0};
        if (!held) return fail(state, "'" + args[1] + "' is not a modifier (Shift, Ctrl, Alt)");
        if (const auto* item = find_item(args[0])) {
            if (op == "hover")
                automation.queue_hover(item->center());
            else
                automation.queue_click(item->center(), op == "double-click" ? 2 : 1, *held);
        }
    } else if (op == "drag") {
        // A widget dragged by an offset in pixels (a timeline strip's edge).
        const auto held = args.size() > 3 ? modifiers(args[3]) : ImGuiKeyChord{0};
        if (!held) return fail(state, "'" + args[3] + "' is not a modifier (Shift, Ctrl, Alt)");
        const auto dx = parse_float(args[1]), dy = parse_float(args[2]);
        if (!dx || !dy) return fail(state, "expected drag <target> <dx> <dy>");
        if (const auto* item = find_item(args[0])) {
            const auto from = item->center();
            automation.queue_drag(from, {from.x + *dx, from.y + *dy}, 8, *held);
        }
    } else if (op == "drag-to-world") {
        const auto* item = find_item(args[0]);
        if (!item) return;
        std::array<float, 3> values{};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto value = parse_float(args[i + 1]);
            if (!value) return fail(state, "'" + args[i + 1] + "' is not a number");
            values[i] = *value;
        }
        const auto to = state.preview.screen_position({values[0], values[1], values[2]});
        if (!to) return fail(state, "the point is not in front of the viewport camera");
        automation.queue_drag(item->center(), *to);
    } else if (op == "click-world" || op == "drag-world") {
        const std::size_t count = op == "click-world" ? 3 : 6;
        const auto held = args.size() > count ? modifiers(args[count]) : ImGuiKeyChord{0};
        if (!held) return fail(state, "'" + args[count] + "' is not a modifier (Shift, Ctrl, Alt)");
        std::array<float, 6> values{};
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = parse_float(args[i]);
            if (!value) return fail(state, "'" + args[i] + "' is not a number");
            values[i] = *value;
        }
        const auto from = state.preview.screen_position({values[0], values[1], values[2]});
        if (!from) return fail(state, "the point is not in front of the viewport camera");
        if (op == "click-world") return automation.queue_click(*from, 1, *held);
        const auto to = state.preview.screen_position({values[3], values[4], values[5]});
        if (!to) return fail(state, "the end point is not in front of the viewport camera");
        automation.queue_drag(*from, *to, 8, *held);
    } else if (op == "key") {
        if (!automation.queue_key(args[0])) fail(state, "unknown key or shortcut '" + args[0] + "'");
    } else if (op == "type") {
        const bool path = args[0].starts_with("$") || args[0].starts_with("~/");
        automation.queue_text(path ? path_utf8(resolve(args[0])) : args[0]);
    } else if (op == "setting") {
        auto& settings = state.settings;
        const auto value = resolve(args[1]).lexically_normal();
        if (args[0] == "resource_root") {
            settings.resource_root = value;
            rescan_resource_root(state);
        } else if (args[0] == "game_root") {
            settings.game_root = value;
        } else if (args[0] == "projects_root") {
            settings.projects_root = value;
        } else if (args[0] == "blender") {
            settings.blender = value;
        } else {
            return fail(state, "unknown setting '" + args[0] + "'");
        }
    } else if (op == "remove") {
        const auto path = resolve(args[0]).lexically_normal();
        const auto relative = path.lexically_relative(options_.output_directory.lexically_normal());
        if (relative.empty() || *relative.begin() == ".." || relative == ".")
            return fail(state, "remove only deletes inside the output directory");
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) return fail(state, "cannot remove " + path_utf8(path) + ": " + error.message());
    } else if (op == "expect" || op == "expect-not" || op == "expect-contains") {
        std::string actual;
        if (args[0].starts_with("item:")) {
            // item:<target> is "missing", "visible", or with a checkbox or
            // menu check "checked"/"unchecked".
            const auto target = parse_ui_target(args[0].substr(5));
            const auto found = automation.find(target);
            if (found.empty() && op == "expect" && args[1] != "missing" && retry_frames_ % 2 == 1)
                automation.scroll_towards(target);
            actual = found.empty() ? "missing"
                     : (found.front()->status & ImGuiItemStatusFlags_Checkable) == 0 ? "visible"
                     : (found.front()->status & ImGuiItemStatusFlags_Checked) != 0   ? "checked"
                                                                                      : "unchecked";
            // A widget that should appear gets the same grace as click.
            if (op == "expect" && actual != args[1] && ++retry_frames_ < retry_limit) return;
            retry_frames_ = 0;
        } else {
            const auto snapshot = snapshot_state(state);
            const auto found = snapshot.find(args[0]);
            if (found == snapshot.end()) return fail(state, "no state key '" + args[0] + "'");
            actual = found->second;
        }
        const bool ok = op == "expect"       ? actual == args[1]
                        : op == "expect-not" ? actual != args[1]
                                             : actual.find(args[1]) != std::string::npos;
        if (!ok) fail(state, args[0] + " is '" + actual + "'");
    } else if (op == "remember" || op == "expect-same" || op == "expect-changed") {
        const auto snapshot = snapshot_state(state);
        const auto found = snapshot.find(args[0]);
        if (found == snapshot.end()) return fail(state, "no state key '" + args[0] + "'");
        if (op == "remember") {
            remembered_[args[0]] = found->second;
            return;
        }
        const auto before = remembered_.find(args[0]);
        if (before == remembered_.end()) return fail(state, "'" + args[0] + "' was not remembered");
        if ((found->second == before->second) != (op == "expect-same"))
            fail(state, args[0] + " is '" + found->second + "', remembered '" + before->second + "'");
    } else if (op == "screenshot" || op == "compare") {
        capture_ = op == "screenshot" ? Capture::screenshot : Capture::compare;
        capture_name_ = args[0];
        capture_budget_ = options_.changed_percent;
        if (args.size() == 2) {
            const auto budget = parse_float(args[1]);
            if (!budget) return fail(state, "'" + args[1] + "' is not a percentage");
            capture_budget_ = *budget;
        }
    } else if (op == "dump-state") {
        const auto path = output_path(args[0]);
        if (!write_text(path, state_snapshot_json(snapshot_state(state))))
            fail(state, "cannot write " + path_utf8(path));
    } else if (op == "lint") {
        std::vector<std::string> problems;
        if (args[0] == "commands") {
            problems = lint_commands(state.commands);
        } else if (args[0] == "theme") {
            // Every theme; the current one is restored (its style applies at the next rebuild).
            const std::string current(ui::theme_name());
            for (const char* theme : {"dark", "high-contrast"}) {
                ui::set_theme(theme);
                for (auto& problem : ui::theme_contrast_problems()) problems.push_back(std::move(problem));
            }
            ui::set_theme(current);
        } else {
            return fail(state, "expected 'lint commands' or 'lint theme'");
        }
        std::string message;
        for (const auto& problem : problems) message += "\n  " + problem;
        if (!problems.empty()) fail(state, std::to_string(problems.size()) + " problem(s):" + message);
    } else if (op == "list-items") {
        for (const auto& item : automation.items()) {
            if (!item.visible() || item.label.empty()) continue;
            const auto line = item.window + "::" + item.label;
            if (!args.empty() && line.find(args[0]) == std::string::npos) continue;
            std::fprintf(stdout, "  %s  (%.0f,%.0f %.0fx%.0f)\n", line.c_str(), item.rect.Min.x, item.rect.Min.y,
                         item.rect.GetWidth(), item.rect.GetHeight());
        }
    } else if (op == "monkey") {
        if (!monkey_) {
            const auto seed = parse_float(args[0]);
            const auto steps = parse_float(args[1]);
            if (!seed || !steps || *steps < 1) return fail(state, "expected 'monkey <seed> <steps>'");
            Monkey monkey;
            monkey.random.seed(static_cast<std::uint32_t>(*seed));
            monkey.remaining = static_cast<int>(*steps);
            // A longer walk on request: RWSMAN_UI_MONKEY_STEPS replaces every count.
            if (const char* longer = std::getenv("RWSMAN_UI_MONKEY_STEPS"))
                if (const auto value = parse_float(longer); value && *value >= 1) monkey.remaining = static_cast<int>(*value);
            // Commands that edit or change the view, never ones that write
            // files, open dialogs or leave the mission.
            for (const auto& command : state.commands.commands()) {
                const std::string_view id = command.id;
                if (id.starts_with("mission.tool_") || id.starts_with("view.markers_") ||
                    id == "view.frame_selection" || id == "view.frame_all" || id == "mission.duplicate" ||
                    id == "mission.delete" || id == "mission.delete_force" || id == "mission.align_ground" ||
                    id == "mission.select_none" || id == "mission.snap_surface" || id == "mission.capture_shot" ||
                    id == "mission.play_shots" || id == "mission.look_through_shot")
                    monkey.commands.emplace_back(id);
            }
            monkey_ = std::move(monkey);
        }
        if (monkey_->remaining-- <= 0) {
            monkey_.reset();
            retry_frames_ = 0;
            return;
        }
        retry_frames_ = 1;  // this step again next time: one action per step
        monkey_action(state, automation, *monkey_);
    } else if (op == "undo-all") {
        // Undo every edit (mission and project), as far as the histories go.
        for (int guard = 0; guard < 100000 && can_undo_edit(state); ++guard) undo_edit(state);
    } else if (op == "add-component") {
        auto lines = args[0];
        std::ranges::replace(lines, '|', '\n');
        if (!add_recipe(state, lines)) return fail(state, "the component was refused (see the log)");
    } else if (op == "resize") {
        int width = 0, height = 0;
        if (std::sscanf(args[0].c_str(), "%dx%d", &width, &height) != 2 || width <= 0 || height <= 0)
            return fail(state, "expected <width>x<height>");
        glfwSetWindowSize(state.window, width, height);
        wait_frames_ = 5;
    } else {
        fail(state, "unknown step");
    }
}

void UiScriptRunner::monkey_action(AppState& state, UiAutomation& automation, Monkey& monkey) {
    auto& random = monkey.random;
    // Portable across standard libraries (the distributions are not).
    const auto pick = [&](const std::size_t count) { return static_cast<std::size_t>(random() % count); };
    const auto unit = [&] { return static_cast<float>(random() % 20001) / 10000.0F - 1.0F; };
    const auto note = [&](std::string action) {
        monkey.recent.push_back(std::move(action));
        if (monkey.recent.size() > 12) monkey.recent.erase(monkey.recent.begin());
    };
    // The selection must always resolve to a record.
    if (state.selection.kind == SelectionRef::Kind::mission_entry && state.mission.scene &&
        selected_mission_record(state).kind == MissionRecordKey::Kind::none)
        return fail(state, "the selection (entry " + std::to_string(state.selection.a) + ") does not resolve");
    if (!state.mission.scene) return fail(state, "monkey needs an open mission");
    // Points to aim at: the records' positions, where markers are.
    std::vector<csf::Vec3> points;
    for (const auto& actor : state.mission.scene->actors())
        if (actor.position) points.push_back(*actor.position);
    for (const auto& group : state.mission.scene->navigation())
        for (const auto& point : group.points)
            if (point.position) points.push_back(*point.position);
    if (points.empty()) points.push_back({});
    const auto on_screen = [&](const csf::Vec3 point) -> std::optional<ImVec2> {
        const auto screen = state.preview.screen_position({point.x, point.y, point.z});
        const auto canvas = state.preview.canvas_rect();
        if (!screen || screen->x < canvas.x + 60 || screen->y < canvas.y + 90 || screen->x > canvas.z - 20 ||
            screen->y > canvas.w - 40)
            return std::nullopt;
        return screen;
    };
    const auto near = [&](const csf::Vec3 point, const float spread) {
        csf::Vec3 moved{point.x + unit() * spread, point.y, point.z + unit() * spread};
        moved.y = mission_ground(state, moved.x, moved.z).value_or(point.y);
        return moved;
    };
    const ImGuiKeyChord modifiers[]{0, 0, 0, ImGuiMod_Shift, ImGuiMod_Ctrl};
    switch (pick(9)) {
    case 0:
    case 1: {
        const auto& id = monkey.commands[pick(monkey.commands.size())];
        note("command " + id);
        state.commands.run(id);
        break;
    }
    case 2:
    case 3: {
        const auto target = near(points[pick(points.size())], pick(2) ? 0.0F : 400.0F);
        const auto held = modifiers[pick(std::size(modifiers))];
        if (const auto screen = on_screen(target)) {
            note("click-world " + std::to_string(target.x) + " " + std::to_string(target.z) +
                 (held == ImGuiMod_Shift ? " Shift" : held == ImGuiMod_Ctrl ? " Ctrl" : ""));
            automation.queue_click(*screen, 1, held);
        }
        break;
    }
    case 4: {
        const auto from = points[pick(points.size())];
        const auto to = near(from, 500.0F);
        const auto a = on_screen(from), b = on_screen(to);
        if (a && b) {
            note("drag-world from " + std::to_string(from.x) + " " + std::to_string(from.z));
            automation.queue_drag(*a, *b);
        }
        break;
    }
    case 5: {
        static constexpr const char* keys[]{"Enter", "Escape", "Backspace", "Delete", "Ctrl+D"};
        const char* key = keys[pick(std::size(keys))];
        note(std::string("key ") + key);
        (void)automation.queue_key(key);
        break;
    }
    case 6:
        note("undo");
        undo_edit(state);
        break;
    case 7:
        note("redo");
        redo_edit(state);
        break;
    case 8:
        // Arm the Place tool with one of the mission's classes.
        if (state.mission.objects && !state.mission.objects->definitions().empty() && state.mission.editor) {
            const auto& definitions = state.mission.objects->definitions();
            const auto& definition = definitions[pick(definitions.size())];
            if (!definition.class_id) break;
            note("place class " + std::to_string(*definition.class_id));
            arm_place_tool(state, {state.mission.editor->package_root(), "this mission", *definition.class_id,
                                   definition.name.value_or(""), definition.type.value_or("")});
        }
        break;
    }
}

void UiScriptRunner::after_render(AppState& state, UiAutomation& automation) {
    for (const auto& failure : automation.take_failures()) {
        if (failed_) break;
        fail(state, failure);
    }
    if (failed_ || capture_ == Capture::none) return;
    capture(state);
    capture_ = Capture::none;
}

void UiScriptRunner::capture(AppState& state) {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
    std::string error;
    if (!capture_back_buffer(state.window, width, height, rgba, error)) return fail(state, error);
    const auto write = [&](const std::filesystem::path& path, const int w, const int h,
                           const std::vector<std::uint8_t>& pixels, const bool replace) {
        std::error_code ignored;
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ignored);
        if (replace) std::filesystem::remove(path, ignored);
        std::string write_error;
        if (!rws::write_png_rgba(path, w, h, pixels, write_error)) {
            fail(state, write_error);
            return false;
        }
        return true;
    };
    if (capture_ == Capture::screenshot) {
        write(output_path(capture_name_), width, height, rgba, options_.overwrite);
        return;
    }
    auto name = capture_name_;
    if (!name.ends_with(".png")) name += ".png";
    const auto golden = options_.golden_directory / utf8_path(name);
    if (options_.update_goldens) {
        write(golden, width, height, rgba, true);
        return;
    }
    int golden_width = 0, golden_height = 0;
    std::vector<std::uint8_t> expected;
    if (!rws::decode_texture_image(golden, golden_width, golden_height, expected, error))
        return fail(state, "no reference " + path_utf8(golden) + " (run with --update-goldens): " + error);
    const auto difference = rws::compare_rgba_images(golden_width, golden_height, expected, width, height, rgba,
                                                     options_.channel_tolerance);
    if (difference.same_size && difference.changed_percent() <= capture_budget_) return;
    const auto stem = utf8_path(name).stem().string();
    write(output_path(stem + ".actual.png"), width, height, rgba, true);
    write(output_path(stem + ".diff.png"), width, height, difference.diff_rgba, true);
    char message[256];
    if (!difference.same_size)
        std::snprintf(message, sizeof(message), "the frame is %dx%d, the reference %dx%d", width, height,
                      golden_width, golden_height);
    else
        std::snprintf(message, sizeof(message), "%.3f%% of the pixels differ (budget %.3f%%); see %s.diff.png",
                      difference.changed_percent(), capture_budget_, stem.c_str());
    fail(state, message);
}

} // namespace rwsman
