> **Archived plan — implemented.** Program and script behavior as built is
> documented in [Mission editor](../../guides/mission-editor.md); this document is
> kept for its corpus audit, rankings, and test plan.

# Format sprint 01: mission programs and remaining scene records

## Outcome

Turn GSC and CSC from generic CSFFBS trees into source-backed program documents,
add a useful script workspace, and type the highest-value SCN records that remain
outside `MissionScene`.

The first deliverable is inspection and reference navigation, not a game-script
virtual machine. A guarded script-editor baseline may follow in the same sprint
only after the Phase 06 CSFFBS authoring gate is satisfied: byte-identical no-op
serialization, reparse and validation, undo/redo, and save-copy or staging output.

At the end of this sprint, a user should be able to:

- browse GSC/CSC scripts, variables, events, conditions, actions, operands,
  comments, flags, folders, and declared resources without using the raw tree;
- follow typed operands to SCN actors, dummies, areas, navigation points, scripts,
  triggers, variables, `Anims.bdd`, and other compatible database records;
- inspect conservative control structure without implying runtime execution;
- find all proven script uses of a selected mission object;
- inspect scene-object animation bindings and selected remaining SCN systems;
- if serialization prerequisites are complete, make a small set of reversible,
  schema-aware script edits and save only to a new or staged file.

## Dependencies and authoring gate

- The generic CSFFBS parser, raw tables, stable source identities, and diagnostic
  model from Phase 01.
- Mission/resource resolution and package-local BDD discovery from Phase 02.
- `MissionScene`, symbol indexing, and Mission Explorer from Phase 03.
- The animation catalog and existing GSC `PLAY_ANMBDD*` evidence chain from
  Phase 05.
- For any write operation: the Phase 06 no-op serializer, edit transactions,
  reopen-and-validate pipeline, and non-destructive save/staging behavior.

Read-only program projection and UI work does not depend on completing the
serializer. No edit control should be enabled merely because its source node is
known.

## Corpus audit

All measurements below come from a read-only scan of `../CSF_unpacks`. Path-level
counts include package copies. Where useful, content hashes are used to state a
deduplicated count. These are observations about this corpus, not format limits.

### GSC mission programs

- 21 files; all parse exactly with no structural variant encountered.
- 17 distinct contents by SHA-256; four paths duplicate shared mission data.
- Every document has the same four top-level sections: `.RECURSOS`, `.VARIABLES`,
  `.SCRIPTS`, and an empty `.POOL`.
- Across all paths: 5,762 scripts, 102,918 actions, 2,784 conditions, 3,876 event
  declarations, 1,898 global variables, and 2,331 script-local variables.
- Across the 17 distinct contents: 4,646 scripts, 77,858 actions, 1,877
  conditions, 3,138 events, 1,300 global variables, and 2,067 local variables.
- There are 199 observed action opcodes and 22 condition opcodes.
- Every script has `.ID`, `.NOMBRE`, `.CARPETA`, and `.FLAGS`. There are no
  duplicate script IDs within a document. Forty scripts omit `.ACCIONES`; an
  absent action section is valid and must not be diagnosed as corruption.
- Script flags use four observed combinations. `.VALIDO` is always `1`, while
  `.TRIGGER` and `.ENABLED` independently vary between `0` and `1`.
- `.CARPETA` has 352 observed spellings and is useful author-defined organization,
  including case variants that must not be normalized away.
- Variables have a stable `{.ID, optional .ARRAY, .TYPE, .NOMBRE, .VALOR}` shape.
  Observed types include `BICHO`, `BOOL`, `NUMERO`, `PATHPOINT`, `SCRIPT`,
  `CLASSID`, `DUMMY`, `ZONA`, `ACTITUD`, `TRIGGER`, `DISFRAZ`, and
  `GRUPO_PATHPOINT`. Values occur as integers, reals, strings, and small
  containers.
- The most frequent distinct-content action opcodes are `COMENTARIO` (10,844),
  `COMENTARIO_ACCION` (8,346), `IF`/`ENDIF` (4,204 each), `CONTINUE` (3,992),
  `PAUSE` (3,957), `SET` (3,159), `WHILE`/`WEND` (1,627 each), and `ARRAY_ADD`
  (1,598). This demonstrates both the value of comments and the need to represent
  structured control without executing it.
- High-volume typed operands include `BICHO`, `PATHPOINT`, `SCRIPT`, `ANM_BDD`,
  `DUMMY`, `TRIGGER`, `EVENT`, `SONIDO_BDD`, `EFECTO_CLASSID`, `ARMA_CLASSID`,
  `ZONA`, `VAR`, and `ARRAYID`.

