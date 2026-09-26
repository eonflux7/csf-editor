# Plan: the mission editor's UI and UX, and a UI test harness

2026-09-26. Phases 0 and 1 are done (see their status below); later phases
are not implemented yet.
[editor-blender-authoring.md](editor-blender-authoring.md) built the editor's
*capabilities*: hello world builds byte-identically through editor operations
and presets. This plan turns those capabilities into one tool that a mission
author works in from start to finish. It also adds a harness that tests the
GUI rather than only the core.

The UI grew out of an inspector: a chunk browser, a hex view, provenance
badges and a mission explorer. Authoring was added to it one tab at a time.
The redesign makes authoring the main path. It models one user creating a
new mission end to end (§2), rebuilds the layout, the editing model and the
look around that path (§3–§7), and puts the inspection tools behind one mode
switch (**Inspect**). None of them are removed.

The existing boundaries stay as they are. `csf::MissionEditor` is the only
writer. Rebuilds happen between frames. GUI-free logic goes in
`rwsman_ui_model` or `csf_core`, commands in `app/commands.cpp`, and colours
in `app/ui/theme.cpp`. Hello world parity is a gate for every phase.

Change IDs (`A3`, `E4`, `T7`, ...) are used in the delivery order (§9).

---

## 1. Audit: the UI today

Evidence comes from renders of `~/dev/csf-mods/hello-world-project` with
`rws-man --screenshot` (Release build, 1600×950, a clean config directory
with the resource root set), and from the code.

| # | Finding | Where |
|---|---|---|
| F1 | The start page describes the app as an inspector ("chunk browser, mission explorer, 3D preview"). It has no **New project**, and its Recent list holds `.scn` files, not projects. | `app/ui/start_page.cpp` |
| F2 | Neither the GUI nor `csf-mod` can create an authoring project; only `tools/hello_world/project.py` can. A project folder passed as a positional argument or dropped on the window fails with "Cannot read complete RWS file"; only `--project` works. | `app/main.cpp`, `app/app_actions.cpp` |
| F3 | Every authoring tool sits in the bottom dock's **Changes** panel: a save bar and 10 tabs (History, Files, Mission, Assets, Presets, Objectives, Cutscene, Flow, Texts, Import) in a strip about 150 px tall. **The bottom dock is hidden when a project opens**, so a new user sees no editing UI at all. "Changes" is the wrong name for it. | `app/ui/mission_editor.cpp` `draw_changes`, `app/ui/shell.cpp` |
| F4 | Placing works "at the ground under the viewport centre". Route and cover points are added one at a time with **Add point at view**. The viewport cannot place anything by clicking, shows no preview of the object before it is placed, and does not support drag and drop. | `app/mission_authoring.cpp` `view_ground_point` |
| F5 | Recipes can only create, never edit. The Objectives and Cutscene forms clear themselves after **Create**. To change an existing objective, patrol, route/script pairing or intro, the user edits raw script text or deletes the generated records by hand. | `create_objectives` (`tools.objectives.clear()`), `create_intro` |
| F6 | There are two save models. Mission edits have undo and an explicit save. Project edits (texts, building placements, resnap) write `project.csfproj` straight away, have no undo, and start a rebuild. | `set_project_text`, `place_building`, `store_texts` |
| F7 | The Explorer groups records by how they are stored. In hello world, 10 of the 24 "actors" are intro camera and target helpers (`INTRO_CAMERA_1` … `INTRO_TARGET_5`), listed alongside the soldiers. Rows read `class 197, ID 100, group -1`. | `app/ui/explorer.cpp` |
| F8 | The Inspector mixes authoring data with forensic data: `Convoy.scn:entry 169 @0x804`, provenance checkmarks, and the undecoded integers `collision`, `flags` and `2nd explosion`, next to name and position. | `app/ui/inspector.cpp` |
| F9 | The workspaces `MISSION SCRIPT ANIM SCENE GEOM HEX` are named after file formats, and each has its own layout. An author only needs two or three of them. | `app/ui/layout.cpp` |
| F10 | The viewport toolbar is ten unlabeled icon dropdowns. The edit tools (select, move, rotate, snap to ground) are in the Changes panel instead of on the viewport. | `app/ui/center.cpp`, `draw_project_bar` |
| F11 | A map-sized area volume tints the whole default view yellow; area fills cover everything unless someone filters them out. | viewport overlays |
| F12 | Commands have bugs, and the command palette misses things. `mission.flow`, `mission.objectives` and the other tab commands *toggle* the bottom panel, so calling one while the panel is open hides it; with the dock hidden, `mission.flow` shows the Missions tab. Presets are not palette entries: searching "guard" finds only the **Behaviour presets** tab. | `app/commands.cpp` `toggle_bottom_panel` |
| F13 | Problems are spread over five places: 68 Diagnostics on hello world (mostly inspection-level), flow findings in the Flow tab, the Height report window, `project->check()` warnings in the log, and editor refusals as toasts. Nothing answers "what is wrong with my mission?" | |
| F14 | Scripts are edited as plain text, with no highlighting, completion or inline errors. The checks on Apply are good. | `app/ui/script_view.cpp` |
| F15 | Build and deploy is an **Export .pak** dialog where the user types paths. GlobalEK text is packaged only by `tools/hello_world/build_texts.sh`. The GUI has no deploy, rollback or playtest loop. | `draw_mission_dialogs` |
| F16 | The cutscene is a list of shots with Up/Remove buttons. It has no timeline, and camera paths are not drawn in the viewport. | `draw_cutscene` |
| F17 | Nothing tests the UI. `--commands` has no assertions, ignores unknown commands and failed `goto:`, and refuses to overwrite its output PNG. | `app/main.cpp` |
| F18 | Only one object can be selected at a time. There is no box select, copy/paste, align or group move. | |
| F19 | Objective text fields mean two different things: typed strings with an authoring project, FLI IDs without one. | `draw_objectives` |
| F20 | The class pickers are filtered combos with about 400 rows (`36 Infanteria Oficial Pistola`). They have no thumbnails, no categories and no hint of what each class is for (character, prop, pickup, vehicle). | `class_items` |
| F21 | Hello world has 8 placements off their height rules. This shows only as status bar text that names a menu path. | status bar |
| F22 | Project placements (trees, plants, the house piece) cannot be selected or moved in the viewport. Only vanilla map instances (`set_map_instance_pose`) and mission records can. | `app/mission_editing.cpp` |

What works well and is kept: the single command registry and palette, Go to
anything, navigation history, the selection outline, the gizmo's
ground-sliding move, undo for mission edits, background loading and
rebuilding, the Blender watch-and-rebuild loop, the overlay system and the
screenshot mode.

---

## 2. The end-to-end journey this plan is built around

**Scenario:** a user makes *"Checkpoint"*, a new mission in the Convoy slot. It
has a sculpted valley, a farmhouse built in Blender, a camp of vanilla tents
and crates, patrolling guards and a dog, a zone around the farmhouse, a radio
to sabotage, three objectives and a short intro. Hello world is the same
journey with less content, which makes it the test for the journey (T9).

Each step lists what the user does, what the UI offers, and what is missing
today.

