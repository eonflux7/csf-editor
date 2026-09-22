> **Archived plan — implemented.** Collision inspection, BSP tooling, and
> collision-only export as built are in [GUI usage](../getting-started/gui.md) and
> [CLI usage](../getting-started/cli.md). Kept for its topology/picking design and
> validation totals.

# Sprint 02 roadmap: collision inspection and spatial tooling

## Sprint outcome

Turn the Sprint 01 collision overlay into a precise, read-only inspection workflow
in `rws-man`:

1. Select a recovered collision triangle or sector directly in the viewport.
2. Inspect its source offsets, material/surface metadata, geometry, hit position,
   normal, and BSP context without fabricating editable chunk-tree nodes.
3. Visualize validated BSP Plane splits, leaf bounds, and the selected path.
4. Inspect large maps in perspective or orthographic top/front/side views with a
   stable grid, axes, coordinate readout, clipping planes, and measurements.
5. Manually pair an unusual collision file when the conventional `_col.rws`
   sibling is absent, while keeping both documents read-only and independently
   validated.
6. Export collision-only geometry to glTF or OBJ with surface metadata and stable
   source identities.

This sprint remains focused on static level collision Worlds. RenderWare Physics
`0x907` bodies, `0x909` ragdolls, `.cmo` primitives, scene-package dependency
resolution, animation, and visual-versus-collision difference analysis remain
follow-up work.

## Why this sprint

Sprint 01 answers whether recovered collision exists and where it lies relative to
the visual scene. It does not answer the questions needed during level analysis:

- Which exact triangle was hit?
- Which recovered sector, material slot, surface ID, and raw surface name own it?
- What is the collision-space coordinate and geometric normal at the cursor?
- Which BSP leaf contains the triangle, and which Plane decisions lead to it?
- Is a gap easier to understand from above, from a side, or through a clipped
  cross-section?
- Can the collision layer be exported without also exporting thousands of visual
  Clumps?

The current whole-scene picker deliberately ignores collision batches. Its result
is only a main-document owner offset, which is not enough once two documents can
contain equal offsets. The current recovered-World API provides safe sector array
layouts but does not represent Plane nodes or their relationship to leaves. These
are the two architectural gaps to close before adding more viewport controls.

As in Sprint 01, the local reference archive is read-only evidence:

```text
E:\dev\re-csf\CSF_unpacks\Ransom
```

Do not copy game resources, screenshots derived from them, or exported meshes into
the repository. Automated tests must construct small synthetic byte streams in
`tests/document_tests.cpp`.

## Current baseline

The repository already provides the foundation for this work:

- `rws::recover_worlds` returns validated sector ranges, array offsets, declared
  and recovered totals, material count, recovery status, and diagnostics.
- `decode_recovered_world_triangle` reads a recovered leaf triangle without
  treating the leaf as a normal parsed child.
- Preview batches distinguish visual Clumps, visual World geometry, and collision
  World geometry.
- The GUI retains the collision source path, declared/recovered counts, surface
  labels, bounds, and diagnostics.
- Collision rendering supports solid, solid-with-wire, wireframe, X-ray, opacity,
  and surface/material/single-color modes.
- The perspective camera supports look, orbit, pan, zoom, fly navigation, and
  separate visual/collision/all framing.
- Whole-scene CPU ray picking already selects visual batch owners.
- `decode_plane_sector` exposes the Plane axis, split value, child kinds, and the
  two child boundary values when a Plane Struct is normally reachable.
- Scene glTF export already consumes recovered Worlds and preserves source offsets
  in node extras and a sibling manifest.

Important limitations in that baseline:

- `pick_scene` skips `collision_world` batches and returns only a bare offset.
- `DrawBatch` does not retain a recovered-sector index or source triangle index.
- Collision geometry is expanded for rendering, so GPU vertex position alone is
  not a durable source identity.
- Plane/leaf hierarchy cannot be inferred from the flat recovered-sector list.
- The GUI has no open-file command or reusable file-dialog path; loading currently
  happens through the command line or drag/drop.
