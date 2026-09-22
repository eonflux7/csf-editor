> **Archived plan — implemented.** The collision-overlay behavior as built is in
> [GUI usage](../getting-started/gui.md), [CLI usage](../getting-started/cli.md),
> and [RWS format notes](../formats/rws-format.md). Kept for its core-recovery
> design, invariants, and validation totals.

# Sprint 01 roadmap: level collision recovery and preview overlay

## Sprint outcome

Deliver an end-to-end, read-only level-collision workflow in `rws-man`:

1. Open a visual map such as `FR01.rws`.
2. Discover and validate its sibling `FR01_col.rws` without modifying either file.
3. Recover the complete collision World through one shared `rws_core` implementation.
4. Render the collision mesh alone or as a translucent, surface-colored overlay on
   the visual scene.
5. Report recovery counts and partial-data diagnostics consistently in the core,
   CLI, exporter, and GUI.

This sprint is the first vertical slice of the broader preview roadmap. It focuses
on static level collision. Object Physics bodies, ragdolls, `.cmo` shapes,
animation playback, dependency graphs, and geometric difference analysis remain
follow-up work.

## Why this sprint

The Ransom FR01 level stores rendering and collision separately:

| Asset | Role | Relevant header totals |
| --- | --- | --- |
| `Maps/FR01/FR01.rws` | Visual Clumps, CSF placements, and visual World | 40 World sectors, 109,855 World triangles, 178,532 World vertices |
| `Maps/FR01/FR01_col.rws` | Static level collision World | 326 World sectors, 325 Plane sectors, 99,653 triangles, 84,884 vertices, 14 materials |

The two Worlds share their horizontal bounds. The collision World extends farther
vertically, so showing it only as ordinary rendered geometry hides useful
differences between visible and solid space.

The collision materials also contain gameplay-facing surface names such as
`Metal`, `Piedra`, `Madera`, `Escaleras`, `Cristal_opaco`, `Barro`, `Tierra`,
`Cemento`, `Vegetacion`, and `Baldosa`. Surface coloring therefore provides more
information than a single-color wireframe.

The reference archive is local, read-only evidence:

```text
E:\dev\re-csf\CSF_unpacks\Ransom
```

Do not copy game resources or derived extracts into the repository. Automated
tests must use small synthetic byte streams constructed in
`tests/document_tests.cpp`.

## Current baseline

The repository already provides most low-level pieces:

- `rws::Document` performs bounded chunk parsing and preserves truncation
  diagnostics.
- `decode_world`, `decode_plane_sector`, and `decode_world_sector` expose typed
  World headers.
- Pyro Material metadata exposes surface IDs and optional names.
- `GeometryPreview::load_scene` scans a World's bytes for validated Atomic Section
  layouts and uploads their triangles.
- `scene_export.cpp` has a second, similar World-sector scanner for glTF export.
- The OpenGL preview already supports depth testing, material batches, whole-scene
  navigation, wireframe, picking by batch owner, and scene statistics.
- `rws-info --validate-types` decodes every currently discovered typed structure in
  both FR01 streams without failure.

There is also a known consistency problem to fix. Generic declared-size tree
walking sees only one Atomic Section in `FR01_col.rws`, because historical World
sector/plugin size defects disrupt normal BSP nesting. The World header says there
are 326 sectors. The preview and exporter work around this with separate validated
byte scans, while `rws-info --summary` reports only the generic chunk tree. Adding
another GUI-only scanner would make this divergence worse.

## Sprint tooling decisions

Sprint 01 should extend the existing C++ tools that link `rws_core`. Do not add an
independent Python or PowerShell World scanner: the main purpose of the sprint is to
make one recovery implementation authoritative across every consumer.

### Required: `rws-info --world-report`

Add a first-class diagnostic report backed by the shared recovered-World API:

```powershell
.\build\Release\rws-info.exe "C:\path\to\map_col.rws" --world-report
```

