# Phase 03: visual Mission Explorer

## Outcome

Make the mission—not the individual RWS—the main visual workspace. Opening an SCN
loads its resolved visual map and collision companion, then overlays selectable
player starts, actors, navigation points and connections, named groups/routes,
dummies, areas/zones, cameras, lights, and other high-confidence spatial records.

Selection must join three views of the same source-backed object:

1. its location and relationships in the 3D viewport;
2. its typed SCN record and raw CSFFBS nodes;
3. scripts/database/resource records that reference it.

This is the first product milestone that should be presented as the CSF Mission
Explorer.

## Dependencies

- Phase 01 generic CSFFBS tree and source identities.
- Phase 02 mission graph and resource resolution.
- Existing scene renderer, collision overlay, picking, orthographic views,
  clipping, measurement, and selection infrastructure.

## In scope

- Typed read-only views for the stable SCN top-level sections.
- Typed actors with name, class ID, transform, flags, group/cell, script, collision,
  and other preserved fields.
- Player metadata and Commando/Sniper/Spy start markers.
- Navigation groups, points, and connections.
- Named route/group presentation where the SCN structure supports it.
- Dummies and folder/hierarchy relationships.
- Areas/zones with the most conservative validated geometry available.
- Cameras, targets, orientation/FOV where established.
- Lights and basic bounds/radius visualization.
- Map-level environment fields in an inspector: sky, fog, water fog, ambience,
  minimap, and related settings.
- A Mission workspace using resolved visual and collision RWS documents.
- Visibility/filter controls per overlay kind.
- Stable picking and selection for overlays.
- GSC/CSC generic script records and BDD record indexes sufficient for
  cross-reference navigation.
- Search by name, class ID, script, group, path/point, identifier, and source file.
- Source-location navigation into the generic CSFFBS tree.
- Read-only export of typed mission summaries and overlays.

## Explicitly out of scope

- Rendering every actor with its final RPC model; markers and bounds are enough
  for this phase.
- Physics simulation, ragdoll posing, or ANM playback.
- Full GSC execution or AI simulation.
- Editing SCN or scripts.
- Guessing zone volume geometry when only a name/reference is understood.
- Treating named string correlation alone as a proven typed reference.
- Replacing the existing low-level RWS Inspector workspace.

## Typed SCN design

Create typed projections over the immutable generic CSFFBS document. Each typed
object retains the source node/entry identity and may carry diagnostics for fields
that are absent, duplicated, or of the wrong kind.

```cpp
struct MissionActor {
    CsfSourceId source;
    std::string name;
    std::string class_id;
    Vec3 position;
    float heading{};
    std::optional<float> pitch;
    std::string script;
    std::int32_t group{};
    std::int32_t cell{};
    std::uint32_t flags{};
    RawFieldMap unknown_fields;
};

struct NavPoint {
    CsfSourceId source;
    NavPointId id;
    Vec3 position;
    std::optional<std::string> name;
    RawFieldMap unknown_fields;
};

struct NavConnection {
    CsfSourceId source;
    NavPointRef origin;
    NavPointRef destination;
    std::optional<NavGroupRef> origin_group;
    std::optional<NavGroupRef> destination_group;
};
```

Use explicit optional values rather than invented defaults. A field that is
missing in source must remain distinguishable from a source value of zero.

## Reference index

Build an index over SCN, GSC, CSC, and BDD strings/typed values:

```text
symbol
├─ definitions
│  ├─ SCN actor / point / dummy / area / camera
│  ├─ GSC/CSC script / variable / event
│  └─ BDD object / animation / weapon / effect / sound
└─ references
   ├─ script operands
   ├─ actor script/class fields
   ├─ route/group membership
   └─ resource dependencies
```

Start with exact, case-preserving matches within the expected typed category.
Expose untyped string matches separately as candidates. Never present a substring
match as a proven reference.

## Viewport design

### Overlay layers

Provide independent layers for:

- player starts;
- actors/entities;
- navigation points;
- navigation connections;
- named groups/routes;
- dummies;
- areas/zones;
- cameras and targets;
- lights;
- visual map;
- collision map.

Each layer needs visible/hidden state, a legend, count, and predictable depth
behavior. Icons and line styles should remain legible in perspective and existing
top/front/side orthographic projections.

### Picking and identity

Extend the existing tagged selection identity rather than overloading RWS chunk
offsets:

```cpp
using MissionSelection = std::variant<
    RwsSelection,
    CollisionHit,
    ActorSelection,
    PlayerStartSelection,
    NavPointSelection,
    NavConnectionSelection,
    DummySelection,
    AreaSelection,
    CameraSelection,
    LightSelection>;
```

Every selection must carry mission document identity plus source entry/node index.
Names are labels, not identity; duplicate and empty names are legal.

Define click precedence explicitly. A practical default is the nearest visible
overlay marker/line within a screen-space threshold, then existing visual/collision
geometry according to the user's picking preference.

### Inspector

