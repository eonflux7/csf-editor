#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off
// windows.h must precede the headers below; they depend on its declarations.
#include <windows.h>
#include <shellapi.h>
// clang-format on
#endif

#include "app_actions.hpp"
#include "app_util.hpp"
#include "app_state.hpp"
#include "authoring.hpp"
#include "mission_authoring.hpp"
#include "commands.hpp"
#include "file_dialogs.hpp"
#include "navigation.hpp"
#include "mission_editing.hpp"
#include "screenshot.hpp"
#include "state_snapshot.hpp"
#include "ui_automation.hpp"
#include "ui_script_runner.hpp"
#include "ui/fonts.hpp"
#include "ui/layout.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"

#include "rwsman/frame_pacing.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::optional<std::filesystem::path> dropped_file;
// When the latest input event arrived (glfwGetTime seconds); wakes an idle frame loop.
double last_input_time = 0.0;

struct LaunchOptions {
    std::optional<std::filesystem::path> initial_path;
    std::optional<std::filesystem::path> project; // Mission project workspace to open.
    // Developer aids: render frames, run commands (one per frame), save the back
    // buffer to a new PNG, and exit. The window stays hidden unless --show is passed.
    std::optional<std::filesystem::path> screenshot;
    std::vector<std::string> commands;
    // A UI scenario script (docs/plans/editor-ux-redesign.md, T2) and where its
    // screenshots, references and failure reports go.
    std::optional<std::filesystem::path> script;
    std::optional<std::filesystem::path> output_dir, golden_dir, dump_state;
    bool update_goldens = false;
    bool overwrite = false; // replace existing output files (screenshots, dumps)
    // Use this directory for settings and layouts instead of the per-user one.
    std::optional<std::filesystem::path> config_dir;
    int screenshot_frames = 12;
    int width = 1400, height = 850;
    bool show = false;
    bool maximize = true;
    bool save_settings = false; // Keep saving settings even in --screenshot mode.
};

// `arguments` excludes the program name. Arguments stay paths until a flag needs text, so
// Windows wide-character file names reach the loader unconverted.
LaunchOptions parse_arguments(const std::vector<std::filesystem::path>& arguments) {
    LaunchOptions options;
    const auto utf8 = [](const std::filesystem::path& path) {
        const auto value = path.u8string();
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    };
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string argument = utf8(arguments[i]);
        const auto path_value = [&]() {
            return i + 1 < arguments.size() ? arguments[++i] : std::filesystem::path{};
        };
        const auto value = [&]() { return utf8(path_value()); };
        if (argument == "--screenshot") {
            options.screenshot = path_value();
            options.maximize = false;
        } else if (argument == "--frames") {
            options.screenshot_frames = std::max(1, std::atoi(value().c_str()));
        } else if (argument == "--size") {
            std::sscanf(value().c_str(), "%dx%d", &options.width, &options.height);
        } else if (argument == "--save-settings") {
            options.save_settings = true;
        } else if (argument == "--show") {
            options.show = true;
        } else if (argument == "--project") {
            options.project = path_value();
        } else if (argument == "--config-dir") {
            options.config_dir = path_value();
        } else if (argument == "--run-script") {
            options.script = path_value();
            options.maximize = false;
        } else if (argument == "--output-dir") {
            options.output_dir = path_value();
        } else if (argument == "--golden-dir") {
            options.golden_dir = path_value();
        } else if (argument == "--update-goldens") {
            options.update_goldens = true;
        } else if (argument == "--overwrite") {
            options.overwrite = true;
        } else if (argument == "--dump-state") {
            options.dump_state = path_value();
        } else if (argument == "--commands") {
            std::stringstream list(value());
            std::string item;
            while (std::getline(list, item, ',')) options.commands.push_back(item);
        } else if (!argument.starts_with("--") && !options.initial_path) {
            options.initial_path = arguments[i];
        }
    }
    return options;
}

void drop_callback(GLFWwindow*, const int count, const char** paths) {
    last_input_time = glfwGetTime();
    if (count > 0) dropped_file = std::filesystem::path(paths[0]);
}

// Installed before the ImGui backend, which chains to them from its own callbacks.
void install_input_callbacks(GLFWwindow* window) {
    glfwSetCursorPosCallback(window, [](GLFWwindow*, double, double) { last_input_time = glfwGetTime(); });
    glfwSetMouseButtonCallback(window, [](GLFWwindow*, int, int, int) { last_input_time = glfwGetTime(); });
    glfwSetScrollCallback(window, [](GLFWwindow*, double, double) { last_input_time = glfwGetTime(); });
    glfwSetKeyCallback(window, [](GLFWwindow*, int, int, int, int) { last_input_time = glfwGetTime(); });
    glfwSetCharCallback(window, [](GLFWwindow*, unsigned int) { last_input_time = glfwGetTime(); });
    glfwSetCursorEnterCallback(window, [](GLFWwindow*, int) { last_input_time = glfwGetTime(); });
    glfwSetWindowFocusCallback(window, [](GLFWwindow*, int) { last_input_time = glfwGetTime(); });
    glfwSetWindowSizeCallback(window, [](GLFWwindow*, int, int) { last_input_time = glfwGetTime(); });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow*) { last_input_time = glfwGetTime(); });
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow*, float, float) { last_input_time = glfwGetTime(); });
    last_input_time = glfwGetTime();
}

