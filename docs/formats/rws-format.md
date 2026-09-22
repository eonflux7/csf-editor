# Commandos: Strike Force RWS notes

Status: working reverse-engineering notes, 2026-09-05. `Confirmed` below means
observed in both supplied ST05 samples or directly described by the RenderWare SDK
stream model. `Hypothesis` means the interpretation still needs cross-file or runtime
validation.

## Sources and terminology

The strongest available baseline is the preserved RenderWare Graphics 3.7.0.2 SDK
and its documentation:

- RenderWare Studio 2.0.1 source and its bundled Graphics SDK.
- <https://github.com/sigmaco/rwsdk-v3.7.0.2>
- <https://rwsreader.sourceforge.net/>
- <https://gtamods.com/wiki/RenderWare_binary_stream_file>
- <https://gtamods.com/wiki/List_of_RW_section_IDs>

The last two are community references and are used as indices, not as evidence for
CSF-specific fields. All CSF-specific claims below come from the supplied files.

## Generic RenderWare chunk header — confirmed

All integers in the supplied PC files are little-endian. Every standard chunk begins
with this 12-byte header:

| Offset | Type | Meaning |
|---:|---|---|
| `0x00` | `u32` | chunk type / section ID |
| `0x04` | `u32` | payload byte count, excluding this header |
| `0x08` | `u32` | packed library version and build stamp |

The next sibling begins at `header_offset + 12 + payload_size`. A container payload
is itself a sequence of chunk headers. A `Struct` (`0x01`) is parent-dependent raw
binary data and must not be recursively scanned.

Both ST05 files use library ID `0x1C020037`. RenderWare's packed-stamp algorithm
decodes it as version `0x37002` (3.7.0.2), build 55 (`0x0037`). This matches the SDK
version expected for the title.

## Core IDs observed

| ID | Name | Role seen in ST05 |
|---:|---|---|
| `0x01` | Struct | parent-specific fixed/array data |
| `0x02` | String | texture name/mask string chunks |
| `0x03` | Extension | container for plugin chunks |
| `0x06` | Texture | sampler plus two strings and extensions |
| `0x07` | Material | color/surface data, optional texture, extensions |
| `0x08` | Material List | material index table and materials |
| `0x09` | Atomic Section | leaf geometry sector in a World BSP |
| `0x0A` | Plane Section | branch node in a World BSP |
| `0x0B` | World | collision/world tree root in `ST05_COL.rws` |
| `0x0E` | Frame List | frame transforms and per-frame extensions |
| `0x0F` | Geometry | vertices, triangles, material list, extensions |
| `0x10` | Clump | model root in `st05.rws` |
| `0x12` | Light | one light in the main sample |
| `0x14` | Atomic | binds a frame to a geometry |
| `0x1A` | Geometry List | geometry count and geometry chunks |
| `0x1F` | Right To Render | standard plugin seen on atomics/geometries |

Observed model hierarchy:

```text
Clump
├─ Struct
├─ Frame List
│  ├─ Struct
│  └─ Extension(s)
├─ Geometry List
│  ├─ Struct
│  └─ Geometry(s)
│     ├─ Struct
│     ├─ Material List
│     └─ Extension
├─ Atomic(s)
├─ optional Light(s)
└─ Extension
```

Observed collision hierarchy:

```text
World
├─ Struct
├─ Material List
├─ Plane Section (recursive BSP branches)
│  └─ ... Plane Section / Atomic Section
└─ Extension
```

## Plugin and game-extension inventory

The full archive establishes that most initially unknown IDs are standard Criterion
plugins. Chunk IDs encode a 24-bit vendor in the upper bits and an 8-bit object ID.
The RenderWare 3.7 header assigns vendors `0x01` to Toolkit, `0x05` to World,
`0x07` to RenderWare Studio, `0x08` to Audio, and `0x09` to RenderWare Physics.

| ID | Name | Full-corpus count | Payload bytes |
|---:|---|---:|---:|
| `0x00000116` | Skin Plugin | 8 | 27,008 |
| `0x0000011D` | Collision Plugin | 8 | 51,074 |
| `0x0000011E` | HAnim Plugin | 1,320 | 33,400 |
| `0x0000011F` | User Data Plugin | 5,963 | 498,122 |
| `0x00000120` | Material Effects Plugin | 20,266 | 1,700,456 |
| `0x00000127` | Anisotropy Plugin | 14,865 | 59,460 |
| `0x0000050E` | Bin Mesh Plugin | 5,935 | 14,430,320 |
| `0xFFFFFF00` | Pyro Studios object metadata | 32,569 | 1,204,437 |

