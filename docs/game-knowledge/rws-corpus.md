# Extracted-game RWS corpus

Full unpacked *Commandos: Strike Force* resource corpus inspected on 2026-09-05.

The complete unpacked set contains 410 `.rws` files totaling 295,884,822 bytes;
all 410 load without a fatal parser error. Fourteen map streams contain 4,989 CSF
scene-instance records. This count includes three identical campaign copies of the
1,171-record FR01 map (`Parachut`, `Ransom`, and `Resist`). Every prototype ID used
by all fourteen files correlates with a loaded Clump through the executable-derived
`prototype ID = 1000 + first valid Pyro Atomic object index` rule.

FR01 contains 1,171 placements: 382 named `ARBUSTO_A`, 59 named `ARBUSTO_B`, and
730 unnamed records. The original standard stream has 419 Atomics; after prototype
cloning, the assembled export contains 2,913 atomic meshes plus 40 World sectors.
Thus the foliage missing from the earlier preview was not extra World geometry—it
was this game-specific placement table.

The three scene-instance floats are visibility controls rather than unknown Atomic
parameters. Executable data flow identifies them as maximum visibility distance,
minimum visibility distance, and fade range. All observed values are zero except
the 441 named FR01 shrub placements, which use maximum distance `5000`; the other
two fields remain zero. The instance report now groups records by these values.

Placement masks use only `0x401`, `0x441`, `0x601`, and `0x641`. Ghidra recovers
these as enabled plus scene-registered, optionally mipmapped (`0x40`) and animated
(`0x200`). The CLI and GUI show these names while retaining the complete raw mask.

All 42 `.rws` files under a `Maps` directory pass the typed decoder sweep with zero
failures and zero ambiguous Geometry triangle layouts. The wider scan also exposed
variable-size Pyro World Sector per-vertex arrays and the executable's systematic
four-byte size overstatement for that plug-in; the schema and serializer defect are
documented in `rws-format.md`.

Four FR02 map streams begin with a standard Table of Contents root. Their entries
consume exactly and point to the following top-level Clump and World chunks with
matching types; no other supplied RWS file contains this root.

## Panzers subset inventory

The focused `Panzers` subset used for the original schema work contains:

The archive contains 1,299 files, including 36 `.rws` files totaling 35,221,739
bytes. All 36 load as bounded RenderWare streams. Only the already-known ST05 map
and collision files produce diagnostics.

| Root chunk | Vendor/object | Files | Bytes | Library stamp |
|---|---|---:|---:|---|
| World (`0x0000000B`) | Core / `0x0B` | 1 | 6,187,753 | RW 3.7.0.2 build 55 |
| Clump (`0x00000010`) | Core / `0x10` | 1 | 28,979,398 | RW 3.7.0.2 build 55 |
| `0x00000907` | RenderWare Physics / `0x07` | 33 | 48,300 | RW 3.7.0.2 build 24 |
| `0x00000909` | RenderWare Physics / `0x09` | 1 | 6,288 | RW 3.7.0.2 build 24 |

The vendor assignment is confirmed by Criterion's `RwPluginVendor` enum:
`rwVENDORID_CRITERIONRWP = 0x000009`. Therefore `0x907` and `0x909` are not Pyro
chunk IDs: they are objects 7 and 9 from the official RenderWare Physics component.

## Physics streams

Every Physics root contains exactly one standard Struct child spanning the root
payload. Object `0x07` files occur beside matching `.rpc` render models under
`Models/Deco`, `Models/Vehi`, and `Models/Weap`. Payloads are 240–5,384 bytes and
contain repeated tagged scalars, vectors, quaternions/matrices, and apparent shape
records. This strongly indicates collision/rigid-body descriptions associated with
the render models.

The sole object `0x09` is `Models/ragdoll.rws`. It contains recognizable bone names
such as `bone20`, which independently supports a Physics ragdoll/constraint role.

Ghidra analysis of `CommXPC.exe` resolves the root types conclusively. The executable
statically contains RenderWare Physics 3.7 and source-identification strings for
`RwpBodyDef.c`, `RwpRagdollDef.c`, `RwpGenericDef.c`, `RwpVolume.c`, `RwpBox.c`,
`RwpSphere.c`, `RwpCapsule.c`, `RwpCylinder.c`, and `RwpTrilist.c`.