// Sleeps until `deadline`, finishing with a short spin: plain sleeps overshoot
// by a millisecond or more (far more with Windows' default timer resolution).
void sleep_until_precise(const std::chrono::steady_clock::time_point deadline) {
    using namespace std::chrono;
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    static const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    const auto coarse = deadline - microseconds(500);
    if (timer && steady_clock::now() < coarse) {
        LARGE_INTEGER due{};
        due.QuadPart = -duration_cast<nanoseconds>(coarse - steady_clock::now()).count() / 100;
        if (due.QuadPart < 0 && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer, INFINITE);
    }
#else
    if (const auto coarse = deadline - milliseconds(1); steady_clock::now() < coarse)
        std::this_thread::sleep_until(coarse);
#endif
    while (steady_clock::now() < deadline) std::this_thread::yield();
}

int run_app(const LaunchOptions& options) {
    const bool scripted = options.screenshot || options.script || !options.commands.empty();
    // Scripted runs report a missing display or GL context as "skipped" (77, as
    // ctest's SKIP_RETURN_CODE for the UI tests), not as a failure.
    const int no_window = scripted ? 77 : 1;
    if (!glfwInit()) {
        std::fprintf(stderr, "cannot initialise GLFW (no display?)\n");
        return no_window;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_STENCIL_BITS, 8); // Selection outline mask.
    glfwWindowHint(GLFW_MAXIMIZED, options.maximize ? GLFW_TRUE : GLFW_FALSE);
    if (scripted && !options.show) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(options.width, options.height, "CSF RWS Tools - rws-man",
                                    nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "cannot create an OpenGL 3.3 window\n");
        glfwTerminate();
        return no_window;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, drop_callback);
    install_input_callbacks(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Multi-viewports stay off: Wayland and X11 support for them is unreliable.
    io.IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // ImGui keeps the pointer to the ini path until DestroyContext() writes it.
    std::string ini_path;
    int run_exit_code = 0;
    {
        rwsman::AppState state;
        state.window = window;

        // Per-user configuration: settings, layouts, and the debug log all live in
        // one directory. Nothing is written to the working directory.
        state.config_dir = options.config_dir ? *options.config_dir : rwsman::config_directory();
        const auto settings_path = state.config_dir.empty() ? std::filesystem::path{}
                                                            : state.config_dir / "settings.ini";
        if (!state.config_dir.empty()) {
            std::error_code error;
            std::filesystem::create_directories(state.config_dir, error);
            ini_path = (state.config_dir / "layout.ini").string();
            // Screenshot runs read the saved layouts but never write them back, like
            // settings.ini (a scripted run must not rearrange the user's panels).
            if (scripted && !options.save_settings)
                ImGui::LoadIniSettingsFromDisk(ini_path.c_str());
            else
                io.IniFilename = ini_path.c_str();
        }
        auto loaded = rwsman::load_settings(settings_path);
        state.settings = std::move(loaded.settings);
        if (!loaded.existed) {
            state.log.push(rwsman::LogLevel::info, "First launch: using default settings");
            rwsman::import_legacy_pairings(state);
        }
        for (const auto& warning : loaded.warnings)
            state.log.push(rwsman::LogLevel::warn, "Settings: " + warning);
        if (!loaded.warnings.empty())
            state.log.push(rwsman::LogLevel::warn, "Some settings were invalid and were reset to defaults");
        rwsman::ui::set_theme(state.settings.theme);
        rwsman::register_commands(state);
        rwsman::apply_viewport_settings(state);
        if (!state.settings.resource_root.empty()) rwsman::rescan_resource_root(state);
        for (const auto& conflict : state.commands.conflicts())
            state.log.push(rwsman::LogLevel::warn, "Shortcut conflict: " + conflict.first + " / " +
                                                       conflict.second + " (" + conflict.shortcut + ")");

        // Scripted runs: a UI script, or the older --commands list as one.
        rwsman::UiAutomation automation;
        std::unique_ptr<rwsman::UiScriptRunner> runner;
        int exit_code = 0;
        if (options.script || !options.commands.empty()) {
            std::vector<rwsman::UiScriptStep> steps;
            rwsman::UiScriptOptions script_options;
            if (options.script) {
                std::ifstream in(*options.script, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                auto parsed = rwsman::parse_ui_script(text);
                if (!in && text.empty()) parsed.errors.push_back("cannot read the script");
                for (const auto& error : parsed.errors)
                    std::fprintf(stderr, "%s: %s\n", rwsman::path_utf8(*options.script).c_str(), error.c_str());
                if (!parsed.errors.empty()) exit_code = 2;
                steps = std::move(parsed.steps);
                script_options.name = rwsman::path_utf8(options.script->filename());
                script_options.base_directory = options.script->parent_path();
            } else {
                script_options.name = "--commands";
            }
            // Commands given with a script run after it.
            for (auto& step : rwsman::ui_script_from_commands(options.commands)) steps.push_back(std::move(step));
            script_options.output_directory =
                options.output_dir ? *options.output_dir
                                   : (options.screenshot ? options.screenshot->parent_path() : std::filesystem::path{});
            script_options.golden_directory = options.golden_dir ? *options.golden_dir : script_options.base_directory;
            script_options.update_goldens = options.update_goldens;
            script_options.overwrite = options.overwrite;
            runner = std::make_unique<rwsman::UiScriptRunner>(std::move(steps), std::move(script_options));
            automation.enable();
            state.ui.deterministic = true;
            // Most specific first, so a fixture under the home directory keeps its name.
            for (const char* variable : {"RWSMAN_UI_FIXTURE", "RWSMAN_UI_CONFIG", "HOME"})
                if (const char* value = std::getenv(variable); value && *value)
                    state.ui.path_aliases.emplace_back(value, variable[0] == 'H' ? "~" : std::string("$") + variable);
        }
        if (exit_code != 0) glfwSetWindowShouldClose(window, GLFW_TRUE);

        if (options.project) rwsman::open_mission_project(state, *options.project);
        else if (options.initial_path) rwsman::open_path(state, *options.initial_path);
        int frame = 0, settled = 0;
        auto last_settings_change = std::chrono::steady_clock::now();
        bool settings_pending = false;
        // Frame pacing: the previous frame decides whether this one waits for input
        // and how soon it may start. Scripted screenshot runs never wait.
        rwsman::FrameRateCounter frame_rate;
        rwsman::FramePacing pacing;
        auto frame_start = std::chrono::steady_clock::now();
        // Scripted runs never wait for a save prompt.
        bool closing_confirmed = scripted;

        while (true) {
            if (glfwWindowShouldClose(window)) {
                // Unsaved mission edits: ask first; the dialog closes the window.
                if (!edits_unsaved(state) || closing_confirmed) break;
                glfwSetWindowShouldClose(window, GLFW_FALSE);
                state.ui.pending_discard = [&closing_confirmed, window] {
                    closing_confirmed = true;
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                };
                state.ui.pending_discard_label = "Exiting";
            }
            if (pacing.min_frame_time > 0.0)
                sleep_until_precise(frame_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                      std::chrono::duration<double>(pacing.min_frame_time)));
            const bool waited = pacing.wait_for_events;
            if (waited) {
                // Compositor traffic (Wayland frame callbacks, buffer releases after a
                // swap) also ends a wait. Only input, a close request, or the timeout
                // should draw a frame, so any other wake-up goes back to waiting.
                const double wait_start = glfwGetTime();
                const double input_before = last_input_time;
                double remaining = pacing.wait_timeout;
                while (true) {
                    glfwWaitEventsTimeout(remaining);
                    if (last_input_time != input_before || dropped_file || glfwWindowShouldClose(window))
                        break;
                    remaining = pacing.wait_timeout - (glfwGetTime() - wait_start);
                    if (remaining <= 0.0) break;
                }
            } else {
                glfwPollEvents();
            }
            const auto now = std::chrono::steady_clock::now();
            if (!waited && frame > 0)
                frame_rate.add(std::chrono::duration<double>(now - frame_start).count());
            frame_start = now;
            state.frame_stats.fps = frame_rate.fps();
            state.ui.animating = false;
            if (dropped_file) {
                rwsman::open_path(state, *dropped_file);
                dropped_file.reset();
            }
            rwsman::poll_mission_load(state);
            rwsman::poll_file_dialogs(state);
            rwsman::poll_authoring(state);
            rwsman::update_shot_preview(state);

            // Fonts and style follow the OS content scale and the user override
            // (checked every frame; rebuilt only when one of them changed).
            rwsman::ui::update_fonts_and_style(window, state.settings.ui_scale);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            // The script's input goes after the backend's, so it wins.
            if (runner) runner->before_frame(state, automation);
            ImGui::NewFrame();

            if (state.ui.deterministic)
                state.preview.set_frame_timing(-1.0, 0.0);
            else
                state.preview.set_frame_timing(state.frame_stats.fps, state.frame_stats.cpu_ms);
            rwsman::ui::draw_frame(state);

            ImGui::Render();
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            const auto clear = rwsman::ui::viewport_color_f(rwsman::ui::Viewport::clear);
            glClearColor(clear.x, clear.y, clear.z, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (state.ui.screenshot_requested) {
                state.ui.screenshot_requested = false;
                const auto path = rwsman::next_screenshot_path(state);
                std::string error;
                if (rwsman::save_screenshot(window, path, error))
                    state.notify(rwsman::LogLevel::ok, "Saved screenshot " + rwsman::path_utf8(path), path.parent_path());
                else
                    state.notify(rwsman::LogLevel::error, "Screenshot failed: " + error);
            }
            if (runner) {
                runner->after_render(state, automation);
                if (runner->failed()) {
                    exit_code = 1;
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }
            if (runner && runner->skipped()) {
                exit_code = 77;
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            const bool script_running = runner && !runner->done();
            if (options.screenshot && exit_code == 0) {
                const bool busy = rwsman::mission_load_active(state) || rwsman::authoring_pending(state) ||
                                  script_running;
                settled = busy ? 0 : settled + 1;
                if (settled >= options.screenshot_frames) {
                    std::string error;
                    std::error_code ignored;
                    if (options.overwrite) std::filesystem::remove(*options.screenshot, ignored);
                    if (!rwsman::save_screenshot(window, *options.screenshot, error)) {
                        std::fprintf(stderr, "screenshot failed: %s\n", error.c_str());
                        exit_code = 1;
                    }
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            } else if (runner && !script_running && exit_code == 0) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            ++frame;
            state.frame_stats.cpu_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count();
            state.frame_stats.record_frame(state.frame_stats.cpu_ms);
            glfwSwapBuffers(window);

            if (!scripted) {
                const auto& settings = state.settings;
                rwsman::FramePacingInput input;
                input.now = glfwGetTime();
                input.last_input = last_input_time;
                input.animating = state.ui.animating || state.preview.animating() ||
                                  state.mission.animation_playing || !state.toasts.empty() ||
                                  ImGui::IsAnyMouseDown() || dropped_file.has_value();
                input.background_work = rwsman::mission_load_active(state) || state.dialog != nullptr ||
                                        rwsman::authoring_pending(state);
                input.text_input = io.WantTextInput;
                input.focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
                input.iconified = glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0;
                pacing = rwsman::decide_frame_pacing(
                    {settings.idle_redraw, settings.fps_limit, settings.background_fps_limit}, input);
                state.frame_stats.idle = pacing.wait_for_events;
            }

            // Settings are saved shortly after the last change, and always at exit.
            if (state.settings_dirty) {
                state.settings_dirty = false;
                last_settings_change = std::chrono::steady_clock::now();
                settings_pending = true;
            }
            if (settings_pending && (!scripted || options.save_settings) &&
                std::chrono::steady_clock::now() - last_settings_change > std::chrono::milliseconds(500)) {
                settings_pending = false;
                state.settings.workspace = rwsman::ui::workspace_key(state.workspace);
                state.settings.theme = std::string(rwsman::ui::theme_name());
                if (const auto message = rwsman::save_settings(settings_path, state.settings); !message.empty())
                    state.log.push(rwsman::LogLevel::error, "Settings not saved: " + message);
            }
        }
        state.settings.workspace = rwsman::ui::workspace_key(state.workspace);
        state.settings.theme = std::string(rwsman::ui::theme_name());
        if (options.dump_state) {
            std::error_code ignored;
            if (options.overwrite) std::filesystem::remove(*options.dump_state, ignored);
            if (std::filesystem::exists(*options.dump_state, ignored)) {
                std::fprintf(stderr, "state not written: %s exists (use --overwrite)\n",
                             rwsman::path_utf8(*options.dump_state).c_str());
            } else {
                std::ofstream out(*options.dump_state, std::ios::binary);
                out << rwsman::state_snapshot_json(rwsman::snapshot_state(state));
            }
        }
        if (runner && exit_code == 0) std::fprintf(stdout, "ui script passed\n");
        run_exit_code = exit_code;
        if (!scripted || options.save_settings)
            if (const auto message = rwsman::save_settings(settings_path, state.settings); !message.empty())
                std::fprintf(stderr, "settings not saved: %s\n", message.c_str());
        state.preview.clear();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return run_exit_code;
}

} // namespace

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::filesystem::path> arguments;
    if (argv != nullptr) {
        for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
        LocalFree(argv);
    }
    return run_app(parse_arguments(arguments));
}
#else
int main(int argc, char** argv) {
    std::vector<std::filesystem::path> arguments;
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    return run_app(parse_arguments(arguments));
}
#endif