- No settings or recent-pairing store currently exists.
- The export API has no collision-only entry point.

## Sprint tooling decisions

### Required: extend the shared World recovery model

Collision selection and BSP display must be based on `rws_core` results, not a GUI
scan of raw bytes. Extend the recovery API with stable leaf identity and an optional
validated topology. Keep recovered topology separate from the conservative parsed
chunk tree.

A suggested shape is:

```cpp
struct RecoveredWorldPlane {
    std::uint64_t chunk_offset{};
    std::uint64_t struct_offset{};
    std::int32_t axis{};
    float split{};
    bool left_is_world_sector{};
    bool right_is_world_sector{};
    float left_value{};
    float right_value{};
    std::optional<std::size_t> left_node;
    std::optional<std::size_t> right_node;
};

struct RecoveredWorldNode {
    enum class Kind { plane, sector } kind{};
    std::size_t value_index{};
    std::optional<std::size_t> parent;
    std::optional<bool> is_left_child;
    Vec3 bounding_box_inf;
    Vec3 bounding_box_sup;
};
```

Names may change during implementation. The result must support these invariants:

- every accepted sector keeps its existing stable recovered-sector index;
- a topology node refers to either one recovered Plane or one recovered sector;
- parent/child references are indices, never pointers into resizable vectors;
- an incomplete or ambiguous topology does not invalidate otherwise usable sector
  geometry;
- diagnostics distinguish flat geometry recovery from topology recovery;
- source bytes and the conservative `Chunk` tree remain unchanged.

Do not expose GUI or OpenGL types from `rws_core`.

### Required: `rws-info --bsp-report`

Add a diagnostic report backed by the shared topology result:

```powershell
.\build\Release\rws-info.exe "C:\path\to\map_col.rws" --bsp-report
```

The stable summary should include, per World:

- declared and recovered Plane counts;
- declared and recovered leaf counts;
- linked and unlinked recovered sectors;
- maximum validated depth;
- invalid, ambiguous, duplicate, overlapping, and truncated node candidates;
- unreachable nodes, cycles, multiple-parent references, and bounds conflicts;
- a final `complete`, `partial`, or `failed` topology status independent of the
  geometry recovery status.

An optional detailed form such as `--bsp-report=nodes` may list node indices,
parents, child sides, offsets, split fields, bounds, and sector indices. A stable
human-readable form is sufficient; JSON remains optional.

`--summary` may add one short BSP recovery line. It must continue to describe the
conservative parsed tree honestly rather than counting recovered nodes as ordinary
children. `--validate-types` should validate recovered Plane structures without
double-counting normally parsed structures.

### Required: deterministic synthetic World/BSP builders

Extend the in-process builders introduced for Sprint 01. Tests need to control:

- Plane and leaf ordering;
- left/right child types and boundary values;
- declared-size defects and physical byte ranges;
- sector/material/triangle identities;
- malformed links, missing nodes, bad axes, invalid bounds, and truncation.

Keep builders in `tests/document_tests.cpp` unless they become large enough to
justify a focused `tests/rws_test_builder.hpp`. Do not write fixture files to disk.

### Required: one reusable native file-dialog path

Manual companion selection requires an explicit GUI file picker. Add a small,
Windows-only file-dialog wrapper suitable for both opening a main `.rws` and
choosing a collision companion. Keep dialog code out of `GeometryPreview`; the app
owns documents and passes validated inputs to the preview.

The dialog must return cancellation without changing current state and should
default to the loaded document's directory. Do not introduce a large cross-platform
UI dependency solely for this sprint.

### Not needed for Sprint 02

- GPU color-ID picking or an off-screen picking framebuffer. CPU ray intersection
  is adequate for the current data size if it uses sector bounds for broad-phase
  rejection and does no work when the viewport is idle.
- Editing, moving, deleting, or retagging collision triangles.
- Rewriting Plane/leaf sizes or material metadata.
- A general-purpose project/session format.
- Arbitrary mesh Boolean operations or collision regeneration.
- Screenshot regression or headless GUI automation.
- A new stand-alone exporter executable.

