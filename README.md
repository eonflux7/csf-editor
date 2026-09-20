# csf-rws-tools

csf-rws-tools is a Windows inspection and reverse-engineering toolkit for
RenderWare Binary Stream (`.rws`) assets from *Commandos: Strike Force*. It
provides the `rws-man` graphical chunk browser and 3D scene preview, command-line
analysis tools, OBJ/glTF export, and a Blender add-on for inspecting and rebaking
the game's lightmaps.

![Assembled Commandos: Strike Force scene preview in rws-man](docs/images/csf-rws-tools-scene-preview.png)

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
  including DDS base textures, secondary-UV lightmaps, material diagnostics, and
  wireframe modes. Map previews automatically discover a same-directory
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
| `csf_core` | Generic `CSFFBS` parser and immutable document model |
| `tools/blender/rws_lightmaps` | Blender material, bake, and DDS staging add-on |

Format notes live in [docs/rws-format.md](docs/rws-format.md), corpus results in
[docs/corpus-findings.md](docs/corpus-findings.md), and planned work in
[docs/roadmap.md](docs/roadmap.md).
The generic container grammar is recorded in
[docs/csffbs-format.md](docs/csffbs-format.md), with the mission-workbench plan in
[docs/the-great-shift/README.md](docs/the-great-shift/README.md).

## Requirements

- Windows x64.
- Visual Studio Build Tools 2026 (18.9.2) with the **Desktop development with
  C++** workload.
- CMake 3.24 or newer, available on `PATH`.
- Git, available on `PATH`.
- An OpenGL-capable graphics driver for the GUI.

CMake downloads the pinned GLFW 3.4 and Dear ImGui 1.91.9b sources the first time
the GUI build is configured. The core-only build does not require GLFW, ImGui, or
OpenGL.

## Quick start

Open PowerShell in the repository root:

```powershell
.\build.ps1
.\test.ps1
.\build\Release\rws-man.exe "C:\path\to\asset.rws"
```

You can also start `rws-man.exe` without an argument and drag an `.rws` or `.rpc` file onto
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

Full-build executables are written to `build\<Config>`. Core-only executables are
written to `build-core\<Config>`.

## Command-line usage

### Inspect a CSFFBS document

`csf-info` selects the parser by the `CSFFBS` magic rather than the filename
extension. It is read-only and returns a nonzero status for structural errors.

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

Use `-` as an export destination to write the inspection format to standard
output. File exports refuse the input path and any destination that already
exists. The recursive corpus report sniffs every regular file by magic and emits
tab-separated per-file and aggregate records; it never modifies the corpus.

### Resolve a mission package

Mission mode builds a read-only dependency graph from an SCN path or a mission
directory. Package-local paths take precedence, matching is Windows-like and
case-insensitive, and every mapped, missing, ambiguous, or case-mismatched edge
retains its source file and byte offset.

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

