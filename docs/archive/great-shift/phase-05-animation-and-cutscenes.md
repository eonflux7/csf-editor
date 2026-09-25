> **Archived plan — implemented.** Decoding and playback as built is documented in
> [Animation and cutscene inspection](../../game-knowledge/animation-cutscene-inspection.md);
> this document is kept for its rationale, risks, and test plan.

# Phase 05: animation and cutscene inspection

## Outcome

Decode RenderWare ANM clips, evaluate them against compatible HAnim/Skin models,
and provide timeline playback in standalone and mission contexts. Connect semantic
animation records from `Anims.bdd` and animation actions from GSC/CSC to previewable
clips. Add a conservative cutscene timeline view for cameras, pauses, sounds,
animations, and explicit sequencing without claiming to emulate the full game
runtime.

## Dependencies

- Phase 01 CSFFBS tree for BDD and script records.
- Phase 02 animation/model dependency resolution.
- Phase 03 script/reference navigation and cameras/dummies.
- Phase 04 RPC skeleton/Skin display and actor model instances.

## In scope

- Typed decoding of RenderWare root `0x1B` used by the supplied `.anm` files.
- Clip header/version, duration, interpolation metadata, keyframe records, and
  previous-frame links or equivalent topology.
- Keyframe validation and stable source identities.
- Association of animation tracks with HAnim node IDs/indices.
- Pose evaluation at arbitrary time.
- Looping, playback speed, play/pause/step/scrub controls.
- Root-motion extraction and optional path display where supported by the data.
- CPU skinning as the correctness baseline; optional GPU skinning after parity.
- Typed `Anims.bdd` records: semantic names, file variants, loop/blend/movement,
  model/hand items, and timed sounds.
- Animation catalog filtered by actor/model compatibility.
- Standalone model/clip laboratory.
- Play selected clips on mission actors without simulating AI.
- Script action cross-references for known animation opcodes.
- Conservative CSC timeline/control-flow visualization.
- Camera/dummy path and FOV preview for explicit cutscene actions.
- glTF export of skeleton/skin and selected animation where representable.

## Explicitly out of scope

- Full GSC/CSC virtual machine or deterministic mission replay.
- Inferring the exact animation active at an arbitrary gameplay moment.
- AI state machines, inverse kinematics, cloth, facial animation, or Physics-driven
  secondary motion unless separate evidence is found.
- Blending every game-specific transition before individual clip playback is
  validated.
- Editing or creating ANM clips.
- Claiming a linear timeline where scripts branch or wait on runtime conditions.
- Audio-bank decoding; sound events may be shown as unresolved logical IDs.

## ANM reverse-engineering plan

### Establish variants

Before implementing one layout globally, group all `.anm` files by:

- RenderWare library/build stamp;
- payload size and header fields;
- keyframe size/flags/interpolator ID;
- exact divisibility and record topology;
- package/model/BDD usage.

All supplied files share root `0x1B`, but that alone does not prove one keyframe
layout. Unsupported variants should remain inspectable as raw chunks.

### Candidate model

```cpp
struct AnimationClip {
    SourceRange source;
    std::uint32_t version{};
    std::uint32_t interpolation_type{};
    std::uint32_t keyframe_count{};
    std::uint32_t flags{};
    float duration{};
    std::vector<AnimationKeyframe> keyframes;
    std::vector<TrackView> tracks;
    std::vector<Diagnostic> diagnostics;
};

struct AnimationKeyframe {
    SourceRange source;
    Quaternion rotation;
    Vec3 translation;
    float time{};
    std::int32_t previous_keyframe{};
    std::optional<std::int32_t> node_id;
};
```

The exact fields must follow sample evidence and external RenderWare implementation
references. Preserve raw record bytes and do not force a node ID into the model if
the stream derives tracks by keyframe chains/order.

### Validation

Validate:

- finite, nonnegative duration and times;
- time within documented tolerance of clip duration;
- quaternion normalization with diagnostic tolerance;
- previous-key references within the keyframe array and without invalid cycles;
- track/keyframe cardinality compatible with target hierarchy;
- translation and rotation finiteness;
- complete payload consumption or a preserved trailing region.

## Pose and skinning design

Maintain distinct transforms:

1. model rest/local frame transforms;
2. animation local bone transforms;
3. evaluated hierarchy/world transforms;
4. inverse bind matrices from Skin;
5. actor/world placement from SCN.

Write synthetic transform tests before visually tuning axes or quaternion order.
Expose a skeleton-only preview to debug association independently from skinning.

CPU skinning is the reference path because it is easy to test numerically and
export. GPU skinning may follow for dense mission playback, but both paths must
agree within a documented tolerance.

## BDD animation catalog

The user-facing unit should normally be a semantic BDD record, not a raw filename.
Show:

- logical name and record source;
- one or more platform/file variants;
- loop and blend-in settings;
- velocity, translation, and rotation metadata;
- compatible model/item/hand context;
- timed sound events;
- scripts and object records that reference it;
- missing/incompatible ANM diagnostics.

Allow raw-file browsing because many clips may be unused or ambiguously catalogued.

