> **Archived plan — implemented.** This decision document and the seven phase
> designs below were the plan for turning the RWS inspector into a mission
> workbench. Every phase shipped; the behavior as built lives in
> [`docs/reference/`](../../reference/) and [`docs/guides/`](../../guides/). The
> phase documents are kept for design rationale and test plans, not as a
> description of current behavior.

# The Great Shift: from RWS inspector to CSF mission workbench

## Decision

The project will grow from a RenderWare stream inspector into a read-only-first
*Commandos: Strike Force* mission workbench and, after its serializers are proven,
a guarded mod-authoring tool.

This is a scope expansion, not a rewrite. The existing RenderWare parser,
collision recovery, preview, picking, export, and inspection code remains the
graphics and low-level asset foundation. New CSF-specific layers will assemble the
game's scene, databases, scripts, models, Physics, and animations into one mission
view.

The product-level unit is no longer one `.rws` file. It is a mission package:

```text
Mission workspace
├─ scene and gameplay placements       .scn
├─ mission and cutscene logic          .gsc / .csc
├─ object/animation/weapon databases   .bdd
├─ package dependency indexes          .vis / .m3d / .phd / .and / .txl
├─ visual models and skeletons         .rpc
├─ skeletal animations                 .anm
├─ object collision and ragdolls       .cmo / Physics .rws
├─ static map and level collision      map .rws / _col.rws
└─ textures, effects, UI, and audio    .dds / .png / .sp / .fbs / .wad
```

## Why this shift

The RWS work has reached the point where it can render and inspect the visual map
and its static collision, but the data that gives the map gameplay meaning lives
outside those streams. The supplied corpus establishes that `.scn` contains
actors, player starts, navigation, named routes, areas, dummies, cameras, and
lights; `.gsc` and `.csc` contain mission and cutscene programs; and the BDD files
connect class IDs to models, collision, Physics, animations, weapons, effects,
materials, and sounds.

Continuing to treat these as unrelated formats would leave `rws-man` as a strong
asset inspector but a weak modding tool. Resolving them together enables concrete
workflows:

- select an actor on the map and inspect its class, model, collision, scripts,
  routes, and animations;
- visualize player starts, navigation graphs, named routes, zones, cameras,
  dummies, and lights over the existing map;
- trace script references to scene objects and database records;
- compare source-like `.cmo` shapes with compiled RenderWare Physics;
- play a selected `.anm` on its HAnim/Skin model;
- validate a staged mod and write only new output files.

## Architectural direction

Keep the format layers independent even when the GUI presents one workspace:

```text
                    ┌────────────────────┐
                    │ Mission workspace  │
                    └─────────┬──────────┘
                              │
              ┌───────────────┼────────────────┐
              │               │                │
       ┌──────▼──────┐ ┌──────▼──────┐ ┌──────▼──────┐
       │ csf_core    │ │ rws_core    │ │ asset I/O   │
       │ CSFFBS, DBs │ │ RW, Physics │ │ images/audio│
       │ packages    │ │ geometry    │ │ staging     │
       └─────────────┘ └─────────────┘ └─────────────┘
```

- `rws_core` remains responsible for RenderWare streams, World recovery,
  Physics records, geometry, and export.
- A new `csf_core` layer owns CSFFBS, typed mission/database views, dependency
  indexes, reference resolution, and mission assembly.
- `rws-man` gains a Mission workspace while retaining its low-level Inspector,
  Geometry, and Scene workspaces.
- A new `csf-info` CLI handles non-RWS inspection, conversion, dependency reports,
  and validation. `rws-info` remains focused on RenderWare.
- A future `csf-man` executable or product rename is a packaging decision, not a
  prerequisite. Do not duplicate the viewport and inspection infrastructure.

## Roadmap

