# Review: The Great Shift, Phases 01-03

- **Date:** 2026-09-19
- **Reviewer:** deepseek-flash
- **Scope:** working-tree diff for Phase 01 (generic CSFFBS core), Phase 02
  (mission package resolution), and Phase 03 (Mission Explorer)
- **Branch:** `great-shift`
- **Method:** source review plus local build, tests, sanitizers, fuzzing, and a
  read-only pass over the reference corpus.

## Verification performed

| Check | Result |
| --- | --- |
| Core-only Debug build (`RWSMAN_BUILD_GUI=OFF`) | Pass (warnings only) |
| Core-only Release build | Pass |
| `rws_core_tests` Debug | Pass |
| `rws_core_tests` Release | Pass, but see T-1 |
| `rws_core_tests` under ASan + UBSan | Pass |
| 200k-input fuzz of `Document::from_bytes` + `export_json` (ASan/UBSan) | No findings |
| 40k-input fuzz of `read_vis/txl/m3d/and/phd` (ASan/UBSan) | No findings |
| `csf-info corpus CSF_unpacks` | 1262/1262 CSFFBS parse, 0 error files |
| All 21 SCN mission graphs | Exit 0, no fatal failures |
| Real `--objects` / `--navigation` / `--graph` (Ambush) | Works |

The container parser, string handling, and adapter bounds checks are in good
shape. The issues below are concentrated in the mission/JSON/GUI layer, in a few
tree-reconstruction edge cases, and in test/performance discipline.

## Findings

### High

#### H-1. `mission_graph_json` corrupts non-ASCII (UTF-8) text

- **Location:** `src/csf_mission.cpp:130-145`
- **Evidence:** `json_escape` escapes every byte `>= 0x7f` as `\u00XX`, so a
  reference spelled `Models/Café.dff` is emitted as `Models/Caf\u00c3\u00a9.dff`
  and parses back as `Models/CafÃ©.dff`. Verified with a crafted CSFFBS string.
- **Impact:** breaks the “stable JSON export” contract for any path or
  diagnostic containing Western-European bytes.
- **Recommendation:** escape only bytes `< 0x20` (as `csf_export.cpp:65` and
  `csf_mission_scene.cpp:126` already do) or decode UTF-8 codepoints explicitly.

#### H-2. `csf-info mission` silently drops requested outputs

- **Location:** `app/csf_info.cpp:228-256`
- **Evidence:** a single `mode` variable is overwritten by each recognized
  option. `mission … --graph out.json --objects` runs `--objects` and never
  creates `out.json`. Confirmed.
- **Impact:** scripts silently lose graph/scene/symbol exports depending on
  argument order; also hides mistakes because exit status stays 0.
- **Recommendation:** make each command/output an independent flag and reject
  genuinely incompatible combinations explicitly.

#### H-3. Sticky mission selection in the viewport

- **Location:** `app/geometry_preview.cpp:2277`; inspector preference at
  `app/main.cpp:1515+`
- **Evidence:** when an overlay is picked, `selected_mission_entry_` is set; when
  the click falls through to collision/scene picking it is not cleared. The
  inspector prefers the mission source while `workspace == Mission`.
- **Impact:** once a marker is clicked, RWS chunk/collision inspection is
  shadowed and the marker highlight persists. No obvious way to deselect.
- **Recommendation:** reset `selected_mission_entry_` in the non-overlay branch
  and whenever `selected_chunk`/`select_collision` is set.

#### H-4. `MissionScene::project` discards the whole typed view on any structural error

- **Location:** `src/csf_mission_scene.cpp:153`
- **Evidence:** the early-out triggers on `document.has_errors()`. A single
  unknown entry type anywhere yields an empty scene. Confirmed with a crafted SCN
  (valid prefix plus one type-99 entry): `--objects` prints nothing, exit 2.
- **Impact:** contradicts Phase 01’s “malformed suffix must not erase the
  validated prefix” and Phase 03’s “invalid records remain inspectable”.