`0xFFFFFF00` is registered by the game on several RenderWare object classes and its
schema depends on the object owning the `Extension`. It is therefore decoded with
owner context, not as one global structure. The supplied corpus contains 369 Atomic,
772 Material, 622 Frame, one World Sector, and one Light instance. Atomic records are
versioned through version 11 and can carry two length-prefixed strings and optional
six-float bounds; Material and Frame records have optional metadata/name records.
Names include Spanish surface terms such as `Baldosa` and `Cemento`. Numeric field
semantics remain conservative until their runtime consumers are traced.

See [corpus findings](../corpus/rws.md) for the separate RenderWare Physics
streams discovered under `Models`.

The Collision plug-in appears on eight World Atomic Sections. Its version `0x37002`
payload embeds a standard Coll Tree (`0x2C`) containing one Struct. The Struct is a
36-byte header (`flags`, minimum and maximum bounds, triangle count, split count),
followed by 16 bytes per split. Each split contains two eight-byte sector descriptors
(`u8 type`, `u8 flags`, `u16 index`, `f32 value`). When flags bit zero is set, a
`u16` triangle-remap array follows with one entry per triangle. The current reader at
`0x0056F030`, stream callback at `0x0056EB30`, and writer at `0x0056EEE0` establish
the byte order and exact size formula. Pre-3.6 legacy trees use a different stream
layout and remain unsupported because none occur in the supplied corpus.

## RenderWare Physics typed records

`CommXPC.exe` contains the RenderWare Physics 3.7 readers statically. Ghidra-backed
control flow proves `0x907` is `RwpBodyDef`, `0x909` is `RwpRagdollDef`, and `0x90B`
is `RwpGenericDef`. The implemented decoder understands the tagged scalar, vector,
quaternion, matrix, transform, recursive volume, body, joint, and lookup-table
records required by every supplied Physics stream. See
[corpus findings](../corpus/rws.md) for tags, reader addresses, and validation.

Body-definition typed output exposes the semantics proven by executable data flow:
mass, center of mass, principal inertia, principal-inertia orientation, scalar
inertia, linear and angular damping, finite-rotation axis, and body flags. Flag
`0x01` enables finite-axis orientation integration and flag `0x02` selects the
oriented principal-inertia representation. Unknown flag bits remain visible. This
corrects the earlier provisional interpretation of the inertia vector and quaternion
as a body/world transform.

The embedded volume's two material floats are friction followed by restitution.
Contact generation combines each coefficient with the matching value from the other
volume using the lower value. The first controls tangential constraint limits; the
second scales separating velocity after an impact. The preceding common shape float
is volume fatness: sphere and capsule implementations use it as radius, while box
bounds expand each half-extent by it.

Shape-specific fields are now identified as follows: capsule stores its half-height;
box stores three half-extents; and cylinder stores radius followed by half-height.
The cylinder bounds code expands both dimensions by fatness. Trilist version 1 caches
the aggregate mass, center of mass, principal inertia, and inertia-orientation
quaternion calculated from its child volumes. A negative cached mass tells the runtime
to recalculate all four values.

Version-1 volumes then store a raw `u16` flag word followed by a `u16` collision-group
index. The collision broad phase uses the second value to address the configured
group-pair rejection table. The supplied corpus leaves both fields zero, so individual
meanings for the first word's bits remain deliberately uninterpreted.

The low 16 bits of a Physics record tag select its type; the high 16 bits contain
the record version. This is an internal Physics serialization grammar inside the
ordinary outer RenderWare `Struct`, not another layer of 12-byte RW chunk headers.

## Typed standard structures — confirmed against ST05

The current decoder consumes the following Struct payloads exactly:

- Clump: three `i32` object counts.
- Table of Contents: `u32` entry count followed by 28-byte entries containing the
  target chunk type, an opaque object ID, absolute file offset, and 16-byte GUID.
- Frame List: `i32 count`, followed by `count` records of a 3x3 float rotation,
  float position, signed parent index, and flags (56 bytes per frame).
- Geometry: four 32-bit header fields followed conditionally by prelight colors,
  up to eight UV arrays, 8-byte streamed triangles, and morph targets containing a
  bounding sphere plus optional position and normal arrays.
