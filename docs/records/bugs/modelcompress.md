# Bug: actor mesh "compresses" / is pulled to the ground when an animation is assigned

Status: fixed

Affected area: mission-actor animation playback in `rws-man`
(`app/geometry_preview.cpp`, `src/animation.cpp`).

## Symptom

When an animation is selected for a mission actor in the Animation workspace, the
actor model appears glitched and is pulled down toward the actor's ground point.
The static (non-animated) actor prototype renders normally.

## Reproduction and measurements

An offline harness was built that links the project's own `rws::` decoder and
skinning code and runs against unpacked reference data.

Model: `CSF_unpacks/Ambush/Models/Char/AlAbri.rpc`
Clip: `CSF_unpacks/Ambush/Anims/Comm/Idle.anm` (also reproduced with
`Anims/Enem/EAtack01A.anm` and other compatible clips; the defect is independent
of clip content).

Two paths from `GeometryPreview::actor_geometry_builder_` were compared:

- static path (no `actor.animation`):
  `transform_point(world_frames[atomic.frame_index], rawVertex)`;
- skinned path (with `actor.animation`): `rws::cpu_skin(...)` at the rest pose
  (empty clip, or a clip evaluated at time 0).

```
static(atom) bounds: [ 34.2,-94.4,-25.3] - [154.4, 85.9, 16.3]   (~180 units tall)
skin (rest) bounds:  [-18.3,-29.3,-42.6] - [ 22.4, 29.3, 30.2]   (~60 units tall)
max per-vertex difference static vs skinned-at-bind = ~153-178 units
```

So at the rest pose the CPU-skinned mesh is **not** the geometry the viewer
normally displays. It is roughly one third the size and centred near the model
origin / skeleton root instead of at the static transform. This is the
"compressed and pulled to the ground" appearance.

It was also verified directly that, when the Frame-List rest pose was used as
the bind pose,

```
bone_world[b] * inverse_bind[b]  !=  atomic/bind transform
```

for essentially every bone. The Frame-List rest transforms therefore are not
the Skin bind transforms. Every bone that the selected clip does not drive kept
this mismatched rest transform, so partial clips were especially affected.

## Where the code is

- `app/geometry_preview.cpp`, `actor_geometry_builder_`, animated branch:
  - `app/geometry_preview.cpp:1537` - pose world matrices copied into
    `world_frames` (`world_frames[i].rotation = {m[0], m[4], m[8], ...}`).
  - `app/geometry_preview.cpp:1631` - `bone_world[b] = {t.rotation[0],
    t.rotation[3], t.rotation[6], ...}`.
  - `app/geometry_preview.cpp:1638` - `rws::cpu_skin(skin_vertices,
    inverse_bind, bone_world)`.
- `src/animation.cpp`:
  - `evaluate_pose` (`src/animation.cpp:451`) - produces column-major world
    matrices via `matrix()` / `multiply()`.
  - `cpu_skin` (`src/animation.cpp:565`) - `multiply(bone_world[bone],
    inverse_bind[bone])` at line 576.
  - `decode_inverse_bind_matrices` (`src/animation.cpp:596`) - reads the raw
    floats in file order.

## Root cause

The Skin matrices and `cpu_skin` already use compatible conventions. Reading a
serialized row-vector `RwMatrix` in file order as a column-major matrix gives
the transpose required by the column-vector helpers. The RenderWare relation

```
inverseBind = atomicFrame * inverse(bindBoneWorld)
```

(the geometry/atomic frame is baked into the inverse binds) becomes the
column-vector relation

```
inverseBind = inverse(bindBoneWorld) * atomicFrame
```

and therefore `bindBoneWorld * inverseBind == atomicFrame` as required.

The actual defect was that `evaluate_pose` seeded untracked bones from the
Frame-List rest pose. That pose does not match the Skin bind pose in these
models. Partial clips such as `Anims/Comm/Idle.anm` consequently supplied the
wrong `bone_world` for nearly the entire hierarchy.

A second defect affected full-body clips such as Ransom's `SMTalk.anm`.
Serialized Skin matrices are padded `RwMatrix` structures: their fourth vector
components are padding, and the final stored word is zero. Treating all 16 raw
words as a homogeneous matrix made `bone_world * inverse_bind` omit the bone
translation, compressing the mesh around the skeleton origin.

## Resolution

`rws::recover_skin_bind_pose` now reconstructs each bind-bone world matrix as

```
bindBoneWorld = atomicFrame * inverse(inverseBind)
```

and derives the corresponding local hierarchy. The animation evaluator accepts
that hierarchy as its base pose, overwrites only frames driven by the clip, and
resolves the resulting world matrices. The GUI passes those matrices directly
to `cpu_skin` and retains the static rendering path if bind recovery fails.
Inverse-bind decoding normalizes the `RwMatrix` padding to a homogeneous fourth
row `(0, 0, 0, 1)` before any 4x4 multiplication. Rigid atomics attached to the
skeleton use the same evaluated pose, so equipment follows its animated frame
instead of remaining in the static pose.

Regression tests cover `RwMatrix` padding normalization, a rotated and
translated atomic transform, and a partial two-bone clip. Bind-time CPU
skinning must reproduce the static atomic transform, and an untracked child
must retain its bind-local offset while following an animated parent.

## Notes

- The inverse-bind matrices are deliberately not transposed during decoding.
- `rws-info --export-animation-gltf` emits animation and skeleton data but no
  skinned mesh, so it is not a visual oracle for this GUI defect.
- Unknown/truncated data handling, the existing animation tests, and all other
  preview paths are unaffected by this report.
