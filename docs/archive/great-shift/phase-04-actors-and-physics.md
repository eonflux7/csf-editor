> **Archived plan — implemented.** Association and inspection behavior as built is
> documented in [Actor and Physics inspection](../../reference/actor-physics-inspection.md);
> this document is kept for its rationale, risks, and test plan.

# Phase 04: actor models and Physics

## Outcome

Replace generic mission markers with resolved model instances where possible and
make object collision understandable. A selected actor can display its RPC model,
skeleton, `.cmo` source-like collision shapes, compiled RenderWare Physics body,
and—when applicable—ragdoll bodies/joints in the same coordinate space.

The goal is inspection and correlation, not running the Physics solver.

## Dependencies

- Phase 02 resource resolver and BDD/PHD associations.
- Phase 03 typed actor placements and Mission workspace.
- Existing Clump, Geometry, Skin, HAnim, `0x907`, and `0x909` decoders.

## In scope

- First-class `.rpc` loading in GUI, CLI, corpus tools, and resource graph.
- DFF-reference to RPC-resolution diagnostics.
- Typed `Objetos.bdd` records needed for model/collision/Physics association.
- Static actor model instancing at SCN transforms.
- LOD listing and manual/diagnostic selection; no automatic fidelity claim until
  distance semantics are confirmed.
- HAnim skeleton/rest-pose rendering.
- A bounded parser for `.cmo` shapes, bone indices, labels, offsets, and hot points.
- CMO shape rendering on object transforms and skeleton bones.
- Rendering `0x907` sphere, capsule, box, cylinder, and Trilist volumes.
- Center of mass, inertia orientation/values, finite-rotation axis, flags, material
  properties, and collision-group inspection.
- Rendering `0x909` ragdoll bodies, body IDs, joint links, and established joint
  fields over a compatible skeleton.
- Side-by-side and overlay comparison of CMO and compiled Physics.
- Actor/model/Physics compatibility and missing-resource diagnostics.
- Standalone model laboratory as well as mission-context inspection.

## Explicitly out of scope

- Real-time collision response or rigid-body simulation.
- Claiming CMO and `0x907` are equivalent when they differ.
- Inventing semantics for unresolved ragdoll joint fields.
- Automatic LOD switching before distance/units are verified.
- ANM playback or animated hitboxes; Phase 05 supplies poses.
- Model or Physics editing.
- Replacing game collision generation or mass-property calculation.

## Association model

The selected actor may resolve through several evidence sources:

```text
SCN actor class ID
└─ Objetos.bdd record
   ├─ MODELO / LOD model references
   ├─ MODELO_COLISION → .cmo
   ├─ PHYSIC + MODEL_FILE → Physics .rws
   └─ character type → shared ragdoll / animation metadata

PHD
└─ independent model ↔ Physics dependency evidence
```

Keep each edge's evidence. If BDD and PHD disagree, report both rather than
silently overriding one.

## Model instance design

Do not duplicate mesh buffers for every actor. Cache decoded/uploaded RPC
prototypes by resolved resource identity and render instances with actor transforms.

The renderer must distinguish:

- map Clump prototypes and embedded scene-instance clones;
- mission actor RPC prototypes;
- first-person hand/item models;
- third-person character models;
- LOD variants;
- collision and Physics overlays.

Picking returns actor identity plus the selected model atomic/bone/shape where
appropriate.

## CMO parser

CMO resembles a bracketed source configuration with comments and fields such as:

- version;
- external shape;
- internal shapes;
- shape type;
- center, dimensions, and radius;
- bone index;
- label;
- hot points;
- object-3D usage.

Implement a small lexer/parser that preserves comments, token spelling, ordering,
unknown fields, and source ranges. Do not reduce the document immediately to only
known shapes; later authoring requires the original structure.

Validate finite numeric values, nonnegative extents/radii, supported shape names,
and bone indices against the resolved skeleton while keeping invalid records
inspectable.

## Physics visualization

### Body definitions (`0x907`)

Render established volume types using the matrices and dimensions already decoded:

- sphere;
- capsule;
- box;
- cylinder;
- recursive Trilist aggregate.

Inspector fields should include:

- shape/version and nested volume path;
- local transform;
- fatness/radius and shape-specific dimensions;
- friction and restitution;
- volume flags and collision group;
- mass and center of mass;
- principal inertia and orientation;
- spherical/scalar inertia approximation;
- linear/angular damping;
- finite-rotation axis and body flags.

### Ragdoll definitions (`0x909`)

