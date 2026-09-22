#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// clang-format off
// windows.h must precede the headers below; they depend on its declarations.
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
// clang-format on
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "app_actions.hpp"

#include "app_util.hpp"
#include "file_dialogs.hpp"
#include "mission_loader.hpp"
#include "mission_overlays.hpp"
#include "navigation.hpp"
#include "rws/decoded.hpp"
#include "rws/obj_export.hpp"
#include "rws/scene_export.hpp"
#include "rws/world_recovery.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace rwsman {
namespace {

#ifdef _WIN32
std::optional<std::filesystem::path> choose_rws_file(const std::filesystem::path& directory) {
    std::array<wchar_t, 32768> path{};
    const auto initial = directory.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter =
        L"RenderWare models, streams, and animation (*.rpc;*.rws;*.anm)\0*.rpc;*.rws;*.anm\0RPC "
        L"Clump models (*.rpc)\0*.rpc\0Animations (*.anm)\0*.anm\0RenderWare streams "
        L"(*.rws)\0*.rws\0All files\0*.*\0\0";
    dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path(path.data());
}

std::optional<std::filesystem::path> choose_mission_file(const std::filesystem::path& directory) {
    std::array<wchar_t, 32768> path{};
    const auto initial = directory.wstring();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter = L"CSF mission scenes (*.scn)\0*.scn\0All files\0*.*\0\0";
    dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return std::nullopt;
    return std::filesystem::path(path.data());
}
#endif

std::size_t count_chunks(const std::vector<rws::Chunk>& chunks, const std::uint32_t type) {
    std::size_t count{};
    for (const auto& chunk : chunks) {
        if (chunk.type == type) ++count;
        count += count_chunks(chunk.children, type);
    }
    return count;
}

} // namespace

const char* workspace_name(const Workspace workspace) {
    switch (workspace) {
    case Workspace::mission:
        return "Mission";
    case Workspace::script:
        return "Script";
    case Workspace::animation:
        return "Animation";
    case Workspace::scene:
        return "Scene";
    case Workspace::geometry:
        return "Geometry";
    case Workspace::inspector:
        return "Inspector";
    }
    return "Scene";
}

void import_legacy_pairings(AppState& state) {
    std::filesystem::path path;
#ifdef _WIN32
    if (const auto* local = std::getenv("LOCALAPPDATA"))
        path = std::filesystem::path(local) / "CSF RWS Tools" / "recent-pairings.txt";
#else
    std::error_code error;
    path = std::filesystem::temp_directory_path(error) / "csf-rws-tools-recent-pairings.txt";
#endif
    if (path.empty()) return;
    std::ifstream input(path);
    std::string line;
    std::vector<RecentPairing> pairs;
    while (std::getline(input, line) && pairs.size() < Settings::max_recent_pairings) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        pairs.push_back({std::filesystem::path(line.substr(0, tab)),
                         std::filesystem::path(line.substr(tab + 1))});
    }
    // Stored newest first; add oldest first so the order is preserved.
    for (auto pair = pairs.rbegin(); pair != pairs.rend(); ++pair)
        state.settings.add_recent_pairing(pair->main, pair->collision);
    if (!pairs.empty()) state.log.push(LogLevel::info, "Imported " + std::to_string(pairs.size()) + " recent collision pairings");
}

void rescan_resource_root(AppState& state) {
    state.discovered = discover_missions(state.settings.resource_root);
    if (!state.settings.resource_root.empty())
        state.info("Found " + std::to_string(state.discovered.size()) + " missions under " +
                   path_utf8(state.settings.resource_root));
}

void apply_viewport_settings(AppState& state) {
    state.preview.set_navigation_speed(state.settings.move_speed);
    state.preview.set_invert_y(state.settings.invert_y);
    state.preview.set_view_style(state.settings.default_view_style);
    state.preview.set_show_hud(state.settings.show_hud);
}

std::filesystem::path next_screenshot_path(const AppState& state) {
    const auto directory =
        (state.config_dir.empty() ? std::filesystem::temp_directory_path() : state.config_dir) / "screenshots";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    std::string stem = state.mission.graph ? path_utf8(state.mission.graph->scene_path().stem())
                       : state.document    ? path_utf8(state.document->source_path().stem())
                                           : std::string("rws-man");
    auto path = directory / (stem + "-" + stamp + ".png");
    for (int suffix = 1; std::filesystem::exists(path, error); ++suffix)
        path = directory / (stem + "-" + stamp + "-" + std::to_string(suffix) + ".png");
    return path;
}

