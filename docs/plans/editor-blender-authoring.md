# Plan: author hello world through the editor

Draft, 2026-09-25; revised after review the same day. Hello world v13 works
in-game (terrain, props, patrols, cover, the radio objective and the travelling
intro), so it is the reference mission. The milestone is to recreate it in the
GUI editor without editing Python or raw script text, then build on the same
operations for new missions. This page plans editor work; most of it is not
implemented yet.

## Goal and reference

The reference is `tools/hello_world/` at the v13 commit. `terrain.py` builds
the map, `csf-mod sector-build` (formerly `build_sec.py`) the sector map, `scene.py` the scene and every script,
`texts.py` the objective strings, and `build.sh` both archives. `scene.py` is
the working specification of each recipe the editor needs.

**Parity test.** For each recipe the editor adopts, building hello world
through editor operations must produce the same mission tree as `scene.py`.
The comparison is on the decompiled source after normalizing generated IDs and
record order. Keep `scene.py` as a test fixture until the editor reproduces all
of it. Playtests are then needed only for new gameplay behavior, not for
regressions in recipes the parity test already covers.

**Decisions (user, 2026-09-25).**

- The goal is new maps and missions. Remixing shipped maps (editable import of
  a vanilla World, extracting its buildings) is not a goal for now.
- Terrain is modelled in Blender only; the editor imports and rebuilds it and
  gets no terrain tools.
- New missions replace a shipped mission's slot; registering extra missions is
  not a goal.
- Blender shows placements and gameplay records view-only. Moving a proxy in
  Blender does not change the project; two-way editing may come later.
- Dummies, navigation, areas, cover groups, camera helpers and scripts are
  authored in the editor only. They are mission records that scripts reference
  by ID, need gameplay context (flow, references, warnings) and must follow the
  compiled ground. Blender shows them as references; a later Blender camera
  import arrives as editor records, never as Blender-owned data.

## Map structure and asset kinds

