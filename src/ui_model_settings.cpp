#include "rwsman/settings.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace rwsman {
namespace {

std::string path_text(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path path_from_text(const std::string_view text) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::string escape(const std::string_view value) {
    std::string result;
    for (const char c : value) {
        if (c == '\\')
            result += "\\\\";
        else if (c == '\n')
            result += "\\n";
        else if (c == '\r')
            result += "\\r";
        else if (c == '\t')
            result += "\\t";
        else
            result += c;
    }
    return result;
}

std::string unescape(const std::string_view value) {
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 == value.size()) {
            result += value[i];
            continue;
        }
        const char next = value[++i];
        result += next == 'n' ? '\n' : next == 'r' ? '\r' : next == 't' ? '\t' : next;
    }
    return result;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// Splits on unescaped tabs. Escaped values never contain a raw tab.
std::vector<std::string_view> split_tabs(const std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto tab = text.find('\t', start);
        parts.push_back(text.substr(start, tab == std::string_view::npos ? tab : tab - start));
        if (tab == std::string_view::npos) break;
        start = tab + 1;
    }
    return parts;
}

bool parse_bool(const std::string_view text, bool& out) {
    if (text == "true" || text == "1") return out = true, true;
    if (text == "false" || text == "0") return out = false, true;
    return false;
}