## Scope

### In scope

- Stable main/collision document identity in selection results.
- Collision sector and triangle ray picking with broad-phase bounds rejection.
- A collision inspector with geometric, material, surface, and source fields.
- Hover coordinate readout and persistent selected-hit marker.
- Shared recovery of validated BSP Plane/leaf topology where possible.
- Plane split, leaf-bound, and selected-path visualization.
- Perspective and orthographic top/front/side projections.
- Adaptive grid, labeled axes, and map-space coordinate display.
- Up to three axis-aligned inspection clipping planes.
- Point-to-point distance and optional axis-delta measurement.
- Manual companion selection, clearing, and validation.
- A small bounded list of recent valid pairings, stored outside the repository.
- Collision-only glTF and OBJ export using the shared recovered-World result.
- CLI access to collision-only export and BSP diagnostics.
- README and format-note updates.

### Explicitly out of scope

- Collision geometry edits or save-back.
- Selecting visual and collision triangles as one editable object model.
- Arbitrary oriented clipping planes or destructive mesh slicing.
- Angle, area, volume, navmesh, pathfinding, or walkability calculations.
- Automatic visual-versus-collision distance/height heat maps.
- Physics body, ragdoll, or `.cmo` selection and rendering.
- Parsing `.scn`, `.vis`, `.phd`, `.m3d`, `.and`, or `.txl` for pairing.
- Searching an entire game tree for possible companions.
- Embedding DDS textures or converting them during collision export.
- Repairing incomplete BSP topology by guessing missing links.

## Technical design

### 1. Collision selection identity

Replace the scene picker's bare-offset result internally with a tagged result. A
suggested representation is:

```cpp
enum class PreviewSource : std::uint8_t { main_document, collision_document };

struct PreviewHit {
    PreviewLayer layer{};
    PreviewSource source{};
    std::uint64_t owner_offset{};
    std::size_t world_index{};
    std::size_t sector_index{};
    std::int32_t triangle_index{-1};
    std::int32_t material_slot{-1};
    float distance{};
    Vec3 position;
    Vec3 geometric_normal;
    std::array<float, 3> barycentric{};
};
```

The main tree's existing `selected_chunk` may remain an offset for ordinary visual
selection. Collision selection must be separate UI state because a recovered leaf
is not necessarily a conservative tree node and belongs to a different document.
Clicking collision must not accidentally reveal a main-document chunk at the same
numeric offset.

Extend preview batch metadata so each expanded triangle can be mapped back to its
World, recovered sector, source triangle, and resolved material. Do not infer the
source triangle later from the draw order unless that mapping is explicitly stored
and tested.

Expected click policy:

- consider only layers that are currently visible;
- choose the nearest positive ray hit in normal depth-tested modes;
- in X-ray mode, still choose the nearest geometric hit, not draw-order output;
- provide an explicit collision-selection preference or modifier when visual and
  collision geometry overlap too closely;
- ignore UI drags, camera gestures, and clicks outside the viewport;
- preserve a selected hit when display style changes, but clear it when its source
  document is unloaded or revalidated into a different recovery result.

### 2. Picking performance and correctness

Use the ray against recovered-sector axis-aligned bounds before testing triangles.
For FR01 this reduces work from the full collision mesh to the candidate leaves.
If profiling shows the sector broad phase insufficient, add a simple immutable BVH
over sector bounds; do not make a GPU picking pass the default design.

Ray construction must share the active camera projection code. Perspective and
orthographic modes require different ray origins but the same world-space hit
contract. Intersection is double-sided because collision rendering defaults to no
backface culling.

Calculate the hit position from the accepted ray distance. Report a normalized
geometric normal from source winding and barycentric coordinates. Degenerate
triangles remain unpickable and should contribute a diagnostic/statistic rather
than producing NaNs.

### 3. Collision inspector

Show the selected collision hit adjacent to the viewport or in a dedicated
expandable panel. At minimum expose:

```text
Source: FR01_col.rws
World: 0x00000000
Sector: 217 / chunk 0x01234567
Triangle: 83 / source offset 0x01235678
Material: 6
Surface: Escaleras (ID 12)
Position: x, y, z
Normal: nx, ny, nz
Vertices: (a), (b), (c)
Barycentric: u, v, w
BSP path: root / right / left / ... / sector 217
```

Use raw surface name and numeric ID when available; do not turn localized names
into parser semantics. Offer small actions to frame the selected sector, copy a
coordinate or diagnostic line, and reveal the closest real chunk only when one
exists. Do not add recovered leaves as fake nodes to the main tree.

Selection highlighting should remain readable in solid, wireframe, and X-ray
modes. Draw the selected triangle with a small depth bias or line overlay and mark
the hit point with a camera-scaled cross. Restore all changed OpenGL state.

### 4. BSP topology recovery

Recover topology only within the physical range of a decoded World and only using
validated Plane and sector candidates with the World's library ID. Reuse the
existing accepted sectors rather than scanning a second time for leaf geometry.

Before implementation, document the exact RenderWare child ordering observed in
at least the synthetic samples and the read-only corpus. The recovery algorithm
must then enforce:

1. A Plane candidate is type `0x0A` and its leading Struct has the World library
   ID and a complete `PlaneSectorInfo` payload.
2. Axis is within the confirmed RenderWare range and all split/boundary values are
   finite.
3. Child kinds agree with the next physical node(s) used to build the preorder
   tree.
4. A recovered source range is used by at most one topology node.
5. Every node has at most one parent and the graph is acyclic.
6. Leaf references resolve to already validated recovered sectors.
7. Derived child bounds are finite, ordered, and contained by the parent bounds
   within a documented floating-point tolerance.
8. The number of linked Plane and leaf nodes is compared with the World header.
9. Ambiguity or truncation produces a partial topology plus diagnostics; it never
   changes flat geometry recovery from usable to failed.

The known Pyro World Sector plug-in four-byte size overstatement must be accounted
for without altering source bytes. Do not infer topology solely from a false
declared end offset.

If the corpus disproves reliable full topology recovery, the minimum acceptable
fallback is a partial validated forest with unlinked recovered leaves clearly
reported. Do not synthesize a balanced tree or assign leaves by spatial proximity.

### 5. BSP visualization

Add independent inspection overlays:

- recovered leaf bounds;
- Plane split rectangles clipped to the current node bounds;
- the root-to-selected-leaf path;
- optionally, a selectable topology depth range.

Use distinct, stable colors for X, Y, and Z split axes. Unlinked leaves may show
their sector bounds but must not appear as though they have a validated BSP path.
Limit visual clutter with `Selected path`, `Selected subtree`, and `All` modes;
default to `Selected path` when a collision hit exists.

Bounds and split overlays are diagnostic lines, not collision geometry, and must
not participate in triangle picking. Rendering them must reuse cached line buffers
or regenerate only when topology/display parameters change.

### 6. Camera projections, grid, and axes

Refactor camera state enough that ray construction, rendering, framing, and
coordinate readout consume one explicit projection/view description.

Required projections:

- Perspective;
- Top, looking along the map vertical axis;
- Front;
- Side.

Confirm and document the repository's map-space vertical axis before labeling the
views. Do not assume glTF's exported Y-up conversion is the same as the source RWS
coordinate convention.

Orthographic views should preserve pan and zoom independently from perspective
orbit state. `Frame visual`, `Frame collision`, `Frame all`, and `Frame selection`
must work in every projection. Disable meaningless orbit/look controls in fixed
orthographic views while retaining pan, wheel zoom, and keyboard movement in the
view plane.

Draw a stable origin triad and an adaptive major/minor grid on the active primary
plane. Grid spacing should follow powers of ten (with a useful intermediate step)
and display source RWS units. Fade minor lines when dense; do not upload an
unbounded map-sized grid.

### 7. Coordinate readout, clipping, and measurement

When the cursor has a visible collision hit, display its exact hit coordinate. If
no triangle is hit in an orthographic view, optionally intersect the cursor ray
with the active grid plane and label the value as a grid coordinate rather than a
surface hit.

