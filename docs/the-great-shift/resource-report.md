# Commandos: Strike Force resource report

## Purpose and status

This report inventories the locally available unpacked *Commandos: Strike Force*
resources and identifies which formats should drive the project's expansion from
an RWS inspector to a mission workbench.

The scan was performed on 2026-09-19 against:

```text
E:\dev\re-csf\CSF_unpacks
```

All operations used for this report were read-only. No game resource was edited or
copied into the repository. Counts describe this corpus and must not be treated as
universal format guarantees.

## Corpus summary

- Missions/package roots: **18**, including `GlobalEK`.
- Files: **18,938**.
- Bytes: **2,556,531,118** (about 2.56 GB decimal).
- File extensions observed: **29**.
- CSFFBS documents found by magic rather than extension: **1,262**.
- RWS-family streams: 410 `.rws`, 1,576 `.rpc`, 8,689 `.anm`, and 17 `.wad`.

Most mission packages repeat global assets. “Distinct content” below is the count
of unique file hashes within an extension and is useful for estimating the real
reverse-engineering surface. It is not an assertion that same-named files with
different hashes have different semantics.

## Extension inventory

| Extension | Files | Bytes | Smallest | Largest | Distinct content | Primary role |
|---|---:|---:|---:|---:|---:|---|
| `.anm` | 8,689 | 187,436,596 | 144 | 321,608 | 1,294 | RenderWare skeletal animations |
| `.dds` | 4,721 | 808,582,112 | 256 | 1,398,256 | not scanned | Map/model textures and lightmaps |
| `.png` | 1,722 | 240,802,065 | 257 | 2,148,223 | not scanned | Model, effect, map, and UI textures |
| `.rpc` | 1,576 | 85,946,625 | 909 | 1,222,263 | 1,462 | RenderWare Clump models |
| `.sp` | 947 | 2,392,679 | 1,842 | 2,627 | not scanned | CSFFBS particle systems |
| `.rws` | 410 | 295,884,822 | 264 | 36,543,940 | not rescanned | Maps, collision, Physics bodies, ragdolls |
| `.cmo` | 242 | 686,425 | 820 | 5,297 | 57 | Text collision/hit-shape definitions |
| `.fbs` | 126 | 670,034 | 178 | 59,942 | not scanned | CSFFBS menu/UI forms and portraits |
| `.bdd` | 108 | 10,150,401 | 100 | 270,376 | 82 | CSFFBS gameplay databases |
| `.raw` | 73 | 1,262,522 | 6,083 | 49,160 | not scanned | Menu raster data |
| `.txt` | 39 | 836,796 | 503 | 44,261 | not scanned | Mixed CSFFBS and non-CSFFBS UI data |
| `.prp` | 28 | 6,227 | 222 | 224 | not scanned | Plain-text mission/player properties |
| `.fli` | 24 | 395,766 | 3,214 | 96,364 | not scanned | UTF-16 localized text in this game |
| `.vis` | 21 | 2,939 | 129 | 156 | 16 | Map package entry points |
| `.sec` | 21 | 2,714,288 | 1,084 | 371,960 | 15 | Binary spatial/sector data; semantics incomplete |
| `.scn` | 21 | 7,163,617 | 51,806 | 684,240 | 17 | CSFFBS mission scene data |
| `.csc` | 21 | 349,114 | 83 | 71,015 | 13 | CSFFBS cutscene scripts |
| `.gsc` | 21 | 12,004,841 | 64,550 | 953,175 | 17 | CSFFBS mission scripts |
| `.txl` | 17 | 204,289 | 2,056 | 17,337 | 17 | Plain-text texture dependency lists |
| `.and` | 17 | 264,765 | 7,887 | 18,744 | 17 | Binary animation dependency/index data |
| `.wad` | 17 | 898,283,426 | 19,391,106 | 71,350,676 | not scanned | RenderWare vendor-8/audio streams |
| `.phd` | 17 | 57,056 | 565 | 4,859 | 17 | Physics/render dependency table |
| `.m3d` | 17 | 51,030 | 844 | 4,076 | 17 | Model dependency table |
| `.dst` | 17 | 1,632 | 96 | 96 | not scanned | Plain-text distance-scale configuration |
| `.vsh` | 8 | 24,040 | 3,005 | 3,005 | not scanned | Plain-text DirectX vertex shader |
| `.psh` | 8 | 31,032 | 3,879 | 3,879 | not scanned | Plain-text DirectX pixel shader |
| `.fnt` | 5 | 68,573 | 13,675 | 13,774 | not scanned | `FontWapa` custom font data |
| `.wav` | 4 | 257,386 | 16,188 | 115,484 | not scanned | Standard RIFF/WAVE menu audio |
| `.sub` | 1 | 20 | 20 | 20 | not scanned | UTF-16 subtitle text |