Selecting an actor should show, when available:

- source SCN file and entry offset/index;
- name, class ID, transform, flags, group/cell, collision, and script;
- class-definition candidates from `Objetos.bdd`;
- referenced GSC/CSC script definitions and use sites;
- route/path/zone memberships or references;
- resolved and unresolved model/Physics/animation dependencies;
- a link to the raw CSFFBS subtree.

Equivalent focused inspectors should exist for points, connections, groups,
dummies, areas, cameras, and lights.

## Work breakdown

### P03-01: typed SCN header and actors

- Decode map/environment fields conservatively.
- Decode player information and start positions.
- Decode actor arrays with source identities and diagnostics.
- Add CLI reports before GUI rendering.

### P03-02: navigation graph

- Decode groups, points, and connections.
- Validate point/group references and duplicate IDs.
- Add graph statistics, connected components, orphan points, and invalid links.

### P03-03: spatial helpers

- Decode dummies/folders, areas, cameras, lights, and bridges where structure is
  established.
- Preserve unsupported subrecords in the generic tree.
- Add bounds and coordinate validation.

### P03-04: mission workspace lifecycle

- Open SCN through native dialog and drag/drop.
- Load resolved visual/collision documents transactionally.
- Keep low-level documents accessible without conflating their ownership.
- Clear all overlay GPU/selection state on mission switch.

### P03-05: overlay renderer and picker

- Add batched marker, line, and simple-volume rendering.
- Support perspective and orthographic views.
- Add stable screen-space picking and selection highlighting.
- Reuse clipping where its meaning is valid for overlays.

### P03-06: symbol/reference index

- Add definitions and typed references across SCN/GSC/CSC/BDD.
- Show candidate-only string matches separately.
- Add “find uses” and “go to source” navigation.

### P03-07: filters, search, and export

- Search/filter overlays by type, name, class, script, group, and diagnostic.
- Export a mission summary and optional overlay geometry/JSON to new files.
- Add screenshot-friendly visibility presets without changing source data.

## Test plan

### Synthetic typed-view tests

1. One actor with all supported fields.
2. Missing optional versus explicit zero values.
3. Duplicate and empty actor names retain distinct identities.
4. Unknown actor fields remain reachable in the generic tree.
5. One navigation group with points and connections.
6. Cross-group connection.
7. Invalid point/group references.
8. Duplicate point IDs.
9. Empty navigation and spatial sections.
10. Dummy hierarchy with missing parent.
11. Camera-target and light-radius records.
12. Typed script/class references versus candidate string matches.
13. Stable source navigation after filtering/sorting.

### Viewport tests

- projection and picking math for point/line markers;
- screen-space hit threshold independent of zoom within documented bounds;
- nearest and priority behavior with overlapping overlay and collision objects;
- hidden or clipped overlays are not picked;
- mission switch clears selection and GPU buffers;
- visual/collision RWS behavior remains unchanged when no mission is loaded.

### Read-only mission matrix

Validate at least:

- one ordinary single-scene mission;
- a large scene such as Snipers;
- a small scene such as Convoy or C47_Cut;
- FR02A/B/C naming and package-copy cases;
- a mission with useful vehicle routes and cutscene cameras;
- a mission with missing/partial optional sections, if present.

Record counts and diagnostics as corpus observations, not hardcoded synthetic test
expectations based on copyrighted fixtures.

## Performance targets

- Batch overlay geometry by layer; do not issue one draw call per actor/point.
- Build typed views and reference indexes on mission load, not every frame.
- Use spatial acceleration or screen-space coarse rejection if point/line picking
  becomes costly on the largest scene.
- Virtualize long actor/script/reference lists.
- Lazy-load RPC, ANM, texture, and Physics payloads; markers need only SCN data.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Coordinate conventions differ among SCN and RWS | Validate landmarks and expose raw/transformed coordinates |
| Names are duplicated or used across categories | Use source identity; category-aware references |
| Large overlay set obscures map geometry | Independent layers, filters, legends, and scale-aware markers |
| Partial schema is presented as complete | Show unsupported fields and typed-view diagnostics |
| Script string correlation creates false links | Separate typed/proven edges from candidate matches |
| Mission workspace destabilizes RWS inspection | Keep document ownership and workspaces separate |

## Definition of done

- Opening an SCN creates a Mission workspace with its resolved visual and collision
  map when available.
- Player starts, actors, navigation, dummies, areas, cameras, and lights with
  established schemas are visible, filterable, and selectable.
- Every overlay selection maps back to stable SCN source identity.
- Actor inspection shows class/script/resource candidates and reference use sites.
- Navigation validation reports orphan/invalid references without preventing usable
  data from rendering.
- Mission switching is transactional and clears stale state.
- Perspective and orthographic picking agree with visible overlays.
- Synthetic typed-view, reference, and viewport tests pass.
- Existing RWS Scene, Geometry, Inspector, collision, and export workflows remain
  available and pass their regression tests.