Provide up to three axis-aligned clipping controls, one per source-space axis. Each
control has enable, direction, and position. Apply clipping consistently to visual
and collision solid/wire passes using shader clip tests; diagnostic markers and the
selected hit should state when their geometry is clipped. Picking must ignore
triangles rejected by active clipping so the inspector agrees with the viewport.

Measurement is a two-click, non-destructive tool:

1. choose a visible collision hit as point A;
2. choose point B;
3. show total distance and absolute per-axis deltas in source RWS units.

Allow clearing and swapping endpoints. Measurement points retain their coordinates
but are cleared when the owning collision document changes. Angle, path, area, and
surface-distance measurement are deferred.

### 8. Manual companion selection and recent pairings

Keep automatic same-directory `_col.rws` discovery as the first default. Add:

- `Open RWS...` for the main document;
- `Open collision companion...` for manual pairing;
- `Clear collision companion`;
- a short `Recent collision pairings` menu.

A manual candidate is accepted only when it loads as an independent `Document`,
contains a decodable World, and recovers at least one sector. Failed validation
must leave the current valid companion and GPU state intact and show the error.

Persist only normalized absolute paths and a small bounded recency order; never
persist document bytes, edits, or game metadata. Store settings in an appropriate
per-user location outside the repository. Missing recent files should be skipped
or removable without repeated modal errors. A recent entry must be reloaded and
revalidated before use; persistence is not trust.

Pairings are keyed by normalized main-document path, not just filename, so maps
with equal basenames in different roots do not collide. Auto-discovery should still
win for a newly opened document unless the user explicitly chooses a remembered
manual pairing.

Opening `_col.rws` directly remains a supported collision-only workflow.

### 9. Collision-only export

Add core entry points that accept the source `Document`/World recovery result and
export only validated collision triangles. Both formats use source RWS coordinates
as input; glTF applies the repository's existing `0.01x` Y-up conversion and records
it in metadata, while OBJ follows the existing exporter convention and documents
its units/axes.

Required glTF behavior:

- one node or mesh primitive grouping that preserves World and sector identity;
- material grouping by resolved World material slot;
- material/surface name and numeric surface ID in extras and the manifest;
- source World, sector, and chunk offsets in extras/manifest;
- normals generated from collision faces when absent;
- invalid triangles omitted with counts reported;
- no external visual textures required.

Required OBJ behavior:

- collision geometry only;
- stable groups for World/sector and `usemtl` names safe for OBJ;
- a sibling MTL with deterministic diffuse surface colors;
- comments or a sibling manifest mapping sanitized names to raw surface metadata
  and source offsets;
- no one-file-per-sector explosion by default.

Expose the operations in the GUI and CLI, for example:

```powershell
.\build\Release\rws-info.exe map_col.rws --export-collision-gltf map.collision.gltf
.\build\Release\rws-info.exe map_col.rws --export-collision-obj map.collision.obj
```

When invoked on a visual file, the CLI exports collision only from Worlds in that
file; it must not perform implicit sibling discovery. The GUI exports its currently
loaded collision document and makes that source path explicit.

Export writes new files and never overwrites an input RWS. Existing output-file
collision behavior should be retained; do not silently replace an unrelated
export set.

## Work breakdown

The order is intentional. Picking and visualization must not invent identities or
topology that the core cannot report.

### S02-01: Stable collision identities and core accessors

Deliverables:

- Add stable World/sector/triangle/material identity needed by preview and export.
- Add range-safe accessors for recovered vertices, triangle source offsets, and
  resolved material slots.
- Separate main-document tree selection from collision-hit selection.
- Add synthetic tests for identity and source-offset calculations.

Acceptance criteria:

- Every rendered collision triangle maps back to one recovered source triangle.
- Equal offsets in main and collision documents cannot alias selection state.
- Invalid vertex/material references remain diagnosed and are not selectable.
- Accessors reject negative, overflowed, and out-of-range indices.

### S02-02: Collision picking and inspector

Deliverables:

- Add sector-bounds broad phase and double-sided triangle ray tests.
- Support picking from perspective and orthographic projections.
- Add hit/triangle highlighting and a collision inspector.
- Add frame-selection and coordinate-copy actions.

Acceptance criteria:

- Clicking a known synthetic triangle reports the expected sector, triangle,
  material, hit position, normal, and barycentric coordinates.
- Hidden or clipped collision is not picked.
- Camera drag gestures do not change selection.
- Picking FR01 occurs on click without a persistent frame-rate penalty.

### S02-03: BSP topology recovery and reporting

Deliverables:

- Add shared validated Plane/leaf topology recovery to `rws_core`.
- Keep partial topology independent from flat sector recovery.
- Add `rws-info --bsp-report` and recovered-topology validation.
- Record confirmed ordering/bounds semantics in `docs/formats/rws-format.md`.

Acceptance criteria:

- Complete synthetic trees recover exact parents, sides, depth, and leaf indices.
- Truncated or ambiguous trees return a partial forest and diagnostics without
  losing valid sectors.
- Cycles, duplicate use, impossible axes/bounds, and count mismatches are rejected.
- Read-only corpus checks report stable Plane/leaf totals across repeated runs.

### S02-04: BSP, bounds, and projection overlays

Deliverables:

- Draw leaf bounds, split planes, and the selected BSP path.
- Add perspective/top/front/side projection controls.
- Add adaptive grid, source-space axes, and cursor coordinates.
- Add frame-selection in all projections.

Acceptance criteria:

- Split planes are clipped to their validated node bounds.
- Axis colors and view labels match confirmed source coordinates.
- Orthographic picking agrees with the visible cursor location.
- Large-map zoom levels do not create unbounded grid geometry or unreadable lines.

### S02-05: Clipping and measurement

Deliverables:

- Add three axis-aligned clipping controls shared by relevant render passes.
- Apply the same clipping predicate to picking.
- Add two-point collision measurement with total distance and axis deltas.
- Preserve/clear tool state predictably on view and document changes.

Acceptance criteria:

- Clipped triangles are neither drawn nor picked.
- Combined, wireframe, and X-ray modes restore OpenGL state after clipping.
- Synthetic points produce expected Euclidean distance and deltas.
- Measurement never modifies document or GPU mesh data.

### S02-06: Manual pairing and lifecycle

Deliverables:

- Add reusable native open-file handling.
- Add manual choose/clear collision actions.
- Persist and present a bounded list of recent validated pairings.
- Make all load failures transactional with respect to current documents.

Acceptance criteria:

- A nonstandard collision filename can be paired and inspected.
- Cancel or invalid selection leaves the active companion unchanged.
- Reopening a recent pair revalidates both paths.
- Switching the main document clears stale collision selection, measurement, BSP,
  and GPU state.
- No settings or recent-path file is written inside the repository.

### S02-07: Collision-only export

Deliverables:

- Add collision glTF and OBJ exporters to `rws_core`.
- Add CLI and GUI commands.
- Preserve surface metadata, source identity, coordinate conversion, and statistics.
- Add synthetic export-content tests without game fixtures.

Acceptance criteria:

- Exported triangle/material counts match the usable recovered result.
- glTF and OBJ distinguish sector/material groups and retain raw surface metadata.
- Partial Worlds export valid triangles and report omissions/diagnostics.
- Existing scene glTF and per-Geometry OBJ exports are not regressed.

### S02-08: Documentation and regression pass

Deliverables:

- Update README controls, pairing, inspection, and export sections.
- Update `docs/formats/rws-format.md` with confirmed BSP evidence and uncertainty.
- Run Release and Debug validation appropriate to changed code.
- Manually validate the read-only Ransom reference pair and another collision map.

Acceptance criteria:

- Documentation distinguishes source RWS axes/units from glTF axes/metres.
- Documentation distinguishes level collision, Collision Plugin `0x11D`, and
  Physics `0x907`/`0x909`.
- Build/test commands remain consistent with scripts and CMake presets.
- No generated build, settings, exported, or game-resource content is committed.