The distinct-content scan used MD5 only as a fast duplicate classifier. It is not
used for security, identity, or file validation.

## Resource layout

The recurring directory/extension relationships are:

| Directory | Dominant content |
|---|---|
| `Anims` | 8,689 `.anm` clips |
| `Models` | 1,560 `.rpc`, 368 `.rws`, 242 `.cmo`, model textures |
| `Maps` | map/collision `.rws`, `.scn/.gsc/.csc`, indexes, textures, and `.wad` |
| `BDD` | six `.bdd` databases per package |
| `gfx` | `.sp` particles, UI data, textures, shaders |
| `MENUS` / `Menus` | `.fbs`, menu images, fonts, and WAV files |
| `Texts` | UTF-16 `.fli` and `.sub` text |

The six BDD names are present once in each of the 18 package roots:

```text
Anims.bdd
Armas.bdd
Efectos.bdd
Materiales.bdd
Objetos.bdd
Sonidos.bdd
```

They are mission/package-local database snapshots, not one global immutable set:
82 distinct contents were found among the 108 files.

## RenderWare-family streams

### `.rpc`: model Clumps

All 1,576 `.rpc` files begin with standard RenderWare Clump root `0x00000010`.
The extension is therefore a game packaging convention rather than a new geometry
format.

A representative character file validates with the existing parser and contains:

- Frame List and hierarchical transforms;
- Geometry and Geometry List;
- Atomics and Materials;
- Skin plugin data;
- HAnim plugins;
- User Data, Bin Mesh, Anisotropy, and Pyro metadata.

Immediate work required:

- accept `.rpc` in file dialogs, drag/drop, and corpus scans;
- treat it as a first-class Clump source;
- resolve `.dff` references in CSF databases/indexes to extracted `.rpc` paths;
- add skeleton/rest-pose display for skinned character models.

### `.anm`: animation streams

All 8,689 `.anm` files begin with RenderWare root `0x0000001B`, named Animation
Animation in the existing chunk table. They use RenderWare 3.7.0.2 build 24 in the
sampled files.

The current parser recognizes the root but has no typed `0x1B` decoder, so an ANM
currently reports zero decoded typed structures. The files range from tiny static
or single-key records to 321 KB clips.

The BDD animation database supplies the semantic layer: logical animation name,
one or more `.anm` files, loop and blend settings, movement/rotation metadata, and
timed sound associations.

An open-source Blender add-on supports common RenderWare ANM import/export and is
useful independent prior art: <https://github.com/Psycrow101/Blender-3D-RW-Anm-plugin>.

### `.rws`: five root populations

The first root chunk classifies all 410 `.rws` files:

| Root | Files | Meaning |
|---:|---:|---|
| `0x00000907` | 351 | RenderWare Physics Body Definition |
| `0x0000000B` | 21 | RenderWare World, generally level collision |
| `0x00000010` | 17 | RenderWare Clump, generally main visual map |
| `0x00000909` | 17 | RenderWare Physics Ragdoll Definition |
| `0x00000024` | 4 | Table of Contents roots for combined FR02 streams |

The existing project already parses all of these root families and has detailed,
evidence-backed Physics schemas. See [the existing corpus findings](../corpus-findings.md)
and [RWS format notes](../rws-format.md).

