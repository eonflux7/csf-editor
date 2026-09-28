#pragma once

#include <filesystem>
#include <string>
#include <vector>

// One shipped mission converted for a game engine that reads only glTF and
// JSON (the Godot import plan): the map's visual and collision glTF, one model
// per actor class, the mission's markers, and manifest.json, which maps
// logical IDs (map/ambush, character/alofic, prop/bidon) to those files.
// GUI-free; `csf-mod export-godot` runs it. Re-running it on the same input
// writes byte-identical files.
namespace csf {

struct GodotExportOptions {
    std::filesystem::path corpus;  // the unpacked missions (CSF_unpacks)
    std::string mission;           // a mission of mission_slots(corpus), any case
    // Written into; it must not exist, be empty, or hold an earlier export
    // (manifest.json of this format).
    std::filesystem::path out;
};

struct GodotExportResult {
    std::vector<std::string> lines;     // what was written, one tab-separated line each
    std::vector<std::string> problems;  // what was skipped, and why
};

// Throws std::runtime_error when the mission or its map cannot be found or read.
GodotExportResult export_godot(const GodotExportOptions& options);

} // namespace csf
