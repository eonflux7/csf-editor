#pragma once

#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
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

// The stance a class's weapons (Armas.bdd .TIPO) put it in, which picks its
// clip library: the first weapon held like a rifle (long guns, the MG: our
// reading), a submachine gun ("smg") or a pistol, or empty hands ("unarmed":
// Desarmado, MANO). Grenades, binoculars and other items are passed over; a
// class with none of these has no stance (empty).
[[nodiscard]] std::string godot_stance(const ObjectDefinition& definition, const WeaponDatabase& weapons);

// markers.json (format `opencsf-markers`): the scene's actors, navigation
// groups as routes, areas and dummies in metres on the glTF axes, angles in
// radians. Each actor carries its class's actor kind (.TIPO, lower case), its
// faction, `asset`, the logical ID `class_assets` gives its class, and
// `stance` (rifle, smg, pistol, unarmed) and `weapon` (the logical ID of the
// model in its hands), which `class_stances` and `class_weapons` give it.
// docs/guides/cli.md describes the fields.
[[nodiscard]] std::string godot_markers_json(const MissionScene& scene, const ObjectDatabase& objects,
                                             const std::map<std::int32_t, std::string>& class_assets,
                                             const std::map<std::int32_t, std::string>& class_stances = {},
                                             const std::map<std::int32_t, std::string>& class_weapons = {});

} // namespace csf