The missing feature is association and visualization: a body definition is local
shape data, not a placed mission entity. `.scn`, BDD, `.phd`, and normalized path
resolution must supply the owning class, render model, and world transform.

### `.wad`: unresolved RenderWare Audio data

Each mission WAD begins with root `0x00000802`; one sample contains 372 top-level
chunks of this type. Vendor `0x08` is RenderWare Audio in the existing vendor map.
The exact bank/event/sample schema has not been recovered.

This format is large by bytes but not initially on the critical path. Defer it
until mission-script sound references or audio modding are a concrete goal. The
initial dependency graph should retain WAD identity and unresolved references
without pretending the payload is understood.

## CSFFBS family

### Magic-based coverage

Exactly 1,262 files begin with `CSFFBS`:

| Extension | CSFFBS files |
|---|---:|
| `.sp` | 947 |
| `.fbs` | 126 |
| `.bdd` | 108 |
| `.scn` | 21 |
| `.gsc` | 21 |
| `.csc` | 21 |
| `.txt` | 18 |

Only the 18 `iconos.txt` copies are CSFFBS. `Interfaz.txt` and the three global
menu text files are not, demonstrating why content sniffing is required.

### Container grammar

The public CSFFBS decoder source and direct inspection agree on a compact generic
grammar:

```text
header
├─ magic: "CSFFBS"
├─ version/reserved fields
├─ entry count
├─ identifier count
└─ string count

entry table: fixed 12-byte records
identifier table: length-prefixed, null-terminated byte strings
string table: length-prefixed, null-terminated byte strings

entry types
0  identifier
1  group  ( )
2  array  [ ]
3  integer
4  float
5  string-table reference
```

Groups and arrays carry element counts and optional identifier-table references.
The format is a serialized typed document tree, not opaque compressed bytecode.

The existing public decoder converts these documents to readable text, but its
implementation renames the input to a backup and writes text at the original path.
That behavior is unsuitable for this project's read-only corpus workflow. The new
core should parse without mutation, preserve offsets/table identities, and export
to a separate destination. Source: <https://github.com/herbert3000/CSFFBS-Decoder>.

### `.scn`: mission scene model

All 21 SCN files contain identifiers for the following systems:

- player data and Commando/Sniper/Spy starts;
- actors/entities (`.BICHOS`);
- names, class IDs, positions, angles, flags, collision, scripts, groups, and cells;
- navigation mesh groups, points, and connections;
- origin/destination point and group links;
- sector map;
- dummy mesh, dummy folders, and hierarchy roots;
- area mesh and areas;
- light mesh and lights;
- water, bridges, ambience, sky, fog, water fog, and atmospheric effects;
- viewport and camera parameters;
- minimap and tactical-map configuration;
- save-game, opening-video, and multiplayer configuration.

The Ambush string table exposes representative semantic objects and route groups:

```text
Espia
Camion_Tropa_01
Camion_Tropa_02
Camara_01
Target_Camara_02
RUTA_Conductor
RUTA_Vigilancia_Patio
RUTA_Garita
RUTA_Traidor
RUTA_Commandos
Rutas_Cutscene_03_Camara
Rutas_Cutscene_03_Target_Camara
Zona_Tropa
Zona_Fusilamientos
Ruta_Camion_Huida
```

This is the authoritative source for placing gameplay entities and spatial tools
over the visual/collision map. Static scene-instance records embedded in the main
map RWS are a different system used to clone visual prototypes such as foliage;
they are not a substitute for SCN actors.

### `.gsc`: mission program

Every GSC contains resources, variables, arrays, scripts, triggers, events,
actions, a pool, and—in 20 of 21 files—conditions. Observed value types include
actors, zones, path points, numbers, booleans, arrays, class IDs, effects, weapons,
and strings.

Representative operations prove that scripts connect directly to the scene:

- create/destroy actors;
- get, reserve, compare, and move to path points;
- move to a path point while playing an animation;
- query actors in zones and change their behavior;
- enable/disable scripts and send actor events;
- add/select weapons and ammunition;
- activate vehicles, link actors to objects, and query vehicle seats;
- set cameras, FOV, minimap targets, and mission objectives;
- play animations, effects, and sounds;
- transition to another mission.

Comments and human-readable Spanish descriptions are present in the string table
and should be preserved and displayed.

### `.csc`: cutscene program

CSC uses the same grammar and script structures, focusing on cutscene init/end,
camera dummies, FOV, filters, sound playback, pauses, and completion conditions.
Some CSC files are nearly empty but still contain valid top-level script/pool
structures. The tool must handle empty lists without presenting them as failures.

### `.bdd`: gameplay databases

The databases provide the semantic joins needed by the Mission Explorer.

#### `Objetos.bdd`

Observed fields cover:

- object ID, secondary reference ID, display name, localization key, type, grade;
- visual model and LOD models;
- behavior/composition and health/damage/energy;
- bounds and collision mode;
- `.cmo` collision model;
- RenderWare Physics model, dynamic/moving state, mass, bounce, and slide;
- sight/head parameters, shadow, respawn, explosions, weapons, vehicles;
- character/vehicle animation bindings.

Representative records connect one class to multiple resources:

```text
Mercedes L3000
├─ Models\Vehi\CA_Mercedes_L3000.dff
├─ Models\Vehi\CA_Mercedes_L3000_L1.dff
├─ Models\Vehi\Mercedes_L3000.cmo
└─ Models\Vehi\mercedesL3000.rws
```

Character records reference visual models, LOD models, shared collision shapes,
and `Models\ragdoll.rws`. Player records also distinguish first-person hand models
and third-person models.

#### `Anims.bdd`

Maps semantic animation names to `.anm` paths and includes loop, blend-in,
velocity, translation, rotation, model/hand items, and timed sounds.

#### `Armas.bdd`

Contains weapon presentation, aiming, inventory, damage, force, dispersion,
cadence, projectile/detonation data, sounds, first/third-person models, animation
sets, bounds, and Physics parameters. This is a high-value early authoring target
after serialization is proven.

#### `Efectos.bdd`

Maps effect IDs to particle systems, textures, lights/glows, trails, and many
timing/color/size parameters.

#### `Materiales.bdd`

Maps material names to impact sounds/effects, decals, footsteps, flags, and color.
Observed names align with the Pyro material surface IDs already exposed from RWS,
including earth, stone, metal, vegetation, wood, glass, mud, tile, cement, stairs,
water, snow, and flesh.

#### `Sonidos.bdd`

Defines logical sound records and the assets/quality/localization/3D playback
properties needed by script and animation references. Full payload semantics were
not exhaustively analyzed for this report.

### `.sp`: particle systems

All 947 SP files use CSFFBS. Common fields cover emission area, duration and gap,
particle count/lifetime, velocity, direction, force, friction, start/end size and
color, UVs, orientation, textures, and optional models.

SP is a good later viewer/editor candidate because the schema is repetitive and
well-labelled, but it does not block mission entity/path/script inspection.

### `.fbs` and CSFFBS `.txt`: user interface

FBS records define panels and controls, positions/sizes, images, text and fonts,
alignment, colors, navigation IDs, sliders, videos, screenshots, and platform
variants. The CSFFBS `iconos.txt` files belong to the same family.

These formats should reuse the generic parser. A rendered UI designer is outside
the initial mission-workbench scope.

## Source-like and dependency formats

### `.cmo`: collision shapes and character hit volumes

CMO is human-readable bracketed data with comments. Observed shapes include box,
sphere, and ellipsoid records with center/dimensions/radius. Character definitions
attach internal shapes and hot points to explicit bone indices and labels such as
pelvis, torso, head, arms, hands, legs, and feet.

This makes CMO useful for:

- character hitbox display over an HAnim skeleton;
- object/vehicle collision display;
- comparing source-like collision with compiled `0x907` Physics;
- later schema-aware editing without reverse-engineering a binary serializer.