void restore_mission_overlays(AppState& state) {
    if (!state.mission.scene) return;
    const auto overlays = make_mission_overlays(*state.mission.scene);
    state.preview.set_mission_overlays(overlays.points, overlays.lines);
}

bool pair_collision(AppState& state, const std::filesystem::path& candidate_path,
                    const bool remember) {
    if (!state.document) return false;
    try {
        auto candidate = std::make_unique<rws::Document>(rws::Document::load(candidate_path));
        const auto worlds = rws::recover_worlds(candidate->chunks(), candidate->bytes());
        if (worlds.empty() || std::none_of(worlds.begin(), worlds.end(), [](const auto& world) {
                return !world.sectors.empty();
            }))
            throw std::runtime_error("Selected companion has no recoverable World sectors");
        state.collision_document = std::move(candidate);
        state.collision_status = "Loaded manual companion " + path_utf8(candidate_path);
        state.info(state.collision_status);
        state.preview.clear();
        restore_mission_overlays(state);
        if (remember) {
            std::error_code error;
            const auto main_path =
                std::filesystem::weakly_canonical(state.document->source_path(), error);
            const auto collision_path = std::filesystem::weakly_canonical(candidate_path, error);
            state.settings.add_recent_pairing(main_path, collision_path);
            state.settings_dirty = true;
        }
        return true;
    } catch (const std::exception& error) {
        state.error("Collision companion unchanged: " + std::string(error.what()));
        return false;
    }
}

void load_document(AppState& state, const std::filesystem::path& path) {
    auto& document = state.document;
    auto& mission = state.mission;
    try {
        auto loaded_document = std::make_unique<rws::Document>(rws::Document::load(path));
        const auto source_extension = lower_ascii(path_utf8(path.extension()));
        if (source_extension == ".rpc" && (loaded_document->chunks().empty() ||
                                           loaded_document->chunks().front().type != 0x10U))
            throw std::runtime_error("RPC root is not a RenderWare Clump (0x10)");
        mission = MissionState{};
        state.selected_program_document = state.selected_program_script = 0;
        document = std::move(loaded_document);
        state.pending_overwrite.reset();
        state.collision_document.reset();
        state.collision_status.clear();
        state.main_is_collision = false;
        state.preview.clear();
        const auto stem = lower_ascii(path_utf8(path.stem()));
        state.main_is_collision = stem.size() >= 4 && stem.ends_with("_col");
        if (state.main_is_collision) {
            const auto worlds = rws::recover_worlds(document->chunks(), document->bytes());
            state.collision_status = !worlds.empty() && !worlds.front().sectors.empty()
                                         ? "Opened collision World directly"
                                         : "The _col file has no recoverable World";
        } else if (source_extension != ".rpc") {
            const auto companion = with_stem_suffix(path, "_col", true);
            std::error_code filesystem_error;
            if (std::filesystem::is_regular_file(companion, filesystem_error)) {
                try {
                    auto candidate = std::make_unique<rws::Document>(rws::Document::load(companion));
                    const auto worlds = rws::recover_worlds(candidate->chunks(), candidate->bytes());
                    if (!worlds.empty() && !worlds.front().sectors.empty()) {
                        state.collision_status = "Loaded " + path_utf8(companion);
                        state.collision_document = std::move(candidate);
                    } else {
                        state.collision_status =
                            "Companion has no recoverable World: " + path_utf8(companion);
                    }
                } catch (const std::exception& error) {
                    state.collision_status = "Companion failed to load: " + std::string(error.what());
                }
            } else {
                state.collision_status = "Companion not found: " + path_utf8(companion);
            }
        }
        state.display_names = resolve_chunk_display_names(document->chunks(), document->bytes(),
                                                          document->scene_instances());
        const auto* first_geometry = find_first_chunk(document->chunks(), 0x0F);
        if (first_geometry)
            state.selected = first_geometry->offset;
        else if (!document->chunks().empty())
            state.selected = document->chunks().front().offset;
        else
            state.selected.reset();
        state.previous_selection.reset();
        const bool has_scene = !document->scene_instances().empty() ||
                               find_first_chunk(document->chunks(), 0x10) != nullptr ||
                               find_first_chunk(document->chunks(), 0x0B) != nullptr ||
                               find_first_chunk(document->chunks(), 0x907) != nullptr ||
                               find_first_chunk(document->chunks(), 0x909) != nullptr;
        state.workspace = has_scene ? Workspace::scene
                                    : (first_geometry ? Workspace::geometry : Workspace::inspector);
        state.content_signature = content_signature("rws", document->bytes());
        reset_navigation(state);
        state.edits.clear();
        state.ui.hex_highlight.reset();
        rebuild_search_index(state);
        rebuild_diagnostics(state);
        state.settings.add_recent_file(path, false);
        state.settings_dirty = true;
        state.ok((source_extension == ".rpc" ? "Loaded RPC Clump model laboratory: "
                                              : "Loaded ") +
                 path_utf8(path) + " (" + std::to_string(document->chunks().size()) + " chunks, " +
                 std::to_string(document->scene_instances().size()) + " scene instances)");
        if (!state.collision_status.empty() && state.collision_status != "Opened collision World directly")
            state.log.push(state.collision_status.starts_with("Loaded") ? LogLevel::info : LogLevel::warn,
                           state.collision_status);
        if (!document->diagnostics().empty())
            state.log.push(LogLevel::warn, std::to_string(document->diagnostics().size()) +
                                               " RWS diagnostics (see the Diagnostics panel)");
    } catch (const std::exception& error) {
        state.error("Could not load " + path_utf8(path) + ": " + error.what());
    }
}