The game's resource dispatcher at `0x006B8CD0` reads the root header and dispatches:

| Root | Reader | Resolved type |
|---:|---:|---|
| `0x907` | `0x00822B20` | `RwpBodyDef` |
| `0x909` | `0x00823B70` | `RwpRagdollDef` |
| `0x90B` | `0x00823270` | `RwpGenericDef` (supported by the game; absent from this corpus) |

The embedded serializer uses self-describing 32-bit tags. Confirmed primitive tags
are `0x01` (u16), `0x03` (u32), `0x04` (u32 array), `0x05` (float), `0x06`
(three-float vector), `0x07` (four-float quaternion), `0x08` (12-float 3x4 matrix),
and `0x09` (position/quaternion transform). A tag's upper 16 bits carry a record
version where applicable, for example `0x00010017`.

`RwpBodyDef` begins with tag `0x18` and embeds a versioned `0x17` volume record.
Volume tag `0x0B` selects its shape. Shapes observed and validated in the corpus are
sphere (`0x0E`), capsule (`0x0F`), box (`0x10`), cylinder (`0x11`), and Trilist
aggregate (`0x13`). Trilist records recursively contain other volume records. Common
volume data comprises a 3x4 matrix, fatness, friction, restitution, and (in version 1)
a flag word and collision-group index. Fatness occupies volume offset `+0x4C`: sphere
and capsule implementations use it as radius, while box bounds expand all three
half-extents by it.

The capsule-specific float is copied to runtime offset `+0x40`. Capsule bounds use
that value as the center-to-cap distance and add the common `+0x4C` radius, proving
it is half-height. Cylinder construction copies its first stream float to `+0x44`
and its second to `+0x40`; bounds use `+0x44` for the two radial axes and `+0x40`
for the axial extent, proving radius-then-half-height ordering. The inspector now
retains the Trilist version-1 aggregate mass properties instead of discarding part of
them. Runtime offset `+0x38` is cached mass and `+0x3C` is center of mass. When mass is
negative, `FUN_008086A0` recomputes those fields from the child volumes and writes
principal inertia plus its orientation quaternion at `+0x48` through `+0x60`.

The next stream floats are stored at volume offsets `+0x54` and `+0x50`. Contact
creation at `0x0081A910` copies them into the two sides of a contact, and
`0x0081A340` combines like fields with `min`. Constraint generation at `0x00814F50`
uses the `+0x54` value for tangential friction bounds and the `+0x50` value to scale
the post-impact normal separating velocity. They are therefore friction followed by
restitution in stream order.

The final version-1 `u16` values are stored at `+0x58` and `+0x5A`, respectively.
Collision candidate generation at `0x008113A0` and pair filtering at `0x0081B700`
use `+0x5A` to index a group-pair rejection bit table, proving that the second value
is the collision group. The first value is preserved as raw volume flags; all 3,065
version-1 volume records in the supplied Physics corpus store `(flags, group) = (0, 0)`,
so no per-bit semantics can be established from these assets.

The body fields are not a world transform. Decompiled mass-property calculation at
`0x00801550`, backed by the volume calculation at `0x007FEB40`, establishes this
layout:

| Body offset | Stream field | Meaning |
|---:|---|---|
| `+0x04` | final vec3 | center of mass |
| `+0x10` | first float | mass (a negative setter input requests density-to-mass conversion) |
| `+0x14` | transform vec3 | principal inertia values |
| `+0x20` | transform quaternion | principal-inertia orientation |
| `+0x30` | following float | scalar/spherical inertia approximation |
| `+0x34` | following float | linear damping; defaults to `0.01` |
| `+0x38` | following float | angular damping; defaults to `0.01` |
| `+0x3C` | following vec3 | finite-rotation axis |
| `+0x48` | u32 | body flags; bit 0 enables finite-axis integration and bit 1 selects oriented principal inertia |