## Test plan

### Automated core tests

Add focused cases to `tests/document_tests.cpp` for:

1. A one-Plane/two-leaf BSP with exact parent, side, bounds, and depth results.
2. A multi-level preorder tree with mixed Plane/leaf children.
3. A topology whose declared sizes include the Pyro four-byte leaf defect.
4. A partial tree with complete usable leaves after one truncated Plane.
5. Invalid Plane axes, NaN/infinite splits, and inverted child bounds.
6. Duplicate leaf use, overlapping ranges, cycles, and multiple parents.
7. Declared/recovered Plane and leaf count mismatches.
8. Vertex and triangle accessor boundaries and exact source offsets.
9. Ray hits at triangle center, edge, and vertex, plus misses and degenerates.
10. Double-sided hits and nearest-hit selection across two sectors.
11. Perspective and orthographic ray construction against known geometry.
12. Clipping predicates agreeing with hit selection.
13. Measurement distance and axis deltas for known points.
14. Collision glTF counts, extras, source IDs, surface fields, and coordinate scale.
15. Collision OBJ groups, MTL names/colors, and metadata mapping.
16. Partial export omitting invalid triangles while retaining diagnostics.

Tests must verify that recovery, inspection, and export leave `Document::bytes()`
byte-identical.

### Automated commands

Run from the repository root:

```powershell
.\build.ps1 -CoreOnly
.\test.ps1 -CoreOnly
.\build.ps1 -Target rws-man
.\test.ps1
```

Use Debug for an additional assertion/bounds pass:

```powershell
.\build.ps1 -Config Debug
.\test.ps1 -Config Debug
```

### Read-only corpus checks

Use the existing local archive only as manual validation input:

```powershell
$map = 'E:\dev\re-csf\CSF_unpacks\Ransom\Maps\FR01'
$out = Join-Path $env:TEMP 'rws-man-sprint-02'

.\build\Release\rws-info.exe "$map\FR01_col.rws" --world-report
.\build\Release\rws-info.exe "$map\FR01_col.rws" --bsp-report
.\build\Release\rws-info.exe "$map\FR01_col.rws" --validate-types
.\build\Release\rws-info.exe "$map\FR01_col.rws" --export-collision-gltf "$out\FR01.collision.gltf"
.\build\Release\rws-info.exe "$map\FR01_col.rws" --export-collision-obj "$out\FR01.collision.obj"
```

Retain the Sprint 01 geometry invariants:

| Check | Expected |
| --- | ---: |
| Collision World recovered sectors | 326 |
| Collision World triangles | 99,653 |
| Collision World vertices | 84,884 |
| Collision materials | 14 |
| Typed decode failures | 0 |

Plane totals and maximum depth must be recorded only after the shared topology
implementation validates them against the corpus. Do not put guessed reference
numbers into tests or suppress existing truncation diagnostics to make a report
appear complete.

### Manual GUI matrix

Verify at minimum:

| Scenario | Expected result |
| --- | --- |
| Pick visible collision in combined mode | Nearest collision hit is identified without confusing main-tree selection |
| Pick in X-ray mode | Nearest geometric hit is stable and documented |
| Select one sector repeatedly | Triangle identity, surface, offsets, and coordinates remain stable |
| Top/front/side views | Projection, grid labels, axes, framing, and picking agree |
| BSP selected path | Root-to-leaf planes and bounds match the selected sector |
| Partial BSP topology | Usable sectors render; missing path is clearly reported |
| Enable each clipping axis | Rendering and picking reject the same clipped geometry |
| Measure two collision points | Distance and per-axis deltas remain visible until cleared |
| Choose a nonstandard companion | New document is validated and replaces the previous companion transactionally |
| Cancel/choose invalid companion | Current valid collision state remains intact |
| Reopen a recent pairing | Paths are revalidated and stale entries are handled cleanly |
| Export collision glTF/OBJ | Only collision geometry is present and counts match the inspector |
| Open `_col.rws` directly | Inspection and export work without a visual sibling |
| Switch to unrelated asset | Old hit, measurement, BSP path, and companion GPU data are cleared |

