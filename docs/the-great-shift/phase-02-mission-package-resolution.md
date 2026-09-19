# Phase 02: mission package resolution

## Outcome

Open a mission as a deterministic, inspectable dependency graph. Given an SCN path
or mission directory, resolve the associated scripts, map streams, collision,
databases, model/Physics/animation indexes, textures, and optional resources while
reporting missing, ambiguous, duplicate, and case-mismatched references.

This phase creates the shared mission model used by the GUI and all later actor,
Physics, animation, validation, and authoring work.

## Dependencies

- Phase 01 generic CSFFBS parser.
- Existing `rws_core` document loading and root classification.
- Read-only filesystem access to an extracted game-resource tree.

## Implementation status

Phase 02 is implemented as of 2026-09-19:

- `csf_core` provides the read-only resource index, deterministic Windows-like
  resolver, mission graph, reverse-use index, adapters, diagnostics, lazy payload
  ownership, content signatures for duplicate SCNs, and stable JSON export.
- Mission discovery covers sibling GSC/CSC/VIS, `Maps/Secs` SEC, the six BDD
  databases, map-local M3D/PHD/AND/TXL, and optional PRP/DST/WAD members.
- VIS, M3D, AND, and TXL use bounded record readers. PHD deliberately exposes
  only bounded extension-validated candidates and retains its complete byte
  stream until the full record schema is established.
- `csf-info mission`, `uses`, and `compare` expose summaries, dependency and
  missing reports, safe graph export, reverse uses, and graph comparison.
- Synthetic tests cover resolution precedence, slash/case handling, DFF-to-RPC
  mapping, traversal rejection, adapters, missing edges, reverse uses, and JSON
  determinism. All 21 reference SCNs build without a fatal error.

The resolver does not claim semantic names for unknown PHD fields. A zero-sized
VIS reference is retained as a missing optional edge, and M3D/AND terminal bytes
remain preserved as adapter tails. These are evidence-preserving limitations,
not guessed schemas. The detailed contract and corpus measurements are in
[`../mission-resolution.md`](../mission-resolution.md).

## In scope

- A mission/package descriptor rooted at a chosen SCN or explicit directory.
- Typed path extraction from VIS, M3D, PHD, AND, TXL, and known SCN fields.
- Discovery of sibling GSC/CSC/SEC and six BDD databases.
- Resource-root and package-root configuration.
- Windows-like case-insensitive path matching implemented deterministically.
- Normalization of `/` and `\` without losing the original spelling.
- Requested `.dff` to extracted `.rpc` model candidate mapping.
- Root sniffing/classification for `.rws`, `.rpc`, `.anm`, CSFFBS, textures, and
  known text formats.
- A directed dependency graph with typed edges and evidence/source locations.
- Missing, ambiguous, duplicate-content, and unexpected-type diagnostics.
- Reverse-use indexes: “which records or missions reference this asset?”
- CLI reports and graph export.
- Read-only lazy loading and bounded caches.

## Explicitly out of scope

- Rendering SCN actors or paths.
- Full semantic decoding of SCN/GSC/CSC/BDD.
- Assuming every index field is understood before exposing known path edges.
- Guessing one winner when two case-insensitive or DFF/RPC candidates are equally
  plausible.
- Parsing WAD audio payloads or assigning semantics to SEC.
- Writing, moving, renaming, or deduplicating corpus files.
- PAK creation.

## Mission graph model

```cpp
enum class ResourceKind {
    mission_scene,
    mission_script,
    cutscene_script,
    database,
    visual_map,
    collision_map,
    render_model,
    physics_body,
    ragdoll,
    animation,
    collision_shape,
    texture,
    particle_system,
    audio_bank,
    spatial_data,
    unknown,
};

struct ResourceNode {
    ResourceId id;
    ResourceKind kind;
    std::filesystem::path resolved_path;
    std::string original_reference;
    ContentSignature signature;
    LoadState state;
    std::vector<Diagnostic> diagnostics;
};

