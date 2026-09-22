# csf-rws-tools

csf-rws-tools is a Windows and Linux inspection and reverse-engineering toolkit
for RenderWare Binary Stream (`.rws`) assets from *Commandos: Strike Force*. It
provides the `rws-man` graphical chunk browser and 3D scene preview, command-line
analysis tools, OBJ/glTF export, and a Blender add-on for inspecting and rebaking
the game's lightmaps.

![The rws-man workbench: Explorer, 3D viewport, and Inspector showing a Commandos: Strike Force mission](docs/images/csf-rws-tools-scene-preview.png)

The project is under active development. It understands many structures used by
*Commandos: Strike Force*, but it is not a general-purpose RenderWare editor and
does not claim complete format support. Unknown and truncated data is preserved so
that partially understood assets can still be inspected safely.

## Features

- Bounds-checked parsing of little-endian RenderWare chunk streams.
- Read-only, bounds-checked parsing of generic `CSFFBS` documents with retained
  raw tables, source offsets, diagnostics, and validated group/array trees.
- Schema-aware inspection of Clumps, Geometry, Materials, Worlds, Frame Lists,
  Atomics, Skin, HAnim, User Data, Bin Mesh, MatFX, and other common plugins.
- ANM `0x1B` decoding, track recovery, pose evaluation, selected-actor CPU
  skinning, playback controls, root motion, and animation glTF export.
- Typed `Anims.bdd` catalogs and conservative CSC cutscene basic-block lanes
  with explicit branches and runtime waits. A dedicated Animation workspace
  traces SCN actor script IDs through GSC `PLAY_ANMBDD*` actions to exact BDD
  records and highlights cutscene camera dummies.
- Decoding of game-specific Pyro Studios metadata, Physics Body/Ragdoll data, and
  CSF scene-instance records.
- A Dear ImGui desktop application with a chunk tree, typed inspectors, hex
  editing, drag-and-drop loading, and save-to-copy behavior.
- Depth-tested OpenGL previews for individual geometry and assembled scenes,
  including DDS and PNG base textures, secondary-UV lightmaps, material
  diagnostics, and wireframe modes. Map previews automatically discover a
  same-directory
  `_col.rws` sibling and can show its recovered level collision as a translucent,
  surface-colored, wireframe, or X-ray overlay.
- Shared, range-checked World-sector and validated BSP topology recovery used by the GUI, glTF exporter,
  `rws-info`, and `rws-corpus`, while the conservative parsed chunk tree remains
  unchanged.
- Wavefront OBJ export for individual geometry and collision-only Worlds.
- glTF 2.0 export for Clumps and assembled scenes, with two UV channels,
  materials, world sectors, CSF placements, and a texture/source manifest.
- Corpus-wide inventory and validation tools for reverse-engineering collections
  of `.rws` files.
- A Blender 4.0+ add-on for resolving exported materials, previewing lightmaps,
  preparing Cycles bakes, and staging game-ready DXT1/DXT3 DDS files.

No game files are included. You must supply assets from your own installation.
This project is not affiliated with Pyro Studios, Eidos Interactive, or the
RenderWare rights holders.

## Project layout

| Component | Purpose |
| --- | --- |
| `rws-man` | Desktop chunk inspector, hex editor, and 3D preview |
| `rws-info` | Inspect, validate, and export one `.rws` or Clump `.rpc` file |
| `rws-corpus` | Recursively inventory a directory of `.rws` and `.rpc` files |
| `rws_core` | Parser, typed decoders, and export library used by all tools |
| `csf-info` | Summarize, validate, search, and inspect one `CSFFBS` document |
| `csf-mod` | Guarded CSFFBS editing, changed-only staging, verified PAK packaging via `pakman-cli`, deployment, and rollback |
| `csf_core` | Generic `CSFFBS` parser, typed views, lossless authoring, and mod-project model |
| `tools/blender/rws_lightmaps` | Blender material, bake, and DDS staging add-on |