The parser should preserve comments, unknown fields, ordering, and original text
where possible.

### `.vis`: mission visual entry point

VIS begins with length-prefixed path strings. Representative paths identify:

- the visual map `.rws`;
- the `_col.rws` collision companion;
- the map texture directory;
- a sky `.dff` model.

Additional numeric tail fields remain unresolved. Phase 02 should first recover
the known references and preserve the tail verbatim.

### `.m3d`, `.phd`, `.and`, and `.txl`

- M3D enumerates model dependencies using requested `.dff` paths.
- PHD pairs or groups render models with Physics `.rws` dependencies and related
  values; exact record semantics need a bounded parser.
- AND indexes animation dependencies; records are more than a flat string list and
  must not be treated as one until its structure is recovered.
- TXL is a plain text list of texture paths with mixed slash styles and casing.

These formats should feed a dependency graph. Unknown record values can remain raw
while known paths become typed edges.

### `.sec`

SEC is binary spatial data containing counts, many coordinate-like floats, and no
useful embedded names in the sampled files. It may relate to sectorization,
visibility, streaming, or gameplay spatial partitioning. Its exact role is not
established here.

Do not block the Mission Explorer on SEC. Preserve it as an unresolved package
member, compare it against SCN sector maps and RWS bounds later, and assign meaning
only after structural and runtime evidence agrees.

### Plain-text configuration

- PRP contains mission/player properties such as available commandos, doors,
  hideouts, allies, streaming, and gas-mask settings.
- DST contains distance and distance-scale sections; all observed files are only
  96 bytes and mostly empty/default.
- VSH/PSH are readable shader source.

These are low-cost parsers but not prerequisites for mission scene assembly.

## How the resources join

### Scene and object resolution

```text
SCN actor
├─ stable source record and placement
├─ name, class ID, group, script, flags
└─ class ID → Objetos.bdd record
               ├─ requested .dff → extracted .rpc Clump
               ├─ .cmo collision shapes
               ├─ 0x907 Physics .rws
               ├─ shared 0x909 ragdoll .rws
               ├─ LOD models
               └─ animation-set references
```

### Animation resolution

```text
GSC/CSC action or object animation slot
└─ semantic animation name
   └─ Anims.bdd
      ├─ one or more .anm clips
      ├─ loop/blend/movement metadata
      └─ timed sound references → Sonidos.bdd / audio resources
```

### Mission package resolution

```text
selected .scn
├─ sibling .gsc / .csc
├─ .vis → visual RWS + collision RWS + textures + sky
├─ .m3d → requested models
├─ .phd → Physics/render associations
├─ .and → animation dependencies
├─ .txl → texture dependencies
├─ six BDD databases
└─ optional .sec / .prp / .dst / .wad
```

The resolver must account for FR02 package copies and names that do not equal the
directory or `.scn` stem. Discovery must be based on explicit references and a
reported deterministic fallback policy, not only “same basename.”

## Capability conclusions

### Physics entities and players on the map

They are feasible, but they are not contained in the visual map RWS alone.

- SCN supplies actor/player placement and gameplay identity.
- BDD supplies class semantics and resource references.
- RPC supplies render geometry, frames, Skin, and HAnim.
- CMO supplies source-like object or bone-attached collision shapes.
- Physics RWS supplies compiled bodies and ragdoll constraints.

The initial explorer can display markers before full model resolution. Exact live
simulation state, forces, contacts, and AI-driven poses remain out of scope.

### Entity and player animations

They are feasible as selected-clip playback:

- decode the ANM root and keyframes;
- associate HAnim node IDs and Skin matrices;
- evaluate a pose and skin vertices;
- use BDD for meaningful animation names and playback metadata.

Reconstructing the exact animation active at an arbitrary gameplay moment requires
script/AI state and is not an initial goal. Scripted cutscene sequences can be
approximated earlier because CSC explicitly names cameras, actions, pauses, and
completion conditions.

### Paths, navigation, zones, and cameras