The stable summary should include, per World:

- World byte offset and library ID;
- declared Plane and World Sector counts;
- recovered sector, triangle, and vertex counts;
- declared triangle and vertex counts for comparison;
- World material count;
- invalid candidates, invalid triangles, invalid material references, duplicate or
  overlapping ranges, and partial-recovery diagnostics;
- a final `complete`, `partial`, or `failed` recovery status.

An optional detailed form such as `--world-report=sectors` may list each recovered
sector's index, offsets, counts, material-window base, and bounds. It is useful for
diagnosing one bad sector but is not required for normal validation.

Keep `--summary` honest: its chunk table describes the conservative parsed tree.
It may append a short recovered-World summary, but it must not silently replace the
meaning of the existing Atomic Section count. `--validate-types` should validate
the recovered structures as well as normally nested chunks, without double-counting
the same structure.

A JSON mode is optional and not a completion requirement. Stable human-readable or
tab-separated output is sufficient for this sprint and avoids adding a serialization
dependency.

### Required: recovered-World corpus reporting

Extend `rws-corpus` to invoke the same recovery API for every World-bearing `.rws`
file. Add per-file fields equivalent to:

```text
world-sectors  world-triangles  world-vertices  world-recovery
326/326        99653/99653      84884/84884     complete
```

Add aggregate complete/partial/failed World counts at the end. This is the main
cross-map regression check: an implementation that fixes Ransom but loses valid
sectors in ST05, FR02, or another map is not acceptable.

`rws-corpus` remains read-only and continues scanning `.rws` files only in this
sprint. First-class `.rpc` scanning is deferred.

### Required: in-process synthetic test builders

Extend the existing byte append helpers in `tests/document_tests.cpp` with small
World/Plane/Sector builders. Keep them in the test translation unit unless their
size clearly justifies a focused `tests/rws_test_builder.hpp` header. Do not generate
fixture files on disk and do not derive fixtures from game resources.

Suggested helpers include:

```cpp
void append_world(...);
void append_plane_section(...);
void append_world_sector(...);
void append_pyro_world_sector_size_defect(...);
```

### Not needed for Sprint 01

- A CSFFBS decoder. Sibling naming is sufficient to pair `FR01.rws` with
  `FR01_col.rws`, and collision geometry plus surface metadata is already inside the
  RWS documents.
- A third standalone World scanner or binary-probing script.
- A dedicated collision OBJ exporter. The existing scene glTF path is sufficient
  for non-GUI geometry validation once it uses the shared recovery result.
- Screenshot regression, headless GUI automation, or a shader build pipeline.

### Deferred tooling

A generic, lossless CSFFBS structural inspector becomes useful when scene-package
loading enters scope. It should precede extension-specific semantic decoders and
support tree, string, and dependency views without discarding unknown records.

A later dependency tool should normalize references from `.scn`, `.vis`, `.m3d`,
`.phd`, `.and`, and `.txl`, report missing files, and account for `.dff` resource
names mapping to extracted `.rpc` Clumps. These tools belong with the later
scene-package/dependency sprint rather than collision overlay work.

## Scope

### In scope

- Move validated World-sector discovery and mesh-layout recovery into `rws_core`.
- Reuse that core result in preview and scene export.
- Surface declared versus recovered World counts and diagnostics in the CLI and GUI.
- Load one optional companion collision document in `rws-man`.
- Auto-discover the conventional sibling `_col.rws` file.
- Render visual-only, collision-only, and combined modes.
- Support solid, translucent overlay, wireframe overlay, and X-ray collision
  display.
- Color collision by decoded surface metadata, with a stable fallback palette.
- Show useful collision statistics and companion-loading status.
- Document the behavior and controls in the README.

### Explicitly out of scope