| Phase | Outcome | Depends on |
|---|---|---|
| [01 — Generic CSFFBS core](phase-01-generic-csffbs-core.md) | Safely parse, inspect, validate, and export all CSFFBS documents | Existing project baseline |
| [02 — Mission package resolution](phase-02-mission-package-resolution.md) | Open a mission as a dependency graph rather than isolated files | Phase 01 |
| [03 — Mission Explorer](phase-03-mission-explorer.md) | Show gameplay objects, paths, zones, cameras, and cross-references over the map | Phases 01–02 |
| [04 — Actors and Physics](phase-04-actors-and-physics.md) | Resolve actor models and visualize object collision, hit shapes, and ragdolls | Phases 02–03 |
| [05 — Animation and cutscenes](phase-05-animation-and-cutscenes.md) | Play ANM clips and inspect script/cutscene animation and camera sequences | Phases 01–04 |
| [06 — Guarded authoring](phase-06-guarded-authoring.md) | Make schema-aware edits with lossless serialization and validation | Phases 01–05 as needed |
| [07 — Mod staging and ecosystem](phase-07-mod-staging-and-ecosystem.md) | Build reproducible, non-destructive mod output and cover secondary formats | Phase 06 |

The phases are ordered by dependency, not by a promise that every item in one
phase must be complete before exploratory work begins in the next. A phase may be
split into implementation sprints, but its definition of done remains the gate for
claiming that capability complete.

## Product milestones

### Milestone A: resource intelligence

Phases 01 and 02. A user can open or scan the extracted resources, inspect any
CSFFBS document, follow dependencies, find missing or ambiguous references, and
answer “where is this asset used?” without modifying game data.

### Milestone B: useful Mission Explorer

Phase 03. A user can open `.scn` and see player starts, actors, routes, navigation,
zones, cameras, dummies, and lights aligned with visual and collision geometry.
Selection links the map object to its source record and referenced scripts.

### Milestone C: character and object laboratory

Phases 04 and 05. Models, skeletons, collision shapes, Physics bodies, ragdolls,
and selected animation clips can be inspected in isolation and in mission context.

### Milestone D: safe mod workflow

Phases 06 and 07. Supported fields can be edited, serialized, reopened, validated,
staged into a separate tree, and packaged without overwriting the source corpus or
game installation.

## Project-wide invariants

These rules apply to every phase:

1. Treat unpacked game resources as read-only reference data.
2. Never require corpus files as committed test fixtures.
3. Preserve entry ordering, unknown fields, raw strings, and malformed/truncated
   tails unless a user explicitly chooses a repair operation.
4. Write modifications and exports to new paths; never silently overwrite inputs.
5. Keep parsing and semantic interpretation separate. A document may parse safely
   before all identifiers have gameplay names.
6. Record source file, byte offset, table index, and stable object identity for
   every inspectable record.
7. Mark inferred relationships and uncertain fields as such in both code and UI.
8. Make reference resolution deterministic, case-insensitive in the same way as
   the Windows game environment, and explicit about ambiguity.
9. Make corpus scans and validators read-only and reproducible from command line.
10. Add synthetic tests for each parser, serializer, resolver, and editor behavior.
11. Keep `build.ps1`, `test.ps1`, CMake presets, and README commands synchronized
    whenever targets or build behavior change.
12. Preserve the current RWS inspection, collision, preview, and export workflows
    throughout the shift.

## Deliberate non-goals

The roadmap does not initially promise:

- a complete clone of the game runtime or AI;
- exact reconstruction of live entity state at an arbitrary gameplay moment;
- a visual programming language that can author every possible GSC instruction;
- creation of a new map from an empty directory;
- support for unrelated RenderWare games;
- automatic distribution of copyrighted game resources;
- in-place modification of the original unpack or installed game;
- semantic names for fields that have not been established from evidence.

The first useful target is a trustworthy mission explorer. Authoring expands only
where the project can serialize, reopen, validate, and explain the result.

## Supporting documents

- [Resource report](../../corpus/resources.md) records the corpus evidence and format
  priorities that motivated the shift.
- [Existing RWS corpus findings](../../corpus/rws.md) remain the authoritative
  detailed record for RenderWare and Physics reverse engineering.
- [RWS format notes](../../formats/rws-format.md) remain the source for chunk schemas.
- [Current implementation roadmap](../rws-roadmap.md) describes the pre-shift RWS
  foundation and should be retained as historical/current low-level context.