void start_mission_load(AppState& state, const std::filesystem::path& path) {
    if (!state.loader) state.loader = std::make_shared<MissionLoader>();
    if (!state.loader->start(path, state.config_dir)) {
        state.warn("A mission is already loading; wait or cancel it first");
        return;
    }
    state.info("Loading mission " + path_utf8(path) + "...");
}

bool mission_load_active(const AppState& state) {
    return state.loader && state.loader->busy();
}

void cancel_mission_load(AppState& state) {
    if (state.loader) state.loader->cancel();
}

void poll_mission_load(AppState& state) {
    if (!state.loader) return;
    auto outcome = state.loader->poll();
    if (auto* failure = std::get_if<MissionLoadFailure>(&outcome)) {
        if (failure->cancelled)
            state.warn("Mission load cancelled: " + path_utf8(failure->input));
        else
            state.notify(LogLevel::error, "Mission unchanged: " + failure->message + " (while " +
                        lower_ascii(failure->stage) + ")");
        return;
    }
    auto* result = std::get_if<MissionLoadResult>(&outcome);
    if (!result) return;

    auto& mission = state.mission;
    mission = std::move(result->mission);
    state.document = std::move(result->visual);
    state.pending_overwrite.reset();
    state.collision_document = std::move(result->collision);
    state.selected_program_document = state.selected_program_script = 0;
    state.main_is_collision = false;
    state.collision_status = state.collision_document
                                 ? "Loaded mission collision map " + path_utf8(result->collision_path)
                                 : "Mission collision map is unresolved";
    state.preview.clear();
    state.preview.set_texture_catalog(mission.graph->textures());
    state.preview.set_mission_overlays(result->overlays.points, result->overlays.lines);
    state.preview.set_mission_actor_models(std::move(result->actor_models));
    state.display_names = resolve_chunk_display_names(
        state.document->chunks(), state.document->bytes(), state.document->scene_instances());
    state.selected.reset();
    state.previous_selection.reset();
    state.workspace = Workspace::mission;
    state.settings.show_explorer = true;
    state.settings.show_inspector = true;
    state.content_signature = content_signature("scn", mission.document->bytes());
    reset_navigation(state);
    state.edits.clear();
    state.ui.hex_highlight.reset();
    rebuild_search_index(state);
    rebuild_diagnostics(state);
    state.settings.add_recent_file(result->input, true);
    state.settings_dirty = true;

    char seconds[16];
    std::snprintf(seconds, sizeof(seconds), "%.2f", result->seconds);
    const std::string success =
        "Loaded mission " + path_utf8(mission.graph->scene_path()) + " (" + seconds + " s, " +
        std::to_string(count_chunks(state.document->chunks(), 0x10)) + " clumps, " +
        std::to_string(mission.scene->actors().size()) + " actors, " +
        std::to_string(mission.scene->navigation_stats().points) + " nav points)";
    state.ok(success);
    state.log.push(state.collision_document ? LogLevel::info : LogLevel::warn,
                   state.collision_status);
    const auto problems = std::ranges::count_if(state.diagnostics, [](const auto& row) {
        return row.severity != DiagnosticSeverity::note;
    });
    if (problems > 0)
        state.log.push(LogLevel::warn, std::to_string(problems) +
                                           " diagnostics while loading (see the Diagnostics panel)");
    // Last line: the success message stays the most recent entry in the status bar.
    state.status = success;
}

