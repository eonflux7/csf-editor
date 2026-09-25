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
#include "commands.hpp"
#include "file_dialogs.hpp"
#include "navigation.hpp"
#include "mission_editing.hpp"
#include "screenshot.hpp"
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
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_STENCIL_BITS, 8); // Selection outline mask.
    glfwWindowHint(GLFW_MAXIMIZED, options.maximize ? GLFW_TRUE : GLFW_FALSE);
    if (options.screenshot && !options.show) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    auto* window = glfwCreateWindow(options.width, options.height, "CSF RWS Tools - rws-man",
                                    nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
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
            if (options.screenshot && !options.save_settings)
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

        if (options.project) rwsman::open_mission_project(state, *options.project);
        else if (options.initial_path) rwsman::open_path(state, *options.initial_path);
        int frame = 0, settled = 0, last_busy_frame = 0;
        std::size_t next_command = 0;
        auto last_settings_change = std::chrono::steady_clock::now();
        bool settings_pending = false;
        // Frame pacing: the previous frame decides whether this one waits for input
        // and how soon it may start. Scripted screenshot runs never wait.
        rwsman::FrameRateCounter frame_rate;
        rwsman::FramePacing pacing;
        auto frame_start = std::chrono::steady_clock::now();
        // Scripted screenshot runs never wait for a save prompt.
        bool closing_confirmed = options.screenshot.has_value();

        while (true) {
            if (glfwWindowShouldClose(window)) {
                // Unsaved mission edits: ask first; the dialog closes the window.
                if (!state.mission.editor || !state.mission.editor->dirty() || closing_confirmed) break;
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

            // Fonts and style follow the OS content scale and the user override
            // (checked every frame; rebuilt only when one of them changed).
            rwsman::ui::update_fonts_and_style(window, state.settings.ui_scale);

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            // Scripted commands (developer aid), one per frame after the UI settled.
            if (rwsman::mission_load_active(state) || rwsman::authoring_pending(state)) last_busy_frame = frame;
            if (frame >= 3 && frame >= last_busy_frame + 3 && next_command < options.commands.size())
            {
                const auto& command = options.commands[next_command++];
                if (command.starts_with("goto:")) {
                    // Developer aid: select the best search match for the text.
                    // "goto:kind:text" restricts the match to one symbol kind (for example "dummy").
                    auto text = command.substr(5);
                    std::string kind;
                    if (const auto colon = text.find(':'); colon != std::string::npos) {
                        kind = text.substr(0, colon);
                        text = text.substr(colon + 1);
                    }
                    for (const auto& result : state.search_index.query(text, 100))
                        if (kind.empty() || kind == rwsman::symbol_kind_name(result.entry->kind)) {
                            rwsman::navigate_to(state, result.entry->target);
                            break;
                        }
                } else if (command.starts_with("palette:") || command.starts_with("goto-palette:")) {
                    // Developer aid: open the palette with a query already typed.
                    const bool go_to = command.starts_with("goto-palette:");
                    state.ui.palette = go_to ? rwsman::UiState::PaletteMode::go_to : rwsman::UiState::PaletteMode::commands;
                    state.ui.palette_just_opened = true;
                    state.ui.palette_prefilled = true;
                    const auto text = command.substr(command.find(':') + 1);
                    std::snprintf(state.ui.palette_query.data(), state.ui.palette_query.size(), "%s", text.c_str());
                } else {
                    state.commands.run(command);
                }
            }

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
            if (options.screenshot) {
                const bool busy = rwsman::mission_load_active(state) || rwsman::authoring_pending(state) ||
                                  next_command < options.commands.size();
                settled = busy ? 0 : settled + 1;
                if (settled >= options.screenshot_frames) {
                    std::string error;
                    if (!rwsman::save_screenshot(window, *options.screenshot, error))
                        std::fprintf(stderr, "screenshot failed: %s\n", error.c_str());
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            }
            ++frame;
            state.frame_stats.cpu_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_start).count();
            glfwSwapBuffers(window);

            if (!options.screenshot) {
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
            if (settings_pending && (!options.screenshot || options.save_settings) &&
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
        if (!options.screenshot || options.save_settings)
            if (const auto message = rwsman::save_settings(settings_path, state.settings); !message.empty())
                std::fprintf(stderr, "settings not saved: %s\n", message.c_str());
        state.preview.clear();
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
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