The operand tags already provide strong reference evidence. In the path-level
scan, simple numeric operands resolved uniquely as follows:

| operand | observed | uniquely resolved | caveat |
|---|---:|---:|---|
| `BICHO` | 10,461 | 10,420 | missing values may include sentinels |
| `DUMMY` | 4,424 | 4,399 | resolve within the sibling SCN |
| `ZONA` | 1,302 | 1,301 | resolve against SCN areas |
| `SCRIPT` | 4,479 | 4,468 | document-local script ID |
| `TRIGGER` | 4,208 | 4,182 | target must also have trigger semantics |
| `ANM_BDD` | 4,395 | 4,266 | package-local animation catalog |

All 6,326 observed `PATHPOINT` operands carry two integers. Their shape matches
the established SCN `(group ID, point ID)` identity; validation must use the pair,
not the point ID alone. `ARRAYID` and `VAR` require lexical/script scope because
local identifiers repeat legitimately across scripts.

`.RECURSOS` provides explicit declaration lists. Across all paths it contains
1,238 sound IDs, 302 object class IDs, 453 effect class IDs, 217 weapon class IDs,
1,733 animation IDs, and 62 FBS paths. These lists are valuable for validation and
editor completion even before their runtime loading behavior is known.

### CSC cutscene programs

- 21 files, 13 distinct contents, and 12 non-empty script documents; all parse
  exactly. The repeated empty documents do not inflate the semantic counts.
- 713 scripts and 2,375 actions use exactly eight observed opcodes:

| opcode | actions | established operand shape |
|---|---:|---|
| `PAUSE` | 900 | `NUMERO`, real |
| `CAMARA_EN_DUMMY` | 502 | `DUMMY`, integer |
| `CUTSCENE_EXE` | 476 | `CUTSCENE`, integer |
| `CONTINUE` | 238 | `CUTSCENE_EXE`, then `CUTSCENE`, integer |
| `WAIT_CONDICION` | 129 | `CUTSCENE_FINISHED`, then `CUTSCENE`, integer |
| `CAM_SETFILTRO` | 120 | `CADENA`, string |
| `CAMARA_FOV` | 7 | `NUMERO`, real |
| `PLAY_SONIDOID` | 3 | `SONIDO_BDD` integer plus two `NUMERO` reals |

- Every `CAMARA_EN_DUMMY` target resolves uniquely to an SCN dummy.
- Every `CONTINUE` and `WAIT_CONDICION` target resolves uniquely to a CSC script
  ID.
- Only 237 of 476 `CUTSCENE_EXE` targets resolve in the same CSC. Another 77
  appear as sibling GSC script IDs and the remainder resolve in neither. These
  must be displayed as candidates rather than asserted call edges.
- Every CSC script has `.ID`, `.NOMBRE`, `.CARPETA`, `.FLAGS`, and `.ACCIONES`.
  `.CARPETA` is always empty and all three flags are always `1`, so these fields
  are structurally established but weak initial editing targets.
- One CSC declares a `.RECURSOS/.SONIDOSID` member. This is evidence for resource
  validation, but not enough to infer universal declaration requirements.

The current `CutsceneTimeline` keeps one reference string and one numeric value.
That loses operands for `PLAY_SONIDOID` and does not preserve the complete tagged
operand structure. Its basic blocks are a useful presentation heuristic, not a
proven game control-flow graph.

### Remaining SCN records

The direct schemas already modeled for actors, navigation points/groups, dummies,
areas, lights, and effects are complete in this corpus. A fresh comparison found
zero unknown direct fields across 3,564 actors, 17,201 points, 1,714 groups, 5,295
dummies, 1,404 areas, 1,382 lights, and 2,055 effects. Work should focus on whole
unmodeled systems:

- `.MALLA_SCENE_OBJS`: 54 path-level records in 16 non-empty sections. Every
  record has string `.ID`, integer `.ANIMACION`, integer `.TIPO_OFFSET`, and real
  `.OFFSET`. All 54 animation IDs resolve uniquely in the package's `Anims.bdd`.
- `.BRIDGES`: 10 path-level instances, representing two value-distinct FR02
  definitions. Each has `.VISUALRWS`, `.PHYSICRWS`, and six control points with
  `.TYPE`, `P1`, `P2`, `.HEIGHT`, and target `.SCN`.