void open_path(AppState& state, const std::filesystem::path& path) {
    if (lower_ascii(path_utf8(path.extension())) == ".scn")
        start_mission_load(state, path);
    else
        load_document(state, path);
}

std::filesystem::path unique_output_path(const std::filesystem::path& desired) {
    std::error_code error;
    if (!std::filesystem::exists(desired, error)) return desired;
    for (int index = 2;; ++index) {
        const auto candidate = with_stem_suffix(desired, "." + std::to_string(index), true);
        if (!std::filesystem::exists(candidate, error)) return candidate;
    }
}

namespace {

// Applies the export policy. Returns the path to write, or nullopt when the user
// has to confirm replacing an existing file first; `retry` then re-runs the export
// with the chosen mode.
std::optional<std::filesystem::path>
choose_output(AppState& state, const std::filesystem::path& desired, const OutputMode mode,
              const std::function<void(OutputMode)>& retry) {
    std::error_code error;
    if (!std::filesystem::exists(desired, error)) return desired;
    // The confirmation covers only the path the dialog showed. If the export now targets
    // another file (the document changed while the dialog was open), ask again.
    if (mode == OutputMode::overwrite && state.confirmed_overwrite == desired) return desired;
    if (mode == OutputMode::unique || state.settings.export_policy == ExportPolicy::new_files_only)
        return unique_output_path(desired);
    state.pending_overwrite = AppState::PendingOverwrite{
        desired,
        [&state, retry, desired] {
            state.confirmed_overwrite = desired;
            retry(OutputMode::overwrite);
            state.confirmed_overwrite.reset();
        },
        [retry] { retry(OutputMode::unique); }};
    return std::nullopt;
}

} // namespace