- **Recommendation:** project the valid prefix and attach the document
  diagnostics instead of returning an empty scene.

#### H-5. Mission overlays are wiped while the mission stays loaded

- **Location:** `app/main.cpp:944` (manual pairing), `1178` (Clear companion),
  `1228` (Reload preview)
- **Evidence:** these paths call `geometry_preview.clear()` but do not reset
  `mission_scene`/`mission_document`, even though a mission is active.
- **Impact:** viewport markers disappear while the Mission tree and inspector
  still show mission objects; overlay picking stops working until mission reload.
- **Recommendation:** preserve overlays on those paths, or clear the mission
  state consistently when the visual/collision pairing changes.

### Medium

#### M-1. Wrong `Selectable` argument for the Mission workspace entry

- **Location:** `app/main.cpp:1316`
- **Evidence:**
  ```cpp
  ImGui::Selectable("Mission", workspace == Workspace::mission, mission_scene != nullptr);
  ```
  The third parameter is `ImGuiSelectableFlags` (an `int`), not `enabled`. It is
  never disabled and, when a mission is loaded, sets
  `ImGuiSelectableFlags_NoAutoClosePopups`, leaving the combo open. The
  `MenuItem` at `app/main.cpp:1200` is correct.
- **Recommendation:** use `ImGuiSelectableFlags_Disabled` (and/or guard the
  assignment) instead of passing a bool.

#### M-2. Unknown-type entry leaks the pending identifier label

- **Location:** `src/csf_document.cpp:307-318`
- **Evidence:** the unknown-type branch never touches `pending_label_entry`.
  With entries `[identifier, type 99, int 42]` the tree attaches the label to
  `42` and skips the unknown entry:
  ```
  [2 @0x30] integer label = 42
  error at 0x2e entry 1: Unknown entry type 99
  ```
- **Impact:** mis-associated labels in the reconstructed tree when an unknown
  type appears between a label and its value.
- **Recommendation:** consume or clear the pending label on the unknown path.

#### M-3. `uses` / `mission` corpus hashing is O(N²)

- **Location:** `src/csf_mission.cpp:500-524`, `app/csf_info.cpp:308`
- **Evidence:** every `MissionGraph::load` recursively scans the package parent
  and FNV-hashes every same-size SCN; `uses` rebuilds the whole graph and index
  per SCN. `csf-info uses … --root CSF_unpacks` did not finish in **600 s** on
  the 18,938-file corpus.
- **Impact:** conflicts with the Phase 02 target “do not hash 2.56 GB on each
  launch”.
- **Recommendation:** compute/cache SCN signatures once, or gate the duplicate
  scan behind an explicit flag; build the resource index once for `uses`.

#### M-4. Overlay rendering is per-primitive, not batched

- **Location:** `app/geometry_preview.cpp:2390-2410`, `make_mission_overlays`
  in `app/main.cpp`
- **Evidence:** one `AddCircleFilled` + `AddCircle` per point, one `line3d` per
  line, and 24 segments per light ring, re-projected every frame. Snipers alone
  has ~1,700 points and 962 links.
- **Impact:** conflicts with the Phase 03 target “do not issue one draw call per
  actor/point”; scaling risk on the largest scenes.
- **Recommendation:** batch by layer into reusable command buffers and cache
  projections between camera changes.

#### M-5. Case-insensitive shared-root matches lose their rule

- **Location:** `ResourceIndex::resolve`, `src/csf_mission.cpp`
- **Evidence:** `case_mismatch` is only assigned inside the `preferred_root`
  branch. The shared-root loop and logical fallback report `shared_root` for both
  exact and case-insensitive hits.
- **Impact:** `docs/mission-resolution.md` promises “every non-exact resolution
  should record the rule used”; this distinction is lost outside the package
  root.
- **Recommendation:** preserve the exact/case-mismatch status per root.