These names are derived from data flow rather than guessed from field values: the
calculation obtains volume, center, and inertia from the shape, multiplies inertia
by mass, stores the center, and selects scalar versus oriented principal inertia.
The simulation step at `0x00802D70` subtracts mass-scaled `+0x34` times linear
velocity from force, and inertia-scaled `+0x38` times angular velocity from torque.
The transform integrator at `0x00811F10` projects angular motion onto `+0x3C` when
flag `0x01` is set, applies an exact axis-angle quaternion update at `0x00811780`,
then integrates the orthogonal remainder. This is the finite-rotation-axis algorithm,
not an upright torque or gravity vector.

All 33 `0x907` files consume exactly with this grammar. The sole `0x909` file also
consumes exactly: it contains 11 embedded body definitions, 10 joint records, ten
joint-pair entries, a lookup table, and 11 body IDs.

Evidence-backed names were saved into the Ghidra project for the dispatcher and the
key readers at `0x00822B20`, `0x00823B70`, `0x00823270`, `0x00829BF0`, `0x008239A0`,
`0x008230A0`, `0x00829950`, `0x00828C10`, `0x00829350`, and `0x00829F70`. Supporting
initializers and mass-property routines are also annotated there.

## RenderWare Studio 2.0.1 source cross-check

The local Studio repository was searched for the Physics object IDs, body-definition
symbols, ragdoll serializers, and Physics stream callbacks. It confirms the vendor
assignment and contains a Workspace icon registration for `rwpID_BODYDEF`, but the
RenderWare Physics component implementation and serialization headers are absent;
the executable itself supplied the missing implementation evidence.
Studio's changelog also refers to Karma physics integration, without including the
removed implementation.

## Pyro Studios object metadata (`0xFFFFFF00`)

Ghidra resolves the formerly unknown extension as one Pyro plugin ID registered on
multiple RenderWare classes by `CSF_AttachPyroObjectMetadataPlugins` at `0x006C23B0`.
It is not a single payload schema. Its owning-object distribution is:

| Owner | Count | Observed version / size |
|---|---:|---|
| Atomic (`0x14`) | 369 | version 11; mainly 88 or 112 bytes |
| Material (`0x07`) | 772 | version 2; mainly 32 bytes, longer when named |
| Frame (`0x0E`) | 622 | version 1; 8 bytes |
| World Sector (`0x09`) | 1 | version 1; 12 bytes |
| Light (`0x12`) | 1 | version 2; 8 bytes |

The matching stream readers are `0x006C1BB0` (Atomic), `0x006C1FD0` (Material),
`0x006C2130` (Frame), `0x006BF6F0` (World Sector), and `0x006BF7F0` (Light).
`0x006C0FA0` proves strings use a `u32` byte length followed by exactly that many
bytes; the terminating null exists only in the runtime allocation. The decoder now
selects the schema from the enclosing object, consumes all 1,765 corpus instances,
and exposes unknown numeric fields without assigning speculative gameplay names.

Material value correlation identifies record word 3 as a flags/mask field and word
4 as a surface-type ID. Named examples include `1=Tierra`, `2=Piedra`, `3=Metal`,
`5=Vegetacion`, `6=Madera`, `7=Cristal_opaco`, `9=Barro`, `10=Baldosa`,
`11=Cemento`, `12=Escaleras`, and `18=escaleras_piedra`. The names strongly support
a collision/audio/gameplay surface classification rather than rendering state.

Of the 369 Atomic records, 204 carry the optional six-float bounds and four carry a
non-empty first string: `ARBOL_3`, `BANDERA_A`, `BANDERA_B`, and `CABLES`. The second
Atomic string is empty in this corpus. This supports treating the first string as an
object name while retaining the second as an unresolved string slot.

`Studio/Examples/Models/Warrior/ragdolls.zip` contains two DFF files named for local
and world ragdoll coordinates. Inspection shows that they are ordinary Clumps with
Geometry, Skin, and HAnim plugins; neither contains a Physics `0x907` or `0x909`
object. They served as an independent Skin decoder fixture, but are not evidence for
the Physics payload grammar. This negative result prevents accidentally transferring
Graphics Skin semantics onto the unrelated Physics ragdoll stream.

## Reproducible scan

```powershell
.\build\Release\rws-corpus.exe "C:\path\to\extracted-game"
```

The scanner reports every RWS path, size, root format, decoded library/build stamp,
diagnostics, root-format totals, and recursive chunk/plugin counts. It is intended
for comparing additional stages without hardcoding ST05 paths.