- Material List: count plus signed remap indices.
- Material: flags, RGBA, textured flag, and three surface-property floats.
- Atomic: frame index, geometry index, flags, and unused word.
- World 3.7 header: root kind, inverse origin, aggregate counts, format, and bounds.
- Plane Section: axis/split and left/right child descriptors.
- Atomic Section: material base, counts, bounds, and collision-presence flag.
- Texture: packed filtering/addressing modes plus name and mask String chunks.
- HAnim: hierarchy metadata and node table.
- Skin: byte-sized bone counts, used-bone table, four packed bone indices and four
  float weights per Geometry vertex, 4x4 inverse matrices per bone, and the split
  header (`boneLimit`, mesh count, RLE count). Skin decoding is contextual because
  the vertex count belongs to the owning Geometry.
- User Data: array count, then an unpadded length-prefixed name, format, element
  count, and typed values for each array. Formats `1`, `2`, and `3` are respectively
  signed 32-bit integer, 32-bit real, and length-prefixed string, matching
  `RpUserDataFormat` in the Studio SDK's `rpusrdat.h`.
- Bin Mesh: mesh/material groups and topology index arrays.
- Collision: versioned `0x11D` wrapper, embedded `0x2C` tree, bounds, split-sector
  descriptors, and optional triangle remap.
- Anisotropy and Right To Render: scalar/pipeline metadata.
- Material Effects: CSF uses effect and slot type `4` (dual pass). Material
  payloads store source/destination blend modes, a texture-present flag, a complete
  embedded Texture chunk, and a zero second-slot terminator. ST05 uses source
  `rwBLENDZERO` (`1`) and destination `rwBLENDSRCCOLOR` (`3`), producing base color
  multiplied by the dual texture. Atomic payloads are a
  four-byte pipeline-enabled flag. Across ST05 all 902 payloads decode exactly:
  645 Material records and 257 Atomic records. All 645 embedded texture references
  end in `_Lm` and select 33 unique external DXT1 files in the map's `Textures`
  directory. The other 27 of the directory's 60 `*_Lm.dds` names occur in the
  CSF-specific scene tail rather than the standard Clumps. Exactly 257 of the 369
  Clump Geometries carry two UV sets, matching the 257 MatFX-enabled Atomics; the
  other 112 Geometries have no UV arrays.
- RenderWare Physics Body and Ragdoll definitions: recursive tagged records,
  including all volume/body/joint structures present in the extracted corpus.

Four FR02 streams begin with a Table of Contents (`0x24`). Every recorded absolute
offset lands on a complete top-level chunk whose type matches the entry. The 16-byte
identifiers have RFC 4122 version-4/variant bit patterns. The intervening 32-bit
field varies independently of type, offset, size, and library stamp, so it remains
named only as an opaque object ID pending a runtime consumer.

All 4,782 applicable model structures and all 26 applicable collision structures
decode without a boundary/count failure. Geometry array sizes consume their Struct
chunks exactly.

The Skin layout has two independent checks. Each of the two CSF chunks is 3,401
bytes and belongs to a 153-vertex Geometry with five bones; the complete formula is
`4 + 5 + 153*4 + 153*16 + 5*64 + 12 = 3401`. RenderWare Studio's bundled
`2wpv_ragdoll_*_coords.dff` files provide a separate RW 3.4 sample: 5,740 vertices,
26 bones, 24 used bones, and a 116,504-byte Skin payload satisfying the same layout.

All 374 User Data payloads consume exactly with no alignment padding assumed. One
representative 85-byte Geometry extension contains three single-value integer arrays:
`hayTagID = 1`, `bNivelTest = 0`, and `FVF.UserData = 0x3003`.

RenderWare's streamed triangle words are ordered as vertex 1, vertex 0, material,
vertex 2 rather than the in-memory `RpTriangle` order. Both candidate arrangements
were range-tested against vertex/material counts: all 369 ST05 geometries uniquely
select the streamed arrangement, with zero memory-order or ambiguous cases.

World Sector triangles retain the in-memory order instead: vertex 0, vertex 1,
vertex 2, material. In ST05, the third word spans each sector's full vertex range,
while the fourth word is bounded by the World's 339 materials.

The OBJ exporter uses the first morph target that contains positions, the first UV
set, optional normals, the validated streamed triangles, and `material_N` groups.
Geometry is currently exported in its local frame; applying Frame List transforms
and extracting texture images remain future work.

## Sample inventory and anomalies

### `st05.rws`

- Physical size: 28,979,398 bytes.
- 244 standard top-level Clump chunks occupy `[0, 0x00DD9095)`.
- Those clumps contain 369 geometries/atomics and 645 Texture chunks.
- At `0x00DD9095`, a repeating CSF-specific scene-instance region begins. Each record
  starts with type `0x00016FC0`, a custom declared size, and the normal library stamp.
- The region is 14,458,929 bytes and contains embedded ordinary chunk headers,
  transforms, numeric IDs, and length-prefixed names such as `ARBOL_3`.