## Cutscene view

CSC scripts contain named cutscene scripts and observed actions for:

- init/end sequences;
- pauses and wait conditions;
- camera placement on dummies;
- camera FOV and filters;
- sound playback;
- animation and actor operations;
- cutscene completion.

Represent control flow honestly:

```text
Cutscene script
├─ ordered basic block
├─ conditional branch
├─ wait/runtime condition
├─ referenced camera/dummy/actor
└─ animation/sound action
```

A timeline lane can show explicit ordered actions and known durations. Branches and
runtime waits must appear as branches/waits, not fabricated absolute timestamps.

## Work breakdown

### P05-01: ANM corpus classifier

- Add `.anm` to corpus tooling.
- Group header/interpolator/keyframe variants.
- Compare representative files with the open-source Blender importer and available
  RenderWare documentation/source evidence.
- Record confirmed layouts before implementing playback.

### P05-02: typed ANM decoder

- Parse header and keyframes with bounded reads.
- Recover tracks/chains and source identities.
- Add detailed CLI report and validation.

### P05-03: pose evaluator

- Map tracks to HAnim hierarchy.
- Implement interpolation and loop/clamp behavior.
- Add skeleton-only playback and transform diagnostics.

### P05-04: skinning and model laboratory

- Implement tested CPU skinning.
- Add timeline controls, clip browser, skeleton/bounds/root-motion overlays.
- Add optional GPU path only after numerical parity.

### P05-05: BDD catalog and actor integration

- Decode typed `Anims.bdd` records.
- Resolve clip variants and compatibility.
- Play a chosen logical animation on a selected mission actor.
- Display sound/event markers.

### P05-06: script and cutscene view

- Type the high-confidence script/action structures and animation/camera opcodes.
- Build basic blocks and explicit branches/waits.
- Add cutscene action lanes and camera preview.
- Link every action back to its raw CSFFBS source.

### P05-07: export and documentation

- Export selected compatible animation/skeleton/skin to glTF.
- Document coordinate, unit, interpolation, and compatibility rules.
- Retain raw metadata not representable in glTF in a manifest.

## Test plan

### ANM and evaluator tests

1. Minimal one-bone, one-key clip.
2. Two keys with known translation interpolation.
3. Quaternion interpolation with sign-equivalent endpoints.
4. Multiple tracks and hierarchy composition.
5. Loop boundary and exact-duration sampling.
6. Clamp behavior for negative/over-duration time.
7. Invalid previous-key index and cycle.
8. Non-finite time/translation/quaternion values.
9. Unsupported keyframe/interpolator variant remains bounded.
10. Root motion extraction in source and mission coordinates.
11. CPU skinning of known weighted vertices.
12. GPU/CPU parity if GPU skinning is added.
13. HAnim mismatch produces compatibility diagnostics, not an out-of-range read.
14. glTF keyframe/timing/node mapping and manifest metadata.

### Script/cutscene tests

- straight-line action sequence;
- conditional branch;
- runtime wait with unknown duration;
- camera/dummy and FOV references;
- missing actor/animation/sound reference;
- duplicate script names with source-stable identity;
- reference navigation from action to SCN/BDD/ANM and back.

### Manual corpus matrix

- common locomotion loop;
- short weapon/hand animation;
- long bespoke character animation;
- vehicle/decorative animation;
- smallest 144-byte clips;
- largest animation sample;
- an animation referenced by a mission script;
- a cutscene with several cameras and target routes.

## Performance targets

- Evaluate only visible/selected animated actors by default.
- Cache decoded clips and track maps by resource/hierarchy identity.
- Avoid allocating per-bone/per-frame in the playback loop.
- CPU skinning should update only active meshes; static mission actors remain
  instanced/rest-pose geometry.
- Timeline/reference indexing occurs on load, not each frame.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| CSF ANM differs from common RenderWare variants | Corpus classification and raw variant fallback before global assumptions |
| Bone mapping appears plausible but is shifted | Skeleton-only view, ID/index diagnostics, synthetic mapping tests |
| Quaternion/axis convention produces mirrored motion | Numeric hierarchy tests and known landmark clips |
| Script timing depends on runtime state | Show branches/waits explicitly; avoid fake absolute timelines |
| Many actors make playback expensive | Animate selected/visible subset and retain static instances elsewhere |
| External importer behavior is treated as specification | Cross-check with corpus and record independent evidence |

## Definition of done

- Supported ANM variants decode with complete payload accounting and stable
  keyframe identities.
- A compatible RPC model can display its skeleton and skinned mesh at an arbitrary
  clip time.
- Timeline playback supports pause, step, scrub, loop, speed, and reset.
- BDD logical animation records resolve to clips with metadata and diagnostics.
- A chosen logical clip can play on a selected mission actor.
- Known script animation and camera actions link to scene/BDD/resource sources.
- Cutscene view represents ordered actions, branches, and runtime waits honestly.
- Selected animations export to glTF with documented coordinate/timing behavior.
- Synthetic evaluator/skinning/control-flow tests and all prior regressions pass.