These are clearly in scope. SCN contains navigation groups, points, connections,
dummies, areas, cameras, and named route-related strings. GSC/CSC contain commands
that refer to path points, zones, dummies, actors, and cameras. The useful feature
is not merely drawing them but providing bidirectional cross-references.

### Scripts

Scripts can be decoded into a lossless generic AST and inspected immediately.
Higher-level control-flow views, reference indexes, and diagnostics can be added
without interpreting every opcode. Full execution or decompilation into a new
source language is not required for the first useful release.

## Format priority

| Priority | Formats | Reason |
|---|---|---|
| P0 | CSFFBS core across SCN/GSC/CSC/BDD | Unlocks the mission model, logic, and semantic joins |
| P0 | VIS/M3D/PHD/AND/TXL path extraction | Turns isolated assets into a mission dependency graph |
| P1 | SCN typed views | Enables actors, starts, navigation, paths, zones, cameras, and lights |
| P1 | RPC first-class support | Existing parser already understands the underlying Clumps |
| P1 | BDD typed views and reference indexes | Resolves class IDs and meaningful animation/weapon/effect data |
| P1 | CMO and Physics association/rendering | Makes object collision and character hit volumes visible |
| P2 | ANM playback | High user value after models/skeletons and BDD joins work |
| P2 | GSC/CSC control-flow and timeline views | Builds on the generic AST and scene reference index |
| P3 | SP particle preview | Useful for effect mods; generic parsing is already covered |
| P3 | PRP/DST and shader viewers | Cheap supplemental inspection |
| P4 | FBS/UI rendering, FontWapa, RAW | Separate UI-modding workflow, not mission-critical |
| P4 | WAD/audio internals, SEC semantics | High uncertainty or weak initial dependency on core goal |

## Existing community tools and the gap

Relevant tools already exist:

- CSFFBS text decoder: <https://github.com/herbert3000/CSFFBS-Decoder>
- RenderWare ANM Blender add-on:
  <https://github.com/Psycrow101/Blender-3D-RW-Anm-plugin>
- Commandos Developing Toolkit: <https://github.com/IanusInferus/cmdt>
- CSF PAK creator discussion:
  <https://forums.revora.net/topic/118162-csf-pak-creator/>

An active CSF mod credits these and other disconnected converters, supporting the
conclusion that the ecosystem lacks an integrated visual mission/resource
workbench rather than another isolated extractor:
<https://www.moddb.com/mods/commandos-strike-force-beta-content-mod>.

The project should interoperate with documented external formats where useful but
must retain its own safe parsers, validators, source identities, and no-overwrite
workflow.

## Open questions

1. Exact SCN record schemas, optional versions, and stable identifiers beyond
   source entry/table identity.
2. The class-ID normalization and precedence rules between package-local BDD data.
3. Exact requested `.dff` to extracted `.rpc` mapping rules and collision cases.
4. Complete VIS numeric tail schema.
5. PHD and AND record boundaries and non-path fields.
6. SEC role and its relation to SCN sector maps and RWS BSPs.
7. ANM interpolation scheme, keyframe-to-HAnim node mapping, and root motion.
8. Remaining Physics ragdoll joint semantics.
9. GSC/CSC opcode signatures, reference operands, and control-flow behavior.
10. WAD/RenderWare Audio bank structure and mapping from `Sonidos.bdd` IDs.
11. Whether loose modified files override package data in every game version, or
    whether a rebuilt PAK is always required.

Each question should be answered with samples, byte offsets, cross-file
correlation, and—when necessary—executable/runtime evidence. Unknowns must remain
visible rather than being hidden behind guessed field names.

## Reproducible read-only inventory commands

The current RWS scanner covers `.rws` only:

```powershell
.\build\Release\rws-corpus.exe "E:\dev\re-csf\CSF_unpacks"
```

Phase 01 should add a content-sniffing CSF corpus command that can reproduce the
full extension, magic, schema, diagnostic, and dependency summaries without
writing beside the inputs. Reports and exports must go to an explicit output path
or standard output.

