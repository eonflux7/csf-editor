# Command-line usage

All console tools are read-only unless a command explicitly names an output
path, and no tool writes beside the file it reads.

## Inspect a CSFFBS document

`csf-info` selects the parser by the `CSFFBS` magic rather than the filename
extension. It is read-only and returns a nonzero status for structural errors.

Windows:

```powershell
$document = "C:\path\to\mission.scn"
.\build\Release\csf-info.exe $document --summary
.\build\Release\csf-info.exe $document --validate
.\build\Release\csf-info.exe $document --tree
.\build\Release\csf-info.exe $document --strings
.\build\Release\csf-info.exe $document --find RUTA_Garita
.\build\Release\csf-info.exe $document --export-text "C:\path\to\new-output.scn.txt"
.\build\Release\csf-info.exe $document --export-json "C:\path\to\new-output.scn.json"
.\build\Release\csf-info.exe corpus "C:\path\to\extracted-game"
```

Linux:

```bash
document="/path/to/mission.scn"
./build/Release/csf-info "$document" --summary
./build/Release/csf-info "$document" --validate
./build/Release/csf-info "$document" --tree
./build/Release/csf-info "$document" --strings
./build/Release/csf-info "$document" --find RUTA_Garita
./build/Release/csf-info "$document" --export-text "/path/to/new-output.scn.txt"
./build/Release/csf-info "$document" --export-json "/path/to/new-output.scn.json"
./build/Release/csf-info corpus "/path/to/extracted-game"
```

Use `-` as an export destination to write the inspection format to standard
output. File exports refuse the input path and any destination that already
exists. The recursive corpus report sniffs every regular file by magic and emits
tab-separated per-file and aggregate records; it never modifies the corpus.

## Resolve a mission package

Mission mode builds a read-only dependency graph from an SCN path or a mission
directory. Package-local paths take precedence, matching is Windows-like and
case-insensitive, and every mapped, missing, ambiguous, or case-mismatched edge
retains its source file and byte offset.

Windows:

```powershell
$scene = "C:\path\to\extracted-game\Ambush\Maps\ST08\Ambush.scn"
$root = "C:\path\to\extracted-game"

.\build\Release\csf-info.exe mission $scene --summary
.\build\Release\csf-info.exe mission $scene --dependencies
.\build\Release\csf-info.exe mission $scene --missing
.\build\Release\csf-info.exe mission $scene --graph "C:\path\to\new-mission-graph.json"
.\build\Release\csf-info.exe mission $scene --objects
.\build\Release\csf-info.exe mission $scene --navigation
.\build\Release\csf-info.exe mission $scene --spatial
.\build\Release\csf-info.exe mission $scene --symbols class:55
.\build\Release\csf-info.exe mission $scene --scene-json "C:\path\to\new-mission-scene.json"
.\build\Release\csf-info.exe mission $scene --summary --root $root
.\build\Release\csf-info.exe mission $scene --summary --package-root "C:\path\to\package"
.\build\Release\csf-info.exe mission $scene --summary --duplicates
.\build\Release\csf-info.exe animations "C:\path\to\BDD\Anims.bdd" --root $root
.\build\Release\csf-info.exe script-animations "C:\path\to\mission.gsc"
.\build\Release\csf-info.exe cutscene "C:\path\to\mission.csc"
.\build\Release\csf-info.exe uses "Ambush\Models\Char\Espia.rpc" --root $root
.\build\Release\csf-info.exe compare $scene "C:\path\to\another.scn"
```

Linux:

```bash
scene="/path/to/extracted-game/Ambush/Maps/ST08/Ambush.scn"
root="/path/to/extracted-game"

./build/Release/csf-info mission "$scene" --summary
./build/Release/csf-info mission "$scene" --dependencies
./build/Release/csf-info mission "$scene" --missing
./build/Release/csf-info mission "$scene" --graph "/path/to/new-mission-graph.json"
./build/Release/csf-info mission "$scene" --objects
./build/Release/csf-info mission "$scene" --navigation
./build/Release/csf-info mission "$scene" --spatial
./build/Release/csf-info mission "$scene" --symbols class:55
./build/Release/csf-info mission "$scene" --scene-json "/path/to/new-mission-scene.json"
./build/Release/csf-info mission "$scene" --summary --root "$root"
./build/Release/csf-info mission "$scene" --summary --package-root "/path/to/package"
./build/Release/csf-info mission "$scene" --summary --duplicates
./build/Release/csf-info animations "/path/to/BDD/Anims.bdd" --root "$root"
./build/Release/csf-info script-animations "/path/to/mission.gsc"
./build/Release/csf-info cutscene "/path/to/mission.csc"
./build/Release/csf-info uses "Ambush/Models/Char/Espia.rpc" --root "$root"
./build/Release/csf-info compare "$scene" "/path/to/another.scn"
```

Graph export refuses an existing destination. VIS, TXL, M3D, AND, and the
validated path portions of PHD have bounded adapters; unparsed bytes and partial
PHD schema status remain explicit. See [Mission package
resolution](../game-knowledge/mission-resolution.md) for the resolution order, graph
schema, and current format evidence. Typed SCN identities, validation, reference
categories, and scene JSON are documented in [Mission Explorer typed-view
contract](../game-knowledge/mission-explorer.md).

Actor class/model/CMO/Physics joins, standalone RPC inspection, lossless CMO
parsing, and ragdoll uncertainty rules are documented in [Actor and Physics
inspection](../game-knowledge/actor-physics-inspection.md). Use
`csf-info mission Mission.scn --associations` for an evidence-backed association
report or `csf-info cmo file.cmo` for the source-backed collision-shape view.

## Inspect one file

Running `rws-info` with only a file prints its complete chunk tree:

Windows:

```powershell
.\build\Release\rws-info.exe "C:\path\to\asset.rws"
```

Linux:

```bash
./build/Release/rws-info "/path/to/asset.rws"
```

Additional modes are:

| Command | Result |
| --- | --- |
| `--summary` | Print counts, payload sizes, and truncation counts by chunk type |
| `--world-report` | Compare declared and recovered World sectors, triangles, vertices, materials, and diagnostics |
| `--world-report=sectors` | Include offsets, counts, material bases, and bounds for every recovered sector |
| `--bsp-report` | Report declared/recovered Plane and leaf totals, topology status, depth, and failure categories |
| `--bsp-report=nodes` | Also list validated topology nodes, parents, sides, bounds, and source identities |
| `--instances` | List decoded CSF placements and their correlated Clump prototypes |
| `--validate-types` | Decode every supported typed structure and return a nonzero exit code on failures |
| `--export-obj <directory>` | Export every decoded Geometry as a separate OBJ file |
| `--export-scene-gltf <file.gltf>` | Export the assembled scene and its manifest |
| `--export-collision-gltf <file.gltf>` | Export only validated World collision geometry to glTF |
| `--export-collision-obj <file.obj>` | Export only validated World collision geometry to OBJ/MTL |
| `--export-clump-gltf <offset> <file.gltf>` | Export the top-level Clump at a decimal or `0x` byte offset |
| `--animation-report[=frames]` | Report ANM variants, tracks, validation, and optionally every source-stable key |
| `--export-animation-gltf <model.rpc> <file.gltf>` | Export a compatible RPC hierarchy/skin and ANM channels |

Examples:

Windows:

```powershell
$asset = "C:\path\to\map.rws"
$output = "C:\path\to\exports"

.\build\Release\rws-info.exe $asset --summary
.\build\Release\rws-info.exe $asset --world-report
.\build\Release\rws-info.exe $asset --bsp-report
.\build\Release\rws-info.exe $asset --instances
.\build\Release\rws-info.exe $asset --validate-types
.\build\Release\rws-info.exe $asset --export-obj "$output\obj"
.\build\Release\rws-info.exe $asset --export-scene-gltf "$output\map.gltf"
.\build\Release\rws-info.exe $asset --export-collision-gltf "$output\map.collision.gltf"
.\build\Release\rws-info.exe $asset --export-collision-obj "$output\map.collision.obj"
.\build\Release\rws-info.exe $asset --export-clump-gltf 0x1234 "$output\clump.gltf"
```