```powershell
.\build\Release\rws-info.exe "C:\path\to\asset.rws"
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

### Scan an extracted corpus

`rws-corpus` recursively scans `.rws` and `.rpc` files and prints a tab-separated per-file
report including declared/recovered World totals and recovery status, followed by
root-format, chunk-type, diagnostic, scene-instance, and aggregate World-recovery totals:

```powershell
.\build\Release\rws-corpus.exe "C:\path\to\extracted-game"
```

It is read-only: it does not modify the files it scans.

## GUI usage

The GUI opens maximized and treats the 3D viewport as its main workspace. When a
loaded document contains Clumps, scene instances, or a World, the assembled
`Scene` workspace opens automatically; otherwise it falls back to a previewable
`Geometry` and then to the `Inspector`. Use the top toolbar or the `1`, `2`, and
`3` keys to switch workspaces. `Ctrl+Space` temporarily hides the optional side
panels so the viewport uses the full application content area.

Open an SCN with `File > Open mission...` (`Ctrl+Shift+O`) or drop it on the
window to enter the Mission workspace. The resolved visual and collision maps
load together. Actors, navigation points/links, dummies, area outlines, and
light-radius rings share the existing 3D camera and work in perspective and all
orthographic projections. The `Overlays` menu controls each layer independently.
Clicking a marker or line selects its stable SCN entry; the right inspector shows
its raw CSFFBS subtree and exact class/name definition or candidate sites. The
left mission tree provides case-insensitive search over names, IDs, class IDs,
scripts, groups, and navigation points.

`View > Scene tree` opens the parsed RenderWare tree and decoded CSF scene
instances on the left. `View > Selection inspector` opens a compact selection
summary on the right. Both panels start hidden and can also be toggled from the
toolbar. Select a tree node or click visible scene geometry to synchronize the
selection; the full `Inspector` workspace shows typed fields and the editable raw
payload.

The `File`, `View`, `Tools`, `Export`, and `Help` menus group document lifecycle,
layout, preview utilities, export operations, and control reminders. `Ctrl+O`
opens an RWS and `Ctrl+S` retains the safe save-copy behavior.

The scene itself has one compact viewport toolbar for visual/collision visibility,
render style, projection, framing, measurement mode, and the **Tools** panel. Scene
counts, geometry totals, collision totals, and camera distance appear as a small
overlay inside the viewport rather than consuming rows above it. **Viewport tools**
opens on the right and contains the less frequent rendering, texture/lightmap,
collision-style, clipping, measurement, BSP, surface, and selected-triangle
controls. The individual-Geometry workspace uses the same compact approach, with
advanced options kept in its **Options** popup.

The preview offers textured, material-index, material-color, UV-checker,
lightmap-UV, lightmap-only, combined base/lightmap, and wireframe views. Its DDS
loader accepts legacy DXT1, DXT3, DXT5, 16-bit mask-based RGB/RGBA, and 32-bit
mask-based RGB/RGBA images. Base
textures use UV1 (`TEXCOORD_0`) and MatFX lightmaps use UV2 (`TEXCOORD_1`). DDS
textures are resolved from a `Textures` directory beside the loaded asset.

When `NAME.rws` is opened, the GUI first checks for `NAME_col.rws` in the same
directory. A valid companion remains a separate read-only document; a missing,
invalid, or partially recovered companion never prevents the visual file from
loading. Opening an `_col.rws` file directly provides collision-only viewing.
The collision controls independently toggle visual and collision layers, select
solid, solid-with-wire, wireframe, or X-ray rendering, choose surface,
material-index, or single-color display, and adjust overlay opacity. Surface mode
uses Pyro Material names/IDs when present and a deterministic palette otherwise.
The translucent pass is intentionally unsorted in this first slice; it is stable
and depth-tested but may show ordinary alpha-ordering artifacts in dense overlaps.

Use **Open RWS...** to choose a main document and **Open collision companion...**
to pair an unusually named World. Pairing is transactional: cancellation or failed
World recovery leaves the active companion unchanged. A bounded recent-pairing list
stores normalized paths in the user's local application-data directory, never in
the repository; recent entries are reloaded and validated before use.

Clicking collision reports stable World/sector/triangle identities, byte offsets,
material and raw surface label, vertices, hit position, geometric normal, and
barycentric coordinates without adding synthetic nodes to the parsed tree. The
selected triangle is highlighted independently of main-tree selection. Inspection
tools provide source-space X/Y/Z clipping, two-hit distance and absolute-axis
deltas, selection framing, coordinate copy, leaf/BSP status controls, and an
explicit collision-picking preference. Fixed orthographic **Top (X/Z)**,
**Front (X/Y)**, and **Side (Z/Y)** views share the collision picker; source RWS
uses Y as the vertical map axis.

Here, a level collision World is the static geometry stored in the sibling file.
It is distinct from the Collision Plugin (`0x11D`) attached to some geometry and
from RenderWare Physics body/ragdoll definitions (`0x907`/`0x909`).

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

The whole-scene view also has a logarithmic movement-speed control for large maps.
Orbit/look gestures are disabled in fixed orthographic views; pan and wheel zoom
remain available.

### Editing and saving

The inspector permits raw payload-byte edits. Changes are kept in memory until
you choose **Save copy**, which writes `<original>.edited.rws`. The GUI does not
overwrite the loaded asset. **Reload edited bytes** refreshes the preview from the
current in-memory data.

Raw editing can still create a game-invalid file. Work on copies and test modified
assets in a disposable game installation.

## Geometry and scene export

Selecting a Geometry in the GUI exposes local-space OBJ export. The same operation
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
- Hex edits are structural byte edits, not a schema-aware authoring system.
- Scene glTF export does not package or convert external DDS textures.
- Modified assets are not guaranteed to load in the game.

Confirmed structures and unresolved questions are documented rather than hidden;
start with [docs/rws-format.md](docs/rws-format.md) before building new decoders or
export behavior.

## Development

Keep parsing and export logic in `rws_core`; the GUI and console programs should
remain clients of that library. Parser, decoder, or exporter changes should add or
update coverage in `tests/document_tests.cpp` and pass:

```powershell
.\test.ps1
```

Generated `build*`, `_deps`, and Python `__pycache__` content must not be committed.
Do not add copyrighted game assets, extracted resources, or generated exports to
the repository.

When reporting a parser problem, include the tool, command, diagnostic text, and
chunk offsets involved. Share a minimal byte sample only if you have the right to
redistribute it.

## License

csf-rws-tools is available under the [MIT License](LICENSE).