Format notes live in [docs/rws-format.md](docs/rws-format.md), corpus results in
[docs/corpus-findings.md](docs/corpus-findings.md), and planned work in
[docs/roadmap.md](docs/roadmap.md).
The generic container grammar is recorded in
[docs/csffbs-format.md](docs/csffbs-format.md), with the mission-workbench plan in
[docs/the-great-shift/README.md](docs/the-great-shift/README.md).
The non-destructive edit-to-staging workflow is documented in
[docs/guarded-authoring-and-mods.md](docs/guarded-authoring-and-mods.md).

## Requirements

Windows x64:

- Visual Studio Build Tools 2026 (18.9.2) with the **Desktop development with
  C++** workload.

Linux:

- GCC 13+ or Clang 16+ with C++20 support, plus Ninja and `pkg-config`.
- X11 and/or Wayland development headers and an OpenGL-capable driver for the GUI.
  On Fedora:

  ```bash
  sudo dnf install mesa-libGL-devel mesa-libEGL-devel libglvnd-devel \
      libX11-devel libXext-devel libXrandr-devel libXi-devel libXcursor-devel \
      libXinerama-devel libXxf86vm-devel libXrender-devel libXfixes-devel libxcb-devel \
      xorg-x11-proto-devel wayland-devel wayland-protocols-devel libxkbcommon-devel
  ```

Both platforms:

- CMake 3.24 or newer, available on `PATH`.
- Git, available on `PATH` (used to fetch the pinned dependencies).

CMake downloads the pinned GLFW 3.4, Dear ImGui 1.91.9b (the `-docking` tag),
stb (`stb_image` and `stb_image_write`), and portable-file-dialogs sources the first
time the build is configured. stb supplies portable PNG decoding and screenshot
encoding, so no system image library is required on either platform. The GUI embeds
its fonts (IBM Plex Sans/Mono and a Lucide icon subset; licenses are in
`app/ui/fonts/LICENSES.md`) and needs no system fonts. On Linux, the native
**Open** dialogs use `zenity` or `kdialog` when one is installed; without either you
can still start from the command line or drag and drop. The core-only build does
not require GLFW, ImGui, or OpenGL.

## Quick start

Open PowerShell on Windows or a terminal on Linux in the repository root.

Windows:

```powershell
.\build.ps1
.\test.ps1
.\build\Release\rws-man.exe "C:\path\to\asset.rws"
```

Linux:

```bash
./build.sh
./test.sh
./build/Release/rws-man "/path/to/asset.rws"
```

You can also start the GUI without an argument and drag an `.rws` or `.rpc` file onto
the window.

Both scripts build **Release** by default. They use the checked-in CMake presets,
perform parallel incremental builds, and configure a build tree only when it is
missing. Useful variants are:

```powershell
# Debug build and tests
.\build.ps1 -Config Debug
.\test.ps1 -Config Debug

# Build only the GUI and its dependencies
.\build.ps1 -Target rws-man

# Build the parser, command-line tools, and tests without GUI dependencies
.\build.ps1 -CoreOnly
.\test.ps1 -CoreOnly

# Force CMake to configure the selected build tree again
.\build.ps1 -Reconfigure

# Run tests without rebuilding the test executable
.\test.ps1 -NoBuild
```

```bash
# Debug build and tests
./build.sh --config Debug
./test.sh --config Debug

# Build only the GUI and its dependencies
./build.sh --target rws-man

# Build the parser, command-line tools, and tests without GUI dependencies
./build.sh --core-only
./test.sh --core-only

# Force CMake to configure the selected build tree again
./build.sh --reconfigure

# Run tests without rebuilding the test executable
./test.sh --no-build
```

Full-build executables are written to `build/<Config>`. Core-only executables are
written to `build-core/<Config>`.

## Command-line usage

### Inspect a CSFFBS document

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

### Resolve a mission package

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
PHD schema status remain explicit. See [Mission resolution](docs/mission-resolution.md)
for the resolution order, graph schema, and current format evidence.
Typed SCN identities, validation, reference categories, and scene JSON are
documented in [Mission Explorer](docs/mission-explorer.md).