- Editing or saving collision assets.
- Rebuilding incorrect declared chunk sizes.
- Treating recovered byte patterns as ordinary editable tree children.
- Visual-versus-collision distance or topology comparison.
- Physics `0x907` body, `0x909` ragdoll, or `.cmo` primitive visualization.
- Full collision triangle/sector picking if it threatens the core overlay slice.
- Parsing `.scn`, `.vis`, `.phd`, `.m3d`, `.and`, or `.txl` dependency formats.
- `.rpc` file-dialog and corpus support.
- HAnim/`.anm` playback.

Deferred items are recorded at the end of this document so the implementation can
leave appropriate extension points without expanding Sprint 01.

## Technical design

### 1. Shared recovered World representation

Add a core API rather than returning pointers into a fabricated chunk tree. A
suggested shape is:

```cpp
struct RecoveredWorldSector {
    std::uint64_t chunk_offset{};
    std::uint64_t struct_offset{};
    std::int32_t material_window_base{};
    std::int32_t triangle_count{};
    std::int32_t vertex_count{};
    Vec3 bounding_box_inf;
    Vec3 bounding_box_sup;
    bool collision_sector_present{};
    std::uint32_t texcoord_sets{};
    std::uint64_t vertices_offset{};
    std::uint64_t normals_offset{};
    std::uint64_t prelight_offset{};
    std::vector<std::uint64_t> texcoord_offsets;
    std::uint64_t triangles_offset{};
};

struct RecoveredWorld {
    WorldInfo header;
    std::vector<RecoveredWorldSector> sectors;
    std::int64_t recovered_vertices{};
    std::int64_t recovered_triangles{};
    std::vector<std::string> diagnostics;
};
```

Names may change during implementation, but the API must provide offsets and
counts without copying all vertex data. Preview and export can then consume the
original byte span directly. Keep recovered structures logically separate from
the conservative chunk tree: byte scanning can identify usable sector layouts but
does not prove every damaged parent/child boundary well enough for structural
editing.

Place the declarations in `include/rws/decoded.hpp` or a focused new core header if
the implementation becomes large. The implementation belongs in `rws_core`, not
`app/`.

### 2. Sector validation rules

A candidate sector is accepted only when all applicable checks pass:

1. The candidate header is type `0x09` and uses the enclosing World's library ID.
2. Its first child is a `Struct` (`0x01`) with the same library ID.
3. The Struct contains the complete 44-byte World Sector header.
4. Triangle and vertex counts are non-negative and their size calculations do not
   overflow 64-bit arithmetic.
5. The expected Struct size exactly matches:

   ```text
   44
   + vertex_count * 12
   + optional packed normals
   + optional prelight RGBA
   + texcoord_set_count * vertex_count * 8
   + triangle_count * 8
   ```

6. Every calculated range remains within the physical World/file range.
7. Every triangle vertex index is within the sector vertex count.
8. Material resolution through `material_window_base + local_material` remains
   bounded by the World Material List. Invalid triangles must be diagnosed and
   skipped, not used for out-of-range access.
9. Accepted sector byte ranges may not overlap. After accepting a sector, advance
   to its validated end rather than scanning its payload for false headers.
10. Recovery stops safely on overflow, impossible counts, or end-of-file; unknown
    and truncated bytes remain untouched.

Account for the documented Pyro World Sector plug-in defect: its declared payload
is four bytes larger than the writer emits. Do not repair the source bytes. Decode
the known payload using the existing owner-aware Pyro decoder and retain a warning
for the inconsistent boundary.

The function should return useful partial results. A mismatch between declared and
recovered totals is a warning unless no usable sectors can be recovered.

### 3. Count invariants and diagnostics

For each World, compare:

- declared versus recovered sector count;
- declared versus recovered vertex count;
- declared versus recovered triangle count;
- material count and invalid material references;
- duplicate/overlapping candidate ranges;
- physically truncated versus structurally invalid candidates.

Do not require the visual World's aggregate vertex total to equal the sum of
render-upload vertices without checking the RenderWare format semantics. Establish
the invariant against the known FR01 samples and synthetic tests before converting
a discrepancy into an error.