| Step | The user... | The UI offers (target) | Gap today |
|---|---|---|---|
| J0 Launch | opens rws-man | **Home**: recent projects with thumbnails and last build status, **New project**, **Open project**, and a smaller **Explore game files** link | F1, F2 |
| J1 Create | names the project, picks a slot, picks a starting terrain | **New project** wizard (E9): name and folder, a slot picker listing the discovered missions with the files that will be replaced, a text ID range reserved automatically, and a terrain start (flat plane of a chosen size, or an existing `.blend`). One click creates the project, clears the slot (`new_mission`), writes a starter terrain, builds and opens it. | F2; only `project.py` does this |
| J2 Terrain | sculpts in Blender | **Map** card: terrain source, last export and build time. **Edit in Blender** starts Blender with the project already set in the add-on. A live link dot turns green on each Send, and the build shows as stages. Height findings go to Problems. | Blender is started by hand; the add-on panel needs the project path typed in |
| J3 Buildings | models the farmhouse in Blender, tags it Building, sends it | The Asset browser gets a **Project buildings** entry with a thumbnail. Dragging it into the viewport places it (proxy mesh until the rebuild finishes). | building placement exists (`place_building`) but works at the view centre and without undo |
| J4 Dressing | scatters trees and plants, places tents, crates and a truck | **Asset browser** panel with categories, thumbnails and search. **Place** tool: a ghost follows the cursor over the ground, click places, Shift+click keeps placing with optional random rotation, and a prop dropped on another object is anchored to it. Box select, duplicate (Alt-drag), align to ground. | F4, F18, F20, F22 |
| J5 Actors | places players, guards and a dog, and gives them behaviours | Characters drop from the browser. The Properties panel shows a **Behaviour** card (Idle, Patrol, Guard post, Animal patrol, Custom script). Picking Patrol starts the **Route** tool in the viewport: click points, drag them, click a segment to insert. The cover group, pause and animations are fields on the card. **Every edit regenerates the behaviour's script** (E1). | F5: presets are create-only forms |
| J6 Players | sets which commandos start, where, and with what kit | **Players** card in Mission settings: availability, start points (drag the markers), a kit per commando with the weapon picker | the kit is a text field: `16,102@100/100` |
| J7 Zones and nav | draws the farmhouse zone, generates the walk grid, links the routes | **Zone** tool: click a polygon, drag its height, see polygon problems live. **Walk grid** component: parameters with a live preview overlay before Apply, regenerated as one step, with a coverage overlay. | "Add point at view"; the walk grid has no preview |
| J8 Interaction | makes the radio usable and ties it to an objective | Select the radio and add **Usable** (prompt text, one use, highlight), then **Make objective** from the same card. The objective's target is picked in the viewport with an eyedropper. | objective targets come from combo lists |
| J9 Objectives and logic | writes three objectives and "kill the officer to alert the camp" | **Objectives board**: cards (primary/secondary, kind, target, text typed in place, done message), reorderable, with the success rule shown. **Triggers**: *When* (enters zone, dies, is used, mission start, custom event, timer) → *Do* (complete objective, show message, raise event, set AI mode, enable/disable) (E10). Hand-written scripts remain available in Script mode. | F5, F19, no trigger builder |
| J10 Intro | frames five shots | **Timeline** panel: shot strips with durations, camera and target gizmos with travel paths in the viewport, a scrub bar, and **Capture from view**. Regenerated like any component. | F16 |
| J11 Check | reviews what is wrong | **Problems** panel: flow findings, height findings, project checks, reference errors, stale outputs and polygon problems in one list. Every row links to its subject, and many have a **Fix** (Resnap, Add INIT sender, Create success check, Open in Blender). | F13, F21 |
| J12 Build and play | builds, deploys and plays | **Build** panel: pipeline stages (World → collision → sectors → lightmaps → mission → GlobalEK text → archives), **Build**, **Deploy to game** (after a confirmation), **Roll back**, the `dist/` build list, and a **Playtest log** (worked / failed plus a note for each build) kept in the project | F15; GlobalEK is built by a script |

Two more principles come from this journey:

- **The viewport is where authoring happens.** Anything with a position is
  created and edited in the viewport, and the panels hold what cannot be
  placed.
- **Everything the user makes can be edited again.** A guard's patrol, an
  objective and a shot keep their parameters. Editing them regenerates what
  they own, as one undo step.

---

## 3. Information architecture and layout

### A. Modes and shell

- **A1. Three modes replace six workspaces.** The top-right switch becomes
  `Mission · Script · Inspect` (`Ctrl+1..3`).
  - **Mission**: the authoring layout below, and the default whenever a
    project or mission is open.
  - **Script**: the program outline, the script editor and the flow graph
    (Phase 5).
  - **Inspect**: the current SCENE, GEOM, ANIM and HEX workspaces as a
    sub-switch inside one mode, unchanged. Loose `.rws`/`.rpc`/`.anm` files
    open here.
- **A2. Home replaces the start page** (J0). It shows project cards with a
  thumbnail (the last viewport frame, cached in the config directory), slot,
  last build and playtest result. The missions under the resource root move
  to "Explore game files". Setup warnings (no resource root, no game root,
  Blender not found) appear as one checklist with buttons that fix them.
- **A3. Opening a project folder works everywhere**: a positional argument,
  drag and drop, recent entries and `File > Open project...`. A folder
  containing `project.csfproj` opens as an authoring project, and a
  mod-project workspace opens as before (F2).
- **A4. Menus are reorganised around authoring.** `File` (projects),
  `Edit` (undo, selection, clipboard, preferences), `View`, `Place` (the
  asset kinds and tools), `Mission` (settings, objectives, triggers,
  cutscene, texts), `Build` (build, deploy, roll back, playtest log) and
  `Help`. The inspection commands move to an `Inspect` menu that appears in
  that mode. The command IDs stay; only the menu paths change.
- **A5. The window title** reads `Checkpoint — Convoy slot • unsaved — CSF
  Mission Editor`, or the file name in Inspect mode.

### B. The Mission mode layout

```
┌ File Edit View Place Mission Build Help ───────────── Mission │ Script │ Inspect ┐
├ ◉ Checkpoint (Convoy)   Map ✓  Layout 42  Actors 9  Zones 2  Objectives 3 ⚠1  Intro 5  Build ✓ 12:03 ┤
├──────────────┬──────────────────────────────────────────────┬────────────────────┤
│ Outliner     │ ⟨tool options: Snap Ground ▾  Grid 50 cm ▾  ⟩│ Properties         │
│ ▸ Players    │ ┌──┐                                         │ ■ OFICIAL   Enemy  │
│ ▾ Enemies    │ │⇱ │                                         │ Transform          │
│   OFICIAL    │ │✥ │            viewport                     │ Identity (class ⊞) │
│   GE_CAMP ⚠  │ │⟳ │                                         │ Behaviour: Patrol  │
│ ▸ Animals    │ │＋│                                         │ Equipment          │
│ ▸ Props      │ │⌇ │                                         │ Used by (3)        │
│ ▸ Zones      │ │▱ │                                         │ ▸ Advanced         │
├──────────────┤ │🎥│                                         │                    │
│ Assets       │ └──┘                                         │                    │
│ [search][⊞≡] │                                              │                    │
├──────────────┴──────────────────────────────────────────────┴────────────────────┤
│ Problems 3 │ History │ Timeline │ Texts │ Output                                   │
├───────────────────────────────────────────────────────────────────────────────────┤
│ ● saved  │ map built 12:03 (2.1 s) │ ⚠ 3 │ OFICIAL (actor 10) │ Blender ●  │ 120 fps │
└───────────────────────────────────────────────────────────────────────────────────┘
```

