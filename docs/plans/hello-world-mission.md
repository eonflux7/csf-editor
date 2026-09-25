# Plan: the hello-world mission

> **Active plan** (drafted 2026-09-25). This page is the plan for rws-man's first
> *new-world* mission. When it ships, move it to [`../archive/`](../archive/) and
> describe the as-built behavior in [`../guides/`](../guides/) and
> [`../game-knowledge/`](../game-knowledge/).

## Progress (2026-09-25)

| Step | State |
|---|---|
| Phase 0 | done: Blender 5.2.2 LTS (`blender`, Fedora package); test install `~/dev/csf_game` (`play-csf.sh`, Wine prefix `~/.local/share/wineprefixes/csf`); deploy/rollback proven on Ransom, including added actors |
| Research | running unattended (`tools/research-loop.py --keep-going --on-task-done tools/task-digest.py`); R4 and R6 answered (KB-world-geometry-5/6) |
| Phase 3 | done: `rws::WorldModel` parse/write (`include/rws/world_model.hpp`); `csf-mod world-audit` rewrites all 42 shipped Worlds byte for byte, and with `--rebuild` a new BSP over each World's triangles recovers `complete` |
| Phase 4 | tooling done: `tools/blender/export_csf_world.py`, `make_test_terrain.py`, `csf-mod world-build`; awaiting the in-game spikes below |
| Spike 1 | built (was deployed; rolled back for spike 2): Convoy with both Worlds rebuilt by `csf-mod world-rebuild` (new BSP, sectors and plug-ins, same triangles). Expect Convoy to look and play exactly as shipped |
| Hello world | **deployed** in the Convoy slot (rollback: `~/dev/csf_game/.csf-mod-backups/1790325368115369428/deployment.state`); built by `tools/hello_world/build_mission.sh` |
| Phase 5 | tooling done: `.csfworld` `prop` (donor scene instances, collision cut from the donor `_col.rws`) and `piece` (donor World triangles in a box) records; `tools/hello_world/build_world.sh` builds the hello-world map (100 m terrain with a hill, Convoy's house, two Convoy trees) |
| Spike 2 | works in-game (with Coll Trees): Convoy's scene and props on a generated 340 m flat World at game height 505 (`~/dev/csf-mods/convoy-flat.pak`) |

Spike commands (from the repository root; workspaces and PAKs live in
`~/dev/csf-mods/`):

```bash
# undo spike 1
./build/Release/csf-mod rollback ~/dev/csf_game/.csf-mod-backups/1790323906053006401/deployment.state
# then deploy spike 2
./build/Release/csf-mod deploy-pak ~/dev/csf-mods/convoy-flat ~/dev/csf-mods/convoy-flat.pak \
    ~/dev/csf_game maps/Convoy.pak --apply
```

Report for each: does the mission load; does it look right; can the commandos
walk (spike 2: on the plane; props have no collision); any crash, with the
`-log` output if there is one.

### In-game results

| Date | Build | Result | Follow-up |
|---|---|---|---|
| 2026-09-25 | spike 2 (flat World, no Coll Trees) | loads; intro cutscene renders over the plane; after spawn the Spy is high in the air, the plane shows as a white area far below, minimap NPCs faded (other height) | every shipped collision sector with triangles carries a Collision plug-in (`0x11D` Coll Tree, 2323/2323); the builder now generates them; spike 2 redeployed with trees |
| 2026-09-25 | spike 2 with Coll Trees | **works**: the generated World and collision load and play | the writer is proven in-game; next the hello-world mission (`~/dev/csf-mods/hello-world.pak`) |
| 2026-09-25 | hello world v1 | crashes while the mission loads | bisect |
| 2026-09-25 | bisect A: spike-2 map + hello scene/scripts | crashes on load: the cause is in the scene or scripts, not the map | suspect: areas parked as degenerate polygons (8/9-point areas collapsed onto 4 positions); parking removed |
| 2026-09-25 | bisect C: A without parking | **loads**; actors (placed for the hello terrain at y 0) spawn under spike 2's plane (y 505) and fall | confirms the degenerate parked areas caused the crash |
| 2026-09-25 | hello world v2 | **works**: loads, plays; one stray fire effect in the air | the scene's `.EFECTOS` sit on Convoy dummies at Convoy heights; v4 sinks every effect dummy over the terrain 30 m under the ground |
| 2026-09-25 | hello world v4 (v3 prototype placement + effects fix; rollback `.csf-mod-backups/1790325368115369428`) | **works**: no stray effects or trees | remaining: own objective text, intro cutscene, scene built from the minimal section set (KB-scn-7) |

## Goal

Build a mission in a world that did not ship with the game, entirely from
rws-man, and have *Commandos: Strike Force* load it and play it to the end:

1. terrain is modelled in **Blender** and compiled to the game's World and
   collision formats by a small exporter;
2. **vanilla** buildings and props are placed on it in rws-man;
3. **vanilla** actors, animations, objectives, a cutscene and scripts are added
   and wired together in rws-man;
4. navigation, sectors and a trigger area make the AI and the scripts work.

This is a limited Stage B: new geometry, everything else reused from the game.
It is also the first real use of the pieces this plan adds to rws-man: an
**asset store** of vanilla content, a **mission flow** view and a **scene
editor** fast enough to prototype in.

## The mission

Small on purpose: every layer of a mission is exercised once, with the least
content that proves it works.

| Layer | Hello-world content |
|---|---|
| World | a 100 m × 100 m flat plane with one low hill, one vanilla ground texture from the corpus |
| Collision | the same plane and hill, plus the building's collision |
| Props | one vanilla building and two vanilla trees, copied from a shipped map |
| Actors | the player commando and one guard (vanilla classes) with a short patrol |
| Navigation | one nav group: a grid of points covering the walkable ground, linked to neighbours |
| Sectors | one sector covering the map |
| Trigger area | one zone at the building door |
| Objective | "Reach the building": entering the zone completes it and wins the mission |
| Cutscene | a short intro: the camera moves along two points, then play starts |
| Environment | sky, fog, ambient light, sounds and minimap config copied from the donor mission |

### Done when

- [ ] The mission package builds from a clean checkout with one documented
      command sequence (Blender CLI → `csf-mod` → package).
- [ ] rws-man opens the built package with no validation errors, and the
      World and collision recover as `complete`.
- [ ] The game loads the mission, the intro cutscene plays, the commando walks
      on the terrain and cannot walk through the building, the guard patrols,
      and entering the zone completes the objective and ends the mission.
- [ ] Every format fact the tools rely on cites a `format-reversal`
      knowledge-base entry (or is marked *copied from the donor, unexplained*).

The game is run **by a person**. The research workspace is static-only; an
in-game test is the one check it cannot give, and it is the last rung of the
[validation ladder](#validation-ladder).

## Key decisions

**D1: replace a vanilla mission slot.** The hello-world package replaces an
existing mission (`csf-mod export-mission` already rebuilds a whole mission
archive from the shipped one). Registering a *new* mission in the menus and
campaign is a separate problem and is deferred. Recommended donor: **Convoy**:
its `.scn` (52 KB) and `.gsc` (65 KB) are the smallest of the twelve shipped
missions, so less of its logic has to be understood or deleted.

**D2: Blender exports a plain text file; rws-man compiles.** The Blender side is a
CLI script that writes `.csfworld`, a line-based triangle list with a material
table (texture name, collision surface name, shade byte) and a per-face role
(visual, collision or both); see `include/rws/world_source.hpp`. It replaced
the planned glTF + JSON sidecar, which would have needed a glTF and a JSON
parser in C++ for no gain. All game-format writing (World, BSP, collision,
`.vis`) is C++ in `rws_core`, because:

- it is unit-tested in `tests/document_tests.cpp` like every other writer;
- the existing World reader (`world_recovery.cpp`) checks every file the writer
  produces, in the same test;
- the GUI can rebuild a world without Blender installed.

**D3: props are vanilla Clumps copied between maps; buildings are World pieces.**
*Correction (2026-09-25):* the shipped map Clumps are vegetation only
(`ARBUSTO_*`, `ARBOL_NORUEGA_*`, Convoy's trunk/canopy pairs); buildings are
baked into the visual World (Convoy's house is the triangles lit by
`EDIFICIO_1*_Lm`). So a building is copied as a **piece**: the donor visual
and collision World triangles inside a box, with their donor materials.
Props and pieces are both placed by the bottom centre of their donor bounds.
Original text: A map `.rws` contains its
own prop models: the Clumps first, then the `0x16FC0` scene-instance records
that place them, then the World ([rws-format.md](../game-knowledge/rws-format.md#csf-scene-instance-record-0x00016fc0)).
An instance finds its model by number (`prototype id − 1000` = the Clump's
first Pyro Atomic index, `FUN_006C3C60`). To place a vanilla building, the
tools copy its Clump chunk from the source map into the new map `.rws` and
write an instance record for it. Prop collision is **baked** into
`_col.rws` in the shipped game, so the world compiler merges each placed prop's
collision triangles into the collision World (the prop's visual mesh is the
first approximation).

**D4: copy what we do not need to change.** Sky, environment, minimap,
tactical map, ambient sound bank and any file the research has not explained
(e.g. `<scene>.dst`) are copied from the donor. The plan names each one, so
it can be replaced when its format is known.

**D5: `format-reversal` is a helper, driven by its research loop.** rws-man
does not wait for "all the research". Each phase lists the research it needs;
the questions are queued in `docs/format-reversal` and answered by
`tools/research-loop.py` (see [Research](#research)).

**D6: an agent drives rws-man live, through the same operations as the UI.**
`rws-man` hosts a project and serves a JSON operation API on a local socket,
either hidden (`rws-man --background --serve`) or inside the normal GUI, so a
person watches the agent's edits as they happen. A thin client
(`rws-man ctl`) sends one operation per shell call and prints the JSON
result, which suits agents such as Claude Code that run one command at a
time and cannot hold a pipe open. A `render` operation saves the viewport to
a PNG the agent can look at. Every editor feature ships with its operation,
so anything a person can do in the GUI, an agent can do and check. See
[Track A](#track-a-agent-control).

**D7: the game runs on this Linux machine.** In-game tests use a separate
test copy of the game under Wine/Proton, never the everyday install;
`csf-mod deploy-pak --apply` installs into it and `csf-mod rollback` restores
it. A person launches the game and reports what happened.

## Pipeline

```text
 Blender (CLI)                 rws-man                                   game
 ─────────────                 ───────                                   ────
 terrain.blend ──► glTF + ──► csf-mod world-build ──► <map>.rws        ┐
                   sidecar      (BSP, sectors,        <map>_col.rws    │
                                 collision merge)     <mission>.vis    │
                                        ▲                              │
 Asset store (vanilla index) ───────────┤ props (Clumps + instances)   ├─► mission.pak
   props · classes · anims · scripts    │                              │   (replaces
   · cutscenes · textures               ▼                              │    Convoy)
                               Scene editor ─────► .scn (actors, nav,  │
                                                    sectors, zones,    │
                               Flow editor ──────► .gsc / .csc          ┘
                                                    (objective, cutscene)
```

## Phases

Track A (agent control) starts first, because every later phase is built and
checked through it. Phases 1 and 2 need no new research and run alongside the
research queue. Every phase ends with a test that can be checked without the
game; phase 8 adds the in-game run.

### Phase 0: setup

- Install Blender 4.x (`sudo dnf install blender`, or the Flathub build
  `org.blender.Blender`) and record the version used; the CLI entry is
  `blender --background --python <script> -- <args>`.
- Pick and record the donor mission (D1) and the corpus ground texture (an
  `FFLR*` terrain `.dds` from the donor's `Textures/`, reused verbatim, so no
  DDS encoding is needed yet).
- Set up the **test install** (D7): a separate copy of the game in its own
  Wine/Proton prefix; record its path and launch command in this page. The
  shipped `Convoy` package is deployed and rolled back once with
  `csf-mod deploy-pak`/`rollback` to prove the loop before anything is changed.
- The research items are queued in `docs/format-reversal` (done 2026-09-25,
  commit `d87fb5e`).

**Exit:** `blender --version` runs headless; the donor, texture and test
install are named in this page; an unmodified Convoy round trip through
`deploy-pak` → play → `rollback` works.

### Track A: agent control

A live, scriptable way to drive rws-man, built once and used by every phase
after it: an agent (Claude Code in the first place) opens a project, queries
it, edits it, renders the view to check the result, and saves, while a person
can watch the same session in the GUI.

**Shape.**

```text
                 ┌────────────── rws-man (host) ───────────────┐
 agent shell ──► │ socket server ─► op queue ─► main loop       │
 rws-man ctl     │  ($XDG_RUNTIME_DIR/rws-man/<session>.sock)   │
   <op> args     │                 ops run between frames:      │
 ◄── JSON ────── │                 AppState / commands.cpp /    │
                 │                 csf::MissionEditor, render   │
                 └──────────────────────────────────────────────┘
   hosts: rws-man --background --serve <project>   (hidden window)
          rws-man <project> + "Allow agent control" (visible GUI)
```

- **One operation set, three callers.** Operations are JSON requests
  (`{"op": "actor.add", "class": 55, "at": [1000, 0, 2000]}`) dispatched on the
  main thread between frames, so they follow the existing rule that mission
  views rebuild at the start of the next frame. The GUI, `rws-man ctl` and
  batch files (`rws-man --background <project> --run steps.jsonl`, for
  reproducible scripts and tests) all call the same dispatcher. Where an
  operation already has a command in `app/commands.cpp`, the operation invokes
  that command, so menus, shortcuts and agents cannot drift apart.
- **Client.** `rws-man ctl` parses its arguments and exits before any window
  or OpenGL setup, so a call costs milliseconds. Forms:
  `rws-man ctl <op> --key value …` and `rws-man ctl --json '<request>'`;
  `--session NAME` picks a host when more than one runs. Output is one JSON
  object: `{"ok": true, …}` or `{"ok": false, "error": "…", "hint": "…"}`,
  with exit code 0/1.
- **Session control:** `session.open`, `session.info`, `save`, `undo`, `redo`,
  `history`, `close`. Edits stay in the `MissionEditor` history until `save`,
  which writes to the project workspace only (never the corpus or the working
  directory).
- **Query:** `list` / `get` / `find` over actors, classes, props, nav
  groups/points/links, zones, dummies, scripts, objectives, flow nodes and
  asset-store entries, with selectors in the existing `goto:kind:text` style
  (`actor:12`, `nav:3/17`, `zone:DOOR`), `--fields` to trim output and a
  default item cap, so answers stay small.
- **Edit:** everything `csf-mod mission-edit` does today, then each phase's
  new operations. Operations are named `<thing>.<verb>`: `actor.add`,
  `actor.move`, `prop.place`, `nav.add`, `nav.link`, `nav.fill`, `zone.draw`,
  `flow.add`, `world.build`, `deploy`.
- **See:** `render` saves a PNG of the viewport and returns its path:
  `--view top|iso|orbit:<selector>|from:<dummy>|current`, `--frame <selector>`
  (fit the camera to a thing), `--overlays nav,zones,actors,collision`,
  `--size WxH`, `--highlight <selector>`. `pick --at x,z` reports what lies at
  a map position (ground height, prop, zone, nav point); `validate` returns the
  current diagnostics.
- **Safety:** a session opened `--read-only` refuses edits; the socket lives in
  the per-user runtime directory with owner-only permissions; `ctl` never
  starts a host implicitly.
- **Docs for agents:** `docs/guides/agent-cli.md` lists every operation with
  one example, and the root `AGENTS.md` points to it, so an agent can use the
  CLI without reading the source.

**Milestones.**

| | Delivers | Exit test |
|---|---|---|
| A1 | background host, socket, `ctl`, session ops, query ops, `render`, `pick`, batch `--run` | an agent opens Convoy, lists its actors, renders a top view with actors highlighted, and the PNG shows them where `list` says they are |
| A2 | every existing `mission-edit` operation as an op, with undo and save | a batch file reproduces a `mission-edit` session byte for byte; the same ops over `ctl` give the same files |
| A3 | "Allow agent control" in the visible GUI | a person watches an agent's `actor.add` appear in the viewport and undoes it with Ctrl+Z |
| A4+ | each phase's new operations, shipped with that phase | named in the phase's exit test |

The dispatcher's GUI-free parts (request parsing, selectors, result JSON) live
in `rwsman_ui_model` and are tested in `tests/document_tests.cpp`; the host
and the render operation live in `app/`.

### Phase 1: mission flow view (read-only)

A new panel that draws a mission's logic as a graph, built from the `.gsc` and
`.csc` programs rws-man already parses (`csf_program`, `csf_script_signatures`)
and the event/objective model recovered by the scripting workstream (done).

- **Nodes:** mission start, script programs (with their trigger condition),
  events (built-in and custom), objectives, cutscenes, mission success/failure,
  and the actors, zones and dummies the scripts name.
- **Edges:** event → program (`.EVENTOS`, `WAIT_EVENT`), program → event
  (`SEND_EVENT`), program → program (`SCRIPT_EXE`, `TRIGGER_ON`), program →
  objective (`SET_OBJETIVO*`, `OBJETIVO_*`), program → cutscene
  (`CUTSCENE_EXE`), program → end (`SET_MISSION_SUCCESS`).
- **Story view:** the same graph ordered by objective, so a mission reads as
  "start → cutscene → objective 1 → … → success".
- Selecting a node shows its script text and selects the actor/zone in the
  viewport; selecting an actor lists the scripts that touch it.
- The graph builder is GUI-free (`rwsman_ui_model`, tested in
  `tests/document_tests.cpp`); the panel is `app/ui/mission_flow.cpp`; its
  commands go in `app/commands.cpp`.

Its first job is teaching: read Convoy, Ransom and Ambush in it and write down
the idioms they use (below). [`ransom-script-flow.html`](../game-knowledge/ransom-script-flow.html)
is the hand-made prototype of this view.

**Exit:** all 21 shipped scenes build a flow graph with no unresolved
references that the scripts themselves do not have; a `--screenshot` of Convoy's
flow is checked in with the guide.

### Phase 2: asset store

An index of every reusable vanilla asset, built once from the corpus and cached
in the per-user config directory (the GUI never writes to the working
directory), with import that copies an asset **and its dependencies** into a
mod project.

| Kind | Source | Key | Import copies |
|---|---|---|---|
| Prop / building | Clumps in each map `.rws` (named by their instance records, e.g. `ARBOL_3`) | source map + Clump offset | Clump chunk, its textures |
| Actor class | `Objetos.bdd` records | class id | the record, model `.rpc`, `.cmo`, `.phd` entry, its animation set (existing `--import-class`) |
| Animation | `Anims.bdd` records + `.anm` | catalog id (`.ID`, game-wide) | record + clip (existing `--import-anim`) |
| Script idiom | shipped `.gsc` programs | mission + program id | parameterised program text |
| Cutscene | shipped `.csc` programs + camera-path dummies | mission + program id | program text + dummies |
| Texture | `Textures/` `.dds` | name | the file |
| Sound | `Sonidos.bdd` | id | record only (WAD banks are out of scope) |

- A browser panel with search (the existing fuzzy matcher), thumbnails for
  props and classes (the existing hidden renderer), and drag-to-viewport.
- **Script idioms** are the mission-design vocabulary: small programs harvested
  from shipped missions in phase 1 ("when actor X dies, complete objective N",
  "when the player enters zone Z, play cutscene C", "patrol between points"),
  stored with their parameters marked, so a new mission is assembled from
  vanilla patterns instead of written from scratch.
- The index builder is GUI-free and reuses mission discovery and the object
  database.

**Exit:** the index lists every shipped Clump, class, animation, program and
texture with its source; importing a class, an animation and a prop into a mod
project and running `csf-mod validate` passes.

### Phase 3: World writer, proven on shipped maps

Before writing a new world, write an old one. Add a World writer to `rws_core`
that serializes a World from its parts (materials, sectors, BSP planes, Pyro
plugins), and prove it on the corpus:

- **Round trip:** recover each shipped World (visual and `_col.rws`) and write it
  back from the recovered data. The target is byte-identical, except where
  the research says the shipped bytes are defective (the Pyro World Sector size
  overstatement); every difference is explained in the test.
- **BSP builder:** a separate function that builds a valid BSP from a triangle
  soup (split on the longest axis until a leaf is under a triangle/vertex
  budget; RenderWare uses 16-bit vertex indices per sector), checked by the
  existing BSP-topology validator.
- **Reader checks:** a `validate-world` check that applies every validity
  test the executable's World reader performs (research R1), so a written file
  is checked against the game's rules, not only ours.

**Exit:** round trip over every shipped World with all differences explained;
a generated BSP over a shipped World's triangles recovers `complete`.
*Met 2026-09-25:* no differences at all once the World is parsed physically:
the Pyro World Sector plug-in holds one byte per **triangle** (8 + N bytes,
declared 12 + N), and every enclosing size includes the 4-byte overstatement.
Authored Worlds declare the actual size (KB-world-geometry-5).

### Phase 4: Blender → game (the flat plane)

- `tools/blender/make_test_terrain.py` (done): headless script that builds the
  plane with a hill, UV-maps it, assigns the corpus ground texture and
  exports `.csfworld`. The test terrain is generated, never committed.
- `tools/blender/export_csf_world.py` (done): the general exporter for any
  scene (Blender metres, Z up → game centimetres, Y up; per-material
  `csf_texture`/`csf_surface`/`csf_shade`, per-object `csf_role`). Later it can
  live in the existing Blender add-on as a menu item.
- `csf-mod world-build <source.csfworld> <donor-map.rws> <new-map.rws>` (done):
  visual World + BSP and collision World + BSP. Visual materials are copied
  from the donor by texture name, collision materials by Pyro surface name;
  every sector gets the plug-ins all shipped sectors carry (Bin Mesh triangle
  list, `FVF.UserData = 0x3003`, Pyro metadata; visual: Right To Render and
  MatFX). The `.vis` stays the donor's while we replace a slot (D1).
- Lightmaps (R6, KB-world-geometry-6): authored visual Worlds keep the
  shipped profile, normals + two UV sets + the donor material's MatFX
  lightmap; the second UV set points at one lightmap texel until the add-on's
  Cycles bake is wired in.

**Exit:** `blender --background` + `csf-mod world-build` produce a map that
rws-man opens, renders, and recovers as `complete` (visual and collision), and
that passes `validate-world`; `rws-man ctl world.build` does the same from a
session and `render --view iso` shows the result.

### Phase 5: props in the new world

- Scene editor: drag a prop from the asset store onto the terrain (snap to the
  collision surface), then move/rotate it with the existing gizmo.
- `world-build` (done) copies the used Clumps (donor prototype ids, unique
  within one donor, KB-world-geometry-7), writes the `0x16FC0` instance records
  (every shipped record re-encodes byte for byte; flags copied from the donor
  instance), and moves each prop's donor collision (triangles inside its donor
  bounds) into `_col.rws`. Pieces copy donor World triangles the same way.
- The level draws a prototype Clump at its own frames as well as its clones
  (KB-world-geometry-11), so a prop's first placement is the copied Clump
  itself: its root frame takes the placement matrix and that record is
  dropped (`assemble_props`). Open: whether dropping the donor's un-instanced
  Clumps is safe (queued scn research; the hello-world map drops 44 of 48).

**Exit:** the building and two trees render in rws-man at their placed
transforms, and a collision pick on the building wall hits, also through
`rws-man ctl prop.place` and `pick`.

### Phase 6: scene: actors, navigation, sectors, zones

Start from the donor `.scn` with its actors, navigation and zones removed, and
the environment kept (D4).

- **Actors:** place from the asset store; the existing `MissionEditor` already
  adds, moves, duplicates and deletes actors with undo.
- **Navigation:** a nav tool in the viewport: click to add points (snapped to
  collision), drag between points to link, and a "fill" command that lays a
  grid of points over walkable collision and links neighbours. Actors get their
  nav cell (`.CELDA` group/point) from the nearest point (KB-scn-6).
- **Sectors:** generate the `.sec` sector map and `.MAPA_SECTORES` for the new
  world (one sector for hello world; research R8).
- **Zones and dummies:** draw a trigger area (polygon on the ground); place
  dummies, including the two cutscene camera-path points.
- Every edit goes through `csf::MissionEditor` (the only writer) and is
  undoable; `csf-mod mission-edit` gets a CLI operation for each, so the whole
  hello-world scene can also be built by a script.

**Exit:** the new `.scn` opens with no validation errors; every actor has a nav
cell; the nav graph is connected; the flow view resolves the zone and actors. The
whole hello-world scene can be rebuilt from one `--run` batch file.

### Phase 7: objective, cutscene and scripts

- In the flow view, add nodes from the idiom library and bind their
  parameters by picking in the viewport (the zone, the objective, the cutscene).
  Under the hood each node is vanilla script text, compiled by the existing
  `compile` path and checked against the script signatures.
- Hello-world logic: mission start → run the intro cutscene (camera between
  the two dummies) → set objective "Reach the building" → player enters the
  zone → complete the objective → `SET_MISSION_SUCCESS`; the guard runs a
  patrol idiom between two nav points.

**Exit:** the flow graph reads start → cutscene → objective → success with no
dangling events; `csf-mod validate` passes; `rws-man ctl flow.list` returns
the same graph an agent can check node by node.

### Phase 8: package, play, iterate

- `csf-mod export-mission` with the donor's archive; `deploy-pak --apply` into
  the test install on this machine (D7); `rollback` to restore. One `ctl`
  operation (`deploy`) runs the build, package and deploy steps, so an agent
  can hand a person a ready-to-play install.
- Generate or copy the dependency tables (`.m3d`, `.txl`, `.and`, `.phd`) as
  research R9 decides.
- A person plays it and reports; the game's own log (the `-log` startup
  option, KB-package-loader-11) and screenshots are attached to the report. Record
  each failure in this page with what it points to; a load failure that the
  static checks did not catch becomes a new research question and a new check.

**Exit:** the *Done when* list above is ticked.

## Research

`docs/format-reversal` answers the format questions; rws-man only asks. Each
question below is a `## Now` item in the named workstream's `next-steps.md`,
run with the research loop, and each tool feature cites the knowledge-base
entry that justifies it.

| # | Question | Workstream | State (2026-09-25) | Needed by |
|---|---|---|---|---|
| R1 | Every validity check the World reader applies to a `0x0B` stream (header, sections, plugins, the Pyro metadata readers) | world-geometry | queued | 3 |
| R2 | Which of the two loaded collision Worlds the runtime uses (`level+0xc` vs the `level+0x9c` preload) | world-geometry | queued | 4 |
| R3 | What a collision triangle's `+6` low byte indexes (surface/material table from which `_col.rws` chunk) | world-geometry | queued | 4 |
| R4 | Minimal valid World + collision stream; whether the Pyro World Sector size overstatement must be reproduced | world-geometry | done: KB-world-geometry-5 (either size loads; write the actual one) | 3, 4 |
| R5 | Clump/prototype registration: how a map's Clumps get their Pyro Atomic object index, uniqueness, and what the placement loader checks (incl. the record `+0x34` flag field) | world-geometry | queued | 5 |
| R6 | Does the World renderer require lightmaps / a second UV set / prelight colours, and what happens without them | world-geometry | done: KB-world-geometry-6 (reproduce the shipped profile; the fallback is open) | 4 |
| R7 | Which `.scn` sections the loader requires (minimal section set) | scn | queued | 6 |
| R8 | The `.sec` per-sector payload and index entries, so a sector map can be generated | scn | queued | 6 |
| R9 | Are `.m3d`/`.txl`/`.and`/`.phd` required at load or regenerated/logged by the game (`LogModels.m3d`, `LogAnims.and` in the extension table at `0x008466c0`) | package-loader | queued | 8 |
| R10 | What `<scene>.dst` holds and whether a copied one is safe on a different map | scn | queued | 8 |
| R11 | Nav group `.TIPO` kinds and whether nav points must lie on collision | scn | queued | 6 |
| R12 | `.BICHOS` records without a nav cell (`.CELDA.PUNTO == -1`): what positions them | scn | queued | 6 |
| R13 | Trigger-area record semantics: how a zone's shape is tested and which events it raises | scn | queued | 6, 7 |
| R14 | Cutscene camera: how `.csc` camera commands consume path dummies | scripting | queued | 7 |

Already answered and used as-is: the `.vis` format (KB-package-loader-15), the
scene-instance record (`0x16FC0`), the World read path and collision use
(KB-world-geometry-1/2), the Pyro sector byte (KB-world-geometry-4), navigation
and sector/cell consumption (KB-scn-5/6), the script VM, events, objectives
and lifecycle (scripting, done), and the object/ID model (done).

**Running it.** From `docs/format-reversal`: add the item to the workstream's
`## Now`, then `tools/research-loop.py --workstream <ws>` (or `--max N` for the
tier-ordered queue). Read an answer with `tools/kb.py about <subject>`; a
stable one is promoted to [`../game-knowledge/`](../game-knowledge/) before a
tool depends on it.

## Validation ladder

Each rung is cheaper than the next and catches a different class of mistake:

1. **Unit tests:** writers round-trip in `tests/document_tests.cpp`; no fabricated
   constants, corpus observations are results, not guarantees.
2. **Corpus round trip:** shipped Worlds rewritten from recovered data; shipped
   CSFFBS files byte-identical through `csf-mod audit`.
3. **Own reader:** a written map recovers as `complete` with a valid BSP.
4. **Game rules:** `validate-world` and `csf-mod validate` apply the checks the
   research recovered from the executable.
5. **In-game:** a person runs the test install.

## Risks and fallbacks

| Risk | Fallback |
|---|---|
| The game rejects a World we generate, and static checks miss why | Bisect from a shipped World: rewrite ST08 unchanged (phase 3), then change one thing at a time (geometry, BSP, materials, plugins) |
| Lightmaps are required | Bake a flat lightmap with the existing add-on; the DDS staging already exists |
| Props need collision the visual mesh does not give | Cut the prop's collision triangles out of its source map's `_col.rws` by its placed bounds |
| AI will not path on generated nav | Copy the shape of a shipped nav group (spacing, link rules) that the research explains |
| `.dst` / dependency tables break on a new map | Keep the donor's files; generate empty-but-valid ones once R9/R10 say how |
| The donor's scripts reference deleted actors | Start the `.gsc`/`.csc` empty and add only hello-world programs |

## After hello world

Out of scope here, in rough order: registering a new mission slot and menu
entry; real terrain with baked lightmaps; water, bridges and effects; new
textures (DDS encoding); multi-sector maps; minimap and tactical-map images for
the new world; new props and models; audio banks.
