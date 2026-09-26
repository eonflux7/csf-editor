# Spec: authoring project format

2026-09-25. Stage 2 of the [editor/Blender plan](editor-blender-authoring.md)
builds on this: it records what a project is made of, which files are sources
and which are generated, and how placements keep their identity through a
terrain reimport.

**Implemented:** `csf::AuthoringProject` (`include/csf/authoring_project.hpp`)
reads and writes both files and builds the World, collision and sector map
(`csf-mod project-build`); hello world is migrated (`tools/hello_world/build.sh`).
Height resolution, the height report and resnap (`csf-mod project-heights`),
and mission text (`text` records; `project-build` writes
`build/<archive stem>/<file>`, e.g. `build/GlobalEK/Texts/Convoy.fli`).
**Not yet:** `.csfworld` version 2
`object` lines, and building the archives into `dist/` from C++ (the
hello-world scripts do that today).

## What exists

A `csf-mod` workspace (`csf::ModProject`) is a *packaging* unit. It holds
`mod-project.json` (the distributable manifest), `local-config.json` (the
unpacked source root), `.csf-mod-state` (one record per replaced or added
package file, with hashes) and `authored/`, where `MissionEditor::save` writes
edited files. `.csf-mission` adds the scene path and the original archive.
Hello world needs two of these workspaces, one per archive (`mission/` for
`maps/Convoy.pak`, `texts/` for `GlobalEK.pak`). Before this format, its
generated map and sector files sat in `mission/extra/` with nothing recording
how they were made.

This spec adds an *authoring* layer on top and keeps the packaging layer as it
is. Export, validation, deployment and rollback keep working unchanged.

## Layout

```text
<project>/
  project.csfproj          authoring manifest (this spec)
  local.csfproj            machine-specific settings; never shared
  sources/                 editable sources; the user's files
    world/terrain.blend
    world/terrain.csfworld  last export of terrain.blend
  mission/                 csf-mod workspace for the mission archive (unchanged)
  texts/                   csf-mod workspace for GlobalEK (unchanged)
  build/                   generated; safe to delete, rebuilt from the above
    Maps/FR03/FR03.rws
    Maps/FR03/FR03_col.rws
    Maps/Secs/Convoy.sec
    world.csfworld          merged World source the compiler read
  dist/                    archives and deployment records of each build
```

`mission/` and `texts/` reference generated files in `build/` through their
`.csf-mod-state` records, replacing the former `extra/`. Deleting `build/`
makes the project stale, not broken: the next build recreates it.

## Format

`project.csfproj` is line-based text like `.csfworld` and `.csf-mod-state`.
The repository has no JSON reader, and line records diff and merge well under
version control. Fields are separated by whitespace; strings that may hold
spaces are double-quoted with `\"` and `\\` escapes. `#` starts a comment line.
Unknown record types are an error, so a newer file is never silently
misread. Paths are relative to the project directory, with `/` separators.

```text
csfproj 1
name "Hello world"
slot Convoy Maps/FR03/Convoy.scn maps/Convoy.pak
texts GlobalEK.pak Texts/Convoy.fli 900 999
donor-map Maps/FR03/FR03.rws Maps/FR03/FR03_col.rws

# World geometry from Blender
asset terrain terrain sources/world/terrain.blend sources/world/terrain.csfworld
asset-export terrain export_csf_world.py 2 sha256:<of the .csfworld>
asset bunker building sources/assets/bunker.blend sources/assets/bunker.csfworld
asset-export bunker export_csf_world.py 2 sha256:<...>

# Buildings, donor pieces and props placed by the editor
building bunker-1 bunker  1200 0 800  90  ground 0
piece house -5650 1020 -6850 -4120 2500 -5440  -2500 0 -2500  0  ground -4
prop tree-1 1,2  2500 0 -2000  0  ground -10
prop plant-1 319  -600 0 -1600  0  ground -5

# Height relations of mission actors (the actors themselves live in the scene)
anchor actor 33 ground 0
anchor actor 15 on actor 33 90.55

# Generated outputs and what they were made from
output build/Maps/FR03/FR03.rws world sha256:<output> inputs sha256:<of the inputs>
output build/Maps/FR03/FR03_col.rws world sha256:<output> inputs sha256:<...>
output build/Maps/Secs/Convoy.sec sectors sha256:<output> inputs sha256:<...>
```

### Records

