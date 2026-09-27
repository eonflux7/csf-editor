# GUI usage

`rws-man` is the CSF Mission Editor. It has three modes, switched at the top
right of the menu bar: **Mission** (`Ctrl+1`) for making missions, **Script**
(`Ctrl+2`) for the mission's programs, and **Inspect** (`Ctrl+3`) for the
file-level workbench, where the window is titled `rws-man`. Inspect groups the
**Scene**, **Geometry**, **Animation** and **Hex** workspaces
(`Ctrl+4`...`Ctrl+7`; bare `1`-`4` still work while the viewport is hovered).
The design behind the layout is [the UX plan](../plans/editor-ux-redesign.md),
and [Mission editor](mission-editor.md) walks through making a mission from
start to finish.

## Home and opening files

With nothing open, **Home** lists recent projects (with their slot, latest
build and last playtest), **New project...**, the shipped missions under your
resource root and a setup checklist.

- **Open** a project folder (`File > Open project...`, or drop the folder on the
  window), a mission with `File > Open mission...` (`Ctrl+Shift+O`), or an `.scn`,
  `.rpc`, `.rws` or `.anm` file by dropping it. Missions load on a worker thread
  with a progress overlay and a **Cancel** button; the current mission stays
  untouched until the new one has loaded completely.
- Set a **resource root** in `Edit > Preferences...` (or Home's setup list) to
  list the missions under it (`<root>/Maps/*/*.scn` or
  `<root>/<package>/Maps/*/*.scn`); the game folder there is where builds are
  deployed.

## Mission mode

- **Mission bar.** Under the menu: the project, and one step per stage of the
  mission (map, actors, zones, objectives, intro, problems) with its count and
  status; click one to go there. **Save** and **Build...** are at its right.
- **Outliner** (left). The mission by what things are: players, enemies,
  animals, vehicles, usable objects, pickups, props, the project's buildings
  and map props, zones, routes, cover, markers, camera paths, cutscene helpers,
  lights and effects, plus the objectives. Click a row to select it,
  double-click to frame it, Shift+click to add and Ctrl+click to toggle; the
  eye hides it in the viewport and the lock keeps it from being picked there.
- **Assets** (under the Outliner). Classes of every discovered mission and the
  project's buildings, by category, with a search: click one and then the
  ground, drag it onto the ground, or double-click it to place it at the view
  centre. **Import a class without placing it** copies a class or an animation
  from another mission.
- **Properties** (right). The selection as cards (Behaviour for a guard or
  animal without one, Transform, Identity, Scripts, Animation overrides,
  Advanced, All fields, Used by, and the card of the component that made it),
  or the mission at a glance when nothing is selected. Selecting something in
  the viewport or the Outliner brings it in front of the tabs beside it (an
  eyedropper pick does not). File offsets and provenance appear only with
  **Developer details** in Preferences. **Objectives** (and triggers),
  **Behaviours** and **Mission** (players, kits, tips, score, environment) are
  tabs beside it.
- **Bottom.** **Problems** (everything the editor checks, with a link to each
  subject and a fix where there is one), **History** (undo), **Intro cutscene**
  (the timeline), **Texts**, **Build** and **Output**.
- **Viewport.** The toolbar has three menus (**View**: shading, projection,
  framing and options; **Show**: map layers and clip planes; **Markers**:
  marker display), focus mode, the height slice, the marker filter and the
  screenshot button. The **tool strip** on the left edge has Select (`Q`),
  Move (`G`), Rotate (`R`), snap to the ground and measure, and the authoring
  tools: Place (`P`), Route (`N`), Zone (`B`) and Cover (`C`).

Editing in the viewport:

- **Move** drags the gizmo's centre along the ground, snapping to the collision
  surface (**Alt** toggles snapping during a drag), or an arrow for one axis;
  **Rotate** turns the heading (**Ctrl** snaps to 15 degrees). **Esc** cancels
  a drag. Moving one of a multi-selection moves them all in one undo step.
- **Place** puts the asset picked in Assets where you click (`[`/`]` turn it,
  Shift keeps placing). **Route**, **Zone** and **Cover** take the points you
  click: **Enter** or a double click creates it, **Backspace** removes the last
  point, **Esc** cancels.
- Shift+click adds to the selection, Ctrl+click toggles, and Shift+drag
  selects everything in a box. **Ctrl+D** duplicates the selection and
  **Delete** removes it; a toast offers **Undo**. Deleting a record that
  scripts or the mission still use opens a dialog that lists those references
  and can delete it anyway (**Shift+Delete** skips it).
- The project's placements (trees, buildings, donor pieces) are selectable like
  mission records: Properties edits their position, heading and height rule,
  and a move or delete rebuilds the map in the background.
- Fields that name a record (an objective's zone, a kit's player, what an
  actor stands on) have an eyedropper: click it, then the record in the
  viewport or the Outliner. Classes, animations, weapons, objectives and
  texts are picked by name from searchable lists, never typed as IDs; an
  animation list has a play button that previews the clip on the actor.
- While the **Intro cutscene** panel is shown (or one of the intro's records is
  selected), each shot's camera path, its direction, its target and the sight
  lines are drawn, the selected shot brighter, with the camera at the
  playhead.

## Script mode

The Outliner lists the scripts by what they do (mission start, triggers, actor
behaviours, those made by recipes, the cutscene); a recipe's script is
read-only until detached. The editor colours the text, marks a syntax error's
line as you type, completes opcodes and operand tags with **Tab**, shows the
current opcode's operands, and **Ctrl+click** on an operand goes to the actor,
zone, marker, route or script it names. **New script...** starts one that
listens to a chosen event. The **Flow** panel's **Graph** shows which events
start which scripts, who raises them and what completes each objective, with
events nothing raises and objectives nothing completes in red; hover to trace
and click a script to open it.

## Inspect mode

- **Inspector** (Script and Inspect modes). Selection only. A breadcrumb
  (`Ambush.scn > Actors > Espia > class 0x37 > Objetos.bdd#1659`) shows where
  the record lives, and every segment is a link. Values are shown in property
  grids: click a value to copy it, right-click an integer for hex/decimal, an
  angle for degrees/radians, a vector for per-component copy. Each value
  carries a **provenance badge**: green check = proven (parsed with the
  expected type, or resolved uniquely), `~` cyan = inferred (candidate,
  fallback, or non-exact resolution), `?` violet = unknown/raw (preserved,
  meaning not decoded), and an amber triangle = diagnosed (missing or
  ambiguous). Hover a badge for the evidence. The `01/10` button opens the raw
  CSFFBS bytes behind a value in place. Pin the inspector to keep a record
  while you browse, or open a second inspector to compare two records.
- **Explorer** (Script and Inspect modes). A search prompt, filter chips (kind,
  diagnostics, dirty), counts, then the tree. Rows show a kind icon, a
  diagnostic marker, and a dirty marker. Right-click any row for Frame,
  Isolate, Copy ID / identity / path, Show in hex, Show references, and
  Export. The mission Explorer also lists the **Classes** in the object
  database and the **Resources** of the mission graph (resolved, ambiguous,
  missing). For a RenderWare file it shows the chunk tree and decoded CSF
  scene instances; chunk rows are colored by declared clump size (with a
  legend and an option to turn it off), and truncated chunks are marked as
  errors.
- **Diagnostics** merges RWS, CSFFBS, mission, resource, object-database,
  animation, and script diagnostics into one sortable, filterable table;
  clicking a row selects its source.
- The rarely needed rendering, texture/lightmap, collision-style, BSP, surface,
  and selected-triangle controls live in the dockable **Render settings** panel
  (`Tools > Open scene / collision tools`).

## Finding things

- **Go to anything** (`Ctrl+P`) searches actors, navigation groups and points,
  dummies, areas, lights, effects, scripts, variables, BDD classes, animations,
  chunks, and resources. `0x2C79D6` jumps to an offset, `#17` to an entry
  index. The **command palette** (`Ctrl+Shift+P`) searches every command the
  same way. Menus, shortcuts, the palette, and `Help > Keyboard shortcuts` all
  read one command registry, so they cannot drift apart.
- **History.** `Alt+Left` / `Alt+Right` (and mouse buttons 4/5) walk back and
  forward through selections, restoring the workspace and camera. Following a
  cross-reference (a script operand to its actor, a "used by" row to the
  script instruction, a class to its BDD record) pushes onto the history, so
  you can always return.
- **Output and toasts.** Loads, builds, saves, screenshots, and settings
  problems are logged with timestamps and severity in the Output panel (copy
  all, save to a new file, filter by level). Results also show a short toast
  with an action (**Undo**, **Open folder**, **Show problems**). The status bar
  shows the latest line.
- **Screenshots** (`F12` or the camera button) save a PNG in
  `<settings>/screenshots`.

## Viewport and markers

Actors, navigation points and links, dummies, extruded areas, light-radius
rings, and effects placed through referenced dummies share the 3D camera and
work in perspective and every orthographic projection. A stats HUD sits at
the bottom left, an axis gizmo at the bottom right (click an axis to snap to
an orthographic view), and hovering a marker names it before you click. The
grid fades out with distance from its centre, and in a mission the ground
without a lightmap is drawn a little darker so markers stand out; each actor
has a contact shadow on the ground under its marker, and the selection a
thicker outline with a soft glow.

Mission markers are drawn on the GPU and stay readable in dense missions:

- **Depth-aware.** Markers behind level geometry are faded (or hidden) instead of
  showing through walls; the selection and its relations stay visible.
- **Shapes per kind** (circle, diamond, square, triangle, ring) with icon badges when
  few markers are on screen. Heading ticks have a fixed pixel length, and markers
  fade out beyond a multiple of the orbit distance.
- **Merged markers.** Markers within a few pixels merge into one, shown by the most
  important member with a ring and a count; clicking opens a picker. Clicking the
  same spot again cycles through overlapping markers. Two-way navigation links draw
  as one line without arrows.
- **Focus mode** (`Z`) dims everything unrelated to the selection: an actor's spawn
  point, an effect's dummy, a navigation point's links and neighbors, and the areas
  an entity stands in. Related markers are highlighted even with focus off, and
  hovering highlights a marker and its lines.
- **Labels:** off, selected, hovered, near the pointer, or all, placed without
  overlaps. Hold `L` over the viewport to show all of them for a moment.
- **Off-screen selection:** an arrow on the viewport edge points at it; click it to
  frame the selection.
- **Filter** (`/`): names, sublayers, or layer text; `#123` for an entry and
  `kind:nav` for a layer. Non-matching markers are dimmed or hidden.
- **Layers** can be split into sublayers (navigation groups, actor factions or
  classes, effect classes, areas, cutscenes); `Alt`-click soloes a layer or
  sublayer. Presets (Navigation, Scripting, Cinematics, Lighting, Collision, and
  your own) switch layer sets in one click.
- **Height slice** (`Y`) shows one floor around the selection or camera target, and
  can cut the level geometry too.
- A **legend** lists visible / total markers per layer, and a **minimap** (`M`)
  shows marker density from above; click or drag it to move the camera.

Marker display options and layer visibility are saved in `settings.ini`.

For the parsed RenderWare tree, the Explorer in `SCENE`, `GEOM`, and `HEX` shows the
chunk tree and decoded CSF scene instances. Chunk rows are colored by declared clump
size (with a legend and an option to turn it off); truncated chunks are marked as
errors. The `HEX` workspace shows typed fields and the editable raw payload.

## Settings, layouts, and logs

Panels dock, tab, resize, collapse, and float. Each workspace keeps its own
layout, and the layouts persist between launches. `View > Reset layout`
restores the current workspace's default, and `View > Panels` shows any panel.
`Ctrl+B` / `Ctrl+I` / `Ctrl+J` toggle the left, right and bottom panels, and
`Ctrl+Space` maximizes the viewport. `View > Theme` switches between the dark
and high-contrast themes.

Everything the GUI writes for itself lives in one per-user directory, never in the
working directory or the repository:
`%LOCALAPPDATA%\CSF RWS Tools\` on Windows, `$XDG_CONFIG_HOME/csf-rws-tools/` (or
`~/.config/csf-rws-tools/`) on Linux. It holds `settings.ini` (resource root, UI
scale, theme, recent files and pairings, panel visibility, viewport defaults, export
policy, marker display options, and camera bookmarks), `layout.ini` (dock layouts), `screenshots/`, and, in
Debug builds, `rws-man-debug.log`. `settings.ini` is plain text and safe to edit; a
missing file yields defaults, and a corrupted one falls back to defaults field by
field and reports what it ignored in the Console.

`Edit > Preferences...` sets the resource root, UI scale (80-200%, on top of the
operating system's display scale; text is rasterized at the physical pixel size;
`Ctrl+=` / `Ctrl+-` / `Ctrl+0` step it by 10% or reset it from anywhere),
theme (dark or high-contrast), viewport defaults, performance, and the export policy. The
default export policy is **new files only**: an export never replaces an existing
file, it writes a numbered new one (`map.scene.2.gltf`). Choose *Confirm before
overwriting* to write to the usual name and be asked first.

Under *Performance*, the window stops redrawing while nothing changes (input, camera
motion, playing animations, and pending work wake it; the status bar then reads
`idle`). *Frame rate limit* caps drawing below the display refresh rate, *When
unfocused* caps it while another window has focus (15 fps by default), and *Frame
timings in the stats HUD* adds CPU and GPU frame times plus draw and culled batch
counts to the viewport HUD.

## Camera controls

| Input | Action |
| --- | --- |
| Left drag | Look from the current camera position |
| Right drag | Orbit the focus point |
| Middle drag | Pan |
| Mouse wheel | Zoom |
| Double-click | Frame the current geometry or scene (on empty ground; on a marker it is a second click) |
| Shift+click / Ctrl+click / Shift+drag | Add to the selection / toggle / box select (Select tool) |
| `Q` / `G` / `R` | Select / Move / Rotate tool (viewport hovered) |
| `P` / `N` / `B` / `C` | Place / Route / Zone / Cover tool (viewport hovered) |
| `W` / `A` / `S` / `D` | Move horizontally while the viewport is hovered |
| `Q` / `E` | Move down/up |
| `Shift` | Move faster |
| `F` / `Home` | Frame the selection / frame everything (viewport hovered) |
| `Z` / `Y` / `M` / `/` | Focus mode / height slice / minimap / filter markers (viewport hovered) |
| Hold `L` | Show all marker labels (viewport hovered) |
| Numpad `1` / `3` / `7`, `5` | Front, side, and top views; toggle perspective |
| `Shift+1`...`9` | Recall camera bookmark (per mission) |
| `Ctrl+Shift+1`...`9` | Store camera bookmark |
| `F12` | Save a PNG screenshot |

The whole-scene view also has a logarithmic movement-speed control for large maps.
Orbit/look gestures are disabled in fixed orthographic views; pan and wheel zoom
remain available.

## Raw byte editing (Inspect)

`Ctrl+O` opens an RWS file. The `HEX` workspace shows typed fields and permits
raw payload-byte edits (type the new value and press `Enter` or leave the
field). Edited bytes are highlighted, counted in the status bar,
listed in the **Changes** panel, and marked in the Explorer. Changes are kept in
memory until you choose **Save copy**, which writes `<original>.edited.rws` (or a
numbered new file). The GUI does not overwrite the loaded asset. **Reload edited bytes** refreshes the preview from the
current in-memory data.

Raw editing can still create a game-invalid file. Work on copies and test modified
assets in a disposable game installation.
