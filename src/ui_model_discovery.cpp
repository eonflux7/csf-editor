#include "rwsman/discovery.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <tuple>

namespace rwsman {
namespace {

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string name_of(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool is_directory(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    return entry.is_directory(error);
}

void scan_maps(const std::filesystem::path& maps, const std::string& package,
               std::vector<DiscoveredMission>& out, const std::size_t limit) {
    std::error_code error;
    for (std::filesystem::directory_iterator map(maps, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && map != end && out.size() < limit; map.increment(error)) {
        if (!is_directory(*map)) continue;
        std::error_code inner;
        for (std::filesystem::directory_iterator file(map->path(), std::filesystem::directory_options::skip_permission_denied, inner);
             !inner && file != end && out.size() < limit; file.increment(inner)) {
            std::error_code type_error;
            if (!file->is_regular_file(type_error)) continue;
            if (lower(file->path().extension().string()) != ".scn") continue;
            out.push_back({file->path(), package, name_of(map->path().filename()), name_of(file->path().stem())});
        }
    }
}

} // namespace

std::vector<DiscoveredMission> discover_missions(const std::filesystem::path& root, const std::size_t limit) {
    std::vector<DiscoveredMission> result;
    std::error_code error;
    if (root.empty() || !std::filesystem::is_directory(root, error)) return result;
    // The root itself may be a package (root/Maps) or a folder of packages.
    const auto direct = root / "Maps";
    if (std::filesystem::is_directory(direct, error)) scan_maps(direct, name_of(root.filename()), result, limit);
    for (std::filesystem::directory_iterator child(root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && child != end && result.size() < limit; child.increment(error)) {
        if (!is_directory(*child)) continue;
        const auto maps = child->path() / "Maps";
        std::error_code maps_error;
        if (std::filesystem::is_directory(maps, maps_error))
            scan_maps(maps, name_of(child->path().filename()), result, limit);
    }
    std::ranges::sort(result, [](const DiscoveredMission& a, const DiscoveredMission& b) {
        return std::tuple(lower(a.package), lower(a.map), lower(a.name)) <
               std::tuple(lower(b.package), lower(b.map), lower(b.name));
    });
    return result;
}

} // namespace rwsman