bool parse_float(const std::string_view text, float& out) {
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const float value = std::strtof(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

bool parse_int(const std::string_view text, int& out) {
    int value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return false;
    out = value;
    return true;
}

std::string format_float(const float value) {
    std::ostringstream stream;
    stream.precision(9);
    stream << value;
    return stream.str();
}

} // namespace

std::filesystem::path config_directory(const std::function<const char*(const char*)>& getenv_fn) {
    const auto env = [&](const char* name) -> const char* {
        return getenv_fn ? getenv_fn(name) : std::getenv(name);
    };
    const auto non_empty = [](const char* value) { return value && *value; };
#ifdef _WIN32
    if (const char* local = env("LOCALAPPDATA"); non_empty(local))
        return std::filesystem::path(local) / "CSF RWS Tools";
    return {};
#else
    if (const char* xdg = env("XDG_CONFIG_HOME"); non_empty(xdg))
        return std::filesystem::path(xdg) / "csf-rws-tools";
    if (const char* home = env("HOME"); non_empty(home))
        return std::filesystem::path(home) / ".config" / "csf-rws-tools";
    return {};
#endif
}

void Settings::clamp() noexcept {
    if (!std::isfinite(ui_scale)) ui_scale = 1.0F;
    ui_scale = std::clamp(ui_scale, 0.8F, 2.0F);
    if (!std::isfinite(move_speed) || move_speed <= 0.0F) move_speed = 1.0F;
    move_speed = std::clamp(move_speed, 0.05F, 50.0F);
    const auto clamp_fps = [](int& value) { value = value <= 0 ? 0 : std::clamp(value, 5, 1000); };
    clamp_fps(fps_limit);
    clamp_fps(background_fps_limit);
    overlays.clamp();
    if (recent_files.size() > max_recent_files) recent_files.resize(max_recent_files);
    if (recent_pairings.size() > max_recent_pairings) recent_pairings.resize(max_recent_pairings);
}

void Settings::add_recent_file(const std::filesystem::path& path, const bool mission) {
    const RecentFile entry{path, mission};
    std::erase_if(recent_files, [&](const RecentFile& value) { return value.path == path; });
    recent_files.insert(recent_files.begin(), entry);
    if (recent_files.size() > max_recent_files) recent_files.resize(max_recent_files);
}

void Settings::add_recent_pairing(const std::filesystem::path& main,
                                  const std::filesystem::path& collision) {
    std::erase_if(recent_pairings, [&](const RecentPairing& value) {
        return value.main == main && value.collision == collision;
    });
    recent_pairings.insert(recent_pairings.begin(), {main, collision});
    if (recent_pairings.size() > max_recent_pairings) recent_pairings.resize(max_recent_pairings);
}

void Settings::set_bookmark(const std::string& signature, const CameraBookmark& bookmark) {
    auto& list = bookmarks[signature];
    std::erase_if(list, [&](const CameraBookmark& value) { return value.slot == bookmark.slot; });
    list.push_back(bookmark);
    std::ranges::sort(list, {}, &CameraBookmark::slot);
}

const CameraBookmark* Settings::bookmark(const std::string& signature, const int slot) const {
    const auto found = bookmarks.find(signature);
    if (found == bookmarks.end()) return nullptr;
    const auto entry = std::ranges::find(found->second, slot, &CameraBookmark::slot);
    return entry == found->second.end() ? nullptr : &*entry;
}

std::string serialize_settings(const Settings& settings) {
    std::ostringstream out;
    out << "# CSF RWS Tools settings. Safe to edit; unknown lines are ignored.\n";
    out << "version = " << Settings::current_version << '\n';
    out << "resource_root = " << escape(path_text(settings.resource_root)) << '\n';
    out << "game_root = " << escape(path_text(settings.game_root)) << '\n';
    out << "projects_root = " << escape(path_text(settings.projects_root)) << '\n';
    out << "ui_scale = " << format_float(settings.ui_scale) << '\n';
    out << "theme = " << escape(settings.theme) << '\n';
    out << "workspace = " << escape(settings.workspace) << '\n';
    out << "show_explorer = " << (settings.show_explorer ? "true" : "false") << '\n';
    out << "show_inspector = " << (settings.show_inspector ? "true" : "false") << '\n';
    out << "show_bottom_dock = " << (settings.show_bottom_dock ? "true" : "false") << '\n';
    out << "show_hud = " << (settings.show_hud ? "true" : "false") << '\n';
    out << "show_clump_colors = " << (settings.show_clump_colors ? "true" : "false") << '\n';
    out << "move_speed = " << format_float(settings.move_speed) << '\n';
    out << "invert_y = " << (settings.invert_y ? "true" : "false") << '\n';
    out << "default_view_style = " << settings.default_view_style << '\n';
    out << "idle_redraw = " << (settings.idle_redraw ? "true" : "false") << '\n';
    out << "fps_limit = " << settings.fps_limit << '\n';
    out << "background_fps_limit = " << settings.background_fps_limit << '\n';
    out << "show_frame_stats = " << (settings.show_frame_stats ? "true" : "false") << '\n';
    const auto& overlays = settings.overlays;
    out << "overlay_labels = " << overlay_labels_name(overlays.labels) << '\n';
    out << "overlay_headings = " << overlay_detail_name(overlays.headings) << '\n';
    out << "overlay_details = " << overlay_detail_name(overlays.details) << '\n';
    out << "overlay_occluded_opacity = " << format_float(overlays.occluded_opacity) << '\n';
    out << "overlay_fade_distance = " << format_float(overlays.fade_distance) << '\n';
    out << "overlay_merge_pixels = " << format_float(overlays.merge_pixels) << '\n';
    out << "overlay_icon_limit = " << overlays.icon_limit << '\n';
    out << "overlay_legend = " << (overlays.show_legend ? "true" : "false") << '\n';
    out << "overlay_minimap = " << (overlays.show_minimap ? "true" : "false") << '\n';
    out << "overlay_dim_filtered = " << (overlays.dim_filtered ? "true" : "false") << '\n';
    out << "overlay_hidden =";
    for (std::size_t i = 0; i < overlays.hidden_layers.size(); ++i)
        out << (i == 0 ? " " : "\t") << escape(overlays.hidden_layers[i]);
    out << '\n';
    for (const auto& preset : overlays.presets) {
        out << "overlay_preset = " << escape(preset.name);
        for (const auto& layer : preset.hidden_layers) out << '\t' << escape(layer);
        out << '\n';
    }
    out << "export_policy = "
        << (settings.export_policy == ExportPolicy::new_files_only ? "new_files_only"
                                                                   : "confirm_overwrite")
        << '\n';
    for (const auto& file : settings.recent_files)
        out << "recent_file = " << (file.mission ? "mission" : "file") << '\t'
            << escape(path_text(file.path)) << '\n';
    for (const auto& pair : settings.recent_pairings)
        out << "recent_pairing = " << escape(path_text(pair.main)) << '\t'
            << escape(path_text(pair.collision)) << '\n';
    for (const auto& [signature, list] : settings.bookmarks)
        for (const auto& bookmark : list) {
            const auto& c = bookmark.camera;
            out << "bookmark = " << escape(signature) << '\t' << bookmark.slot << '\t'
                << format_float(c.yaw) << '\t' << format_float(c.pitch) << '\t'
                << format_float(c.distance) << '\t' << format_float(c.orthographic_scale) << '\t'
                << format_float(c.pan_x) << '\t' << format_float(c.pan_y) << '\t'
                << format_float(c.navigation[0]) << '\t' << format_float(c.navigation[1]) << '\t'
                << format_float(c.navigation[2]) << '\t' << format_float(c.center[0]) << '\t'
                << format_float(c.center[1]) << '\t' << format_float(c.center[2]) << '\t'
                << format_float(c.radius) << '\t' << c.projection << '\n';
        }
    return out.str();
}

SettingsLoad parse_settings(const std::string_view text) {
    SettingsLoad result;
    result.existed = true;
    auto& s = result.settings;
    std::size_t line_number = 0, position = 0;
    const auto warn = [&](const std::string& message) {
        if (result.warnings.size() < 20)
            result.warnings.push_back("settings line " + std::to_string(line_number) + ": " +
                                      message);
    };
    // A file that contains NUL bytes is not a settings file at all.
    if (text.find('\0') != std::string_view::npos) {
        result.warnings.push_back("settings file is not text; using defaults");
        return result;
    }
    while (position <= text.size()) {
        const auto newline = text.find('\n', position);
        const auto raw = text.substr(
            position, newline == std::string_view::npos ? std::string_view::npos : newline - position);
        position = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        ++line_number;
        const auto line = trim(raw);
        if (line.empty() || line.front() == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) {
            warn("expected `key = value`");
            continue;
        }
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        bool ok = true;
        if (key == "version") {
            int version{};
            if (!parse_int(value, version))
                ok = false;
            else if (version > Settings::current_version)
                warn("written by a newer version; unknown settings are ignored");
        } else if (key == "resource_root") {
            s.resource_root = path_from_text(unescape(value));
        } else if (key == "game_root") {
            s.game_root = path_from_text(unescape(value));
        } else if (key == "projects_root") {
            s.projects_root = path_from_text(unescape(value));
        } else if (key == "ui_scale") {
            ok = parse_float(value, s.ui_scale);
        } else if (key == "theme") {
            s.theme = unescape(value);
        } else if (key == "workspace") {
            s.workspace = unescape(value);
        } else if (key == "show_explorer") {
            ok = parse_bool(value, s.show_explorer);
        } else if (key == "show_inspector") {
            ok = parse_bool(value, s.show_inspector);
        } else if (key == "show_bottom_dock") {
            ok = parse_bool(value, s.show_bottom_dock);
        } else if (key == "show_hud") {
            ok = parse_bool(value, s.show_hud);
        } else if (key == "show_clump_colors") {
            ok = parse_bool(value, s.show_clump_colors);
        } else if (key == "move_speed") {
            ok = parse_float(value, s.move_speed);
        } else if (key == "invert_y") {
            ok = parse_bool(value, s.invert_y);
        } else if (key == "default_view_style") {
            ok = parse_int(value, s.default_view_style);
        } else if (key == "idle_redraw") {
            ok = parse_bool(value, s.idle_redraw);
        } else if (key == "fps_limit") {
            ok = parse_int(value, s.fps_limit);
        } else if (key == "background_fps_limit") {
            ok = parse_int(value, s.background_fps_limit);
        } else if (key == "show_frame_stats") {
            ok = parse_bool(value, s.show_frame_stats);
        } else if (key == "overlay_labels") {
            const auto parsed = parse_overlay_labels(value);
            if (parsed) s.overlays.labels = *parsed;
            ok = parsed.has_value();
        } else if (key == "overlay_headings" || key == "overlay_details") {
            const auto parsed = parse_overlay_detail(value);
            if (parsed) (key == "overlay_headings" ? s.overlays.headings : s.overlays.details) = *parsed;
            ok = parsed.has_value();
        } else if (key == "overlay_occluded_opacity") {
            ok = parse_float(value, s.overlays.occluded_opacity);
        } else if (key == "overlay_fade_distance") {
            ok = parse_float(value, s.overlays.fade_distance);
        } else if (key == "overlay_merge_pixels") {
            ok = parse_float(value, s.overlays.merge_pixels);
        } else if (key == "overlay_icon_limit") {
            ok = parse_int(value, s.overlays.icon_limit);
        } else if (key == "overlay_legend") {
            ok = parse_bool(value, s.overlays.show_legend);
        } else if (key == "overlay_minimap") {
            ok = parse_bool(value, s.overlays.show_minimap);
        } else if (key == "overlay_dim_filtered") {
            ok = parse_bool(value, s.overlays.dim_filtered);
        } else if (key == "overlay_hidden") {
            s.overlays.hidden_layers.clear();
            if (!value.empty())
                for (const auto part : split_tabs(value))
                    if (!part.empty()) s.overlays.hidden_layers.push_back(unescape(part));
        } else if (key == "overlay_preset") {
            const auto parts = split_tabs(value);
            ok = !parts.empty() && !parts[0].empty();
            if (ok) {
                OverlayPreset preset{unescape(parts[0]), {}};
                for (std::size_t i = 1; i < parts.size(); ++i)
                    if (!parts[i].empty()) preset.hidden_layers.push_back(unescape(parts[i]));
                std::erase_if(s.overlays.presets,
                              [&](const OverlayPreset& other) { return other.name == preset.name; });
                s.overlays.presets.push_back(std::move(preset));
            }
        } else if (key == "export_policy") {
            if (value == "new_files_only")
                s.export_policy = ExportPolicy::new_files_only;
            else if (value == "confirm_overwrite")
                s.export_policy = ExportPolicy::confirm_overwrite;
            else
                ok = false;
        } else if (key == "recent_file") {
            const auto parts = split_tabs(value);
            ok = parts.size() == 2 && (parts[0] == "mission" || parts[0] == "file") &&
                 !parts[1].empty();
            if (ok)
                s.recent_files.push_back({path_from_text(unescape(parts[1])), parts[0] == "mission"});
        } else if (key == "recent_pairing") {
            const auto parts = split_tabs(value);
            ok = parts.size() == 2 && !parts[0].empty() && !parts[1].empty();
            if (ok)
                s.recent_pairings.push_back(
                    {path_from_text(unescape(parts[0])), path_from_text(unescape(parts[1]))});
        } else if (key == "bookmark") {
            const auto parts = split_tabs(value);
            CameraBookmark bookmark;
            auto& c = bookmark.camera;
            ok = parts.size() == 16 && parse_int(parts[1], bookmark.slot) && bookmark.slot >= 1 &&
                 bookmark.slot <= 9 && parse_float(parts[2], c.yaw) &&
                 parse_float(parts[3], c.pitch) && parse_float(parts[4], c.distance) &&
                 parse_float(parts[5], c.orthographic_scale) && parse_float(parts[6], c.pan_x) &&
                 parse_float(parts[7], c.pan_y) && parse_float(parts[8], c.navigation[0]) &&
                 parse_float(parts[9], c.navigation[1]) && parse_float(parts[10], c.navigation[2]) &&
                 parse_float(parts[11], c.center[0]) && parse_float(parts[12], c.center[1]) &&
                 parse_float(parts[13], c.center[2]) && parse_float(parts[14], c.radius) &&
                 parse_int(parts[15], c.projection);
            if (ok) {
                c.valid = true;
                s.set_bookmark(unescape(parts[0]), bookmark);
            }
        } else {
            continue; // Unknown keys are forward-compatible, not warnings.
        }
        if (!ok) warn("invalid value for `" + std::string(key) + "`");
    }
    s.clamp();
    return result;
}

SettingsLoad load_settings(const std::filesystem::path& path) {
    std::error_code error;
    if (path.empty() || !std::filesystem::is_regular_file(path, error)) return {};
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        SettingsLoad result;
        result.existed = true;
        result.warnings.push_back("could not read " + path_text(path) + "; using defaults");
        return result;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parse_settings(buffer.str());
}

std::string save_settings(const std::filesystem::path& path, const Settings& settings) {
    if (path.empty()) return "no settings directory is available";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return "could not create " + path_text(path.parent_path()) + ": " + error.message();
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << serialize_settings(settings);
        if (!output) return "could not write " + path_text(temporary);
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return "could not replace " + path_text(path);
    }
    return {};
}

} // namespace rwsman
