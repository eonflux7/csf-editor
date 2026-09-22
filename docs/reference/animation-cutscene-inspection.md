# Animation and cutscene inspection

Phase 05 adds bounded decoding and playback for RenderWare `0x1B` ANM clips,
typed `Anims.bdd` records, selected mission-actor playback, conservative CSC
control-flow inspection, and glTF skeleton/animation export.

## Supported ANM layouts

The common header is five little-endian fields: stream version, interpolator ID,
keyframe count, flags, and duration. The decoder currently evaluates:

- interpolator `1`: standard 36-byte HAnim keyframes;
- interpolator `2`: the corpus' 22-byte compressed keyframes followed by a
  24-byte translation offset/scale record. Previous-frame offsets use a logical
  24-byte stride.

The compressed scalar is Criterion's sign/4-bit-exponent/11-bit-mantissa value,
not IEEE binary16. Unsupported interpolators remain bounded and their unread
payload remains available as raw bytes. Every decoded key retains its physical
source offset, size, raw previous value, and record bytes.

Validation covers duration and time ranges, finite transforms, quaternion
length, previous-key topology, monotonic track time, payload accounting, and
hierarchy cardinality. Evaluation normalizes quaternions and uses shortest-path
spherical interpolation. Negative and over-duration time can be clamped or
looped explicitly.

## Coordinate and skinning rules

Four transforms stay distinct: RPC rest/local frames, evaluated animation local
frames, hierarchy/world frames, and Skin inverse binds. Mission placement is
applied after skinning. CPU skinning is the reference implementation; invalid
bone indices and zero weights cannot address outside the decoded arrays.

The viewer evaluates only the selected animated mission actor. Other actors use
their cached rest-pose prototype. Playback controls provide play/pause, reset,
single-frame step, scrub, loop, and speed. This is clip inspection only: it does
not run AI or infer the animation that gameplay would choose.

Mission Explorer has a dedicated **Animation** workspace (or press `4`). Its
actor panel first lists evidence-backed assignments recovered through either a
direct `Objetos.bdd` animation field or the complete SCN actor script ID -> GSC
`PLAY_ANMBDD*` action -> numeric `Anims.bdd` ID chain. Each result shows the
script, opcode, and source entry that established the link. The much larger
model-context-compatible catalog is kept in a separate collapsed fallback list;
compatibility is not presented as proof that gameplay assigns a clip.

## Animation catalog and cutscenes

`Anims.bdd` is projected into logical records with file variants, loop/blend
metadata, movement vectors where present, model/item/hand context, sound IDs,
unknown fields, and source-stable CSFFBS identities. File variants use the same
case-aware mission resource resolver as other dependencies.

CSC actions are classified conservatively as animation, camera/dummy, FOV,
sound, pause, runtime wait, branch, actor, init, or end actions. Ordered actions
form basic blocks. Conditional branches and waits split blocks and retain an
unknown runtime duration. Logical sound IDs are displayed without claiming to
decode an audio bank.

Camera actions whose numeric dummy ID resolves into the SCN appear under
**Cutscene camera dummies** in the Animation workspace. They are also drawn as
magenta points and direction wedges in the viewport. Selecting one shows every
recognized CSC script/action/source entry that references it. FOV actions remain
separate ordered actions unless the source explicitly establishes a stronger
association.

## Command-line use

```powershell
.\build\Release\rws-info.exe clip.anm --animation-report
.\build\Release\rws-info.exe clip.anm --animation-report=frames
.\build\Release\rws-info.exe clip.anm --export-animation-gltf model.rpc clip.gltf
.\build\Release\rws-corpus.exe E:\path\to\unpacked-resources
.\build\Release\csf-info.exe animations BDD\Anims.bdd --root E:\path\to\package
.\build\Release\csf-info.exe script-animations Maps\Mission\Mission.gsc
.\build\Release\csf-info.exe cutscene Maps\Mission\Mission.csc
```

The glTF export contains the frame hierarchy, skin joints/inverse-bind matrices,
and translation/quaternion channels. A sibling manifest retains RenderWare
library/interpolator/flag/source metadata and states that source coordinates are
preserved; no unproven unit or axis conversion is applied.

## Limits

The tool does not implement the GSC/CSC VM, AI, IK, blending graphs, facial or
cloth animation, animation editing, or audio-bank decoding. A script action is
not assigned an absolute time unless the source carries one. Unsupported ANM
variants remain inspectable and are not guessed into a supported layout.
