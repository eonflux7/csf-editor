#pragma once

#include "rwsman/history.hpp"
#include "rwsman/viewport_overlays.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rwsman {

// Per-user directory that holds settings.ini, the per-workspace layouts, and the
// debug log: %LOCALAPPDATA%\CSF RWS Tools on Windows, $XDG_CONFIG_HOME/csf-rws-tools
// (or ~/.config/csf-rws-tools) elsewhere. `getenv` is injectable for tests.
// Returns an empty path when no suitable base directory exists.
[[nodiscard]] std::filesystem::path
config_directory(const std::function<const char*(const char*)>& getenv_fn = {});

struct RecentFile {
    std::filesystem::path path;
    bool mission{};
    [[nodiscard]] friend bool operator==(const RecentFile&, const RecentFile&) = default;
};

struct RecentPairing {
    std::filesystem::path main, collision;
    [[nodiscard]] friend bool operator==(const RecentPairing&, const RecentPairing&) = default;
};

struct CameraBookmark {
    int slot{}; // 1..9
    CameraSnapshot camera;
    [[nodiscard]] friend bool operator==(const CameraBookmark&, const CameraBookmark&) = default;
};

enum class ExportPolicy {
    new_files_only,    // Default: never replace an existing file.
    confirm_overwrite, // Ask before replacing.
};

struct Settings {
    static constexpr int current_version = 1;
    static constexpr std::size_t max_recent_files = 12;
    static constexpr std::size_t max_recent_pairings = 8;

    std::filesystem::path resource_root;
    // Game installation (holds maps/<Mission>.pak) and mission-editing projects.
    // An empty projects_root means <config dir>/projects.
    std::filesystem::path game_root;
    std::filesystem::path projects_root;
    // The Blender executable (empty: `blender` from PATH), for Edit in Blender.
    std::filesystem::path blender;
    float ui_scale{1.0F}; // User override, clamped to 0.8..2.0.
    std::string theme{"dark"};
    std::string workspace{"scene"};
    std::vector<RecentFile> recent_files;
    std::vector<RecentPairing> recent_pairings;

    // Panels.
    bool show_explorer{true};
    bool show_inspector{true};
    bool show_bottom_dock{true};
    bool show_hud{true};
    // Show file offsets, entry numbers and provenance in Mission mode too
    // (they are always shown in Inspect mode).
    bool developer_details{};
    bool show_clump_colors{true};

    // Viewport defaults.
    float move_speed{1.0F};
    bool invert_y{};
    int default_view_style{};
    OverlayOptions overlays;

    // Frame pacing: skip redraws while nothing changes, and optional frame-rate caps
    // (0 = no cap beyond vsync). The background cap applies while unfocused.
    bool idle_redraw{true};
    int fps_limit{};
    int background_fps_limit{15};
    bool show_frame_stats{}; // CPU/GPU frame times and draw counts in the stats HUD.

    ExportPolicy export_policy{ExportPolicy::new_files_only};

    // Camera bookmarks keyed by a mission's content signature.
    std::map<std::string, std::vector<CameraBookmark>> bookmarks;

    [[nodiscard]] friend bool operator==(const Settings&, const Settings&) = default;

    void clamp() noexcept;
    // Moves `file` to the front of the recent list (deduplicated, bounded).
    void add_recent_file(const std::filesystem::path& path, bool mission);
    void add_recent_pairing(const std::filesystem::path& main, const std::filesystem::path& collision);
    void set_bookmark(const std::string& signature, const CameraBookmark& bookmark);
    [[nodiscard]] const CameraBookmark* bookmark(const std::string& signature, int slot) const;
};

struct SettingsLoad {
    Settings settings;
    // True when parsing found problems. The result still holds every value that
    // could be read; the rest are defaults.
    std::vector<std::string> warnings;
    bool existed{};
};

// Text form: `key = value` lines, `#` comments, values escape `\\`, `\n`, `\t`.
[[nodiscard]] std::string serialize_settings(const Settings& settings);
[[nodiscard]] SettingsLoad parse_settings(std::string_view text);
// A missing file is not an error: it yields defaults with existed == false.
[[nodiscard]] SettingsLoad load_settings(const std::filesystem::path& path);
// Writes through a temporary file and renames it into place. Returns an error
// message, or an empty string on success.
[[nodiscard]] std::string save_settings(const std::filesystem::path& path, const Settings& settings);

} // namespace rwsman