Show all embedded bodies and their IDs, then draw joint relationships using only
confirmed anchors/axes/limits. Unresolved numeric joint fields remain labelled by
record offset/type. Provide a table view even when skeleton association fails.

### Comparison

Support three modes:

- CMO only;
- compiled Physics only;
- both, with distinct colors and discrepancy diagnostics.

Useful measurements include bounds difference, center difference, per-bone shape
presence, and gross volume mismatch. These are analytical comparisons, not proof
that one representation is wrong.

## Work breakdown

### P04-01: first-class RPC support

- Expand file dialogs, drag/drop, CLI usage, and corpus scanning.
- Add `.rpc` root/type validation and clear labeling as Clump.
- Add prototype caching and standalone model workspace entry.

### P04-02: typed object database view

- Decode stable `Objetos.bdd` record fields and IDs.
- Resolve visual, LOD, collision, Physics, ragdoll, and animation-related references.
- Report duplicate/missing class definitions.

### P04-03: mission model instancing

- Place resolved RPCs at SCN transforms.
- Preserve marker fallback for unresolved/invalid models.
- Add visibility budgets and class/model filters.

### P04-04: skeleton and CMO

- Render HAnim frames/bones and name/ID labels.
- Implement lossless CMO parsing and diagnostics.
- Attach and render shape records by bone index.
- Show hot points and unresolved bone mappings.

### P04-05: Body Definition overlays

- Generate line/solid primitive meshes for all observed shapes.
- Support nested Trilist transforms.
- Draw mass, inertia, and finite-axis helpers.
- Add shape picking and source inspection.

### P04-06: ragdoll overlay

- Associate ragdoll body IDs with skeleton nodes where evidence permits.
- Draw body and joint graph.
- Expose unresolved joint data without speculative labels.

### P04-07: comparison and reports

- Add CMO-versus-Physics overlay and quantitative bounds/center reports.
- Add CLI model/object/Physics association reports.
- Document all association rules and confidence.

## Test plan

### Core tests

1. CMO lexer handles comments, bracket nesting, identifiers, strings, and numbers.
2. Known box/sphere/ellipsoid records decode with exact source ranges.
3. Unknown CMO fields and ordering are preserved.
4. Invalid/negative dimensions diagnose without discarding sibling shapes.
5. Bone index lookup succeeds/fails deterministically.
6. DFF-to-RPC and BDD/PHD associations retain their evidence.
7. Prototype cache shares mesh data across actor instances.
8. Physics primitive bounds match synthetic decoded volumes.
9. Nested Trilist transforms compose correctly.
10. Ragdoll body/joint identity survives filtering and selection.
11. Comparison metrics use a documented coordinate space.

### Manual corpus matrix

- one skinned character and its Soldier CMO hit volumes;
- one player/first-person hands record;
- a simple decorative rigid body;
- a vehicle with CMO and `0x907` data;
- a nested Trilist body;
- `Models\ragdoll.rws` over a compatible character skeleton;
- missing model/collision/Physics references;
- many repeated actors sharing one RPC prototype.

## Performance targets

- Upload each resolved RPC prototype once per graphics context.
- Instance repeated actors without expanding full CPU/GPU geometry per actor.
- Generate Physics primitives once per unique body definition where possible.
- Skeleton/shape label rendering must be optional and culled.
- Lazy-load actor models based on visibility/selection or an explicit preload
  policy; mission markers remain usable while loading.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| SCN actor transform convention is incomplete | Show raw and composed transforms; validate against landmarks |
| Requested DFF maps to wrong RPC | Display resolution rule and allow explicit candidate inspection |
| Bone IDs, frame indices, and CMO bone indices differ | Keep mappings explicit and diagnose rather than offset-guessing |
| Physics local matrices are composed in wrong order | Synthetic transform tests plus visual comparison with known models |
| Dense actor geometry overwhelms viewport | Instancing, culling, budgets, and marker fallback |
| Ragdoll fields remain uncertain | Render only confirmed relationships; show raw typed records for the rest |

## Definition of done

- RPC is accepted and reported as a first-class Clump format throughout the tools.
- Resolved mission actors can render their RPC model at the SCN placement while
  unresolved actors retain useful markers.
- A selected skinned model can show its HAnim rest skeleton.
- CMO collision/hit shapes render on object or bone transforms with source-backed
  selection.
- All observed `0x907` shape families render and expose established physical data.
- The supplied `0x909` ragdoll can be inspected as bodies and joints, with clear
  uncertainty for unresolved fields.
- CMO and compiled Physics can be compared without treating either as disposable.
- Prototype/overlay caches remain bounded and mission switching clears ownership.
- Synthetic tests and existing rendering/export regressions pass.