Actor class/model/CMO/Physics joins, standalone RPC inspection, lossless CMO
parsing, and ragdoll uncertainty rules are documented in
[Actor and Physics inspection](docs/actor-physics-inspection.md). Use
`csf-info mission Mission.scn --associations` for an evidence-backed association
report or `csf-info cmo file.cmo` for the source-backed collision-shape view.

### Inspect one file

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

### Scan an extracted corpus

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

## GUI usage

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
and work in perspective and all orthographic projections. Orientation and navigation
arrows expose record direction. The viewport's floating toolbar holds shading,
projection, **Frame**, **Layers** (with legend swatches for each overlay kind),
**Measure**, **Clip**, a **View** popover, and screenshot. A stats HUD sits at the
bottom left, an axis gizmo at the bottom right (click an axis to snap to an
orthographic view), and hovering a marker names it before you click. The rarely
needed rendering, texture/lightmap, collision-style, BSP, surface, and
selected-triangle controls live in the dockable **Render settings** panel
(`Tools > Open scene / collision tools`).

For the parsed RenderWare tree, the Explorer in `SCENE`, `GEOM`, and `HEX` shows the
chunk tree and decoded CSF scene instances. Chunk rows are colored by declared clump
size (with a legend and an option to turn it off); truncated chunks are marked as
errors. The `HEX` workspace shows typed fields and the editable raw payload.

The menus group document lifecycle, layout, preview utilities, export operations,
and help. `Ctrl+O` opens an RWS and `Ctrl+S` retains the safe save-copy behavior.

### Settings, layouts, and logs

