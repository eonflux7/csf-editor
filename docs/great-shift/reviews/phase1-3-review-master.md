# Master review: The Great Shift, phases 01-03

- **Date:** 2026-09-19
- **Scope:** the complete current working tree, including tracked modifications
  and untracked Phase 01-03 implementation and documentation files
- **Inputs:** `phase1-3-review-deepseekflash.md` and
  `phase1-3-review-luna-xhigh.md`, rechecked against the source
- **Method:** source/diff review, contract comparison, supported MSVC builds, and
  the Debug test suite

## Verification

| Check | Result |
| --- | --- |
| `git diff --check` | Pass (line-ending notices only) |
| `./test.ps1 -CoreOnly -Config Debug` | Pass, 1/1 test |
| `./build.ps1 -Target rws-man` | Pass |
| Review of every tracked and untracked implementation file | Complete |

The earlier corpus, sanitizer, and fuzz results were not repeated for this
synthesis. They remain useful evidence from the DeepSeek review, but they do not
invalidate the correctness and lifecycle issues below.

## Summary

The parser's bounds checks and raw-data preservation are generally sound. The
main release blockers are that the default/CI test configuration performs no
assertions, overlapping roots can make unique logical resources ambiguous,
typed projection discards all usable data after any structural error, and
mission graph JSON corrupts UTF-8 text. Mission loading also does substantially
more filesystem work than its documented interactive/cached design allows.

This review contains four high-severity, nine medium-severity, and six
low-severity findings. Findings that described the same root cause have been
merged. False positives and non-actionable suggestions from the input reviews
are listed at the end.

## Findings

### High

#### H-1. The default and CI test runs compile out every assertion

- **Locations:** `tests/document_tests.cpp` throughout; `CMakeLists.txt:93-95`;
  `.github/workflows/ci.yml:20`
- **Evidence:** the test executable uses plain `assert` for all checks. Both
  scripts default to Release, CI invokes `./test.ps1 -CoreOnly` without a Debug
  override, and the generated Release test project defines `NDEBUG`.
- **Impact:** the advertised Release and CI passes exercise setup and code that
  occurs outside assertions, but validate no result. Regressions in the new
  parser, resolver, scene projection, JSON, and overlay math can all pass CI.
- **Recommendation:** replace `assert` with an always-evaluated test/check
  facility, or make the test target undefine `NDEBUG`. Running only Debug in CI
  is a weaker fallback because Release-specific behavior remains unchecked.

#### H-2. Overlapping roots duplicate logical candidates

- **Locations:** `src/csf_mission.cpp:231-267`, `324-337`, `438-446`
- **Evidence:** the default index adds both the package root and its parent. The
  parent recursively indexes the package again. `logical_` stores resource
  indices without deduplicating physical paths, so a nested resource that reaches
  logical fallback can appear once through each root. `resolve` then classifies
  the two indices as ambiguous even though they identify the same file.
- **Impact:** a unique resource can become unresolved solely because the default
  roots overlap. The graph retains two identical candidate paths and the GUI may
  fail to load a required map/resource.
- **Recommendation:** deduplicate logical candidates by canonical filesystem
  identity before deciding ambiguity. Preferably avoid recursively indexing the
  package twice while retaining declared root-precedence metadata.

#### H-3. One structural error erases the entire typed mission view

- **Location:** `src/csf_mission_scene.cpp:150-158`
- **Evidence:** `MissionScene::project` returns immediately whenever
  `document.has_errors()` is true. An unknown entry after valid actors or
  navigation therefore produces an empty scene.
- **Impact:** this contradicts Phase 01's usable-prefix rule and Phase 03's
  requirement that invalid records remain inspectable. CLI object reports and
  GUI overlays lose all validated records because of an unrelated malformed
  suffix.
- **Recommendation:** reject only non-CSFFBS input. Project the validated tree
  that is present and attach the document diagnostics to the typed scene.

#### H-4. Mission graph JSON corrupts UTF-8 strings

- **Locations:** `src/csf_mission.cpp:130-147`, `626-637`, `660-732`
- **Evidence:** CSFFBS strings are converted from Windows-1252 to UTF-8 before
  becoming graph references. `mission_graph_json` then escapes each byte at or
  above `0x7f` independently as `\u00XX`. UTF-8 `caf\xC3\xA9` consequently
  becomes JSON text that decodes as `caf\u00C3\u00A9` (`cafÃ©`).