A shipped map (Convoy's `FR03.rws`) holds one World and 48 Clumps. The World is
terrain and buildings merged into one static mesh (33k triangles, 95 materials)
with baked lightmaps; its collision is a second World (`_col.rws`). The Clumps,
placed by scene-instance records, are repeated decor such as trees. Everything
interactive is an actor: an `.rpc` model by class, placed in the `.scn`.

The project uses matching asset kinds:

| Kind | Made in | Placed by | Built into |
|---|---|---|---|
| terrain | Blender, where it is modelled | (fixed) | the World and its collision |
| building | Blender (or a donor World piece), at its own origin | the editor | the World: its triangles are merged at each placement, so it gets lightmaps and collision |
| prop | a donor Clump (later: Blender, stage 8 spike) | the editor | Clump and scene instance in the map `.rws`, collision cut into `_col.rws` |
| actor | a donor class (later: custom models) | the editor | a `.scn` actor record, with imported class files |

A terrain change never requires placing anything again: the World is rebuilt
from the terrain and the placements, which keep their IDs and positions, and
the editor reports what no longer sits on the ground (handoff item 5).

## What the editor does today, and the gaps

| Hello-world feature | `scene.py` / tool | `MissionEditor` / GUI today | Gap |
|---|---|---|---|
| New mission in a donor slot, donor content removed | `scene.py` writes the scene; `build_mission.sh` deletes every donor script | open a donor mission; delete actors and scripts one by one | "new mission from slot" operation: keep `.MUNDOVIS`, databases and classes; clear actors, dummies, areas, navigation and scripts |
| Terrain, house piece, trees, plants | `terrain.py` → `.csfworld` → `world-build` | none in the GUI | World import and rebuild; props and pieces as project placements (see stage 2) |
| Sector map | `build_sec.py` paired terrain triangles into convex polygons | done: `rws::build_sector_map`, `csf-mod sector-build`, byte-identical to v13 | |
| Actors, prop actors, pickups | `ACTORS`, `prop_actors()` | add, duplicate, delete, class, look, faction, scripts, animations | ground height (stage 2) and presets |
| Donor classes and animations | `--import-class`, `--import-anim` | `import_class`, `import_animation` | asset browser with provenance (stage 4) |
| Walk grid (`MALLA`) and its links to routes | `walk_grid()`, `navigation()` | move, add, delete, link and unlink points in existing groups | add and delete navigation groups; generate a walk grid that avoids obstacles |
| Patrol routes, cover group (`.TIPO 3`) | `nav_group`, `patrol()` | as above | route tool and presets that write the group and the actor script together |
| Zones (the house) | `areas()` | move, insert and remove points; height | add and delete areas |
| Camera dummies and helpers | `dummies()`, `camera_helpers()` | move, duplicate, delete | add dummies; the cutscene editor (stage 6) |
| Objectives, events, mission success | `scripts()`, `CHECK_BOTH` | raw script text | recipes and the flow view (stage 5) |
| Usable radio on a crate | class 484 with class 211's look, lifted onto class 383 | `set_actor_look` | "usable object" preset; placing on a supporting object |
| Objective text | `texts.py`, `build_texts.sh` → GlobalEK | none | text table editing and packaging (stage 5) |
| Two archives, deployment, rollback | `build.sh`, `csf-mod deploy-pak` | project export of one archive | one project build covering both archives |

Undo already exists: every `MissionEditor` operation is one undo step
(`include/csf/mission_edit.hpp`).

## Responsibilities

| Work | Primary tool | Current capability / needed work |
|---|---|---|
| Terrain, buildings, custom static geometry | Blender | `.csfworld` exporter and C++ World compiler already work; add editor import/rebuild and source tracking |
| Mesh topology, sculpting, UVs, normals, LOD meshes | Blender | Keep mesh authoring in Blender; validate the supported game material/mesh profile on import |
| Custom collision shapes | Blender | Export separate collision meshes; editor assigns surfaces, previews collision and compiles the collision World |
| Lightmap unwrap, lights for baking, baking | Blender | Existing Cycles/DDS staging add-on; connect its results to authored World materials and packaging |
| Texture painting | Blender or an image editor | Editor previews and packages textures; existing lightmap DDS staging does not establish a complete custom-material pipeline |
| New reusable prop models | Blender → CSF compiler → editor | Static meshes can become World geometry today; a custom Clump/RPC prop is a research spike (stage 8) |
| New characters, rigs, skinning, animation clips | Blender → future converters | Separate research track; vanilla imports work now |
| Place buildings, trees, props, actors, pickups | CSF editor | Actor transforms/imports exist; add asset browser, World-piece and donor-prop placement, ground snapping |
| Patrols, cover, sectors, walk grid, trigger zones, dummies | CSF editor (Blender shows them view-only) | Point/area editing exists; add group/zone creation, route tools, walk-grid and sector generation, behavior presets |
| Objectives, interactions, mission success, text | CSF editor | Add forms and reusable script recipes, a flow view, and text editing/package support |
| Camera paths, look-at targets, timing, fades | CSF editor | Add shot authoring; compile to the travelling-camera pattern in KB-scripting-44 (v13) |
| Environment, build, deployment and rollback | CSF editor | Existing parts need one project workflow covering mission and GlobalEK archives |

Blender is the project's mesh authoring tool; another DCC could eventually
supply the same intermediate assets. Gameplay and camera paths do not require
Blender. Optional Blender camera-curve import comes after native editor shots
and must compile to supported game behavior.

## Handoff contract

1. A project manifest tracks each source `.blend`, exported asset, stable asset
   ID, exporter version, settings and content hash. Retain `.csfworld` for
   World geometry; version extensions explicitly. Keep generated output
   separate from editable sources. Machine-specific Blender/corpus paths stay
   in local settings, not the manifest. The manifest format gets its own short
   specification before stage 2 builds on it.
2. **Edit in Blender** opens the source and supplies a reference collection
   containing editor placements and mission landmarks. Reference objects are
   excluded from geometry export; saving Blender must not replace gameplay.
3. **Export to CSF project** writes an intermediate file atomically. The editor
   detects it, validates and compiles in the background, then shows a change
   summary. Failure keeps the last working asset available.
4. **Apply update** replaces geometry by stable identity, preserving actor,
   zone, route, objective and script IDs. Reimport is one undoable operation.
   Removing an asset with placements produces a resolvable reference error.
5. Placements distinguish absolute height, terrain-relative height and an
   offset from a supporting object. A radio on a crate retains that relation.
   Changed terrain reports floating actors, buried props and invalid routes;
   resnapping is explicit and reviewable. Camera paths keep absolute height.
6. Changes invalidate dependent collision, sector/navigation checks and baked
   lighting. Build reports stale outputs. It never silently regenerates
   authored routes or moves mission objects.

**Identity.** A World is compiled as a whole (one BSP), so the unit of
replacement is defined as follows:

- Each Blender mesh object exported as World geometry (the terrain, a custom
  building) is one asset. Its ID is a custom property (`csf_asset_id`, set like
  the materials' `csf_texture`/`csf_surface`), assigned on first export and
  kept through renames and mesh reorders. A building asset is exported about
  its own origin; its placements say where it stands.
- Each building, `piece` and `prop` placement has its own project ID and names
  its asset or donor. Placements are not Blender geometry.
- Reimport replaces an asset's triangles and recompiles the World; placements
  and mission records keep their IDs.

**Props leave the Blender output.** Until stage 2, `terrain.py` appended the
house `piece` and the tree and plant `prop` records to the `.csfworld` it
exported. Under this contract, the project owns those records: the editor
places them and merges them into the World build. Blender sees them only as
references. Hello world's 1 piece, 14 trees and 15 plants are migrated (stage 2).

Use the existing conversion exactly once: Blender metres/Z-up to game
centimetres/Y-up, `game = 100 * (x, z, -y)`, plus the exporter's UV convention.
Store origin/pivot and material/surface identity explicitly.

The CSF compiler owns game binary output. Blender owns mesh source. The editor
owns placements and mission records. Editor-placed vanilla props may appear as
Blender references for composition and baking; their placement remains in the
project unless the user explicitly chooses to bake them into geometry.

## Delivery order and acceptance checks

Each stage ends with a build that is deployed and played; stages after 3 also
extend the parity test.

### 1. Imported models in project preview

Fix project preview resolution of imported classes: v12's crate (Escape class
383) is packaged but absent from the project preview. This is small,
independent work and the editor needs it before any placement work.

**Accept:** opening the v13 project shows the crate, the doberman and the radio
model given by `set_actor_look`, with a test covering imported-class
resolution.

**Done (2026-09-25).** The preview also drew Convoy's original collision map
under the project's World. `ResourceIndex::add_overlay` and
`MissionEditor::resource_index` now resolve every project or imported file to
the file its content came from, and the app resolves models, weapons and
animations through that index (`MissionState::resources`). The collision map is
replaced from the project like the visual map. Checked by rendering the v13
project: textured crate and doberman, and the generated 16-sector collision.

### 2. Project manifest, World import and core geometry queries

- Write the manifest specification (handoff item 1) and the asset/placement
  identity rules above. **Done:** [editor-project-format.md](editor-project-format.md);
  `csf::AuthoringProject` reads and writes it and builds `build/` incrementally
  (`csf-mod project-build`), merging terrain, building assets at their
  placements, pieces and props; `rws::build_map_files` holds the World build
  that `world-build` used to do inline.
- Add World import/rebuild around `WorldModel`, `world_source` and
  `map_assembly`, applied between frames through the mission-editing boundary.
- Move `piece`/`prop` records into project placements and migrate hello world.
  **Done:** `tools/hello_world/layout.py` holds the map layout, `terrain.py`
  exports the terrain only (and saves `terrain.blend`), `project.py` writes the
  project, and `build.sh` creates it. The build's World, collision and sector
  map, and every packaged mission file, are identical to v13's. Actor `anchor`
  records are not written yet (nothing uses them before the height report).
- Add a ground query to `rws_core`: height and normal at (x, z), and a
  downward ray against the collision World. Snapping, floating/buried reports
  and route checks all need it. **Done:** `rws::GroundQuery` over a World
  source's collision faces (`highest`, and `below` a height for floors under
  roofs), `csf-mod world-ground`. Placements do not use it yet.