Everything the GUI writes for itself lives in one per-user directory, never in the
working directory or the repository:
`%LOCALAPPDATA%\CSF RWS Tools\` on Windows, `$XDG_CONFIG_HOME/csf-rws-tools/` (or
`~/.config/csf-rws-tools/`) on Linux. It holds `settings.ini` (resource root, UI
scale, theme, recent files and pairings, panel visibility, viewport defaults, export
policy, and camera bookmarks), `layout.ini` (dock layouts), `screenshots/`, and, in
Debug builds, `rws-man-debug.log`. `settings.ini` is plain text and safe to edit; a
missing file yields defaults, and a corrupted one falls back to defaults field by
field and reports what it ignored in the Console.

`Edit > Preferences...` sets the resource root, UI scale (80-200%, on top of the
operating system's display scale; text is rasterized at the physical pixel size),
theme (dark or high-contrast), viewport defaults, and the export policy. The
default export policy is **new files only**: an export never replaces an existing
file, it writes a numbered new one (`map.scene.2.gltf`). Choose *Confirm before
overwriting* to write to the usual name and be asked first.

### Camera controls

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
| Numpad `1` / `3` / `7`, `5` | Front, side, and top views; toggle perspective |
| `Shift+1`...`9` | Recall camera bookmark (per mission) |
| `Ctrl+Shift+1`...`9` | Store camera bookmark |
| `F12` | Save a PNG screenshot |

The whole-scene view also has a logarithmic movement-speed control for large maps.
Orbit/look gestures are disabled in fixed orthographic views; pan and wheel zoom
remain available.

### Editing and saving

The `HEX` workspace permits raw payload-byte edits (type the new value and press
`Enter` or leave the field). Edited bytes are highlighted, counted in the status bar,
listed in the **Changes** panel, and marked in the Explorer. Changes are kept in
memory until you choose **Save copy**, which writes `<original>.edited.rws` (or a
numbered new file). The GUI does not overwrite the loaded asset. **Reload edited bytes** refreshes the preview from the
current in-memory data.

Raw editing can still create a game-invalid file. Work on copies and test modified
assets in a disposable game installation.

## Geometry and scene export

Selecting a Geometry in the GUI exposes local-space OBJ export (in the Inspector's Decoded section). The same operation
is available for every Geometry through `rws-info --export-obj`.

**Export whole scene (glTF)** writes three sibling files:

```text
map.gltf
map.bin
map.manifest.json
```

The glTF is Y-up and keeps each Atomic, resolved CSF placement, and World Sector as
a separately named node. Transforms are baked into positions. Original normals,
material assignments, base UVs, and existing lightmap UVs are retained where
available; zero-area and coplanar duplicate World faces are removed. glTF extras
and the manifest record texture names and original RWS source offsets.

CSF's centimetre-scale coordinates are converted to glTF metres (`0.01x`). The
conversion is recorded in the glTF metadata and manifest so it can be reversed.
DDS textures are referenced in the manifest but are not copied or converted.

**Export selected Clump (glTF)** exports the top-level Clump containing the current
tree selection. Its default filename includes the Clump's source offset. The CLI
equivalent accepts that offset explicitly with `--export-clump-gltf`.

**Export collision only (glTF/OBJ)** uses only validated sectors in the active
collision document (or a directly opened `_col.rws`). It never discovers a sibling
in the CLI. glTF applies the documented `0.01x` conversion and records source
offset/material/surface metadata in extras and its manifest. OBJ preserves source
RWS axes and units, uses stable World/sector groups and sanitized material names,
and writes sibling MTL and manifest files. Invalid triangles are omitted and
reported; input RWS bytes are never modified.

## Blender lightmap workflow

The included add-on requires Blender 4.0 or newer. Install
[tools/blender/rws_lightmaps.zip](tools/blender/rws_lightmaps.zip) using
**Edit > Preferences > Add-ons > Install from Disk**.

For material and lightmap preview:

1. Import an exported `.gltf` into a clean Blender scene.
2. In the 3D View, press `N` and open the **RWS Lightmaps** tab.
3. Select the export's sibling `.manifest.json` file.
4. Confirm the detected `Textures` directory.
5. Choose **Configure Imported Materials**.
6. Switch between **Base**, **Lightmap**, and **Base x Lightmap**.

The add-on connects base textures to the first UV layer and lightmaps to the
second, preserves foliage alpha, and keeps `FFLR*` terrain materials opaque. Its
legacy DDS color handling and the game's `2.0x` lightmap modulation are enabled by
default.

It can also prepare selected meshes for Cycles light-only baking and stage baked
lightmaps as legacy DXT1/DXT3 DDS files. Staging applies CSF's default `0.5` RGB
encoding scale and recreates the original archive/map/`Textures` hierarchy in a
separate output directory. The portable encoder requires no external tools;
NVIDIA Texture Tools can optionally accelerate large exports.

See the [Blender add-on guide](tools/blender/rws_lightmaps/README.md) for bake
memory estimates, material behavior, encoder setup, and the complete game-ready
DDS workflow.

## Known limitations

- Parsing and export behavior is based on the currently studied *Commandos:
  Strike Force* corpus; other RenderWare games and versions may differ.
- Several game-specific fields and chunk types remain unidentified.
- RWS hex edits remain structural byte edits; guarded schema-aware authoring is
  currently limited to the reviewed CSFFBS field set exposed by `csf-mod`.
- Scene glTF export does not package or convert external DDS textures.
- Modified assets are not guaranteed to load in the game.

Confirmed structures and unresolved questions are documented rather than hidden;
start with [docs/rws-format.md](docs/rws-format.md) before building new decoders or
export behavior.

## Development

Keep parsing and export logic in `rws_core`; the GUI and console programs should
remain clients of that library. Parser, decoder, or exporter changes should add or
update coverage in `tests/document_tests.cpp` and pass:

Windows:

```powershell
.\test.ps1
```

Linux:

```bash
./test.sh
```

Generated `build*`, `_deps`, and Python `__pycache__` content must not be committed.
Do not add copyrighted game assets, extracted resources, or generated exports to
the repository.

When reporting a parser problem, include the tool, command, diagnostic text, and
chunk offsets involved. Share a minimal byte sample only if you have the right to
redistribute it.

## License

csf-rws-tools is available under the [MIT License](LICENSE).