Expose the result in `rws-info`. The ordinary chunk summary should remain an honest
summary of the conservative parsed tree; add a separate recovered-World line or
section instead of silently changing what `count` means. Example:

```text
Recovered World sectors: 326/326
Recovered World triangles: 99653/99653
Recovered World vertices: 84884/84884
```

### 4. Companion document ownership

Keep the main and collision assets as independent `rws::Document` objects. A
suggested GUI state is:

```cpp
std::unique_ptr<rws::Document> document;
std::unique_ptr<rws::Document> collision_document;
std::string collision_status;
```

Loading or reloading the main document must clear stale companion GPU and document
state before discovering a new companion. A companion parse failure must not make
the main file unusable.

Auto-discovery rules:

- Opening `NAME.rws` checks the same directory for `NAME_col.rws`.
- Opening `NAME_col.rws` may check for `NAME.rws`, but it must still be usable as a
  collision-only document when the visual sibling is absent.
- Match the suffix case-insensitively on Windows.
- Do not search parent directories or guess unrelated names.
- Validate content, not just the filename: the companion must contain a decodable
  World root and at least one recovered sector.
- Never write to or normalize the companion file.

Add a manual `Open collision...` action only if it can reuse the existing native
file-dialog path without materially delaying the overlay. Otherwise record it for
Sprint 02 and display the expected companion path in the status text.

### 5. Preview data separation

Do not mix collision materials into the visual texture/lightmap controls. Extend
preview batches with an explicit layer and stable source identity, for example:

```cpp
enum class PreviewLayer : std::uint8_t {
    visual_clump,
    visual_world,
    collision_world,
};
```

Each collision batch should retain enough metadata for future selection:

- companion document identity;
- sector offset or recovered-sector index;
- material slot;
- decoded surface ID/name when present.

GPU vertex duplication is acceptable for the Sprint 01 data size, but avoid one
draw call per triangle. Continue batching by layer/material/sector as needed.

Collision loading must use the shared core recovery result. Remove the duplicated
layout scanner from `GeometryPreview::load_scene` once both preview and export use
the new API.

### 6. Collision color policy

Prefer decoded Pyro Material metadata, then fall back to material slot. Use a stable
palette so screenshots and comparisons remain meaningful across runs.

Suggested initial semantic palette:

| Surface family | Suggested color |
| --- | --- |
| Stone, concrete, tile | neutral gray |
| Earth, mud | brown |
| Vegetation | green |
| Wood | tan/orange |
| Metal | blue |
| Glass | cyan |
| Stairs | yellow |
| Unknown | magenta or deterministic material-index color |

Matching should prefer numeric surface IDs once confirmed. Case-insensitive name
matching is an acceptable fallback for known Ransom names, but preserve and display
the raw name and ID. Do not bake Spanish strings into parsing semantics.

### 7. Render modes and controls

Add a collision section to the Whole RWS Scene controls:

- `Show visual scene`
- `Show level collision`
- `Collision style`: `Solid`, `Solid + wire`, `Wireframe`, `X-ray`
- `Color by`: `Surface`, `Material index`, `Single color`
- opacity slider, default approximately `0.35`
- `Frame visual`, `Frame collision`, and `Frame all`

Expected behavior:

- Combined mode defaults to depth-tested translucent collision over the textured
  or material-colored visual scene.
- X-ray disables collision depth testing or uses a two-pass equivalent while
  leaving the visual pass depth-tested.
- Collision wireframe is visually distinct from the existing visual wire overlay.
- Backface culling defaults off for collision inspection.
- Camera navigation and visual selection continue to work when collision is hidden.
- If no companion is present, collision controls are disabled and the reason is
  shown without consuming viewport space with repeated errors.

OpenGL state changed for transparency, polygon mode, blending, depth tests, or line
width must be restored before returning control to ImGui.

