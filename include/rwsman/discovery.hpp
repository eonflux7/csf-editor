#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace rwsman {

struct DiscoveredMission {
    std::filesystem::path path; // The .scn file.
    std::string package;        // Directory that contains "Maps" (a package root).
    std::string map;            // Directory under Maps.
    std::string name;           // File stem.
    [[nodiscard]] friend bool operator==(const DiscoveredMission&, const DiscoveredMission&) = default;
};

// Lists mission scenes under a resource root, without walking the whole tree:
// `root/Maps/<map>/*.scn` and `root/<package>/Maps/<map>/*.scn`. Results are
// sorted by package, map, and name (case-insensitive). Unreadable directories are
// skipped, and at most `limit` missions are returned.
[[nodiscard]] std::vector<DiscoveredMission> discover_missions(const std::filesystem::path& root,
                                                               std::size_t limit = 5000);

} // namespace rwsman