- **B1. Mission bar** (the second row). Each authoring step (Map, Layout,
  Actors, Zones, Objectives, Intro, Build) shows a count and a status;
  clicking one opens the right panel or tool. It works as a checklist that
  shows what is missing (for example "Objectives ⚠: no mission success")
  without locking the user into a wizard. It replaces the Changes panel's
  project bar. Save, undo and redo move to the toolbar and `Edit`.
- **B2. The Outliner replaces the Explorer in Mission mode.** It groups by
  gameplay meaning: Players, Enemies (by group), Animals, Props & pickups,
  Buildings (project placements), Vegetation (project props), Navigation
  (routes, cover, walk grid), Zones, Cameras & cutscenes, Lights & effects,
  Objectives, Triggers and Scripts. Each row has a kind icon, the name, a
  quiet ID, and a problem badge. Show/hide (eye) and lock toggles appear on
  hover for each row and category, and hidden or locked objects cannot be
  picked in the viewport. **Records owned by a component are nested under
  it**, so the ten intro helpers live under "Intro cutscene" (F7).
  Multi-select, rename (F2), drag to reorder where order matters
  (objectives, shots), and a context menu (Duplicate, Delete, Frame, Isolate,
  Show in Script/Inspect). The storage-oriented Explorer remains in Inspect.
- **B3. Asset browser**: a panel, docked under the Outliner by default. See E3.
- **B4. Properties replaces the Inspector in Mission mode.** It is made of
  cards: *Transform* (position, heading, height rule and anchor), *Identity*
  (name, class shown with a thumbnail, look, faction), and one card per
  component (Behaviour, Usable, Equipment, Objective). *Used by* lists the
  scripts, objectives and triggers that reference the record, as links.
  *Advanced* is collapsed and holds the raw fields, decoded where known
  (collision, flags and explosion as named options with the raw value next
  to them). File offsets, entry numbers and provenance badges show only in
  Inspect mode or with **Developer details** turned on in Preferences (F8).
  With several objects selected, the panel shows the fields they share
  (mixed values shown as `—`).
- **B5. Bottom dock**: **Problems** (E7), **History** (the undo list, now
  including project edits), **Timeline** (the cutscene, E11), **Texts** (the
  project strings, with where each is used), and **Output** (the log, build
  output and the Console). It is **open by default in Mission mode**, and a
  command that asks for a tab always shows it and never toggles it off (F12).
- **B6. Status bar**: save state, build state (fresh, stale, building with
  progress, failed), problem count (click to open Problems), the selection,
  the Blender link, and fps. The hint text that names menu paths goes
  (F21).
- **B7. Default layouts are generated for each mode** at 1280×720,
  1920×1080 and 2560×1440 at 150%, and checked by the harness (T6).
  `View > Reset layout` resets the current mode.

### C. Viewport

- **C1. A vertical tool strip** on the viewport's left edge: Select (`Q`),
  Move (`G`), Rotate (`R`), Place (`P`), Route (`N`), Zone (`Z`), Cover
  (`C`), Camera shot (`K`) and Measure (`M`). `G`/`R` stay (Blender habit).
  `Esc` returns to Select and `Shift+Space` opens a tool pie. A tool's
  options appear in one **tool options bar** across the top of the viewport
  (snap mode, grid size, random rotation for Place, closed or open route for
  Route). This replaces the Changes panel's tool buttons (F10).
- **C2. View controls regrouped.** The ten icon dropdowns become three
  labeled menus: **View** (camera, ortho, frame, bookmarks), **Show**
  (layers: map, collision, markers by kind, labels) and **Overlays** (walk
  grid coverage, sectors, height findings, camera paths). A small
  "what's hidden" badge appears when filters hide things.
- **C3. Overlays that fit authoring.** Areas are drawn as outlines with a
  faint fill, full fill only when selected or hovered (F11). Area volumes
  larger than 50% of the map are drawn outline-only. Routes have direction
  arrows and a pause glyph at each point. Cover points show facing wedges.
  Cameras show a frustum and travel path. Hovered objects get a soft
  highlight, and a line connects a selected record to what references it
  (a guard to its route and cover group).
- **C4. Picking in the viewport**: click to select, Shift to add, Ctrl to
  toggle, drag on empty space to box-select. Alt+click cycles through
  objects stacked under the cursor. Marker hit targets get bigger for
  touchpads.