### 8. Collision statistics

Show compact, verifiable status near the viewport:

```text
Visual: 40 sectors / 109855 triangles
Collision: 326/326 sectors / 99653 triangles / 14 materials
Companion: FR01_col.rws
```

If recovery is partial, use an attention color and show the first diagnostic in a
tooltip or expandable details row. The viewport may still render validated sectors.

## Work breakdown

The order below is intentional. Do not begin UI overlay work by copying the existing
scanner.

### S01-01: Core World-sector recovery

Deliverables:

- Add the shared recovered-World API.
- Move candidate validation and layout calculations out of the GUI/exporter.
- Preserve source bytes and conservative chunk parsing.
- Return partial recovery diagnostics and aggregate statistics.

Acceptance criteria:

- Synthetic complete and defective Worlds recover the expected sectors.
- Arithmetic and range checks cover malformed counts and truncation.
- No game-resource files are added to the repository.
- Existing parser tests still pass.

### S01-02: Consolidate consumers

Deliverables:

- Make `GeometryPreview` consume the core recovery result.
- Make scene glTF export consume the same result.
- Remove duplicated World-sector layout scanning.
- Add `rws-info --world-report` and recovered totals to summary/validation output.
- Add per-file and aggregate recovered-World reporting to `rws-corpus`.

Acceptance criteria:

- Preview, exporter, and CLI agree on recovered sector/triangle counts.
- `rws-info --world-report` distinguishes declared, conservatively parsed, and
  recovered counts.
- Corpus output classifies each World recovery as complete, partial, or failed.
- Existing visual FR01 export behavior is not regressed.
- A partial World remains exportable/previewable with diagnostics.

### S01-03: Companion discovery and lifecycle

Deliverables:

- Discover a same-directory `_col.rws` companion.
- Load it as a separate read-only `Document`.
- Keep clear status for found, absent, invalid, and partially recovered companions.
- Clear/reload companion GPU state when the main document changes.

Acceptance criteria:

- `FR01.rws` finds `FR01_col.rws`.
- A standalone model or Physics `.rws` does not trigger misleading collision state.
- Missing or corrupt companions do not prevent main-document inspection.
- Opening `_col.rws` alone provides collision-only viewing.

### S01-04: Collision GPU layer

Deliverables:

- Upload recovered collision sectors as a distinct preview layer.
- Retain sector/material/source metadata on batches.
- Implement solid, wire, translucent overlay, and X-ray passes.
- Add separate visual/collision visibility and framing.

Acceptance criteria:

- All validated collision triangles can be displayed without crashes or index
  overruns.
- Transparency and X-ray modes do not corrupt later ImGui/OpenGL rendering.
- Toggling collision does not rebuild unrelated texture resources every frame.
- Camera framing works for visual-only, collision-only, and combined bounds.

### S01-05: Surface metadata and UI

Deliverables:

- Resolve collision material metadata using the World Material List and Pyro
  Material extension.
- Add semantic, material-index, and single-color modes.
- Add opacity and collision-style controls.
- Display recovered/declared counts, material count, filename, and diagnostics.

Acceptance criteria:

- Known Ransom surface names receive stable semantic colors.
- Unknown surfaces remain visible and identifiable.
- Raw surface ID/name remains accessible in the inspector or status UI.
- Collision controls are disabled cleanly when no valid layer exists.

### S01-06: Documentation and regression pass

Deliverables:

- Update README feature and GUI-usage sections.
- Record the shared recovery API and any newly confirmed invariants in
  `docs/formats/rws-format.md`.
- Run release and debug validation appropriate to the changed code.
- Manually validate against the read-only Ransom sample.

Acceptance criteria:

- Documentation distinguishes level collision World, Collision Plugin `0x11D`,
  and RenderWare Physics `0x907`/`0x909`.
- Build and test commands in README remain consistent with scripts and presets.
- No generated `build*`, `_deps`, cache, or game-resource content is committed.