- **Impact:** references, diagnostics, and paths with Western-European text are
  changed by the supposedly stable graph export.
- **Recommendation:** preserve UTF-8 bytes and escape only JSON control bytes,
  as the other two JSON exporters do, or decode code points before emitting
  `\u` escapes. Add a round-trip test through a standards-compliant JSON parser.

### Medium

#### M-1. Scene JSON emits non-standard `nan` and `inf` tokens

- **Locations:** `src/csf_mission_scene.cpp:53-57`, `138-145`, `373-416`
- **Evidence:** scalar floats are not checked for finiteness. Actor angles,
  environment values, area heights, and light radii are streamed directly.
  Only vector positions pass through the finite check in `vec3`.
- **Impact:** a structurally valid document can make `--scene-json` write a file
  that standard JSON parsers reject.
- **Recommendation:** serialize non-finite values as `null` with a typed
  diagnostic, or reject them during projection. Validate all JSON tests by
  parsing the output, not by substring comparison.

#### M-2. Navigation validation treats missing and duplicate identities as usable

- **Locations:** `src/csf_mission_scene.cpp:205-235`, `283-319`;
  `app/main.cpp:805-821`
- **Evidence:** a missing group ID becomes `0`; points are reduced to sets keyed
  by `(group_id, point_id)`; duplicate group and point IDs are still considered
  valid endpoints. Overlay construction then stores one position per key, so a
  duplicate silently overwrites the earlier point.
- **Impact:** ambiguous links can be reported as valid, connected-component and
  orphan counts are wrong, and the GUI can draw a link to an arbitrary duplicate
  point.
- **Recommendation:** keep optional identity separate from display fallback
  values. Resolve links only against a unique explicit group/point pair and mark
  missing or duplicated endpoints invalid/ambiguous with a reason.

#### M-3. Typed field projection silently accepts duplicates and wrong types

- **Locations:** `src/csf_mission_scene.cpp:29-80`, `178-280`
- **Evidence:** `child` returns the first matching field. Wrong-typed values
  simply become `nullopt`, duplicate known fields are excluded from
  `unknown_fields`, and almost none receive a diagnostic. In addition,
  `.SEGUNDA_EXPLOSION` is declared known at lines 195-197 but has no typed member,
  so it disappears from both the typed record and `unknown_fields`.
- **Impact:** malformed or extended records can look like clean records with a
  missing value, while source data is silently absent from the typed view. This
  conflicts with the documented duplicate/wrong-kind diagnostic and unknown
  field contracts.
- **Recommendation:** collect all fields by name, require exactly one value of
  the expected kind, diagnose zero/multiple/wrong-kind cases, and retain every
  unprojected field as raw evidence.
- **Status:** newly found during master review.

#### M-4. Scene JSON omits data already present in the typed model

- **Location:** `src/csf_mission_scene.cpp:373-416`
- **Evidence:** the export omits actor and navigation `unknown_fields`, navigation
  point `group_id`, heading, and pitch, dummy heading and pitch, and the duplicate
  group/point counts from `NavigationStats`.
- **Impact:** consumers cannot reproduce the typed projection or its validation
  results from `csf-mission-scene-1`, even for valid source data. The omissions
  are not marked as intentional schema boundaries.
- **Recommendation:** include all projected fields and validation counters, or
  explicitly version and document a deliberately reduced summary schema.
- **Status:** newly found during master review.

#### M-5. Mission overlay and selection state become inconsistent

- **Locations:** `app/main.cpp:933-959`, `1175-1179`, `1227-1230`;
  `app/geometry_preview.cpp:2242-2287`
- **Evidence:** manual collision pairing, clearing the companion, and reloading
  the preview call `geometry_preview.clear()`, which removes overlays while
  leaving `mission_scene` and the Mission workspace active. Separately, a click
  that falls through to visual/collision picking never clears
  `selected_mission_entry_`.
- **Impact:** markers can disappear while the mission tree remains loaded, and a
  stale mission selection continues to shadow the RWS/collision selection in the
  inspector.
- **Recommendation:** make mission overlay ownership independent of preview
  cache clearing (or rebuild it immediately), and clear mutually exclusive
  selections whenever another selection kind wins.

#### M-6. Reverse-use lookup is case-sensitive after case-insensitive resolution

- **Locations:** `src/csf_mission.cpp:648-657`; `app/csf_info.cpp:294-317`
- **Evidence:** `MissionGraph::uses` compares `std::filesystem::path` values by
  lexical equality. A query whose case differs from the indexed Windows path can
  miss an edge that the resolver itself accepted as a case mismatch.