struct DependencyEdge {
    ResourceId source;
    ResourceId target;
    DependencyKind kind;
    SourceLocation evidence;
    ResolutionStatus status;
};
```

Node identity must not rely solely on a lower-cased path. Preserve the resolved
filesystem identity, original reference spelling, package/root context, and
content signature separately.

## Resolution policy

Resolution must be predictable and explainable. A proposed order is:

1. Explicit path supplied by the user.
2. Exact normalized relative path within the active package/resource root.
3. Case-insensitive match of that complete normalized path.
4. Format-specific mapping, such as requested `.dff` to same-stem `.rpc`.
5. Configured shared/global resource roots in declared precedence order.
6. A unique indexed candidate with the same logical path.
7. Otherwise unresolved or ambiguous; never silently pick the first directory
   enumeration result.

Every non-exact resolution should record the rule used. Ambiguous results retain
all candidates for inspection.

Do not canonicalize away meaningful raw strings. Store both the original reference
and normalized lookup key.

## Format adapters

### VIS

Recover and type the observed leading paths:

- visual map RWS;
- collision companion RWS;
- texture directory;
- sky model.

Preserve unresolved numeric/tail bytes with offsets. Validate that resolved RWS
roots match the expected World/Clump/TOC role but allow partial missions to load.

### M3D

Recover the model dependency records and original `.dff` references. Confirm
record boundaries across all 17 files rather than treating all printable strings
as independent entries.

### PHD

Recover render-model and Physics references plus bounded raw values. Represent a
candidate association explicitly even before every numeric field has a name.

### AND

Recover animation path records and their boundaries. The first observed string is
length-prefixed, but the file is not proven to be a flat path list; retain unknown
fields and validate the whole corpus before documenting a complete schema.

### TXL

Parse non-empty lines as path references while retaining line number, original
slashes/case, duplicates, and undecodable bytes.

### BDD and scripts

At this phase, use the generic CSFFBS tree to extract only high-confidence path
references and database membership. Full typed record views arrive in Phase 03.

## CLI design

Suggested commands:

```powershell
csf-info mission path\to\Mission.scn --summary
csf-info mission path\to\Mission.scn --dependencies
csf-info mission path\to\Mission.scn --missing
csf-info mission path\to\Mission.scn --graph mission.json
csf-info uses path\to\Anims\Enem\SMCm.anm --root extracted-game
csf-info compare mission-a.scn mission-b.scn
```

Reports should include:

- chosen mission/package/resource roots;
- every discovered member and its kind;
- every resolution rule used;
- missing and ambiguous references;
- expected-versus-actual root magic/type;
- duplicate content under different paths;
- references that leave configured roots;
- unused index entries where the evidence supports that conclusion.

## Work breakdown

### P02-01: resource index

- Build a read-only path index for configured roots.
- Store exact and normalized lookup keys.
- Sniff content once and cache bounded metadata.
- Add collision tests for paths differing only by case.

### P02-02: simple index adapters

- Implement TXL and known VIS fields first.
- Add bounded M3D, PHD, and AND readers as their record boundaries are confirmed.
- Preserve all unknown tails and fields.

### P02-03: mission discovery

- Discover GSC/CSC/SEC/BDD and optional PRP/DST/WAD members.
- Handle FR02 names and package copies without same-basename assumptions.
- Support explicit user overrides for unusual layouts without persisting paths in
  the repository.

### P02-04: graph and diagnostics

- Add typed nodes/edges and evidence locations.
- Add missing, ambiguous, case mismatch, unexpected-type, and outside-root
  diagnostics.
- Build reverse-use indexes.

### P02-05: CLI and graph export

- Add mission summary, dependency, missing, uses, and compare reports.
- Export a documented JSON graph for external analysis.
- Keep graph ordering deterministic for useful diffs.

### P02-06: cache and lifecycle

- Load documents lazily.
- Bound open byte buffers and decoded-document caches.
- Invalidate only affected graph nodes when an explicit staging copy changes in a
  later phase.

## Test plan

### Synthetic resolver tests

1. Exact relative path resolution.
2. Slash normalization.
3. Unique case-insensitive resolution.
4. Case-insensitive ambiguity.
5. DFF-to-RPC same-stem mapping.
6. Multiple RPC candidates remain ambiguous.
7. Package-local versus shared-root precedence.
8. Missing target retains a graph edge and diagnostic.
9. Unexpected root type is diagnosed.
10. Path traversal outside configured roots is rejected or explicitly marked.
11. Duplicate references retain separate evidence locations.
12. Same content at multiple paths is reported without filesystem mutation.
13. VIS with known prefix and unknown tail preserves both.
14. Deterministic JSON ordering.
15. Mission unload releases document/cache ownership cleanly.

### Read-only corpus validation

- Build all 21 SCN-rooted mission graphs.
- Explain the four duplicate SCN contents and package-local differences without
  merging their path identities.
- Resolve the visual/collision pairs already known to the RWS preview.
- Measure exact, normalized, DFF-to-RPC, missing, and ambiguous resolution counts.
- Sample every index adapter across all files of its extension.
- Record unexplained PHD/AND/VIS tail variants before claiming a complete schema.

## Performance targets

- Initial resource indexing should be linear in file count and avoid parsing all
  large payloads.
- Opening one mission should load metadata and required scene/map documents, not
  every referenced animation or texture.
- Reverse-use indexing should reuse parsed tables across package-local queries.
- Content hashes should be optional or cached; do not hash 2.56 GB on each launch.
- UI callers must be able to cancel a long scan between files.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Windows game lookup rules differ from host/filesystem assumptions | Make normalization rules explicit and report the rule used |
| Same stem maps to several RPCs | Preserve candidates and require context or user choice |
| Package copies contain near-duplicate global data | Keep package identity separate from content identity |
| Unknown index values are discarded | Store source ranges and raw fields in every adapter |
| Eager dependency loading consumes excessive memory | Separate graph metadata from lazily loaded documents |
| Resolver becomes coupled to GUI state | Keep it in `csf_core` and drive it from CLI tests first |

## Definition of done

- A selected SCN produces a deterministic mission graph with source-backed edges.
- VIS, TXL, and validated portions of M3D/PHD/AND contribute typed references.
- GSC, CSC, SEC, BDD, map, collision, model, Physics, animation, texture, and
  optional package members are represented even when unresolved.
- Path resolution reports exact, normalized, mapped, missing, and ambiguous cases.
- Requested DFF references can resolve to RPC candidates without renaming files.
- `csf-info` can print dependencies, missing references, reverse uses, and stable
  JSON graphs.
- All 21 local missions can be scanned read-only without a fatal failure.
- Synthetic resolution tests and existing RWS tests pass in Release and Debug.
