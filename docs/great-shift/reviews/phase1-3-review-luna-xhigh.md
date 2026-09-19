# Great Shift phases 1–3 review

Review date: 2026-09-19
Scope: current working-tree changes for phases 1–3, including tracked and
untracked implementation files. The existing review directory was not read.

## Summary

The Release core build, Release GUI target, Debug core build, and their test
runs completed successfully. The main risks are in malformed-but-parseable
inputs and in resolution behavior that is not covered by the current synthetic
tests. I found six actionable issues:

1. Package-local logical matches can lose to a shared-root exact match.
2. Zero-length VIS references are treated as a fatal truncation and dropped.
3. Reverse-use lookup is case-sensitive even though resource resolution is not.
4. Scene JSON can contain non-standard `NaN`/`Inf` tokens and therefore be
   invalid JSON.
5. Navigation validation collapses missing and duplicate identities into usable
   keys.
6. The no-overwrite export path has a temporary-file symlink/TOCTOU window.

## Findings

### High — package-local logical matches can lose to a shared-root exact match

`ResourceIndex::resolve` checks the preferred package root only for an exact
relative spelling or a case-insensitive match (`src/csf_mission.cpp:306-309`).
It then checks exact matches in other roots (`src/csf_mission.cpp:319-323`) and
only searches the logical-path index at the end (`src/csf_mission.cpp:324-337`).

This violates the documented package-local precedence for references whose
spelling is logical rather than package-relative. For example, with a local
asset at `Maps/ST08/Models/Hero.rpc` and a shared-root asset at
`Models/Hero.rpc`, resolving `Models/Hero.dff` can select the shared-root exact
candidate before considering the package-local logical candidate. The selected
visual/model/collision asset can therefore come from the wrong package, or the
result can become ambiguous when the local candidate is included only in the
late logical search.

The preferred root should be searched through its logical candidates (including
the DFF-to-RPC variant) before any non-preferred root is consulted. Add a test
where the package contains only an anchor-nested candidate and a shared root
contains an exact candidate.

### High — zero-length VIS references are treated as fatal truncation and dropped

`read_vis` rejects `length == 0` along with an actually truncated field at
`src/csf_mission.cpp:357-362`. It emits an error, stops parsing the four-field
prefix, and does not append an empty reference. This contradicts the phase-2
contract that a zero-sized VIS reference is retained as a missing optional edge.

An empty collision, texture-directory, or sky field can therefore erase the
remaining VIS fields and their evidence. An empty first field can also leave a
mission with no visual-map edge, causing the GUI mission load to fail with
“Mission has no resolved visual map” even though the VIS record itself was
structurally present.

Permit zero-length fields, advance past their four-byte length, append an empty
reference with the appropriate dependency kind, and let normal resolution
retain it as a missing edge. Keep the truncation diagnostic for insufficient
bytes or an invalid oversized length. Add tests for zero in each of the four
positions and for bytes following a zero field.

### Medium — reverse-use lookup is case-sensitive despite Windows-like resolution

The resolver deliberately folds ASCII case, but `MissionGraph::uses` compares
the query path and resolved path with a raw lexical equality at
`src/csf_mission.cpp:648-655`. On Windows, asking for
`models\\hero.rpc` will not necessarily match an indexed file stored as
`Models\\Hero.rpc`, even though the graph resolved the same reference with a
`case-mismatch` status. `csf-info uses` can consequently report zero uses for a
valid asset.

Normalize both sides with the same slash and case-folding logic used by the
index, or compare through `ResourceIndex::find_path`/a canonical resource
identity. Add a reverse-use test with different path casing and separators.

### Medium — scene JSON can be invalid for finite-looking structural inputs

`real()` accepts every binary32 value at `src/csf_mission_scene.cpp:53-57`, but
only positions are screened for finiteness by `vec3`. The generic JSON helper
prints optional numbers directly at `src/csf_mission_scene.cpp:138-145`, and it
is used for actor headings/pitches, environment reals, area heights, and light
radii (for example `src/csf_mission_scene.cpp:393-413`). A structurally exact
CSFFBS document containing a NaN or infinity in one of those fields produces
`nan` or `inf`, neither of which is valid JSON. The command can therefore
claim to write scene JSON while emitting a file that standard JSON parsers
reject.

Either convert non-finite numeric values to `null` and emit a typed diagnostic,
or reject them during projection. Add a scene-JSON test containing NaN and
infinity in scalar fields and validate the output with a JSON parser.

### Medium — navigation validation collapses missing and duplicate identities

The typed projection stores a point's group as `group.id.value_or(0)` at
`src/csf_mission_scene.cpp:213-224`, then validates links through a map keyed
only by `(group_id, point_id)` at `src/csf_mission_scene.cpp:283-306`. A group
without an ID is therefore treated as group `0`, and duplicate group IDs or
duplicate point IDs are merged into the same lookup set. A link can be marked
`valid=true` even though its endpoint is missing, duplicated, or belongs to an
ambiguous group; connected-component and orphan counts are affected too.

Keep identity validity separate from the display fallback value, and only use a
unique, explicitly defined `(group ID, point ID)` pair for link validation. When
an identity is missing or duplicated, retain the link but mark it invalid or
ambiguous and report the reason. Add tests for a missing group ID and for two
groups sharing an ID with overlapping point IDs.

### Medium — temporary export files have a symlink/TOCTOU overwrite window

Both export implementations check whether `<output>.csf-info.tmp` exists and
then open it for writing without exclusive creation:

- `app/csf_info.cpp:208-223`
- `src/csf_export.cpp:255-267`

The check and the truncating `std::ofstream` open are not atomic. A local
attacker who can modify the output directory can race the check or place a
dangling symlink at the temporary path, redirecting the generated data into an
arbitrary file. This weakens the documented no-overwrite guarantee even though
the final `copy_file(..., none)` protects an already-existing destination.

Create the temporary file with an OS-level exclusive-create primitive, keep it
in the destination directory, write and flush it, then publish it with an
atomic no-replace operation where available. Treat a pre-existing symlink or
failed exclusive create as a hard error. Add a test for an occupied temporary
path; platform-specific race testing may be needed for the full security
property.

## Verification

The following checks passed during this review:

- `./build.ps1 -CoreOnly`
- `./test.ps1`
- `./build.ps1 -Target rws-man`
- `./build.ps1 -CoreOnly -Config Debug`
- `./test.ps1 -Config Debug`

The existing tests cover the normal VIS adapter, basic precedence, ordinary
case mismatch resolution, typed scene projection, and deterministic exports,
but they do not cover the edge cases above.
