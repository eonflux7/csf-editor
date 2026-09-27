# Actor and Physics inspection

Phase 04 keeps association evidence separate. An SCN actor joins to every
`Objetos.bdd` definition with the same class ID. Each model, LOD, CMO, Physics,
ragdoll, or animation reference is then resolved independently through the
mission resource index. Duplicate class definitions remain visible and no
candidate is selected silently. PHD-derived Physics edges remain independent
graph evidence; they do not overwrite an object-database reference.

Use the association report to inspect these decisions:

```powershell
csf-info mission Mission.scn --associations --root D:\Game\Unpacked
```

The report prints the source BDD entry and field, resolution rule, requested
spelling, and resolved path. A `.dff` request resolved to `.rpc` is explicitly
reported as `dff-to-rpc`. Missing and ambiguous references retain their candidate
sets in the mission graph.

## RPC model laboratory

`.rpc` is treated as a first-class RenderWare Clump container by `csf-editor`,
`rws-info`, and `rws-corpus`. Opening an RPC directly enters the model laboratory.
The root must be a Clump (`0x10`); a differently typed root is rejected. Rest-pose
HAnim bones are drawn in green and bone IDs can be enabled from the viewport
toolbar. LOD entries are listed as explicit variants; the UI does not claim an
automatic distance rule.

Repeated scene instances reuse the uploaded prototype vertex range. Each draw
has its own affine transform, so repeated instances no longer expand a second
copy of the mesh in the GPU vertex buffer. Mission/document switches destroy the
owned preview buffers and textures. Mission actors with a uniquely resolved
visual RPC are placed at their SCN position/heading/pitch and share one prototype
per normalized resource identity. Loading is bounded to 32 unique RPC prototypes
and 512 actor instances per mission; actors beyond the budget, or actors with
missing/invalid models, retain their markers.

## CMO source model

The CMO reader is lossless at the source level: it retains every token, comment,
spelling, unknown field, ordering, bracket tree, and byte range. A derived shape
view recognizes box, sphere, ellipsoid, capsule, and cylinder records plus center,
dimensions, radius, bone index, label, offset, object-3D use, and hot points.
Invalid sizes and unresolved bone indices are diagnostics; they do not remove the
record or its siblings.

```powershell
csf-info cmo Models\Soldier.cmo
```

## Compiled Physics and ragdolls

Body Definition (`0x907`) inspection exposes recursive volume paths, matrices,
fatness or shape dimensions, material coefficients, flags, collision group, mass,
center of mass, principal/scalar inertia, inertia orientation, damping, and the
finite-rotation axis. Bounds utilities compose nested Trilist transforms in
RenderWare right/up/at-plus-position convention.
The standalone viewport draws box edges, sphere great circles, capsule/cylinder
rings, recursive Trilist children, center-of-mass crosses, and finite-axis helpers
in the same object space. Ragdoll bodies use a distinct color.

Ragdoll Definition (`0x909`) retains bodies, body IDs, joint pairs, and each joint's
typed record in source order. Unknown joint numbers are shown as `u32`, four-float,
or `0x1A` triple records with the record offset. Their meanings are deliberately
not guessed.

CMO-versus-Physics comparison uses object-local coordinates after applying each
shape's local transform. It reports axis-aligned bounds extent delta, center delta,
and the ratio of the two bounding-box volumes. These are analytical discrepancy
signals, not an equivalence test or an assertion that either representation is
incorrect.