- **Impact:** `csf-info uses` can report zero uses for an asset present in the
  graph.
- **Recommendation:** compare a canonical resource identity or the same
  slash-normalized ASCII-folded key used by `ResourceIndex`.

#### M-7. Mission loading and `uses` repeatedly scan and hash the corpus

- **Locations:** `src/csf_mission.cpp:434-526`; `app/csf_info.cpp:300-315`
- **Evidence:** each graph rebuilds recursive indexes, recursively scans the
  package parent for same-size SCNs, and hashes candidates. `uses` repeats that
  work for every SCN. The DeepSeek review's 18,938-file run did not finish in
  600 seconds.
- **Impact:** the reverse-use command is practically unusable on the reference
  corpus, and GUI mission opening violates the documented interactive/cached
  design.
- **Recommendation:** build one shared resource index and SCN signature cache per
  root, pass it into graph loads, and make duplicate-content scanning explicit or
  cached.

#### M-8. Indexed file symlinks can resolve outside configured roots

- **Locations:** `src/csf_mission.cpp:225-267`, `528-559`
- **Evidence:** `recursive_directory_iterator` entries are accepted with
  `is_regular_file`, which follows a file symlink, but the target is never
  canonicalized and checked for containment. A safe-looking in-root reference
  can therefore resolve to and later open an out-of-root target.
- **Impact:** the `outside_root` policy is lexical rather than physical, contrary
  to the documented root confinement and the earlier review's path-safety
  conclusion.
- **Recommendation:** reject symlinks/reparse points or canonicalize each target
  and verify it remains below an allowed canonical root before indexing it.
- **Status:** newly found during master review.

#### M-9. Pending identifier labels cross unknown entries

- **Location:** `src/csf_document.cpp:305-318`
- **Evidence:** the unknown-type branch inserts and consumes an unknown node but
  leaves `pending_label_entry` intact. In `[identifier, unknown, integer]`, the
  identifier is attached to the integer after the unknown record.
- **Impact:** the reconstructed tree associates a label with the wrong value on
  unsupported input.
- **Recommendation:** consume/clear the pending label on the unknown entry and
  emit a diagnostic explaining that it could not be attached safely.

### Low

#### L-1. Conflicting mission modes are silently resolved by argument order

- **Location:** `app/csf_info.cpp:225-288`
- **Evidence:** one `mode` variable is overwritten for every output option.
  `--graph out.json --objects` succeeds in object mode and never creates the
  requested graph.
- **Impact:** the usage text presents modes as alternatives, so they need not be
  combinable, but silently ignoring an explicitly requested output is still
  unsafe for scripts.
- **Recommendation:** reject multiple modes, or implement independent output
  flags where combinations are useful.

#### L-2. The Mission workspace combo passes a boolean as flags

- **Location:** `app/main.cpp:1314-1324`
- **Evidence:** the third `ImGui::Selectable` argument is
  `ImGuiSelectableFlags`, not an enabled boolean. `mission_scene != nullptr`
  never disables the entry and becomes `NoAutoClosePopups` when true.
- **Impact:** Mission can be selected without a mission and the combo can remain
  open unexpectedly.
- **Recommendation:** guard the assignment or use
  `ImGuiSelectableFlags_Disabled` when no mission is loaded.

#### L-3. Shared-root case mismatches are reported only as `shared-root`

- **Location:** `src/csf_mission.cpp:319-337`
- **Evidence:** only the preferred-root branch assigns `case_mismatch`; shared
  roots use `shared_root` for exact and case-folded matches alike.
- **Impact:** case problems outside the package root are absent from diagnostics
  despite the documented case-mismatch reporting.
- **Recommendation:** represent root selection and spelling match as separate
  attributes, or add combined statuses.

#### L-4. Empty TXL lines make later evidence line numbers wrong

- **Location:** `src/csf_mission.cpp:376-393`
- **Evidence:** the adapter consumes all consecutive CR/LF bytes in one loop but
  increments `line` only once. For `A\n\nB`, `B` is reported on line index 1
  instead of 2.
- **Impact:** dependency evidence points at the wrong source line after any blank
  line, violating the documented original-line preservation.
- **Recommendation:** consume one logical line ending at a time and advance the
  counter for every empty line; treat CRLF as one ending.