## Test plan

### Automated core tests

Add focused blocks to `tests/document_tests.cpp` for:

1. A complete World with two sectors and exact aggregate counts.
2. A sector with positions, packed normals, prelight colors, two UV sets, and
   triangles, verifying every calculated offset.
3. A Pyro World Sector extension with the known four-byte declared-size excess.
4. A World whose outer/Plane sizes are truncated but whose sector Struct layouts
   are physically complete and recoverable.
5. Negative vertex or triangle counts.
6. Multiplication/addition overflow attempts.
7. A triangle containing an out-of-range vertex index.
8. An out-of-range material-window calculation.
9. A false `0x09` byte pattern inside vertex data that must not become a sector.
10. Overlapping or duplicate candidate sector ranges.
11. Partial recovery where declared and recovered counts differ and a diagnostic is
    retained.

Tests should verify that `Document::bytes()` remains byte-identical before and after
recovery.

### Automated commands

Run from the repository root:

```powershell
.\build.ps1 -CoreOnly
.\test.ps1 -CoreOnly
.\build.ps1 -Target rws-man
.\test.ps1
```

Use Debug for an additional bounds/assertion pass before completing the sprint:

```powershell
.\build.ps1 -Config Debug
.\test.ps1 -Config Debug
```

### Read-only corpus checks

The following are validation commands, not committed test dependencies:

```powershell
$map = 'E:\dev\re-csf\CSF_unpacks\Ransom\Maps\FR01'

.\build\Release\rws-info.exe "$map\FR01.rws" --summary
.\build\Release\rws-info.exe "$map\FR01.rws" --world-report
.\build\Release\rws-info.exe "$map\FR01.rws" --validate-types
.\build\Release\rws-info.exe "$map\FR01_col.rws" --summary
.\build\Release\rws-info.exe "$map\FR01_col.rws" --world-report
.\build\Release\rws-info.exe "$map\FR01_col.rws" --validate-types
.\build\Release\rws-corpus.exe 'E:\dev\re-csf\CSF_unpacks\Ransom'
```

Expected reference totals:

| Check | Expected |
| --- | ---: |
| Visual World recovered sectors | 40 |
| Visual World triangles | 109,855 |
| Visual World vertices | 178,532 |
| Collision World recovered sectors | 326 |
| Collision World triangles | 99,653 |
| Collision World vertices | 84,884 |
| Collision materials | 14 |
| Typed decode failures | 0 |

Keep the existing truncation diagnostics unless the implementation proves they are
false. Successful recovery does not authorize rewriting or suppressing evidence of
the declared-size defects.

### Manual GUI matrix

Verify at minimum:

| Scenario | Expected result |
| --- | --- |
| Open `FR01.rws` with sibling present | Visual scene loads; collision companion is reported and can be enabled |
| Visual only | Existing texture, lightmap, wireframe, navigation, and selection behavior remains usable |
| Collision only | 326 recovered sectors render and collision bounds can be framed |
| Combined depth-tested | Collision is translucent and correctly aligned with the visual level |
| Combined X-ray | Occluded collision remains visible without breaking later UI rendering |
| Surface colors | Known materials have stable colors; unknown materials use fallback colors |
| Open `FR01_col.rws` directly | Collision-only preview works without requiring `FR01.rws` |
| Open an unrelated model/Physics stream | No stale FR01 collision layer remains |
| Remove/rename companion temporarily | Main scene still works and UI reports the companion as absent |
| Reload edited main bytes | Companion remains read-only and is revalidated/reloaded predictably |

## Performance targets

These are guardrails rather than hard benchmarks:

- No per-frame parsing or filesystem probing.
- Companion discovery and World recovery occur on load/reload only.
- Collision visibility/style/opacity changes reuse existing GPU buffers.
- Collision batches are grouped sufficiently to avoid per-triangle draw calls.
- The combined FR01 scene remains interactively navigable on the current target
  development machine.