void open_folder(const std::filesystem::path& folder) {
    if (folder.empty()) return;
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    // Detached xdg-open; the path is passed as an argument, never through a shell. The
    // double fork hands xdg-open to init, and the intermediate child is reaped here, so no
    // zombie is left behind.
    const auto path = folder.string();
    const pid_t child = fork();
    if (child == 0) {
        setsid();
        if (fork() == 0) {
            execlp("xdg-open", "xdg-open", path.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    if (child > 0)
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
#endif
}

void save_copy(AppState& state, const OutputMode mode) {
    if (!state.document) return;
    try {
        const auto desired = with_stem_suffix(state.document->source_path(), ".edited", true);
        const auto chosen = choose_output(state, desired, mode, [&state](OutputMode m) { save_copy(state, m); });
        if (!chosen) return;
        const auto& output = *chosen;
        state.document->save_as(output);
        state.notify(LogLevel::ok, "Saved " + path_utf8(output), output.parent_path());
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Save copy failed: ") + error.what());
    }
}

void open_document_dialog(AppState& state) {
#ifdef _WIN32
    if (const auto path = choose_rws_file(
            state.document ? state.document->source_path().parent_path() : std::filesystem::path{}))
        load_document(state, *path);
#else
    request_file_dialog(state, DialogKind::document,
                        state.document ? state.document->source_path().parent_path() : std::filesystem::path{});
#endif
}

void open_mission_dialog(AppState& state) {
#ifdef _WIN32
    const auto directory = state.mission.graph ? state.mission.graph->scene_path().parent_path()
                           : state.document    ? state.document->source_path().parent_path()
                                               : std::filesystem::path{};
    if (const auto path = choose_mission_file(directory)) start_mission_load(state, *path);
#else
    request_file_dialog(state, DialogKind::mission,
                        state.mission.graph ? state.mission.graph->scene_path().parent_path()
                        : state.document    ? state.document->source_path().parent_path()
                                            : std::filesystem::path{});
#endif
}

void open_companion_dialog(AppState& state) {
#ifdef _WIN32
    if (state.document)
        if (const auto path = choose_rws_file(state.document->source_path().parent_path()))
            pair_collision(state, *path, true);
#else
    if (state.document) request_file_dialog(state, DialogKind::companion, state.document->source_path().parent_path());
#endif
}

const rws::Document* collision_export_document(const AppState& state) {
    return state.main_is_collision ? state.document.get() : state.collision_document.get();
}

const rws::Chunk* selected_clump(const AppState& state) {
    return state.document && state.selected
               ? find_enclosing_clump(state.document->chunks(), *state.selected)
               : nullptr;
}

void export_scene_gltf(AppState& state, const OutputMode mode) {
    auto& document = state.document;
    if (!document) return;
    try {
        const auto desired = with_stem_suffix(document->source_path(), ".scene.gltf", false);
        const auto chosen = choose_output(state, desired, mode, [&state](OutputMode m) { export_scene_gltf(state, m); });
        if (!chosen) return;
        const auto& output = *chosen;
        const auto stats = rws::export_scene_gltf(document->chunks(), document->scene_instances(),
                                                  document->bytes(), output);
        std::ostringstream message;
        message << "Exported " << stats.atomic_instances << " atomic meshes ("
                << stats.custom_instances << " CSF placements, " << stats.unresolved_instances
                << " unresolved) and " << stats.recovered_world_sectors
                << " recovered World sectors (" << stats.world_sectors << " exported meshes) to "
                << path_utf8(output);
        state.notify(LogLevel::ok, message.str(), output.parent_path());
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Scene export failed: ") + error.what());
    }
}

void export_selected_clump_gltf(AppState& state, const OutputMode mode) {
    const auto* clump = selected_clump(state);
    if (!clump) return;
    try {
        std::ostringstream suffix;
        suffix << ".clump_0x" << std::hex << std::uppercase << clump->offset << ".gltf";
        const auto desired = with_stem_suffix(state.document->source_path(), suffix.str(), false);
        const auto chosen = choose_output(state, desired, mode, [&state](OutputMode m) { export_selected_clump_gltf(state, m); });
        if (!chosen) return;
        const auto& output = *chosen;
        const auto stats = rws::export_clump_gltf(*clump, state.document->bytes(), output);
        state.notify(LogLevel::ok, "Exported Clump with " + std::to_string(stats.atomic_instances) +
                                       " Atomics to " + path_utf8(output),
                     output.parent_path());
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Clump export failed: ") + error.what());
    }
}

void export_collision_gltf(AppState& state, const OutputMode mode) {
    const auto* source = collision_export_document(state);
    if (!source) return;
    try {
        const auto desired = with_stem_suffix(source->source_path(), ".collision.gltf", false);
        const auto chosen = choose_output(state, desired, mode, [&state](OutputMode m) { export_collision_gltf(state, m); });
        if (!chosen) return;
        const auto& output = *chosen;
        const auto stats = rws::export_collision_gltf(source->chunks(), source->bytes(), output);
        state.notify(LogLevel::ok, "Exported collision glTF: " + std::to_string(stats.triangles) +
                                       " triangles to " + path_utf8(output),
                     output.parent_path());
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Collision export failed: ") + error.what());
    }
}

void export_collision_obj(AppState& state, const OutputMode mode) {
    const auto* source = collision_export_document(state);
    if (!source) return;
    try {
        const auto desired = with_stem_suffix(source->source_path(), ".collision.obj", false);
        const auto chosen = choose_output(state, desired, mode, [&state](OutputMode m) { export_collision_obj(state, m); });
        if (!chosen) return;
        const auto& output = *chosen;
        const auto stats = rws::export_collision_obj(source->chunks(), source->bytes(), output);
        state.notify(LogLevel::ok, "Exported collision OBJ: " + std::to_string(stats.triangles) +
                                       " triangles to " + path_utf8(output),
                     output.parent_path());
    } catch (const std::exception& error) {
        state.notify(LogLevel::error, std::string("Collision export failed: ") + error.what());
    }
}

} // namespace rwsman