- **Status:** newly found during master review.

#### L-5. Filesystem errors can abort whole mission/corpus operations

- **Locations:** `src/csf_mission.cpp:517-525`, `574-585`;
  `app/csf_info.cpp:300-315`
- **Evidence:** duplicate detection uses the throwing `filesystem::equivalent`
  overload and unguarded hashing. Map-local enumeration also uses throwing
  operations, and `uses` has no per-scene recovery.
- **Impact:** one inaccessible or transiently failing file can abort a mission
  load or the complete reverse-use scan.
- **Recommendation:** use `error_code` overloads and downgrade per-resource
  failures to source-bearing diagnostics.

#### L-6. Temporary export creation is not exclusive

- **Locations:** `src/csf_export.cpp:240-276`; `app/csf_info.cpp:208-223`
- **Evidence:** both implementations check whether a predictable `.csf-info.tmp`
  path exists and then open it with a truncating `ofstream`. The check/open pair
  is racy and follows a symlink placed between them.
- **Impact:** another local process can redirect or truncate the temporary write,
  weakening the no-overwrite guarantee. The final destination copy itself does
  correctly refuse replacement.
- **Recommendation:** create the temporary file atomically with exclusive-create
  semantics, then publish with no-replace semantics.

## Hardening and coverage gaps

- `max_table_records` still permits substantial `RawString` reservation
  amplification from a comparatively small length-header-only file. Derive
  allocation limits from remaining bytes and a practical memory budget.
- Adapter reads load complete files, and PHD duplicates the complete byte stream
  into `unknown_tail`. Add a file-size policy if these APIs are meant to accept
  untrusted inputs.
- Add CLI-level tests for mode conflicts, exports, exit codes, and existing
  temporary paths. Add direct tests for partial typed projection, UTF-8 graph
  JSON, non-finite scene JSON, preferred-root logical resolution, duplicate and
  missing navigation identities, reverse-use case folding, symlink containment,
  and TXL blank-line evidence.

## Disposition of the input reviews

### Merged or retained with changed scope

- The two missing/duplicate navigation identity reports are one finding (M-2).
- Sticky selection and disappearing overlays share a mission lifecycle/state
  boundary and are grouped in M-5.
- `.SEGUNDA_EXPLOSION` is one concrete instance of the broader typed-field
  preservation failure in M-3.
- Mission output option handling is retained but downgraded to low severity:
  the documented syntax makes the modes alternatives, so the defect is silent
  acceptance rather than failure to support combined output.
- Throwing `equivalent` is broadened to the general per-resource filesystem
  error problem in L-5.

### Removed as false positives or unsupported claims

- **Shared-root exact matches preceding logical fallback follow the detailed
  contract.** Both `docs/mission-resolution.md` and the Phase 02 resolution
  policy put configured shared roots before the unique logical-path fallback.
  The broader phrase “package-local paths take precedence” is imprecise, but the
  implementation's ordering is not itself a deviation. H-2 retains the separate
  physical-candidate duplication bug caused by overlapping roots.
- **Zero-length VIS fields are accepted.** At `src/csf_mission.cpp:358-369`,
  `!length` tests whether the optional 32-bit read failed; it does not test
  `*length == 0`. A zero field is appended and parsing continues.
- **Overlay API calls are not one GPU draw call per primitive.** ImGui batches
  compatible draw-list geometry. The code does perform per-frame CPU projection
  and tessellation, which may deserve profiling, but the review did not
  demonstrate the claimed draw-call violation.
- **New GCC warning noise is not a supported-target failure.** The project is
  explicitly MSVC/Visual Studio 2026 targeted, and the supported `/W4` builds
  completed without those warnings. Cross-compiler cleanup can be handled
  separately.
- The generic “missing tests” lists are not independent product defects; the
  actionable cases are captured in the findings and coverage section above.

## Recommended fix order

1. Make Release/CI tests effective (H-1).
2. Deduplicate resources indexed through overlapping roots (H-2).
3. Preserve valid typed prefixes and correct both JSON exporters (H-3, H-4,
   M-1).
4. Repair navigation identity and typed-field validation (M-2, M-3).
5. Make mission overlay/selection state coherent and make scene JSON complete
   (M-4, M-5).
6. Cache mission indexes/signatures and normalize reverse-use identity (M-6,
   M-7).
7. Address root containment, evidence accuracy, filesystem resilience, and the
   remaining CLI/UI/export hardening items.