- Diagnostic/status UI must not iterate over every triangle each frame.

If alpha blending requires sorting, start with a documented unsorted overlay or a
two-pass wire/solid method; do not add expensive per-frame triangle sorting in this
sprint.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| False sector detection in arbitrary bytes | Require matching stamps, exact Struct layout, range-safe counts, valid indices, and non-overlap |
| Parser and preview disagree | Make one core recovery result the source for CLI, export, and GUI |
| Collision transparency produces ordering artifacts | Prefer stable depth-tested overlay; document limitations before considering sorting/OIT |
| Companion naming differs in other levels | Keep discovery conservative and expose future manual selection rather than broad filesystem guessing |
| Surface IDs/names vary by locale or level | Preserve raw metadata and provide deterministic material-index fallback |
| Damaged streams recover only partially | Render validated sectors, display recovered/declared counts, and retain diagnostics |
| Large scene reloads feel slow | Recover and upload only on load/reload; cache textures and GPU buffers as today |
| Collision offsets collide with main-document offsets in selection | Add source-document/layer identity rather than using bare offsets globally |

## Definition of done

Sprint 01 is complete when all of the following are true:

- A shared `rws_core` API recovers validated World sectors without modifying source
  bytes.
- Preview, scene export, and CLI use that API rather than separate byte scanners.
- `rws-info --world-report` distinguishes conservative chunk-tree counts from
  declared and recovered World counts.
- `rws-corpus` reports per-file and aggregate World recovery status using the same
  core API.
- Opening Ransom `FR01.rws` automatically finds and validates `FR01_col.rws`.
- The GUI can show visual-only, collision-only, and combined collision overlay
  modes.
- Collision supports depth-tested translucent, wireframe, and X-ray inspection.
- Collision can be colored by surface metadata with a deterministic fallback.
- The GUI reports companion path, declared/recovered sectors, triangles, vertices,
  materials, and partial-recovery diagnostics.
- Synthetic parser/recovery tests cover complete, defective, false-positive, and
  overflow cases.
- Release and Debug builds/tests pass.
- Ransom validation reaches 40 visual World sectors and 326 collision sectors with
  the reference aggregate counts above.
- Existing geometry, scene, lightmap, export, and save-to-copy behavior is not
  regressed.
- No game resources, generated build output, or unrelated user changes are added.

## Follow-up backlog

The following ideas are intentionally preserved for later sprints.

### Sprint 02 candidates: collision inspection

- Collision triangle/sector picking and inspector integration.
- BSP Plane/leaf bounds and split visualization.
- Orthographic top/front/side cameras, grid, axes, coordinate readout, clipping
  planes, and measurement tools.
- Manual companion selection and recently used pairings.
- Export collision-only glTF/OBJ with surface metadata.

### Sprint 03 candidates: object Physics

- Render `0x907` sphere, capsule, box, cylinder, and Trilist volumes.
- Overlay Physics bodies on companion `.rpc` render Clumps.
- Draw center of mass, inertia orientation, finite-rotation axis, flags, and
  collision groups.
- Render `0x909` ragdoll bodies and joints over an HAnim skeleton.
- Parse and compare source-like `.cmo` collision shapes with compiled Physics
  streams.
- Use `.phd` dependencies to resolve render/Physics pairings.

### Sprint 04 candidates: analysis and scene packages

- Visual-versus-collision distance and height heat maps.
- Collision-only and visual-only discrepancy highlighting.
- Walkable-slope and normal-direction filters.
- Parse `.scn`/`.vis` as a scene-package entry point.
- Dependency graph for `.m3d`, `.phd`, `.and`, `.txl`, textures, models, Physics,
  and animations.
- Support `.rpc` as a first-class RenderWare Clump extension in GUI and corpus tools.
- Preview scene fog, camera settings, sky, water fog, and placement visibility
  distance/fade controls.
- HAnim plus `.anm` playback.
