# Mission Explorer typed-view contract

The Mission Explorer is a read-only projection over an immutable CSFFBS SCN and
the Phase 02 mission dependency graph. It never owns or rewrites corpus bytes.
The visual and collision RWS documents remain independent `rws_core` documents;
switching missions validates replacements before clearing the previous viewport,
overlay, and selection state.

## Stable identity and optional values

Every typed object carries:

- source file;
- semantic entry index;
- byte offset and size of that entry.

Names are display labels and are never identity. Empty and duplicate names are
retained. Missing scalar values are represented as `std::optional`; a missing
field therefore differs from a source value of zero. Unsupported actor fields
remain reachable in the generic tree and are listed in `unknown_fields`.

The established projections are:

- `.MUNDOVIS` scalar environment fields and `.PLAYER`/`.INICIO_*` metadata;
- `.BICHOS` actor records, including ID, class, transform, collision, flags,
  secondary-explosion setting, optional faction/portrait, explicit animation
  bindings, door box, optional script, and navigation cell;
- `.MALLA_NAVEGACION` groups, points, local links, and cross-group links;
- `.MALLA_DUMMIES` records and nested folder memberships;
- `.MALLA_AREAS` polygon points, height/flag fields, occlusion, and reverb fields;
- `.MALLA_LUCES` position, color, modulation, and radius;
- `.EFECTOS` ID, class, dummy placement, priority, and share-group records.

The GUI presents these typed values before the raw subtree. It draws orientation
and navigation arrows, extrudes area outlines by their height, uses each SCN light
color, places effects through an exact dummy-ID join, and exposes dummy/light
folder membership with per-folder visibility controls. Missing or ambiguous
effect dummy references remain visible as diagnostics rather than being guessed.

## Placement and rotation conventions

Actor `.ANGULO` and `.ANGULO_X` values are degrees. Navigation-point and dummy
`.ROT`/`.ROT_X` values are radians and are not inferred from their magnitude or
implicitly normalized. The viewport converts actor angles exactly once at the
model/overlay boundary.

An actor with a non-negative, uniquely resolved `.CELDA` group/point uses that
navigation point as its effective spawn position. Actor `.POS` remains visible as
the authored fallback and is used when the cell is absent, negative, ambiguous,
or has no position. The reference corpus stores identical `.POS` and cell-point
positions for every resolvable actor, but the editor keeps both values distinct
so modified files expose a disagreement instead of silently losing it.

No record is promoted to a camera, player-start position, or zone volume solely
because its name suggests that role.

## Navigation validation

Point identity is the pair `(group ID, point ID)`, and both components must be
explicit and unique. Validation counts duplicate group IDs, duplicate point IDs
within a group, missing or ambiguous endpoints, connected components, and points
with no valid incident link. Links with bad endpoints are retained with
`valid=false` and an `invalid_reason`; usable parts of the graph still render.

## References

The reference index has three explicit roles:

- `definition`: a typed SCN name, `Objetos.bdd` object ID/name, or established
  script/database named record;
- `typed-reference`: an actor class ID or typed script field;
- `candidate`: an exact case-preserving string match with no proven typed edge.

Substring matches are used only by the GUI filter and are never returned as
references. `class:<integer>` is the CLI symbol form for actor/object class joins.

## CLI

```powershell
csf-info mission Mission.scn --objects
csf-info mission Mission.scn --navigation
csf-info mission Mission.scn --spatial
csf-info mission Mission.scn --symbols class:55
csf-info mission Mission.scn --symbols Espia
csf-info mission Mission.scn --scene-json new-overlay.json
```

`--scene-json` uses schema `csf-mission-scene-1` and includes source identities,
environment fields, actor records, retained unknown fields, complete navigation
topology and validation counters, dummies, area polygons, lights, effects, folder
memberships, and typed diagnostics. Non-finite floating-point values serialize
as `null` with a diagnostic. It refuses an existing path and publishes through
an exclusively created same-directory temporary file.

## Corpus observations

A read-only scan of the 21 locally available SCNs produced 3,564 actors, 1,714
navigation groups, 17,201 points, 8,070 valid connections, 5,295 dummies, 1,404
areas, 1,382 lights, and 2,055 effect records. No connection referenced a missing
group/point in that corpus. These are observations, not parser limits or fixture
expectations.