#### M-6. Throwing `std::filesystem::equivalent` during mission load

- **Location:** `src/csf_mission.cpp:519`
- **Evidence:** the throwing overload is used on each same-size candidate.
- **Impact:** a permission/transient error on one candidate aborts the whole
  load; in `uses` it aborts the entire scan because there is no per-scene guard.
- **Recommendation:** use the `error_code` overload and downgrade to a warning.

### Low

#### L-1. Parser allocation amplification

- **Location:** `src/csf_document.cpp` (`max_table_records = 16M`, then
  `output.reserve(count)`)
- **Impact:** a ~64 MB file can force ~900 MB of `RawString` reservations.
  Bounded, but easy to tighten.

#### L-2. Unbounded adapter file reads

- **Location:** `read_file` / `read_phd_candidates` in `src/csf_mission.cpp`
- **Impact:** whole-file loads with no cap; PHD copies the entire file into
  `unknown_tail`.

#### L-3. Missing group IDs collapse to `0`

- **Location:** `src/csf_mission_scene.cpp` (`point.group_id = group.id.value_or(0)`)
- **Impact:** distinct anonymous groups share a bucket for duplicate-point and
  adjacency validation, with no diagnostic.

#### L-4. `.SEGUNDA_EXPLOSION` listed as known but never parsed

- **Location:** actor projection in `src/csf_mission_scene.cpp`
- **Impact:** the field is excluded from `unknown_fields` yet has no typed field,
  so the “unknown fields remain listed” guarantee is incomplete.

#### L-5. New compiler warnings

- `-Wmissing-field-initializers` on aggregate initializations in
  `src/csf_mission.cpp` and `src/csf_mission_scene.cpp`.
- `-Wmisleading-indentation` at `src/csf_mission_scene.cpp:383,399`.
- Harmless but noisy under `/W4`.

### Test / build gaps

#### T-1. Release builds assert nothing

- **Evidence:** Release uses `-DNDEBUG` (confirmed in `build-rel/build.ninja`);
  all of `tests/document_tests.cpp` uses `assert`. `test.ps1` and `build.ps1`
  default to Release, so the entire suite (existing and the new 462 lines) is a
  no-op and the “Release tests pass” claims in the phase docs are vacuous.
- **Recommendation:** add a `CHECK` macro that always evaluates, or run tests in
  Debug/CI.

#### T-2. Missing coverage for promised behavior

- No test for `write_new_export` overwrite/input refusal, despite Phase 01
  promising it.
- No CLI-level tests for `csf-info` (mission option combinations, exports,
  corpus exit codes).
- No test for the option-order behavior in H-2.

## Notes / non-issues

- **Path safety is good.** `traversal_or_absolute` rejects `..`, absolute, UNC,
  and drive-relative references before lookup, and resolution only opens files
  from the pre-built index, never the raw reference string.
- **Exports are non-destructive.** `write_new_export` refuses the input path and
  existing destinations; `-` writes to stdout with diagnostics on stderr.
- **Parser robustness is strong.** Bounds/overflow handling, string
  terminator/embedded-null handling, depth cap, trailing-byte preservation, and
  the unknown-type diagnostic all withstood corpus and sanitizer fuzzing.
  Two intentional dead diagnostics remain (container underflow), which is
  harmless.

## Suggested fix order

1. H-1 non-ASCII JSON, H-2 dropped outputs, H-3 sticky selection
2. H-4 partial projection, H-5 overlay/dataload consistency
3. M-1 GUI flag bug, M-2 label leak
4. M-3/M-4 performance, T-1 test activation
5. Remaining medium/low items

## Corpus observations used

- 1,262 CSFFBS documents, entry types 0–5 only, all `reserved=007f, version=1`,
  zero structural errors.
- All 21 SCN graphs load without fatal failure.
- Snipers: 1,712 navigation points / 962 connections; Ambush: 492 points /
  178 connections.