- **C5. HUD**: the fps and triangle stats move into the Output/debug overlay
  (Developer details). The HUD keeps the tool hint ("Click to add route
  point · Enter to finish · Backspace removes last") and the snap state.

### D. Command palette and discoverability

- **D1.** Every preset, component, tool and asset kind is a palette entry
  ("Add guard on patrol", "Draw zone", "Place: tent", "New objective: reach
  a zone"). Searching "guard" finds the actions, not a tab (F12).
- **D2.** Go to anything ranks gameplay records above chunks and resources
  in Mission mode, and understands `actor:`, `zone:`, `objective:` and
  `script:` prefixes.
- **D3.** Every tool and card header has a **?** that opens a short
  explanation. It replaces the long `dim_text` paragraphs above each tab
  today.
- **D4.** Help → **Keyboard shortcuts** is laid out per mode and printable.
  **Getting started** opens an interactive checklist (the Mission bar) on an
  empty project.

---

## 4. The editing model (underlying changes)

- **E1. Components: recipes that can be edited again.** Each preset or
  recipe instance becomes a stored *component* with its parameters and the
  IDs of what it owns. Components cover guard patrol, guard idle, animal
  patrol, cover group, walk grid, objectives, equipment, tips, intro
  cutscene, and the new usable object, pickup and trigger. Editing a
  parameter deletes the component's owned records and runs the recipe again
  with the same IDs, in one `batch`, as one undo step.
  - Storage: the `mission_ops` text syntax with explicit IDs already
    serializes these recipes, so a component *is* an ops line plus an
    ownership list. It lives in an editor-owned, unpackaged file
    (`mission/components.csfops`) that `MissionEditor` tracks like any
    mission file, so undo, dirty state and save come for free. (Decision
    Q3.)
  - Drift: when the project opens, each component is regenerated in memory
    and compared with the records it owns. If a script was edited by hand,
    the component is marked **Modified** and offers *Keep my edits (detach)*
    or *Regenerate*. Nothing is overwritten silently.
  - Vanilla missions have no components; all their records are unmanaged
    and editable as now.
  - Tests: regenerating from unchanged parameters is byte-identical; after
    every edit, undo restores bytes; parity.sh passes when hello world is
    built from components.
- **E2. One undo history and one save for project and mission.** Project
  edits (text strings, placements, anchors, resnap, lightmap records) become
  undoable steps in the same history, as snapshots of `project.csfproj`,
  which is small. `Ctrl+S` saves both. The map rebuild runs from the
  in-memory project into `build/` (generated output), so the user sees
  building placements before saving. Closing with unsaved project edits
  asks, as mission edits already do (F6).
- **E3. Viewport-first placement.**
  - Ray-to-ground picking against the collision World (`rws::GroundQuery`
    extended with a ray cast), with a fallback to the World's visual
    triangles.
  - Place tool: the selected asset's model follows the cursor, drawn
    translucent, and turns red where it cannot go (no ground, inside another
    prop's footprint). Click places it. Shift keeps the tool active. Wheel
    or `[`/`]` rotates, and **Random rotation** and **Align to slope** (for
    props only) are options.
  - Dragging from the Asset browser into the viewport places an asset the
    same way.
  - Dropping onto another object (the radio on the crate) writes an
    `anchor` height rule relative to that object (handoff item 5 of the
    Blender plan).
  - The asset browser has category tabs (Characters, Animals, Vehicles,
    Props, Pickups, Buildings, Vegetation, Helpers) built from `.TIPO` and
    class data, favourites, "used in this mission", and provenance ("from
    Ransom"; imported on first use, as `place_asset` already does). It shows
    thumbnails of each class's model, rendered offscreen once and cached in
    the config directory, as a grid or a list (F20).
- **E4. Multi-selection.** `SelectionRef` becomes a selection set with a
  primary item. Move, rotate, delete and duplicate apply to the whole set as
  one batch. Also: copy/paste (as ops text on the clipboard, so a user can
  paste into a text editor or into `csf-mod mission-ops`), **Align to
  ground**, **Distribute**, and **Group** (a named selection set kept in the
  project for the Outliner) (F18).
- **E5. Project placements are first-class.** Props, pieces and buildings in
  `project.csfproj` get a `MissionRecordKey` kind (`placement`, keyed by
  their string ID), so they can be selected, moved and deleted in the
  viewport, listed in the Outliner and edited in Properties. While a drag or
  rebuild is under way, the preview draws the placement's Clump or building
  proxy at its new pose, and the World rebuild runs in the background after
  release, as today (F22).
- **E6. Picking references from the viewport.** Every field that refers to a
  record (objective target, route, cover group, kit actor, look-at target)
  has a crosshair button. Clicking it and then an object in the viewport or
  Outliner fills the field, and only valid kinds highlight while picking.
  Combo lists stay as the fallback.
- **E7. Problems as a live model with fixes.** `rwsman_ui_model` gains a
  problem aggregator over:
  - editor refusals
  - `MissionFlow` findings
  - height findings
  - `AuthoringProject::check()`
  - stale build outputs
  - `area_polygon_problems`
  - unresolved classes and animations
  - text range errors
  - component drift

  Each problem carries a severity, a subject key, a stable ID (so a user can
  dismiss it) and an optional fix command. The Mission bar and the Outliner
  badges read from it. The 68 inspection diagnostics stay in Inspect mode's
  Diagnostics panel (F13, F21).
- **E8. A build pipeline with visible stages.** `project-build`, the
  lightmaps, the mission archive and the GlobalEK archive become one
  pipeline with per-stage state (fresh, stale with the reason, running,
  failed with the message), shown in the Build panel and the status bar.
  **Deploy to game** asks for confirmation (it replaces game files), writes
  rollback records as `csf-mod deploy-pak` does, and lists deployments with
  **Roll back** buttons. The **Playtest log** stores `playtest <build-id>
  worked|failed "note"` records in the project, a per-project version of the
  in-game results table in the hello world plan. GlobalEK packaging moves
  from `build_texts.sh` into the core (F15).
- **E9. Creating a project, in the core.** `csf::create_authoring_project`
  (GUI-free), plus `csf-mod project-new <dir> --slot Convoy [--flat
  8000x8000 | --blend file]`, handles what `tools/hello_world/project.py` and
  `build.sh` do for set-up. It picks a free text ID range by scanning the
  projects under `projects_root`, writes `project.csfproj` and `mission/`,
  runs `new_mission`, and writes a flat starter terrain (as `.csfworld`,
  plus a starter `.blend` when Blender is configured). Hello world's
  `build.sh` switches to it; parity is unchanged.
- **E10. Triggers: basic scripts without script text.** A trigger component
  is *When* (event) + *If* (optional: a zone or objective state) + *Do*
  (actions). It compiles to one script of known shape, like the other
  recipes. The first events and actions are only the ones proven in-game or
  in the KB: mission start/INIT, entering a zone, actor killed, object used,
  a custom event, and a timer; complete or fail an objective, show a
  message, raise an event, set the AI alert/combat mode, and enable or
  disable an actor or ghost. Each action's operands are checked against
  `csf_script_signatures`. Actions whose effect has not been confirmed in
  game carry an "unverified" tag. **Convert to script** detaches a trigger
  into ordinary script text.
- **E11. Cutscene timeline.** The intro component is shown as a timeline:
  one strip per shot, with the duration dragged at its edge, plus cut
  markers. In the viewport, the camera start/end and target are gizmos, and
  the travel path is drawn. Scrubbing the timeline moves the view camera.
  **Look through shot** reuses `view_shot`. It is the same recipe as today,
  regenerated on edit.
- **E12. Behaviour switching.** Changing an actor's behaviour on the
  Behaviour card (Idle → Patrol) replaces its component. The route is kept
  when both behaviours have one. The actor's scripts that are not owned by a
  component are never touched.
- **E13. Undo labels and toasts.** Every step has a human label
  ("Move OFICIAL", "Patrol GE_CAMP: add point 4"). Toasts for destructive
  actions carry **Undo**, so users no longer need a confirmation dialog for
  every delete. The option to force a delete while scripts reference the
  record becomes a dialog that lists those references.
- **E14. Text is typed where it is used.** Any field that takes game text
  (objective label, done message, prompt, trigger message) takes the string.
  An ID is allocated from the project's range when the edit is applied, and
  the Texts tab lists each string with its users. Without an authoring
  project, the same fields show an FLI ID picker with a preview of the
  string, clearly labeled (F19).
- **E15. The Blender link.** A **Blender executable** path in Preferences.
  **Edit in Blender** opens the asset's `.blend` with the add-on's project
  path set through `--python-expr`. It creates a starter `.blend` from a
  template when none exists. The status bar dot shows the last Send, and the
  add-on reports its version so the editor can warn about an old add-on.

---

## 5. Scripts (Script mode)

- **S1.** Script mode layout: the program outline (grouped as Mission start,
  Triggers, Behaviours, Cutscene, Unmanaged) with component-owned scripts
  marked and read-only unless detached; the editor; and the script's
  references and "raised by / listens to" on the right.
- **S2.** The editor gets syntax highlighting (opcodes, operand tags,
  numbers, strings), a gutter showing the errors from the existing check
  pass, completion of opcodes and operand tags from
  `csf_script_signatures`, a signature hint for each opcode, and Ctrl+click
  on an operand to go to the actor, zone or animation it names. The
  textarea stays the implementation (ImGui `InputTextMultiline` with a
  highlighting overlay) until that is shown to be too slow.
- **S3.** A **flow graph** view of `MissionFlow` (read-only first): events
  as nodes, scripts as nodes, and edges for raises and listens; unraised
  events and objectives nobody completes are shown as problems. It is drawn
  with ImDrawList, with no new dependency.
- **S4.** A new script starts from a template (`script_template`) that is
  already listening to a chosen event.

---

## 6. Look and feel

- **V1. Tokens stay in `theme.cpp`.** The accent is kept for selection, focus
  and the primary action only. Status tokens (ok, warn, error, dirty) are
  kept. A **kind palette** is added: one colour per gameplay kind (player,
  enemy, animal, prop, building, vegetation, route, cover, zone, camera,
  trigger, objective). It is used everywhere the same kind appears: Outliner
  icons, viewport markers, Properties headers and Problems rows. The
  current viewport colours are its seed. The dark, light and high-contrast
  themes get their text contrast checked by a unit test (WCAG AA 4.5:1 on
  `bg0`/`bg1`/`bg2`).
- **V2. A type scale.** Inter at four sizes (caption 13, body 15, heading
  15 semibold, title 24; built: the planned 11/13/15/20 read too small on 1x
  screens) on a 4 px spacing grid. Iosevka is used only for
  IDs, code, hex and numbers in tables. The status bar switches to Inter
  with tabular numbers.
- **V3. Cards instead of separators.** Property sections are `bg1` cards
  with a 6 px radius, a header row (kind icon, title, **?**, a menu) and
  28 px field rows. Label columns get a fixed width that scales with the
  UI scale.
- **V4. Button hierarchy.** Primary (filled accent, at most one per
  context), secondary (outlined), quiet (icon with tooltip) and destructive
  (red on hover or in confirmations). `ui::widgets` gets helpers for each,
  so panels stop picking styles ad hoc.
- **V5. Icons.** The Lucide subset grows to cover the kinds and tools:
  user, shield, dog, package, building-2, trees, route, map-pin,
  square-dashed, video, flag, scroll-text, zap, hammer, rocket, play, eye,
  eye-off, lock, magnet, grid-3x3, ruler, crosshair, pipette and
  circle-help. `tools/generate_ui_fonts.py` gains the list, and the
  licenses stay recorded.
- **V6. Empty states and progress.** Every panel with nothing in it shows a
  one-line explanation and one primary action (Objectives: "No objectives
  yet — **Add objective**"). Long work shows determinate progress where the
  stages are known (build, load). Toasts are shorter and carry an action
  (Undo, Open folder, Show problems).
- **V7. Viewport look.** A grid that fades with distance, a darker
  unlit-ground tint so markers stand out, a contact disk under each marker,
  billboard kind icons with labels on hover (the current default), and a
  thicker selection outline with a soft glow. None of this changes the
  model rendering.
- **V8. Branding.** In Mission mode the window and Home read "CSF Mission
  Editor". In Inspect mode it stays "rws-man". The executables keep their
  names (decision Q8).

---

## 7. UI test harness

The aim is for UI regressions to fail in `ctest`, and for an agent or the user
to script and check a GUI session without looking at screenshots.

- **T1. Move testable logic into `rwsman_ui_model`.** This covers the
  Outliner tree builder, the problem aggregator (E7), component
  forms/validation, placement math (ray to ground, snapping, anchors),
  selection sets, the tool state machines (route, zone, place, box select)
  and Mission bar statuses. These are pure functions with unit tests. The
  panels only draw them.
- **T2. Scenario scripts.** `rws-man --run-script file.uiscript` replaces the
  `--commands` list with a line-based script (the old flag keeps working and
  becomes sugar for it):
  ```
  open-project ~/dev/csf-mods/hello-world-project
  wait idle
  command view.ortho_top
  tool route
  click-world 3700 0 -3300          # projected through the current camera
  click-world 3500 0 -3500
  key Enter
  expect selection.kind nav_group
  expect count nav_groups 12
  click "Properties/Behaviour/Pause" ; type 4 ; key Enter
  expect history.top "Patrol GE_CAMP: pause 4 s"
  undo
  expect files.identical-to-saved
  screenshot patrol.png
  ```
  Failures stop the run with a non-zero exit code, the line number and a
  state dump. Unknown commands, `goto` without a match and `expect`
  mismatches all fail (F17). `--overwrite` allows rewriting output files.
- **T3. Addressable widgets.** In test mode, the `ui::widgets` helpers record
  each item's label path and rectangle per frame (`"Properties/Behaviour/Pause"`).
  `click`, `type` and `drag` inject events into `ImGuiIO`, so scripts
  exercise real ImGui input handling. Only helper-drawn widgets can be
  addressed, which also pushes panels onto the shared helpers (V4).
- **T4. Deterministic frames.** In script mode the frame loop uses a fixed
  time step, never idles, waits for background jobs with `wait idle`
  (loader, authoring job, thumbnails), fixes the DPI and font atlas, keeps
  toasts from expiring, and seeds any randomness (random rotation).
- **T5. State dumps.** `--dump-state out.json` and `expect <path> <value>`
  read one JSON projection of `AppState`: mode, selection set, open panels,
  editor revision, history labels, record counts by kind, problems by ID,
  build stage states and component states. It gives agents a stable surface
  in place of pixels.
- **T6. Screenshots and goldens, in two tiers.**
  - *Committed goldens* only from **synthetic fixtures** that the test
    builds itself (a minimal scene plus a flat World written by the World
    writer, no game assets), so no game data enters the repository. They
    are compared with a per-pixel tolerance and a changed-area budget, and
    the diff image is written next to the failure.
  - *Local tier*: when `../CSF_unpacks` exists, corpus-backed scenarios
    (hello world, Ransom) run their assertions. Their screenshots go to
    `build/ui-shots/` with an HTML contact sheet for review, and are never
    committed.
  - The matrix: 1280×720, 1920×1080 and 2560×1440 at 150%, with dark and
    high-contrast themes, for Home, Mission with a selection, Script and
    Build.
- **T7. ImGui's own checks as failures.** Scenarios run on the Debug build
  too, with `io.ConfigDebugHighlightIdConflicts` enabled. An ID conflict or
  an ImGui assertion (routed to a handler that records the failure instead
  of aborting) fails the test.
- **T8. `ctest` integration.** A `rws_man_ui_tests` target with the label
  `ui` runs every `tests/ui/*.uiscript`. On Linux it runs under `xvfb-run`
  when there is no display, and is skipped with a clear message when no GL
  context can be made. CI runs the synthetic tier. `./test.sh --ui` /
  `./test.ps1 -Ui` run it locally.
- **T9. The end-to-end journey as a test.** `tests/ui/hello_world_e2e.uiscript`
  (local tier) creates a project in a temporary directory with
  `project-new`, places hello world's actors, props, routes, zones,
  objectives and shots **through viewport clicks and Properties fields**,
  saves, and compares every mission file with `tools/hello_world/parity.sh`'s
  output. It proves the GUI path, not just the operations. Explicit IDs come
  from the script where the recipe needs them.
- **T10. Invariant and monkey tests.** A seeded random walk of commands,
  tool gestures, undo and redo, on the synthetic fixture. It checks that
  nothing asserts, that undoing everything restores byte-identical files,
  and that the selection always resolves. It runs for a bounded number of
  steps in `ctest`, and longer on request.
- **T11. Registry lint.** A unit test checks that every command has a label,
  category, palette keywords and help text; that there are no shortcut
  conflicts within a mode; that every panel and every tool can be reached
  from a menu and from the palette; and that "show" commands never toggle
  their panel off (F12).
- **T12. Performance budget.** A scenario opens Ransom (the largest
  mission) and fails if the median CPU frame time with everything shown
  exceeds a set budget, or if Outliner and Problems rebuilds exceed theirs.
  It guards the aggregation added by E7 and B2.

---

## 8. Smaller fixes found during the audit

These can land at any time, before the phases.

1. The `mission.*` panel commands show and focus their tab; they no longer
   toggle it (F12).
2. With the dock hidden, `mission.flow` shows the Missions tab; it should
   open Changes › Flow (F12).
3. A project folder opens from a positional argument, drag and drop, and
   recents (F2, A3).
4. `--commands` fails on unknown IDs and on `goto` without a match, and
   `--screenshot` gets `--overwrite` (F17).
5. The bottom dock is shown when a mission opens with an editor (F3), until
   the new layout replaces it.
6. Area volumes larger than half the map are drawn as outlines by default
   (F11).
7. Hello world's 8 off-height placements appear in the Diagnostics panel
   with a Resnap action, not only in the status bar (F21).

---

## 9. Delivery order

Each phase ends with its acceptance check, `./test.sh` plus the new UI tests,
`tools/hello_world/parity.sh`, and a Debug-build screenshot pass.

### Phase 0: the harness first, and the fixes

T2, T3 (for the existing widgets), T4, T5, T7, T8, T11, plus the synthetic
fixture (T6), and §8 items 1–7.
**Accept:** a script opens hello world, selects OFICIAL, moves it with the
gizmo by `drag-world`, undoes, and asserts the files are unchanged, in CI
(synthetic fixture) and locally (hello world).

**Done (2026-09-26).** `rws-man --run-script` (`app/ui_script_runner.cpp`,
parser `rwsman/ui_script.hpp`, guide [ui-tests.md](../guides/ui-tests.md)).
Widgets are addressed through ImGui's test-engine hooks
(`IMGUI_ENABLE_TEST_ENGINE`, `app/imgui/`), so every widget can be
addressed, not only the ones drawn by our helpers. ImGui assertions and ID
conflicts are recorded as failures. The fixture is
`tests/ui/make_fixture.cpp`, and `./test.sh --ui` / `ctest -L ui` run the
scripts. `tests/ui/local/hello_world_move.uiscript` is the acceptance check
and passes on Release and Debug. §8 items 1–6 are fixed. Item 7 (height
findings as problems) moves to the Problems panel in Phase 1. The CI job
(`linux-ui-tests`, Xvfb and llvmpipe) has not run yet. The first script run
found mixed-case marker-preset command IDs, which are now lower-case.

### Phase 1: shell and information architecture

A1–A5, B1–B7, C1 (the tool strip hosting the existing tools), C2, C3, D1–D3,
V1–V6, and the E7 problem aggregator (read-only). Components are not in yet:
the Behaviour and Objective cards show only what exists.
**Accept:** the golden matrix (T6) for Home, Mission, Script and Inspect; no
authoring feature reachable only from the bottom dock; each hello world
record reachable from the Outliner in at most two clicks; registry lint
passes.

**Done (2026-09-26).** Built:

- **Shell.** Modes and the panel registry (`app/ui/layout.cpp`, `show_panel`);
  the old Changes tabs are separate panels. The Mission bar, Outliner, Properties
  (cards) and Problems (`rwsman/problems.hpp`) are new.
- **Chrome.** The new status bar, window title and Home. Menus are File, Edit,
  View, Place, Mission, Build, Inspect and Help.
- **Viewport.** Toolbar menus View, Show and Markers; the tool strip with `Q`
  for Select; zone fills stay faint until selected.
- **Look.** Kind colours and icons (`rwsman/entity_kind.hpp`, used by the
  Outliner, Properties, Problems and the viewport markers); 140 Lucide icons;
  a caption size; text on the 15 px body; cards, buttons by role, empty states
  and help markers (`app/ui/widgets.hpp`).
- **Palette.** Every behaviour preset and "Add objective" is a command.
- **Contrast and tests.** A contrast lint over both themes fixed dim text
  (4.34:1 on raised surfaces) and error red (4.41:1). `layout_matrix.uiscript`
  is the T6 matrix: Mission, Script and Inspect at 1280×720 and 1920×1080,
  high contrast and 150%. Every script, and a tour of every panel and
  command, passes on Release and Debug with no ImGui assertions or ID
  conflicts.

Moved to Phase 2: box select and multi-select (C4, with E4), the tool hint in
the HUD (C5), and ranking gameplay first in Go to (D2). Project placements
are listed in the Outliner but not yet selectable (E5).

### Phase 2: editing in the viewport

E3 (ground ray, Place tool, drag from the Asset browser, thumbnails), E4, E5,
C4, C5, the Route and Zone tools, E6.
**Accept:** a scenario places a tent, a crate, and the radio anchored on the
crate, draws a zone and a patrol route, and box-selects and moves three trees
with one undo step; the placements survive a terrain rebuild; T10 runs clean.

**Done (2026-09-26).** Built (`app/viewport_tools.cpp`):

- **Tools.** Place (`P`), Route (`N`), Zone (`B`, since `A` strafes the
  camera) and Cover (`C`) on the tool strip. Each has an options panel in the
  viewport and a hint above the HUD. Place shows where the asset lands and
  which way it faces (`[`/`]` turn, random heading, Shift or "keep placing"
  to continue). Route, Zone and Cover draw a sketch: Enter or a double click
  creates it, Backspace removes the last point, and zones show polygon
  problems while drawing. Each creation is one undo step.
- **Assets panel.** Category chips (Characters, Vehicles, Props, Pickups,
  Buildings). Clicking a row arms the Place tool, dragging a row onto the
  ground places it there, and a double click places it at the view centre.
- **Multi-selection (C4, E4).** Shift+click adds, Ctrl+click toggles, and
  Shift+drag draws a box, both in the viewport and in the Outliner.
  Dragging the primary's gizmo moves the whole set in one undo step (mission
  records as one batch, placements as one project step). Delete, Duplicate
  and "Align to the ground" apply to the set.
- **Placements (E5).** `MissionRecordKey::Kind::placement`: project
  placements are viewport markers, Outliner rows, a Properties card
  (position, heading, height rule and offset) and a gizmo handle. Their
  selection survives the map rebuild, and the rebuild keeps the camera,
  markers, textures and hidden records.
- **Picking (E6).** Eyedropper buttons on the objective target, the kit's
  actor and an actor's new **Height** card ("stands on" another actor or a
  placement, the radio on the crate). A pick is answered by a click in the
  viewport or the Outliner, and the selection comes back afterwards.
- **Go to (D2).** Gameplay records rank first outside Inspect mode, and
  `actor:`, `zone:`, `route:`, `marker:`, `script:`, `class:` and the other
  kind prefixes filter.
- **Tests.** `fixture_viewport_tools.uiscript` covers placing, dragging an
  asset in, the Zone and Route tools, the group move, Ctrl toggle, group
  delete and an eyedropper. `local/hello_world_placements.uiscript` works on
  a copy of hello world: it moves three trees in one step, deletes two,
  checks that they survive the rebuild, and stands the radio on a tree with
  the eyedropper. `fixture_monkey.uiscript` is T10: three seeded walks of
  150 random tool commands, clicks, drags, keys, undo and redo, each undone
  back to identical bytes. The script language gained modifiers on pointer
  steps, `drag-to-world`, `copy`, `monkey` and `undo-all`.

Fixed on the way:

- Viewport clicks were not handled after the pick-event change (restored,
  with the authoring tools' clicks).
- The gizmo grabbed clicks meant for the Place tool.
- A second click on a marker counted as a double click and reset the view.
- Clicking a route link left a selection nothing could resolve (it now
  selects the route).
- Opening a project no longer rewrites `project.csfproj` when a check
  rebuilds nothing.
- Icon buttons have addressable IDs.

Not done: thumbnails and favourites in the asset browser, the translucent
model under the Place cursor with a red "cannot go here" state, dropping
onto an object to anchor it (the Height card's eyedropper does this),
copy/paste, Distribute and Group, and dimming invalid kinds while picking.
Thumbnails move to Phase 6 with V7; the rest are Phase 3 candidates.

### Phase 3: components and one undo history

E1, E2, E12, E13, E14, the Behaviour, Usable, Equipment, Players and
Objectives cards, and the walk grid as a component with a preview.
**Accept:** every hello world recipe edited after creation (add a patrol
point, change a pause, retarget an objective, retype its text) regenerates
byte-identically to the matching ops; parity.sh passes when built from
components; a hand-edited owned script is reported as Modified and never
overwritten.

**Done (2026-09-26).** Built:

- **Components (E1).** `csf/mission_components` (GUI-free): a component is
  its operation lines with explicit IDs, the records they made, and a
  fingerprint of those records, in the workspace's `components.csfops` (a
  `MissionEditor` file, so undo, dirty state and save include it; saved
  beside `authored/`, never packaged). Editing one deletes its records and
  runs its new lines through `MissionEditor::replace_in_place`, which puts
  what was added where the deleted records were in every list (records,
  cross-group links, folders), so the files equal a fresh build of the
  edited lines. Links other components made to its groups are made again.
  Lines without IDs get free ones first. `csf-mod mission-ops --components`
  and `csf-mod mission-components list|check|set|regenerate|detach|delete`.
- **Acceptance.** Hello world built from components is byte-identical
  (`parity.sh --components`), each of its ten components regenerates
  identically, and adding a patrol point, removing one, changing a pause,
  retargeting an objective, retyping its text, changing a shot's length and
  an idle loop each equal a fresh build of the edited operations. A hand edit
  makes a component **Modified**: its card offers Keep my edits (detach) or
  Regenerate, its fields are disabled, and viewport moves then go to the
  records, so nothing overwrites the edit.
- **GUI.** Every recipe the GUI makes (Behaviours presets, the Cover tool,
  objectives, equipment, tips, the intro) is a component. Properties shows its
  card (`app/ui/component_card.cpp`): behaviour switching between patrol and
  post (E12), pause, loop, route points, cover, idle loop, walk animation,
  grid spacing, objectives (kind, target with an eyedropper, text typed into
  the project's string, secondary), kits, tips, shot lengths, and the raw
  lines. The Objectives and Intro tabs show the cards of components without a
  record to select. Moving a component's guard or route point in the viewport
  edits its lines; deleting its record deletes the component (a route point
  is removed from the line). The Outliner nests a patrol's route under its
  guard and the intro's helpers under one row. The walk grid preset has a
  viewport preview.
- **One save (E2).** Project edits stay in memory until Ctrl+S, which saves
  the mission and the project; the title, status bar, Save button and the
  close and open prompts count both. The map rebuild runs from the in-memory
  project; the build's own records (`local.csfproj`) are still written.
- **E13.** Deletes show a toast with **Undo**; a delete refused because
  something refers to the record opens a dialog listing the script references,
  with **Delete anyway**.
- **Tests.** Unit tests on the synthetic mission (regeneration equals a fresh
  build, undo, Modified and regenerate, detach, delete, ID pinning, the list
  file saved beside `authored/`). `fixture_components.uiscript` edits a patrol
  from its card, adds a point, switches behaviour, moves the guard, marks it
  Modified and regenerates it, makes a cover group with the tool, deletes the
  component, force-deletes the player through the dialog, edits objectives in
  their tab and generates a previewed walk grid. `hello_world_placements`
  checks that project edits wait for Save. The script language gained
  `add-component`; `name_last_item` makes combo boxes addressable.

Not done: turning an existing plain actor into a component (adopting it),
keeping an adopted actor's other scripts (E12), group moves and "Align to the
ground" of component records (they edit the records directly, which marks the
component Modified), an **Add objective** on the objectives card, and the
Usable, Pickup and Trigger components (Phase 5 with triggers).

### Phase 4: project lifecycle

A2 Home, E9 (`project-new` in the core and the wizard), E15 Blender link, E8
build pipeline, deploy, roll back and playtest log, GlobalEK packaging in the
core.
**Accept:** from a clean config directory, create "Checkpoint" in the Convoy
slot through the wizard, open it in Blender from the editor, send a changed
terrain, build both archives, deploy and roll back, all without a terminal.
Hello world's `build.sh` uses `project-new` and stays identical.

**Done (2026-09-26).** Built:

- **E9, in the core.** `csf/project_pipeline`: `mission_slots` (the corpus'
  missions with a scene beside a map), `create_authoring_project` (the
  project files, a text ID range clear of the donor's IDs and the other
  projects' ranges, a flat starter terrain of the donor map's most used ground
  texture and surface by upward-facing area, its map build, and the mission
  workspace with the slot emptied and the built map files registered) and
  `csf-mod project-new`. For Convoy the starter ground is `FFLRA11B`/`Tierra`,
  hello world's own, and a second Convoy project gets IDs 1000-1099.
- **E8.** `build_archives` (map, texts and lightmaps brought up to date, then
  the mission archive and GlobalEK.pak into `dist/<build-id>/` with
  `build.json`), `find_original_archive` (the `original` setting, else the
  test install's oldest deployment backup, else its own file), `deploy_build`
  (both archives, each with its rollback state, the first rolled back if the
  second fails), `deployment_state`, `roll_back_build`, and the `playtest`
  record. New records: `playtest` in project.csfproj; `original` and
  `deployment` in local.csfproj. CLI: `project-archives`, `project-deploy`,
  `project-rollback`, `project-playtest`. GlobalEK packaging no longer needs
  `build_texts.sh`.
- **GUI.** Home has **New project...** and lists projects with slot, latest
  build and last playtest; the wizard (`app/ui/project_panels.cpp`) creates
  and opens a project. The Build panel gains Archives (the originals, Build
  archives, the builds), Test install (Deploy after a confirmation, the
  deployments with their state and Roll back) and the Playtest log.
  Commands: `file.new_project` (Ctrl+N), `build.archives` (Ctrl+Shift+B),
  `build.deploy`, `build.edit_in_blender`.
- **E15.** A Blender setting (Preferences, Home's setup list). **Edit in
  Blender** runs `tools/blender/open_project.py`, which loads the CSF add-on
  (installed or from the source tree), sets the project and csf-mod, and makes
  `terrain.blend` from the starter terrain when there is none. The status bar
  shows when Blender last sent. A Send while the project has unsaved edits
  merges the registered assets instead of being ignored.
- **Fixes on the way.** Build records (`output` lines) no longer mark the
  project unsaved; a project without assets or placements keeps the slot's
  map instead of failing to build; the playtest button moved left of the
  toasts, which covered it.
- **Tests.** Unit tests for the new records, slots, text ranges and originals.
  `fixture_new_project.uiscript` runs the wizard on the fixture. The local
  `new_project_pipeline.uiscript` (`RWSMAN_CORPUS`, `RWSMAN_TEST_INSTALL`)
  is the acceptance run: create Checkpoint in Convoy's slot, build both
  archives, deploy into a copy of the install, roll back, record a playtest,
  save, all through the GUI. Headless Blender checks: the starter `.blend`
  is made and Send exports it back. The script language gained `setting`,
  `remove` and path expansion in `type`.

Not done: hello world's `build.sh` still sets up its project itself (its
recipe writes the mission from `scene.py` into a fresh workspace, which an
emptied slot would only get in the way of); project thumbnails on Home;
the add-on reporting its version; deploying also launching the game (Q7).

### Phase 5: scripts and triggers

E10, S1–S4.
**Accept:** "kill the officer → camp alert" and "enter zone → complete
objective" built as triggers match the equivalent hand-written scripts; the
editor highlights and completes the opcodes in hello world's scripts; the
flow graph shows v10's unraised INIT as a problem when INIT is removed.

**Done (2026-09-26).** Built:

- **E10, triggers.** `csf::Trigger` (`csf/mission_recipes`): When (mission
  start, the player enters a zone, an actor dies, the player uses an object,
  a custom event, a timer), If (an objective is or is not complete), Do
  (complete an objective, show a message, raise an event, sound the alarm,
  set an actor's alert or combat behaviour, make an object usable or not,
  win the mission). One trigger script of known shape, plus a START_GAME
  setup script for zones (the player's zone events) and used objects (the
  ghost). Fires once: a zone stops reporting the player, the others end with
  TRIGGER_OFF, as Convoy's alarm script does. The `trigger` operation line
  makes it a component. **Acceptance:** "kill the guard, sound the alarm"
  and "enter the zone, complete objective 1" compile byte-identically to
  the same scripts written by hand (unit test). Proven by default (Q9): the
  custom-event When and the alarm (Convoy's CREA_ESTIMULO_ACUSTICO plus
  ACTIVAR_ALARMA, not yet played in a mission of ours) appear only with
  "Offer unverified events and actions" and are marked unverified.
- **GUI.** A Triggers section in the Objectives tab: the New trigger form and
  a card per trigger (When/If/Do with eyedroppers for zones and actors,
  typed message texts with a project), **Convert to script** (detach).
- **S1.** The Script mode outline groups scripts as Mission start, Triggers,
  Behaviours (actor scripts), Made by recipes (with the component's name) and
  Cutscene. A recipe's script is read-only in the editor, with Detach and
  edit.
- **S2.** The editor (`app/ui/script_editor.cpp`, text logic in
  `rwsman/script_syntax`): syntax colours drawn over the input (theme
  `Syntax` colours), the syntax error's line marked as it is typed, the
  signature table's advisories, Tab completion of opcodes and operand tags
  (most used first, the list under the word), the current opcode's signature,
  and Ctrl+click on an operand to go to the actor, zone, marker, route or
  script it names.
- **S3.** The Flow panel's Graph: events, scripts and objectives in columns,
  edges for starts, raises, sets up and completes; an event that starts
  scripts but that nothing raises, and an objective nothing completes, are
  red. Hover to trace, click a script to open it.
- **S4.** New script... asks for the event it listens to (the engine events
  hello world uses, and the mission's own raised events) and whether it is
  global.
- **Tests.** Unit tests for the trigger shapes and the syntax helpers.
  `fixture_triggers.uiscript` (form, eyedropper, card, unverified toggle),
  `fixture_scripts.uiscript` (Tab completion applied, and INIT becoming
  unraised in the flow once its sender is deleted). Goldens updated for the
  outline and editor.

Not done: the If on zone state (only objectives), Up/Down to choose among
completions (Tab takes the first), the graph's layout for large shipped
missions (a long column; no zoom), and hello world's v10 file checked
through the GUI (the fixture reproduces the unraised INIT instead).

### Phase 6: cutscene timeline, the full journey, docs

E11, V7, V8, T9, T10 (long run), T12. `docs/guides/gui.md` and
`docs/guides/mission-editor.md` rewritten around the journey, and
`AGENTS.md` updated for the modes, the harness and where tests go.
**Accept:** T9 passes (hello world rebuilt through the GUI, byte-identical),
and a playtest of a GUI-made mission that is not hello world is recorded in
its playtest log.

---

## 10. Decisions

The user accepted every recommendation (2026-09-26); they are the plan.

| # | Question | Decision |
|---|---|---|
| Q1 | Replace the six workspaces with `Mission · Script · Inspect`, keeping the inspection tools inside Inspect? | Yes |
| Q2 | Shortcut scheme: Blender-like (`G`/`R`, click select, Shift add), Unity-like (`QWER`), or a mix? | Blender-like tools with `Q` for select, since authors already work in Blender |
| Q3 | Where components are stored: `mission/components.csfops` (tracked by `MissionEditor`, undo for free) or records in `project.csfproj`? | `components.csfops`: one format with `csf-mod mission-ops` |
| Q4 | Project edits go through undo and an explicit save, instead of saving immediately? | Yes (E2) |
| Q5 | UI goldens committed only from synthetic fixtures, with corpus scenarios run locally? | Yes: keeps game data out of the repository |
| Q6 | `AGENTS.md` says tests go in `tests/document_tests.cpp` (4,200 lines). Allow `tests/ui/` for scenario scripts and a second unit-test file for the UI model? | Yes |
| Q7 | Should **Deploy** also launch the game (the Windows executable, or Wine/Proton on Linux)? | Deploy and roll back only at first; launching later |
| Q8 | Name the product "CSF Mission Editor" in Mission mode? | Yes; the binaries keep their names |
| Q9 | Should triggers (E10) be limited to KB-proven actions, or also offer unverified ones with a warning? | Proven by default, unverified behind a toggle |

## 11. Risks

- **Component regeneration versus hand edits.** Drift detection must never
  silently overwrite a user's script edits. The Modified state and explicit
  detach cover this, and T10 exercises it.
- **Scope.** Phases 1 and 2 on their own fix most of the audit. Phases 3–6
  can ship one at a time, and none of them blocks the in-game checks still
  pending from the Blender plan (stage 2 hill, stage 7 bake, spike A1).
- **ImGui limits.** A timeline, a node graph and thumbnails are custom
  ImDrawList and FBO work. Nothing here needs a new UI library. Dear ImGui
  Test Engine was considered for T2–T3; its licence is not MIT and it would
  add a dependency, so the harness is our own and small.
- **Headless GL in CI.** Needs Mesa llvmpipe under `xvfb-run`; if CI cannot
  make a context, the UI tier is skipped rather than failed, and runs
  locally.
- **Thumbnail cost.** Thumbnails are rendered lazily on a budget of a few
  per frame and cached by the model file's hash in the config directory,
  never in the project or the working directory.
