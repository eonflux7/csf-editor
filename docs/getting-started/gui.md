# GUI usage

`rws-man` is a workbench: a menu bar with workspace tabs across the top, an
**Explorer** on the left, the **viewport** (or script listing, or hex view) in the
middle, an **Inspector** on the right, an optional bottom dock (**Console**,
**Diagnostics**, **References**, **Changes**, **Missions**), and a status bar. With
no document open, a start page lists recent files, the missions under your resource
root, and the key bindings.

- **Open** a mission with `File > Open mission...` (`Ctrl+Shift+O`), pick one from
  the start page or the Missions tab, or drop an `.scn`, `.rpc`, `.rws`, or `.anm`
  file on the window. Missions load on a worker thread with a progress overlay and
  a **Cancel** button; the current mission stays untouched until the new one has
  loaded completely. Set a **resource root** in `Edit > Preferences...` to list the
  missions under it (`<root>/Maps/*/*.scn` or `<root>/<package>/Maps/*/*.scn`).
- **Workspaces** are the tabs `MISSION SCRIPT ANIM SCENE GEOM HEX` (`Ctrl+1`...`Ctrl+6`;
  bare `1`-`4` still work while the viewport is hovered). A loaded document opens in
  `SCENE`, or `GEOM` and then `HEX` when it has no scene; a mission opens in
  `MISSION`.
- **Layout.** Panels dock, tab, resize, collapse, and float. Each workspace keeps its
  own layout (the script workspace gives the listing more width), and the layouts
  persist between launches. `View > Reset layout` restores the current workspace's
  default. `Ctrl+B` / `Ctrl+I` / `Ctrl+J` toggle the Explorer, Inspector, and bottom
  dock, and `Ctrl+Space` maximizes the viewport.
- **Go to anything** (`Ctrl+P`) searches actors, navigation groups and points,
  dummies, areas, lights, effects, scripts, variables, BDD classes, animations,
  chunks, and resources. `0x2C79D6` jumps to an offset, `#17` to an entry index. The
  **command palette** (`Ctrl+Shift+P`) searches every command the same way. Menus,
  shortcuts, the palette, and `Help > Keyboard shortcuts` all read one command
  registry, so they cannot drift apart.
- **History.** `Alt+Left` / `Alt+Right` (and mouse buttons 4/5) walk back and forward
  through selections, restoring the workspace and camera. Following a cross-reference
  (a script operand to its actor, a "used by" row to the script instruction, a class
  to its BDD record) pushes onto the history, so you can always return.
- **Inspector.** Selection only. A breadcrumb (`Ambush.scn > Actors > Espia > class
  0x37 > Objetos.bdd#1659`) shows where the record lives, and every segment is a
  link. Values are shown in property grids: click a value to copy it, right-click an
  integer for hex/decimal, an angle for degrees/radians, a vector for per-component
  copy. Each value carries a **provenance badge**: green check = proven (parsed with
  the expected type, or resolved uniquely), `~` cyan = inferred (candidate,
  fallback, or non-exact resolution), `?` violet = unknown/raw (preserved, meaning not
  decoded), and an amber triangle = diagnosed (missing or ambiguous). Hover a badge
  for the evidence. The `01/10` button opens the raw CSFFBS bytes behind a value in
  place. Pin the inspector to keep a record while you browse, or open a second
  inspector to compare two records. Fields are read-only; the lock icon marks where
  the guarded-authoring editors will attach.
- **Explorer.** One frame for every workspace: a search prompt, filter chips
  (kind, diagnostics, dirty), counts, then the tree. Rows show a kind icon,
  a diagnostic marker, and a dirty marker. Right-click any row for Frame, Isolate,
  Copy ID / identity / path, Show in hex, Show references, and Export. The mission
  Explorer also lists the **Classes** in the object database and the **Resources**
  of the mission graph (resolved, ambiguous, missing).
- **Console and toasts.** Loads, exports, saves, screenshots, and settings problems
  are logged with timestamps and severity in the Console (copy all, save to a new
  file, filter by level). Export and save results also show a short toast with an
  **Open folder** action. The status bar shows the latest line.
- **Diagnostics** merges RWS, CSFFBS, mission, resource, object-database, animation,
  and script diagnostics into one sortable, filterable table; clicking a row selects
  its source.
- **Screenshots** (`F12` or the camera button) save a PNG in `<settings>/screenshots`.

Actors, navigation points/links, dummies, extruded areas, SCN-colored light-radius
rings, and effects placed through referenced dummies share the existing 3D camera
and work in perspective and all orthographic projections. The viewport's floating
toolbar holds shading, projection, **Frame**, **Layers**, **Markers**, focus and
height-slice toggles, a marker filter, **Measure**, **Clip**, a **View** popover, and
screenshot. A stats HUD sits at the bottom left, an axis gizmo at the bottom right
(click an axis to snap to an orthographic view), and hovering a marker names it
before you click. The rarely needed rendering, texture/lightmap, collision-style,
BSP, surface, and selected-triangle controls live in the dockable **Render settings**
panel (`Tools > Open scene / collision tools`).

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

The menus group document lifecycle, layout, preview utilities, export operations,
and help. `Ctrl+O` opens an RWS and `Ctrl+S` retains the safe save-copy behavior.

## Settings, layouts, and logs

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
| Double-click | Frame the current geometry or scene |
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

## Mission editing

With a mission open, the Inspector's **Edit** section changes the selected actor,
dummy, light, navigation point, area or map prop, and **All fields** edits any
stored value. In the viewport, **G** toggles the move tool (drag the center along
the ground, snapping to the collision surface; drag an arrow for one axis) and
**R** the rotate tool (**Ctrl** snaps to 15 degrees, **Esc** cancels a drag).
**Ctrl+D** duplicates the selection and **Delete** removes it (refused while scripts
still reference it; **Shift+Delete** forces). The Script workspace edits scripts as
text. **Ctrl+Z** / **Ctrl+Y** undo and redo, **Ctrl+S** saves the mission project,
and **Ctrl+E** exports the mission archive. The **Changes** panel holds the
history, the changed files, mission properties, adding actors and importing
classes or animations from another mission. `rws-man --project <folder>` opens a
saved project. Set the game folder in Preferences so the export finds the shipped
archive. See [Mission editor](../guides/mission-editor.md).

## Editing and saving

The `HEX` workspace permits raw payload-byte edits (type the new value and press
`Enter` or leave the field). Edited bytes are highlighted, counted in the status bar,
listed in the **Changes** panel, and marked in the Explorer. Changes are kept in
memory until you choose **Save copy**, which writes `<original>.edited.rws` (or a
numbered new file). The GUI does not overwrite the loaded asset. **Reload edited bytes** refreshes the preview from the
current in-memory data.

Raw editing can still create a game-invalid file. Work on copies and test modified
assets in a disposable game installation.