The tail begins with 23 `ARBOL_3` instance records containing embedded Matrix
chunks. At `0x00DD9BA2` they are followed by a standard RenderWare World chunk that
extends to physical EOF (its declared size overruns the file by 32 bytes). The
parser decodes the instance records and conservatively recovers this World when its
stamp, leading Struct child, and EOF boundary all agree.

### CSF scene-instance record (`0x00016FC0`)

The executable's writer (`FUN_006C47C0`) and reader (`FUN_006C5660`) establish this
layout. Offsets are relative to the outer record header:

| Offset | Type | Meaning |
|---:|---|---|
| `0x00` | `u32` | custom type `0x00016FC0` |
| `0x04` | `u32` | declared size, `92 + name_length` |
| `0x08` | `u32` | RenderWare library ID |
| `0x0C` | `u32` | prototype ID |
| `0x10` | `u32` | instance ID |
| `0x14` | `f32` | optional maximum visibility distance; non-positive disables the far limit |
| `0x18` | `f32` | optional minimum visibility distance; non-positive disables the near limit |
| `0x1C` | `f32` | visibility fade range at either enabled distance limit |
| `0x20` | `u32` | Pyro Atomic metadata flags; decoded below |
| `0x24` | chunk | standard Matrix (`0x0D`, 64-byte payload) containing a 52-byte Struct |
| `0x3C` | `9 x f32` | Matrix right/up/at basis |
| `0x60` | `3 x f32` | Matrix position |
| `0x6C` | `u32` | Matrix flags (observed as `3`) |
| `0x70` | `u32` | name byte length |
| `0x74` | bytes | non-null-terminated optional prototype name |

The physical record length is `116 + name_length`, deliberately 24 bytes larger
than the value in its size field. It therefore cannot be advanced using ordinary
RenderWare `12 + declared_size` chunk framing.

Prototype resolution is numeric, not name-based. `FUN_006C4090` subtracts 1000 from
the record's prototype ID, `FUN_006C3C60` finds the first loaded Clump whose first
valid Pyro Atomic object index matches that value, clones the entire Clump, copies
the IDs/parameters/name/flags into runtime metadata, and applies the record Matrix
to the clone's root frame. This is the same mapping used by the preview and glTF
exporter; names such as `ARBOL_3` are metadata rather than lookup keys.
The final transform call uses combination mode zero; its callee directly copies all
16 matrix words, proving that the placement Matrix replaces (rather than post- or
pre-concatenates) the cloned Clump root transform.

The three visibility fields are copied by `FUN_006C4090` through setters
`FUN_006C01D0`, `FUN_006C0220`, and `FUN_006C0270`. Their getters optionally apply
the engine distance scale and feed `FUN_006CBBF0`, which compares camera-to-instance
distance against the maximum and minimum limits. Inside the fade range it returns
`(maximum - distance) / fade` at the far edge or `(distance - minimum) / fade` at
the near edge. FR01's named shrub placements use a maximum of `5000`; all other
observed scene-instance distance fields are zero.

The placement flag word is the cloned Atomic's Pyro metadata mask. The original
export-property parser at `FUN_006C1060` provides direct names for bits `0x002`
(`bEsAgua`, water), `0x040` (`bMipmaps`), `0x080` (`bEsCristalRompible`, breakable
glass), `0x100` (`bEsBackPlane`), and `0x200` (`bTieneAnimacion`, animated).
Runtime control flow identifies `0x001` as the Atomic enabled state and `0x400` as
the scene-registration state: Clump activation toggles `0x001`, scene insertion sets
`0x400`, and scene teardown clears it. Unknown bits remain visible in the raw mask.
The supplied placement records use only `0x001`, `0x040`, `0x200`, and `0x400`, in
the four combinations `0x401`, `0x441`, `0x601`, and `0x641`.

### Pyro World Sector per-vertex data and size defect

The full map corpus adds variable-size Pyro World Sector payloads absent from the
original ST05 sample. The reader at `0x006BF6F0` reads a version, a presence word,
then exactly one byte per World Sector vertex. `FUN_006BD470` later copies each byte
into the high byte of a 16-bit field at offset six in an eight-byte runtime vertex
record. Its finer gameplay/rendering meaning remains conservatively unnamed.

The executable also proves a serializer defect: `PyroWorldSectorMetadataStreamGetSize`
at `0x006BF6C0` reports `vertex_count + 12`, whereas the writer at `0x006BEEB0`
emits only `vertex_count + 8` bytes. The declared plug-in payload therefore consumes
the first four bytes of the following chunk header. This explains several apparent
collision-World nesting/truncation anomalies; the decoder now excludes that
four-byte over-declared tail instead of interpreting it as metadata.