| Record | Meaning |
|---|---|
| `csfproj <version>` | first line; this spec is version 1 |
| `name <string>` | display name |
| `slot <mission> <scene> <archive>` | the shipped mission slot (decision: slots are replaced, not added): mission name, package-relative scene, game-relative archive |
| `texts <archive> <file> <first> <last>` | text file in GlobalEK and the project's reserved string ID range; the build refuses IDs outside it |
| `donor-map <visual> <collision>` | package-relative donor Worlds that supply materials, props and pieces |
| `asset <id> <terrain\|building> <blend> <export>` | Blender geometry: the source `.blend` and its `.csfworld` export. A terrain is built where it was modelled; a building is exported about its own origin and built at each of its placements |
| `asset-export <id> <exporter> <version> <hash>` | the exporter and version that wrote the export, and the export's hash |
| `building <id> <asset> <x y z> <yaw> <height>` | a building asset, placed; its triangles are merged into the World |
| `piece <id> <box x0 y0 z0 x1 y1 z1> <x y z> <yaw> <height>` | donor World triangles inside a box, placed |
| `prop <id> <donor-instances> <x y z> <yaw> <height>` | donor scene instances (and their Clumps), placed |
| `anchor actor <actor-id> <height>` | height relation of a scene actor |
| `text <id> <string>` | a mission string (UTF-8) for the texts file; the ID is within the reserved range |
| `lightmap <name> <source.png>` | a baked lightmap; built into `build/<map folder>/Textures/<name>.dds` (DXT1) |
| `building <id> <asset> <x y z> <yaw> <height>` | (see above) a building placement stands on the terrain |
| `output <path> <kind> <hash> inputs <hash>` | a generated file, its hash and the hash of what it was built from |
| `playtest <build-id> <worked\|failed> <note>` | a playtest of `dist/<build-id>/`'s archives (the Build panel's log) |

`<id>` is a project-unique name (`[A-Za-z0-9_-]+`). The editor assigns one on
creation (`tree-1`, `tree-2`, ...) and it can be renamed; references are
rewritten on rename. Scene actors keep their gameplay IDs, which the mission
already owns.

`<height>` is one of:

- `absolute`: `y` is the game height.
- `ground <offset>`: `y` is recomputed from the ground under (x, z) plus the
  offset. The ground is the collision World built from the project's assets,
  without its props and pieces, so a prop never stands on itself.
- `on <kind> <id> <offset>`: stands on another placement or actor
  (`on actor 33 90.55`: the radio on the crate). The offset is from the
  supporting object's height.

Stored `y` values are the last resolved heights, and the build uses them as
they are. A terrain change never rewrites them silently: the editor reports
each placement whose resolved height differs from its stored `y` by more than
1 cm, and resnapping is one reviewable, undoable operation. A cycle of `on`
references is an error.

## Asset identity

Each Blender mesh object exported as World geometry carries a custom property
`csf_asset_id`. The exporter assigns one on first export (from the object name,
made unique) and keeps it through renames. `.csfworld` version 2 adds one line
before an object's faces:

```text
csfworld 2
object terrain
...
```

A version-1 file has no `object` lines and is one asset with the ID in its
`asset` record. The compiler still builds one World from all of them. Asset kinds follow the
map structure described in the [editor plan](editor-blender-authoring.md#map-structure-and-asset-kinds);
the Blender add-on (stage 3) sets the kind and ID when it tags an object. Asset
identity is used for change summaries ("terrain: 5000 → 5200 faces"), for
invalidation and for the placements' reference errors. The reference errors
matter once assets are more than terrain, such as a custom building a prop
stands `on`.

## Build graph

| Output | Inputs | Operation |
|---|---|---|
| `build/world.csfworld` | terrain exports, building exports at their placements, `piece` and `prop` records | merge |
| `build/Maps/.../<map>.rws`, `_col.rws` | `build/world.csfworld`, donor maps | `compile_world_source` + `map_assembly` |
| `build/Maps/Secs/<mission>.sec` | the merged World's collision faces | `rws::build_sector_map` (`csf-mod sector-build`) |
| `mission/authored/...` | editor state | `MissionEditor::save` |
| `texts/...` | the project's strings | text table writer (stage 4) |
| `dist/<build>/...` | both workspaces | `export_mission_pak` ×2, manifest like `build.sh`'s `build.json` |

An output is stale when the hash of its inputs differs from its `output`
record, or when the output file is missing or has a different hash. The build
rebuilds only stale outputs and reports what it rebuilt. Ground heights are
resolved against the asset-only collision World, which is itself an
intermediate: it is rebuilt when an asset export changes.

## Local settings

`local.csfproj` holds what differs per machine: the unpacked corpus root
(today's `local-config.json` `source_root`), the Blender executable, and the
test install used for deployment (`corpus`, `blender`, `test-install <path>`),
plus `original <archive> <path>` (an untouched shipped archive the builds
start from, when it is not the test install's first deployment backup) and
`deployment <build-id> <archive> <state file>` (each deployment and the
state file that rolls it back). It is excluded from sharing and from the
input hashes.

## Hello world migration

1. `terrain.py` stops appending the house piece, trees and plants to its
   export; `sources/world/terrain.csfworld` holds only the terrain.
2. A one-off script writes `project.csfproj`: the `slot`, `texts`,
   `donor-map` and `asset` records, one `piece`, 14 `prop` trees and 15 `prop`
   plants with `ground` heights (their current offsets: -4, -10 and -5), and
   `anchor` records for the camp actors and the radio on the crate.
3. The project build must produce World and sector files identical to v13's
   (`build.sh` output). That is the migration's acceptance test, and then
   `mission/extra/` is removed.
4. `terrain.py` placed props at its analytic height function; the ground query
   interpolates the exported triangles. The first height report therefore lists
   small differences, typically a few centimetres. Resnapping them is optional.

## Decisions

- `.blend` files live in the project's `sources/`, so a project folder is
  self-contained (user, 2026-09-25).
- `dist/` keeps every build with its rollback records; a cleanup command for
  old builds comes later (user, 2026-09-25).