- Four `.MUNDOVIS` vector containers occur in every SCN but are skipped by the
  scalar-only environment projection: `.MTACTICO_ORIGEN`,
  `.MTACTICO_DESTINO`, `.ORIGEN_RTS`, and `.DESTINO_RTS` (84 vectors total).
- `.AGUAS`: 17 path-level water records in 11 non-empty scenes, each with the
  same 19 named material/oscillation/normal-map fields.
- Every SCN has `.MAPA_SECTORES`, `.PUNTUACION_MAXIMA`,
  `.PUNTUACION_MINIMA`, and player/start metadata. The sector path and score
  values vary meaningfully by mission.
- `.MULTIPLAYER` is structurally consistent but all mode toggles are zero in all
  21 files. It has little demonstrated editor value in this sprint.

### Animation-catalog records related to scripts

Across 16 distinct non-empty `Anims.bdd` contents there are 6,679 logical records,
8,126 file variants, 3,200 variants with `.SOUNDS`, and 4,837 timed sound events.
The current projection flattens sounds onto the logical animation, losing their
owning file variant. Every record also has `.NUM_ANIMS_PS2`, `.NUM_ANIMS_XBOX`,
and `.SONIDO_PC/PS2/XBOX`; platform-to-file ordering is not yet proven.

## Ranked scope

| rank | work item | value | effort | confidence | primary surfaces |
|---:|---|---|---|---|---|
| 1 | Shared typed GSC/CSC program and operand model | very high | medium | high structure, medium semantics | Script workspace, raw/source inspector, CLI |
| 2 | Scope-aware reference index and validation | very high | medium-high | high for tagged references | Mission Explorer, search/find uses, diagnostics |
| 3 | Script editor baseline with guarded scalar/reference edits | high | high due serializer gate | high for flags/replacements; low for structural edits | Script workspace, change list, staging |
| 4 | Conservative control-structure and event view | high | medium | medium | Script outline, flow view, CLI report |
| 5 | CSC complete operands and camera/sound sequence view | high | low-medium after item 1 | high shapes, medium control flow | Animation workspace, camera preview, CLI |
| 6 | SCN scene-object animation bindings | high | low-medium | high | Mission/Animation workspace, animation uses |
| 7 | Animation variants with per-file sounds | high | medium | high structure, uncertain platform order | Catalog, clip picker, sound timeline |
| 8 | SCN bridges and cross-scene transition overlay | medium-high | medium | high fields, uncertain runtime role | Mission graph, viewport, dependencies |
| 9 | Tactical-map vectors and source-backed mission metadata | medium | low | high values, medium transform semantics | Environment inspector, minimap, CLI |
| 10 | Water records as read-only material data | medium | low-medium | high fields, uncertain units/geometry | Environment/texture inspector |

Ranks 1 through 6 are the sprint core. Rank 7 is strongly related and should be
included if capacity permits. Ranks 8 through 10 are ordered follow-ups that use
the same typed-projection and source-navigation infrastructure.

## Program model

Build one lossless typed view shared by GSC and CSC. It remains a projection over
the immutable generic document and retains source identity and raw-node access at
every level.

```cpp
struct ScriptFlags {
    std::optional<bool> trigger;
    std::optional<bool> enabled;
    std::optional<bool> valid;
};

struct ProgramOperand {
    CsfSourceId source;
    std::string tag;
    std::variant<std::monostate, std::int32_t, float, std::string> value;
    std::vector<ProgramOperand> children;
    std::vector<RawField> unknown_fields;
};

struct ProgramEvent {
    CsfSourceId source;
    std::string name;
};

struct ProgramInstruction {
    CsfSourceId source;
    std::string opcode;
    std::vector<ProgramOperand> operands;
};

struct ProgramVariable {
    CsfSourceId source;
    std::int32_t id{};
    std::string type;
    std::string name;
    bool is_array{};
    ProgramOperand initial_value;
};

struct ProgramScript {
    CsfSourceId source;
    std::int32_t id{};
    std::string name;
    std::string folder;
    ScriptFlags flags;
    std::vector<ProgramVariable> local_variables;
    std::vector<ProgramEvent> events;
    std::vector<ProgramInstruction> conditions;
    std::vector<ProgramInstruction> actions;
};
```

Do not normalize tag or opcode spelling in the stored model. A separate lookup
key may be used for classification. Unknown and nested operands remain complete
and ordered; repeated tags such as CSC's two `NUMERO` operands are legal.

### Reference typing

Reference meaning should come first from an operand tag and expected value shape,
then from opcode context where necessary:

| tag | initial target |
|---|---|
| `BICHO` | SCN actor ID |
| `DUMMY` | SCN dummy ID |
| `ZONA` | SCN area ID |
| `PATHPOINT` | SCN `(group ID, point ID)` |
| `SCRIPT` | document-local script ID |
| `TRIGGER` | document-local trigger-enabled script ID |
| `EVENT` | named event declaration/use candidate |
| `ANM_BDD` | package-local animation record ID |
| `CLASSID` | object database record ID |
| `EFECTO_CLASSID` | effect database record ID |
| `ARMA_CLASSID` | weapon database record ID |
| `SONIDO_BDD` | sound database record ID |
| `ARRAYID` / `VAR` | scope-aware program variable |

Missing references remain inspectable. Sentinel values need to be classified from
repeated context before being reported as errors. Cross-document candidates must
show every source of evidence and never silently choose a target.

### Conservative control structure

Recognize balanced structural markers such as `IF`/`ELSE`/`ENDIF`,
`WHILE`/`WEND`, and `FOREACH`/`ENDFOR` for indentation, folding, and diagnostics.
Represent `WAIT_CONDICION`, `WAIT_EVENT`, and pauses as waits with unknown runtime
completion unless a literal duration is present.

Do not claim a complete CFG merely from delimiter names. `CONTINUE`, script and
trigger execution, event delivery, array expressions, and nested condition
operators need opcode-specific evidence before adding non-fallthrough edges.

## Script workspace and editor baseline

### Read-only baseline

Add a Script workspace shared by GSC and CSC with:

- an outline grouped by source `.CARPETA`, script, events, conditions, and actions;
- a variable panel showing scope, ID, declared type, array status, initial value,
  uses, and shadowing/ambiguity diagnostics;
- an instruction list with stable indentation, opcode/category filtering, and all
  positional operands;
- comments rendered as comments while retaining their source nodes;
- click-through from each typed operand to its SCN/BDD/program target and back;
- raw CSFFBS subtree and byte offset beside the typed presentation;
- search by script/event/variable name, opcode, operand tag/value, source entry,
  actor/dummy/area, and diagnostic;
- a reference summary for the currently selected Mission Explorer object;
- a conservative flow view that distinguishes lexical structure, proven target
  edges, candidates, waits, and unknown runtime behavior.

This workspace is useful before writing support exists and should be delivered
first.

### Guarded editing baseline

Only enable this section after the Phase 06 authoring gate passes.

Initial editable operations, in order:

1. Toggle `.ENABLED` on a script. Preserve `.TRIGGER` and `.VALIDO` unless the
   user makes a separately supported change.
2. Replace a typed reference operand through a compatible target picker.
3. Replace finite numeric, boolean, or string scalar operands without changing
   instruction structure.
4. Change a variable's initial scalar value when its declared type and source
   representation agree.
5. Optionally toggle `.TRIGGER` after validation confirms trigger references and
   event behavior remain internally consistent.

Every edit must show source, old/new typed value, raw representation, affected
use sites, and validation changes. Undo must restore the exact previous raw value.
Saving must use a new path or explicit staging root, reopen the result, rerun
structural and mission reference validation, and compare the intended edit set.

Do not include action insertion/deletion/reordering, free-form opcode entry,
variable creation, event creation, arbitrary string-table surgery, or graphical
node-programming in the baseline. Those require proven container/link rebuilding
and substantially more semantic validation.

## SCN additions

### Scene-object animations

Add a source-backed record for `.MALLA_SCENE_OBJS` with ID, animation ID, offset
type, and offset. Resolve the animation ID through the package catalog. Show the
record under both Mission and Animation workspaces and include reverse uses from
the animation record.

Treat `.TIPO_OFFSET` as a raw enum value until its meanings are independently
established. Do not apply `.OFFSET` to playback time automatically.

### Bridges

Model visual/collision RWS references and each control point's type, endpoints,
height, and target SCN. Add dependency edges immediately. Draw control geometry
only as an explicitly labelled bridge/transition overlay; do not claim trigger or
streaming semantics.

### Environment and metadata

Type the four tactical/RTS vectors currently skipped by `EnvironmentField` and
retain each source. Add a mission metadata record for sector-map path, scoring,
player selection, and start metadata. Raw endpoints may be displayed before any
derived minimap transform is enabled.

### Water

Project the stable 19-field water record schema and resolve its normal-map
texture. Initially expose it only in the inspector and CLI. Shader preview,
geometry association, ranges, units, and editing remain follow-up research.

