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
resolution](../reference/mission-resolution.md) for the resolution order, graph
schema, and current format evidence. Typed SCN identities, validation, reference
categories, and scene JSON are documented in [Mission Explorer typed-view
contract](../reference/mission-explorer.md).

Actor class/model/CMO/Physics joins, standalone RPC inspection, lossless CMO
parsing, and ragdoll uncertainty rules are documented in [Actor and Physics
inspection](../reference/actor-physics-inspection.md). Use
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

See [Geometry and scene export](../guides/export.md) for output layout and file
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