Linux:

```bash
asset="/path/to/map.rws"
output="/path/to/exports"

./build/Release/rws-info "$asset" --summary
./build/Release/rws-info "$asset" --world-report
./build/Release/rws-info "$asset" --bsp-report
./build/Release/rws-info "$asset" --instances
./build/Release/rws-info "$asset" --validate-types
./build/Release/rws-info "$asset" --export-obj "$output/obj"
./build/Release/rws-info "$asset" --export-scene-gltf "$output/map.gltf"
./build/Release/rws-info "$asset" --export-collision-gltf "$output/map.collision.gltf"
./build/Release/rws-info "$asset" --export-collision-obj "$output/map.collision.obj"
./build/Release/rws-info "$asset" --export-clump-gltf 0x1234 "$output/clump.gltf"
```

See [Geometry and scene export](export.md) for output layout and file
contents.

## Scan an extracted corpus

`rws-corpus` recursively scans `.rws` and `.rpc` files and prints a tab-separated per-file
report including declared/recovered World totals and recovery status, followed by
root-format, chunk-type, diagnostic, scene-instance, and aggregate World-recovery totals:

Windows:

```powershell
.\build\Release\rws-corpus.exe "C:\path\to\extracted-game"
```

Linux:

```bash
./build/Release/rws-corpus "/path/to/extracted-game"
```

It is read-only: it does not modify the files it scans.

## Export a mission for a game engine

`csf-mod export-godot` converts one mission of an unpacked corpus into files a
game engine reads without knowing RenderWare: glTF for geometry and JSON for
everything else.

```bash
./build/Release/csf-mod export-godot ../CSF_unpacks Ambush ~/dev/godot-opencsf/legacy
```

It writes:

| Path | Contents |
|---|---|
| `manifest.json` | Logical IDs (`map/ambush`, `character/alofic`, `prop/bidon`) to files, with each one's corpus-relative source |
| `maps/<Mission>/visual.gltf` | The map, as `rws-info --export-scene-gltf` writes it (with its `.bin` and `.manifest.json`), with textures bound (below) |
| `maps/<Mission>/collision.gltf` | The collision World, as `--export-collision-gltf` writes it |
| `maps/<Mission>/sky.gltf` | The sky dome the mission's `.vis` names, when it has one: a sphere of about 42 m to draw around the camera |
| `maps/<Mission>/markers.json` | Actors (with the logical ID of their model), navigation groups as routes, their links, areas and dummies, in metres and radians (below) |
| `characters/<model>.gltf` | One model per actor class whose model lives under `Models/Char`: skinned, on a skeleton named and shaped for Godot's `SkeletonProfileHumanoid` (below) |
| `anims/<library>.gltf` | A clip library: the skeleton and its clips, no mesh. One per stance (`rifle`, `smg`, `pistol`: 55 roles each; `unarmed`: 14, the civilians'), `actions` (taken out, sitting, the MG post, car seats) and `custom` (the one-off scene clips of `Anims/Costum` under their own names: smoking, talking, repairing, ...) |
| `weapons/<model>.gltf` | The third-person model (`Armas.bdd` `.FILE2`) of each character class's weapon, as `weapon/<model>` |
| `props/<model>.gltf` | One model per other actor class |
| `textures/<corpus path>.png` | Every texture those files name, decoded from DDS; the path is the source's, in lower case (`textures/ambush/maps/st08/textures/sdet_01a.png`) |

In every glTF it writes, a material's base texture is its `baseColorTexture`
on `TEXCOORD_0`, with `alphaMode` `MASK` for textures whose transparent pixels
are almost all fully transparent and `BLEND` for the rest (glass). glTF has no
lightmap slot, so a lightmapped material names its lightmap PNG in the extra
`lightmap_uri` (relative to the `.gltf`), sampled with `TEXCOORD_1` and
multiplied with the base texture (at twice its value by default, as in the
editor's viewport). A texture is looked up
beside the file that names it, then in each `Textures` folder from there up to
the mission package, ignoring case; one found nowhere is a `problem` line.

Characters are written by `rws::export_character_gltf`: one node per Frame at
the bind pose the Skin plugin expects, the skinned Geometry with
`JOINTS_0`/`WEIGHTS_0`, and rigid parts (canteen, bag) under their bone. Every
shipped human is the same 44-joint 3ds Max Biped, so one table names the
joints with `SkeletonProfileHumanoid` names (`Hips`, `LeftUpperArm`, ...; the
rest keep `bone_<HAnim id>`) and moves three of them to the profile's parents
(Hips under Root, the clavicles under the Chest, the holster dummies under the
Hips); world poses and skinning are unchanged. Clips are sampled at 30 frames
a second through `evaluate_pose`, the viewport's evaluator.

Which clip plays which role is our own table in `src/csf_godot_export.cpp`
(the game chooses by slot in code not joined to data yet), read from the clip
names. Soldiers' clips are a weapon prefix (`SF` rifle, `SM` submachine gun,
`SP` pistol) and a shared suffix, so the three stances have the same roles:
`idle`, `idle_alert`, their `_fidget`s, `suspicious`; `walk` (and `_left`,
`_right`), `walk_alert` (and `_back`, `_left`, `_right`), `run` (and `_back`,
`_left`, `_right`, `_crouched`), `crouch`, `stand_up`, `crouch_idle`,
`crouch_walk` (and directions); `shoot`, `crouch_shoot`, `reload`,
`crouch_reload`, `throw_grenade`, `throw_grenade_b`; `hit`, `hit_alert`,
`hit_leg`, `crouch_hit`, `gassed`, `crouch_gassed`; `die`, `crouch_die`,
`run_die`, `die_blast`, `die_fire`, `die_gas`; and turns
(`[alert_|crouch_]turn_{left,right}_{90,180}`). The turns' letters were
measured, not guessed: `DG`/`AG`/`HG` start from the standing, alert and
crouched poses (by the hips' height), `A` turns right and `B` left (by the
hips' yaw). A class's stance comes from its weapons (`csf::godot_stance`:
the first `.ARMAS` entry whose `Armas.bdd` `.TIPO` is a rifle, SMG, pistol or
empty hands).

A `weapon/<model>` entry has the weapon's `stance` and two frames, as 12
numbers (basis columns x, y, z, then the origin) in the glTF's own space:
`grip`, the frame the mesh hangs on, and `muzzle`, the frame whose 3ds Max
user property is `tag=100`. On every shipped third-person weapon the grip's
X runs along the barrel with Z up, its origin where the hand closes, and the
muzzle is at the barrel's end. The weapon is held with its grip frame on the
character's joint `bone_50` (HAnim 50, a dummy under the right hand): this
was checked by eye against the alternative of hanging the Clump's root
there, with rifle, SMG and pistol in their idle, aim and shoot poses.

The manifest's `anim/<library>` entry has `roles`: each role's `loop`, its
`speed` in metres a second (`.VEL`; the clips stay in place), `clip` (the
original name) and `clips`. A role whose record lists several files has one
clip each, the first named as the role and the others `<role>_2`, `_3`, ...
(`die`, `die_2`), for the game to pick from. Each clip has its `source` and,
when the record times sounds to it, `sounds`: `[seconds, sound ID]`. What the
common sound `-2` is, is unknown: its times don't match the walk's foot
plants, and some fall after the clip's end.

`markers.json` (format `opencsf-markers`, version 1) uses the glTF files'
axes and metres, so a position is where the map shows it. Angles are
radians: `yaw` turns about +Y so that +Z, where every model faces, turns to
(sin yaw, 0, cos yaw), then `pitch` turns about X (Godot's default Euler
order: `rotation = (pitch, yaw, 0)`). It holds:

| Key | Contents |
|---|---|
| `actors` | `id`, `name`, `class`; `kind`, the class's actor kind (`.TIPO` in `Objetos.bdd`, lower case: `aleman`, `ruso`, `player`, `decorativo`, `camion`, `item_arma`, ...); `faction` (`german`, `allied`, `neutral`, `player`) from the actor's `.BANDO`, else from its kind (humanoid kinds only; `ruso` counts as Allied, our reading); `asset`, the logical ID of its class's model; `stance` for characters with one (`rifle`, `smg`, `pistol`, `unarmed`), naming their clip library; `weapon`, the logical ID of the weapon that gives it, when it has a model; `position` (its `.CELDA` navigation point when it has one, else `.POS`), `yaw`, `pitch` when not 0, and `point`, that `[group, point]` |
| `routes` | Each navigation group: `id`, `name`, `kind` (`.TIPO`: `path` 0, `patrol` 2, `cover` 3, `ladder` 4), `points` (`id`, `name` when set, `position`, `yaw`) and `links`, `[from point, to point]` pairs |
| `route_links` | Links between two groups' points: `[group, point, group, point]` |
| `areas` | `id`, `name`, floor polygon `points` and `height` |
| `dummies` | `id`, `name`, `position`, `yaw`, `pitch` when not 0 |

The output folder must be new, empty or an earlier export; the command refuses
any other folder. Re-running it on the same corpus writes byte-identical files.
An actor class without exactly one resolvable model, or a scene that cannot be
opened, is printed as a `problem` line and skipped; the map is still written.

## Build and rewrite Worlds

`csf-mod` writes map Worlds through `rws::WorldModel`. Each command writes new
files next to its output path (`<map>.rws` and `<map>_col.rws`) and refuses to
replace existing ones without `--overwrite`.

```bash
# Prove the writer: every World rewrites byte for byte; --rebuild also builds a
# new BSP over each World's triangles and checks it recovers as complete.
./build/Release/csf-mod world-audit ../CSF_unpacks --rebuild

# Rebuild one map's visual and collision Worlds (new BSP, sectors, plug-ins),
# keeping its Clumps and scene instances.
./build/Release/csf-mod world-rebuild ../CSF_unpacks/Convoy/Maps/FR03/FR03.rws out/FR03.rws

# Terrain from Blender: export .csfworld, then compile it with a donor map's
# materials (visual by texture name, collision by surface name).
blender --background --factory-startup --python tools/blender/make_test_terrain.py -- \
    out/terrain.csfworld --size 100 --hill-height 4
blender --background scene.blend --python tools/blender/export_csf_world.py -- out/scene.csfworld
./build/Release/csf-mod world-build out/terrain.csfworld \
    ../CSF_unpacks/Convoy/Maps/FR03/FR03.rws out/FR03.rws [--keep-props] \
    [--texture <name> <image.png|dds> <like-donor-texture>]... [--lightmap <name> <image.png|dds>]...

# A shipped map as .csfworld (visual and collision, materials, collision shades
# and surface colours),
# to edit in Blender and build again with the map as donor and --keep-props.
# --check builds it back and compares every triangle.
./build/Release/csf-mod world-source ../CSF_unpacks/Convoy/Maps/FR03/FR03.rws out/FR03.csfworld [--check]

# An authoring project (docs/reference/project-format.md): build its map,
# collision map and sector map into <project>/build/ when they are stale. A
# `warning` line names each lightmap much brighter than the slot map's own.
./build/Release/csf-mod project-build ~/dev/csf-mods/hello-world-project [--force]
# Placements and anchored actors off their height rules (> 1 cm), and the resnap.
./build/Release/csf-mod project-heights ~/dev/csf-mods/hello-world-project [--resnap]
# A mission built from operations and presets (include/csf/mission_ops.hpp),
# and how its scripts connect (events, objectives, findings).
./build/Release/csf-mod mission-ops out/mission ../CSF_unpacks/Convoy/Maps/FR03/Convoy.scn my.ops
# (behaviours take start=<event> to begin on a mission event instead of INIT;
# triggers take when=alerted, Convoy's camp alarm, and when=body-found watch=<actors>)
# The same with each recipe kept as an editable component (out/mission/components.csfops),
# then listed, checked (each regenerates identically), and one edited in place.
./build/Release/csf-mod mission-ops out/mission ../CSF_unpacks/Convoy/Maps/FR03/Convoy.scn my.ops --components
./build/Release/csf-mod mission-components out/mission ../CSF_unpacks/Convoy/Maps/FR03/Convoy.scn list
./build/Release/csf-mod mission-components out/mission ../CSF_unpacks/Convoy/Maps/FR03/Convoy.scn check
# (--ground <terrain.csfworld> before the action when a component is a walk grid)
./build/Release/csf-mod mission-components out/mission ../CSF_unpacks/Convoy/Maps/FR03/Convoy.scn set 6 1 pause=5
./build/Release/csf-mod mission-flow ../CSF_unpacks/Convoy/Maps/FR03/Convoy.scn --workspace out/mission

# A new authoring project in a shipped slot (a flat starter terrain and an emptied
# mission), its two archives into dist/<build-id>/, deploying them into a test
# install and rolling them back, and the playtest log.
./build/Release/csf-mod project-new ~/dev/csf-mods/checkpoint --slot Convoy --name Checkpoint \
    --projects-root ~/dev/csf-mods --test-install ~/dev/csf_game
./build/Release/csf-mod project-archives ~/dev/csf-mods/checkpoint   # --original-mission/--original-texts <pak>
./build/Release/csf-mod project-deploy ~/dev/csf-mods/checkpoint <build-id>
./build/Release/csf-mod project-rollback ~/dev/csf-mods/checkpoint <build-id>
./build/Release/csf-mod project-playtest ~/dev/csf-mods/checkpoint <build-id> worked "guards patrol"

# Pieces of another mission's map: a project.csfproj `donor fr01 Ransom Maps/FR01/FR01.rws
# Maps/FR01/FR01_col.rws` record, then `piece barn donor=fr01 lightmaps=EDIFICIO_3 <box> ...`
# keeps only the triangles lit by those lightmap groups; project-build copies the
# textures and baked lightmaps they name into build/ (see editor-project-format.md).
# `texture SANDBAG sources/textures/SANDBAG.png FFLRA11B` gives a model a texture of its own.
# project-lightmaps then lists and packages lightmaps and textures alike.

# Buildings and baked lightmaps of an authoring project.
./build/Release/csf-mod project-place ~/dev/csf-mods/hello-world-project hut-1 hut 3000 16.3 -2500 0 --ground 0
./build/Release/csf-mod project-lightmap ~/dev/csf-mods/hello-world-project HUT_Lm sources/lightmaps/HUT_Lm.png
./build/Release/csf-mod project-lightmaps ~/dev/csf-mods/hello-world-project   # list and package them

# What the Blender add-on loads as reference, and how it registers an asset.
./build/Release/csf-mod project-reference ~/dev/csf-mods/hello-world-project out/reference
./build/Release/csf-mod project-asset ~/dev/csf-mods/hello-world-project hut building \
    sources/buildings/hut.blend sources/buildings/hut.csfworld

# The mission's sector map (Maps/Secs/<Mission>.sec) from the same source's
# collision faces, and the ground height and normal under points.
./build/Release/csf-mod sector-build out/terrain.csfworld out/Convoy.sec
./build/Release/csf-mod world-ground out/terrain.csfworld 0 0 2500 -2000
```

`--keep-props` keeps the donor's Clumps and scene-instance records in the new
map. `--texture` and `--lightmap` add textures of the map's own: DXT1 DDS files
in `Textures/` next to the new map and a copy of the donor's `.txl` listing
them (see [blender-authoring.md](blender-authoring.md), "A map without a
project"). `.csfworld` is documented in `include/rws/world_source.hpp`; the exporter's
conventions (units, axes, custom properties) are in its module docstring.