## Performance targets

These are guardrails rather than hard benchmarks:

- No ray/triangle intersection loop unless the user requests hover readout or a
  click/measurement operation.
- Sector bounds reject most collision triangles before exact ray tests.
- No per-frame raw-byte topology recovery or filesystem probing.
- BSP line buffers rebuild only when the document or overlay scope changes.
- Grid geometry is bounded and based on the visible range, not the full coordinate
  extent at fixed resolution.
- Clipping changes uniforms/state and reuses existing mesh buffers.
- Collision export streams or stages data within practical memory for FR01.
- Combined FR01 inspection remains interactively navigable on the target machine.

If hover readout is too costly, throttle it or require a modifier; keep click
selection immediate and deterministic.

## Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Broken declared sizes make Plane ordering ambiguous | Validate physical candidates and child sequence; return a partial forest rather than guessing |
| Collision and visual offsets alias | Tag every hit with source document and layer identity |
| Picking expanded GPU triangles loses source identity | Store explicit World/sector/triangle mapping during upload |
| CPU picking stalls on large maps | Reject by sector bounds and run exact tests only on interaction |
| Orthographic rendering and picking disagree | Build both projection matrix and ray from one camera description |
| X-ray draw order changes the selected triangle | Define picking by nearest geometry, independent of render order |
| Grid obscures collision or shimmers | Use adaptive spacing, fading, and depth-aware line styling |
| Clipping hides geometry but picker still finds it | Share one source-space clipping predicate between rendering logic and CPU hit tests |
| Manual pairing loads an unrelated World | Require explicit user choice, validate recovery, and display both source paths |
| Recent paths leak into the repository | Store a bounded per-user settings file outside the worktree |
| OBJ names cannot preserve raw surface strings safely | Sanitize identifiers and retain raw values in comments/manifest |
| Partial export appears complete | Report recovered/usable/skipped counts and carry diagnostics into the manifest/status |

## Definition of done

Sprint 02 is complete when all of the following are true:

- Collision selection uses a tagged main/collision source identity and stable
  World/sector/triangle indices.
- The GUI can select and highlight a collision triangle and show its offsets,
  vertices, material, raw surface metadata, hit coordinate, normal, and barycentric
  coordinates.
- Picking respects visibility, clipping, active projection, and nearest-hit rules.
- `rws_core` exposes validated BSP Plane/leaf topology independently from flat
  recovered geometry, with partial results and diagnostics.
- `rws-info --bsp-report` distinguishes declared, conservatively parsed, and
  recovered topology counts/status.
- The GUI can show leaf bounds, split planes, and the selected validated BSP path.
- Perspective plus top/front/side orthographic inspection, adaptive grid, axes,
  coordinate readout, and frame-selection work on the reference map.
- Axis-aligned clipping and two-point distance measurement agree with picking.
- A manually selected companion is validated transactionally, can be cleared, and
  can be reopened from a bounded per-user recent-pairing list.
- Collision-only glTF and OBJ export preserve sector/material/surface/source
  metadata and report skipped invalid data.
- Synthetic tests cover topology, identity, picking, clipping, measurement, and
  export without committing game assets.
- Release and Debug builds/tests pass.
- FR01 retains the Sprint 01 recovery totals of 326 sectors, 99,653 triangles,
  84,884 vertices, and 14 materials.
- Existing scene preview, collision overlay, tree selection, scene glTF, Geometry
  OBJ, lightmap, and save-to-copy behavior is not regressed.
- No game resources, exports, recent-path settings, generated build content, or
  unrelated user changes are added to the repository.

## Follow-up backlog

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
- Add a dependency graph for `.m3d`, `.phd`, `.and`, `.txl`, textures, models,
  Physics, and animations.
- Support `.rpc` as a first-class RenderWare Clump extension in GUI and corpus
  tools.
- Preview scene fog, camera settings, sky, water fog, and placement visibility
  distance/fade controls.
- Add HAnim plus `.anm` playback.