## Animation-catalog refinement

Replace record-level flattened sound events with file-variant records containing
their own sound list. Preserve and expose the platform count/sound fields, but do
not assign file variants to PC/PS2/Xbox slots until ordering is validated.

Keep `MODEL3D_ITEM` and `MANO_ITEM` as distinct source fields. Compatibility
search may derive model/item/hand keys, but the stored projection must not collapse
them based on substring matching.

## CLI surfaces

Add or extend commands along these lines:

```text
csf-info program Mission.gsc --summary
csf-info program Mission.gsc --scripts
csf-info program Mission.gsc --script 4550
csf-info program Mission.gsc --references
csf-info program Mission.gsc --diagnostics
csf-info program Mission.gsc --json -
csf-info cutscene Mission.csc --references
csf-info mission Mission.scn --script-uses actor:170
csf-info mission Mission.scn --scene-objects
csf-info mission Mission.scn --bridges
```

If authoring is enabled later, use explicit verbs and destinations, for example:

```text
csf-info edit-script Mission.gsc --script 4550 --enabled false --out staged/Mission.gsc
```

The exact CLI spelling may follow existing conventions, but text and JSON must
preserve deterministic ordering and source identities. Inspection commands never
write; edit commands never overwrite the input or an existing destination.

## Work breakdown

### FS01-01: GSC/CSC corpus schema report

- Add a reproducible read-only report for program sections, record counts,
  opcodes, operand signatures, variable types, flags, resources, and duplicates.
- Record path-level and content-deduplicated totals.
- Preserve representative source entry identities without committing corpus data.

### FS01-02: shared program projection

- Implement scripts, flags, folders, variables, events, conditions, actions, and
  complete ordered operands.
- Preserve missing optional sections and all unknown/nested data.
- Add deterministic program JSON and focused CLI reports.

### FS01-03: reference resolver and validator

- Resolve scene, program, variable, resource, and database operand categories.
- Add scope-aware variable/array identity.
- Separate proven, candidate, missing, ambiguous, and sentinel/unclassified uses.
- Feed typed sites into the mission symbol and reverse-use indexes.

### FS01-04: Script workspace

- Add folder/script outline, variables, instructions, comments, search, filters,
  diagnostics, raw-source navigation, and bidirectional mission links.
- Add conservative structural folding and flow presentation.
- Keep large scripts virtualized and build indexes once per load.

### FS01-05: CSC specialization

- Type all eight observed operand signatures without discarding repeats.
- Resolve camera dummy and proven script references.
- Show FOV, filter, pause, wait, and sound actions without fabricated timestamps.
- Downgrade unresolved `CUTSCENE_EXE` targets to explicit candidates.

### FS01-06: scene-object animations

- Project `.MALLA_SCENE_OBJS` and resolve animation IDs.
- Add Mission/Animation workspace lists, reverse uses, CLI output, and diagnostics.
- Keep offset type/value raw until semantics are confirmed.

### FS01-07: guarded editor baseline

- Begin only after no-op serialization and transactional edit prerequisites pass.
- Add `.ENABLED`, typed reference, scalar operand, and compatible initial-value
  edits in the stated order.
- Add undo/redo, change list, validation diff, save-copy/staging, and reparse.
- Leave structural editing disabled.

### FS01-08: animation variants and remaining SCN records

- Attach sounds to animation file variants and expose raw platform metadata.
- Add bridges, tactical vectors, mission metadata, and read-only water records in
  ranked order as sprint capacity allows.

## Test plan

### Program projection tests

1. Script with all fields and stable source identities.
2. Script with no actions, events, conditions, or locals.
3. All four observed script-flag combinations.
4. Global and local variables of every observed value representation.
5. Array and scalar variables with the same numeric ID in different scopes.
6. Instruction with zero, one, repeated, nested, and unknown operands.
7. Repeated operand tags retain ordering.
8. Comments preserve exact display text and raw string bytes.
9. Unknown opcode remains inspectable and round-trippable.
10. Deterministic text/JSON export.

### Reference and structure tests

- actor, dummy, area, path-point-pair, script, trigger, event, animation, class,
  effect, weapon, sound, variable, and array references;
- missing, ambiguous, sentinel, wrong-category, and cross-document candidates;
- local variable shadowing and array scope;
- balanced and malformed `IF`/`ELSE`/`ENDIF`, `WHILE`/`WEND`, and
  `FOREACH`/`ENDFOR` sequences;