### Recovered World sectors

Declared-size tree walking remains the authoritative structural view, but it is
not sufficient for every CSF World: the Pyro size defect and historically damaged
BSP parent boundaries can hide physically complete leaves. `rws_core` therefore
provides a separate, read-only recovered-World view. It scans only inside a decoded
World's physical range and accepts an Atomic Section (`0x09`) only when its library
stamp and leading Struct agree with the World, its 44-byte header is complete, and
the exact Struct size is explained by positions, optional packed normals,
prelight RGBA, zero to eight UV sets, and eight-byte triangles. All arithmetic is
checked in 64 bits. Accepted ranges cannot overlap, and scanning resumes at the
validated Struct end so header-like vertex bytes cannot become false sectors.

Recovery records source offsets and array offsets rather than copying mesh data.
Triangle vertex indices and `material_window_base + local_material` are validated
against the sector and World Material List. Invalid triangles remain diagnosed and
are skipped by rendering/export consumers; original bytes and conservative tree
children are never repaired or fabricated. A World is `complete` when recovered
sector, triangle, and vertex totals match its header and all triangle/material
references are valid, `partial` when at least one usable sector remains but those
invariants fail, and `failed` when no usable sector is found.

On the Ransom FR01 reference pair this invariant is exact: the visual World
recovers 40 sectors, 109,855 triangles, and 178,532 vertices; the collision World
recovers 326 sectors, 99,653 triangles, and 84,884 vertices with 14 materials.
Successful recovery does not suppress the original truncation diagnostics.

### Recovered BSP topology

The physical World node order is confirmed as preorder: a Plane Section's leading
Struct is followed by its left subtree and then its right subtree. Each child-kind
word selects a Plane (`0`) or World/Atomic Section (`nonzero`). RenderWare plane
axis values in the studied streams are `0`, `4`, and `8` for source X, Y, and Z.
`left_value` supplies the left child's upper bound on that axis and `right_value`
supplies the right child's lower bound; the World bounds seed the root. Source Y is
therefore the map-space vertical axis. This source convention is distinct from the
exporter's unit conversion: glTF remains Y-up but scales every RWS unit by `0.01`.

Topology recovery is a separate read-only view over the same accepted leaves. A
Plane candidate must have the World library stamp, a complete 24-byte Struct,
axis `0/4/8`, and finite split/boundary values. The preorder child kinds, unique
physical ranges, parent ownership, acyclicity, resolved leaf identity, derived
bounds, and declared Plane/leaf totals are then validated. References are vector
indices rather than pointers. Invalid, ambiguous, truncated, unreachable, duplicate,
multiple-parent, cyclic, and bounds-conflict categories are reported independently.
Partial or failed topology never discards usable flat recovered geometry and never
changes the conservative `Chunk` tree.

On the read-only FR01 collision World, this process validates 325 Planes and 326
leaves, links all sectors, reaches maximum depth 16, and reports complete topology.
These are observed corpus results rather than guessed constants in automated tests.
The known four-byte Pyro World Sector size overstatement is handled by physical
candidate validation; bytes and declared chunk sizes remain untouched.

Range-safe core accessors expose recovered vertex positions, exact eight-byte
triangle source offsets, vertex indices, and resolved material slots. Collision
picking first intersects sector bounds, then performs double-sided triangle tests;
degenerate or invalid triangles cannot produce a hit. Collision export consumes
the same accessors and topology-independent sector list.

### `ST05_COL.rws`

- Physical size: 6,187,753 bytes.
- Starts with one World chunk and a 15-entry Material List.
- The World declares 6,189,685 payload bytes, or 6,189,697 bytes including its
  header. The physical file is therefore 1,944 bytes shorter than declared.
- The rightmost Plane Section inherits the same EOF truncation.

This may be a damaged/cut sample or a tolerated exporter defect. Do not rewrite the
declared sizes until another copy or another stage confirms the intended behavior.

## Parsing rules used by CSF RWS Tools

1. Read only complete 12-byte headers.
2. Perform all end calculations in 64 bits.
3. Clamp declared payloads to their containing range and flag truncation.
4. Recursively parse only known container types; keep Struct and unknown chunks raw.
5. Require nested candidates to use the stream's library stamp. This avoids
   manufacturing chunks from float/index data.
6. Preserve original bytes exactly unless a user explicitly edits them.

These rules are intentionally conservative. Typed editing will be enabled one schema
at a time after round-trip tests exist for that schema.