- Port `build_sec.py` to a tested core sector-map operation. **Done:**
  `rws::build_sector_map` (`csf-mod sector-build`) reproduces v13's
  `Convoy.sec` byte for byte; `build_sec.py` is removed.
- Build both archives (mission and GlobalEK) from the project, with rollback
  records, replacing `build.sh`. (`build.sh` now writes each build into the
  project's `dist/<build-id>/`.)
- The height report: resolve each placement's height rule with the ground
  query and list those more than 1 cm from their stored height; resnap as one
  reviewable operation. **Done:** `AuthoringProject::height_report`/`resnap`,
  `csf-mod project-heights [--resnap]` (actors through `MissionEditor`);
  `scene.py` writes the actors' anchors (the radio on its crate). Raising the
  terrain 50 cm reports all 30 placements and 14 actors; one resnap and a
  rebuild leave none. Hello world's first report lists 8 trees and plants 1-10
  cm off (terrain.py placed them by its height formula). Found and fixed on the
  way: saving copied a project's `build/` map into `authored/`, detaching it
  from later rebuilds; the build now also refreshes the workspace's hashes.
- Rebuild from the editor: detect a changed export, run the project build in
  the background and reload the map between frames. **Done:** `app/authoring.cpp`
  watches the exports of an open authoring project, rebuilds and reports heights
  on a worker thread, reloads the mission (after a save when it has unsaved
  edits), and shows the Height report window with Resnap all
  (`app/ui/height_report.cpp`; commands under Mission > Authoring project).
  Checked with Release and Debug builds: a raised terrain opens as rebuilt with
  44 findings; resnap, save and reload leave none.

**Accept:** change the hill in Blender, reimport, reopen the project, build and
play. Placements keep their positions; those the hill now buries or lifts are
reported and follow it after an explicit resnap. An invalid export leaves the
working project intact.

### 3. Blender add-on

Package `tools/blender` as an installable add-on with a CSF panel; the headless
exporters stay usable from scripts.

- **Tag** mesh objects as terrain, building or collision-only, and give each an
  asset ID on first export.
- **Send to rws-man** exports the terrain and each building asset (about its
  own origin) into the project's `sources/`, atomically. The editor watches
  them, rebuilds in the background and shows the change summary and height
  report.
- **Reference import** loads the project's compiled state into a locked
  `CSF reference` collection that export skips: the World, placed buildings,
  props and actors (glTF from rws-man's exporter), and the gameplay records
  (navigation routes as curves, areas as outlined volumes, cover points and
  dummies as markers, camera paths). It is refreshed on request, view-only.
- **Import a donor model** (a Clump, an `.rpc` or a World piece) as an editable
  mesh, as a starting point for a new asset.

**Accept:** with hello world open in both tools, raise the hill in Blender with
the camp, routes and zones visible, send it, and see rws-man rebuild; every
placement keeps its position, and the height report lists what the hill now
covers or lifts.

**Done (2026-09-25).** `tools/blender/csf_authoring` (guide:
[blender-authoring.md](../guides/blender-authoring.md)); the `.csfworld`
exporter moved into it (`export_csf_world.py` wraps it and exports hello
world's terrain unchanged). rws-man adds `csf-mod project-reference` (built map
glTF, actor class models, markers JSON) and `project-asset`, and reloads a
project whose `project.csfproj` another tool changed.
`tools/blender/csf_authoring_test.py` passes headless (reference loaded and
never exported, unchanged terrain re-sends identically, a building exports about
its origin and registers, a raise is reported). Live: with rws-man open, a
Blender send of a 50 cm raise rebuilt and reloaded the map and opened the height
report with 44 findings. Limits: reference models come from rws-info's scene
export, which spreads the parts of the burnt truck (class 93); the reference
World is shown as wire.

### 4. Mission structure, placement and behavior presets

- "New mission from slot" (see the gap table).
- `MissionEditor` operations for adding and deleting navigation groups, areas
  and dummies; walk-grid generation over the map that avoids obstacles and
  links routes to the grid (`walk_grid()` and the `CONEXIONES` in
  `navigation()`).
- Asset browser: search vanilla classes, props and World pieces with provenance
  and dependency import. Snap to terrain or to a supporting object, duplicate,
  and place several at once.
- Named presets: guard on patrol, guard using cover, dog on patrol, decorative
  prop actor, pickup, usable object. Each preset writes the actor, its groups
  and its script together.
- The usable-object preset assembles the ghost behavior, model, prompt,
  highlight, one-use state and completion event (v12's radio). The cover preset
  configures both the cover group and the alert/combat `MOVIL_A_PARAPETO` modes.
  Generated IDs are allocated internally.

**Accept:** starting from an empty Convoy slot, place the camp, the crate and
radio, the guards, the dog and the pickup, and set up the patrols and the cover
with forms and viewport tools. Parity with `scene.py` for the placements,
navigation and actor scripts.

### 5. Mission flow, objectives and text

First build the read-only event/program/objective graph. Then add authoring
recipes for start/intro (including sending `INIT`, KB-scripting-14), enter
zone, actor death, use object and all-primary-objectives complete. Include
primary/secondary flags and text labels.

Save strings to the scene's text file in GlobalEK. GlobalEK is shared by every
mission, so each project reserves a text ID range recorded in its manifest
(hello world uses `09xx`), and the build refuses IDs outside it or used by
another loaded project.

Keep arbitrary imported scripts editable as text and visible as opaque graph
nodes. Only recognized recipes get structured editing; do not guess how to
rewrite arbitrary script control flow. Warn about unresolved references,
unraised custom events and malformed zones without claiming full runtime proof.

**Accept:** recreate all three hello-world objectives with their text; complete
primary objectives in either order, and finish with or without the radio.
Parity with `scene.py` for all scripts except the cutscene.

### 6. Cutscene editor

Edit shots as forms first: capture the camera from the viewport, then set start
and end, look-at target, duration and cuts. Show helper paths separately from
AI navigation. Compile to the v13 pattern (KB-scripting-44; Ambush): invisible
class-197 actors, type-0 paths, `CREATE_VIEWPOINT`, `CAMARA_EN_DUMMY`, movement
and timing.

Add scrub/play preview afterwards, with an explicit limit: the game's
navigation and tracking timing can only be approximated. Arbitrary spline easing
and imported Blender camera animation require separate verification;
`CAMARA_PLAY_ANM` has no shipped example.

**Accept:** reproduce v13's five travelling shots through the UI, adjust one
duration, build, play and regain player control at the end. Full parity with
`scene.py`. From then on the parity tests can use a stored copy of its output
instead of running it.

### 7. Baking

Connect the existing lightmap add-on to project material dependencies and
incremental builds. Hello world has no baked lightmaps (its terrain uses plain
tiled textures), so this stage uses a small separate test map with a custom
building.

**Accept:** edit and rebake the building in Blender, update it once in the
editor, and verify its lighting and packaged textures in-game.

### 8. Research spikes: custom props and characters

Not delivery stages. A custom independently placed prop (a new Clump and
class) has had no in-game spike. Characters, rigs and animation export need
format and game-class compatibility work. Each gets a spike with a go/no-go
gate before any editor work.

## Implementation boundaries

`csf::MissionEditor` remains the canonical mission-tree writer; UI operations
and CLI/agent operations share its undoable actions. World geometry compilation
and the ground and sector queries stay in `rws_core`. Project import applies
finished results between frames, through the existing mission-editing boundary.
Flow/index/recipe models belong in GUI-free libraries; panels live in `app/ui`,
commands in `app/commands.cpp`.

Ship each feature with the corresponding operation API, rather than building
a second authoring system for agents. Preserve unknown donor data and raw
scripts. Tests should cover reference preservation, failed imports, dependency
invalidation, round trips and `scene.py` parity; human playtests establish
gameplay behavior.

## Status

v13 (five travelling shots with fixed look-at helpers, independent paths and
constant-height motion) works in-game and is the reference build. v12 remains
the earlier confirmed fallback. Stage 1 is done. Stage 2: the project format,
hello world's migration, the sector map, the ground query, the height report
and rebuilding from the editor are done; stage 3, the Blender add-on, is done.
The stage 2 acceptance playtest (change the hill in Blender, play) is open.