- runtime waits remain duration-unknown;
- camera dummy resolution and unresolved `CUTSCENE_EXE` candidates.

### SCN and animation tests

- scene-object animation reference success, missing, and ambiguity;
- raw offset type preservation;
- bridge references, six-point representative shape, and unresolved target SCN;
- tactical vectors distinguish missing from explicit zero;
- water unknown fields and texture-reference preservation;
- sound events remain attached to their file variants.

### Editor tests, after the authoring gate

- `.ENABLED` edit changes only the intended scalar and reparses identically
  elsewhere;
- typed reference picker rejects incompatible categories;
- numeric edits reject non-finite values;
- shared string-table edits use explicit copy-on-write or global replacement;
- undo/redo restores exact bytes and typed values;
- changed output reopens and passes structural/reference validation;
- failed validation cannot replace an existing staged output;
- input and corpus files are never overwritten.

### Corpus gates

- Project all 21 GSC, 21 CSC, and 21 SCN files without a fatal error.
- Account for every action and condition; unknown semantics are allowed, dropped
  operands are not.
- Reproduce the recorded counts or document corpus drift.
- Group all unresolved references by tag, value, file, and source entry.
- If editing is enabled, byte-compare no-op serialization for every structurally
  valid CSFFBS file before allowing any GUI save operation.

## Performance targets

- Open and index a typical GSC interactively without loading referenced payloads.
- Virtualize script/action lists; the corpus includes documents with tens of
  thousands of program nodes.
- Build program and reverse-reference indexes once per mission load.
- Filter/search without reparsing documents.
- Revalidate only affected scopes after an in-memory edit, with an explicit full
  mission validation before save.

## Risks and mitigations

| risk | mitigation |
|---|---|
| Opcode names are mistaken for complete VM semantics | Keep generic ordered operands authoritative; type only evidenced relationships |
| Lexical structure is presented as executable CFG | Distinguish folding/sequence, proven edges, candidates, and runtime waits |
| Sentinel IDs become false missing-reference errors | Classify repeated sentinel contexts and retain an unclassified state |
| Local variables resolve against the wrong script | Use document plus script scope and source identity, not numeric ID alone |
| Duplicate/case-varied folders or events are merged | Preserve spelling and source identity; normalize only lookup keys |
| A convenient editor permits unsafe structural changes | Gate writes on Phase 06 and limit the baseline to supported scalar/reference edits |
| CSC target IDs are forced into one namespace | Retain same-document, sibling-document, and unresolved candidates separately |
| Platform animation variants are assigned by guesswork | Expose raw counts/flags until ordering has independent evidence |
| Remaining SCN scope crowds out the script milestone | Treat ranks 1–6 as core; schedule ranks 7–10 only after the Script workspace is usable |

## Explicitly out of scope

- A complete GSC/CSC virtual machine or deterministic mission replay.
- Free-form source compilation or decompilation into a new language.
- Claiming exact runtime order across events, waits, triggers, and AI state.
- Automatic repair of unresolved references or sentinel values.
- Action/condition insertion, deletion, or reordering in the baseline editor.
- Editing an original corpus file in place.
- Assigning semantics to `raw_next_entry` before independent evidence exists.
- Multiplayer editing based only on invariant zero-valued samples.
- Applying scene-object offsets, bridge control points, or tactical-map vectors as
  runtime transforms before their conventions are validated.
- Water shader/geometry editing or platform animation-slot naming without further
  evidence.

## Definition of done

- All corpus GSC and CSC files project into source-backed program records without
  losing any operand, variable, flag, comment, or unknown node.
- The Script workspace can browse, search, filter, inspect, and navigate scripts,
  variables, events, conditions, actions, operands, and raw sources.
- High-confidence operand categories resolve bidirectionally to mission/program/
  database records with explicit missing and ambiguity diagnostics.
- Control presentation distinguishes lexical structure from proven execution
  edges and never fabricates absolute timing.
- CSC retains all operands, resolves every valid camera dummy reference, and
  represents unresolved cutscene targets as candidates.
- Scene-object animation records resolve to the animation catalog and appear in
  Mission and Animation workflows.
- CLI text and JSON reports are deterministic and include source identities.
- If the authoring gate is not complete, the sprint remains honestly read-only.
- If guarded editing is enabled, every supported edit has undo/redo, typed
  validation, exact intended-change comparison, reparse, and non-destructive
  save-copy/staging behavior.
- Existing Mission Explorer, Animation, RWS inspection, export, and tests remain
  functional.
