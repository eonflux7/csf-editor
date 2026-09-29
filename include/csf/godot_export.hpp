#pragma once

#include "csf/mission_scene.hpp"
#include "csf/object_database.hpp"
#include "csf/program.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Shipped missions converted for a game engine that reads only glTF and JSON
// (the Godot import plan): each map's visual and collision glTF, one model per
// actor class, each mission's markers, and manifest.json, which maps logical
// IDs (map/ambush, character/alofic, prop/bidon) to those files. Several
// missions share one folder: models, sounds and weapons are written once, and
// the clip libraries hold every mission's clips.
// GUI-free; `csf-mod export-godot` runs it. Re-running it on the same input
// writes byte-identical files.
namespace csf {

struct GodotExportOptions {
    std::filesystem::path corpus;  // the unpacked missions (CSF_unpacks)
    std::vector<std::string> missions;  // missions of mission_slots(corpus), any case
    // Written into; it must not exist, be empty, or hold an earlier export
    // (manifest.json of this format).
    std::filesystem::path out;
};

struct GodotExportResult {
    std::vector<std::string> lines;     // what was written, one tab-separated line each
    std::vector<std::string> problems;  // what was skipped, and why
};

// Throws std::runtime_error when a mission or its map cannot be found or read.
// With several missions, each problem starts with its mission's name.
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
// The map's own effects (.EFECTOS: fires, smoke, glows at dummies) name
// theirs by the logical ID `effect_assets` gives it.
// docs/guides/cli.md describes the fields.
[[nodiscard]] std::string godot_markers_json(const MissionScene& scene, const ObjectDatabase& objects,
                                             const std::map<std::int32_t, std::string>& class_assets,
                                             const std::map<std::int32_t, std::string>& class_stances = {},
                                             const std::map<std::int32_t, std::string>& class_weapons = {},
                                             const std::map<std::int32_t, std::string>& effect_assets = {});

// What missions/<Mission>.json is made from: the scene, its mission (.gsc) and
// cutscene (.csc) programs, and the names the scripts' numbers stand for.
struct GodotMissionSources {
    const MissionScene* scene{};
    const ProgramDocument* mission{};
    const ProgramDocument* cutscene{};
    std::map<std::string, std::string> texts;         // Texts/<Mission>.fli: ID as written ("0012") -> UTF-8
    std::map<std::string, std::string> global_texts;  // Texts/Global.fli ("G017"), which scripts name as g017
    std::map<std::int32_t, std::string> sounds;       // Sonidos.bdd ID -> sound/<name>
    std::map<std::int32_t, std::string> animations;   // Anims.bdd ID -> record name
    std::map<std::string, std::vector<std::string>> animation_roles;  // lower-case record name -> "<library>/<role>"
    std::map<std::int32_t, std::string> classes;      // Objetos.bdd class -> name
    std::map<std::int32_t, std::string> weapons;      // Armas.bdd ID -> name
    std::map<std::int32_t, std::string> effects;      // Efectos.bdd ID -> effect/<name>
};

// missions/<Mission>.json (format `opencsf-mission`): what the mission's data
// says, for logic written by hand: texts, objectives, global variables, events,
// every script with its body as readable text (numbers that name something
// followed by that name), the names behind those numbers, what the scene sets
// per actor at load (its player, attached scripts, portraits) and the flow's
// findings. It interprets nothing. docs/guides/cli.md describes the fields.
[[nodiscard]] std::string godot_mission_json(const std::string& mission, const GodotMissionSources& sources);

} // namespace csf
