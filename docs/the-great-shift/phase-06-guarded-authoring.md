# Phase 06: guarded authoring and lossless serialization

## Outcome

Permit schema-aware edits only for structures that can be serialized, reopened,
validated, and explained. Begin with high-value scalar/reference changes in
CSFFBS databases and selected mission records, then expand to spatial edits and
scripts. All output goes to new files or a staging tree; original resources remain
untouched.

This phase converts the workbench from read-only analysis into a mod-authoring
tool without abandoning the project's preservation discipline.

## Dependencies

- Phase 01 raw and semantic CSFFBS model with complete source identities.
- Phase 02 dependency graph and validators.
- Typed views from Phases 03–05 for whichever records become editable.
- Existing save-copy precedent in `rws-man`.

## Entry gate

Do not begin general CSFFBS editing until a serializer can reproduce all
structurally valid corpus documents byte-for-byte when no changes are made, or can
document a narrowly justified canonical difference. Byte-identical no-op output is
the preferred and initial requirement.

Each editable schema additionally requires:

1. at least two independent agreeing samples or stronger executable/runtime
   evidence;
2. source offset/type documentation;
3. value and reference validation rules;
4. serialize/reparse tests;
5. preservation behavior for unknown siblings/tails;
6. a clear user-facing description of the change.

## In scope

- Lossless CSFFBS serialization from preserved raw tables/tree.
- Rebuilding entry links/counts/table indices where an edit requires it.
- Byte-identical no-op round trips across the corpus.
- Transactional edit commands with undo/redo.
- Dirty-state and change-list UI.
- Field-level validation before commit to the in-memory document.
- Save copy and save into an explicit staging root.
- Reopen-and-validate before reporting a save successful.
- Structural and semantic diff views.
- Initial editing candidates:
  - BDD weapon/object/material/effect scalar values;
  - supported string/path references;
  - actor position/angle and selected flags;
  - navigation point position after graph validation;
  - supported player/mission properties;
  - script enable flags and selected well-understood operands.
- Reference-aware rename/change operations where all use sites are known.
- Preservation of unedited unknown fields, strings, ordering, and trailing data.
- Export of a machine-readable change manifest.

## Explicitly out of scope

- Editing raw corpus files in place.
- Enabling a field merely because its numeric type is known.
- Arbitrary tree surgery before insertion/deletion serialization is proven.
- Free-form script source compilation in the first authoring slice.
- ANM, RPC mesh, Skin, ragdoll, WAD, or SEC authoring.
- Automatic repair of unresolved references without user choice.
- Claiming that a structurally valid file is guaranteed to be accepted by the
  game; runtime testing remains separate.

## Serializer strategy

### Preserve before canonicalize

For no-op and scalar edits, reuse original bytes for unchanged records and strings
where practical. Avoid re-encoding the whole document merely to change one float.

Support progressively harder edit classes:

1. fixed-width scalar replacement;
2. same-length string replacement;
3. string-table replacement with offset/length rebuild;
4. table entry append/reindex;
5. container insertion/deletion and link/count rebuild.

Do not expose a harder edit class until its serializer and tests are complete.

### Output transaction

```text
in-memory edited document
  → serialize to temporary file in destination directory
  → reopen with csf_core
  → structural validation
  → semantic/reference validation
  → compare intended edit set
  → atomically publish new destination path
```

If any step fails, retain the in-memory edits and diagnostics but do not replace a
previous valid staged output.

### Undo/redo

Represent edits as commands against stable source/semantic identities:

```cpp
struct EditCommand {
    EditTarget target;
    TypedValue before;
    TypedValue after;
    ValidationSnapshot validation;
};
```

Structural operations need identity remapping; fixed-width edits can initially use
entry/table identity directly. Undo must restore exact prior raw value/string data,
not a display-normalized version.

## Validation layers

1. **Field validation:** type, finite values, ranges, enums, bit masks.
2. **Record validation:** required fields, uniqueness, local invariants.
3. **Document validation:** container/table integrity and typed section rules.
4. **Mission validation:** references, graph connectivity, resource kind.
5. **Output validation:** reopen, compare intended changes, preserve unknown data.
6. **Runtime validation:** external/manual game test; never implied by the first
   five layers.

The UI should distinguish errors that block serialization from warnings that may
still be intentional mod behavior.

## Initial authoring slices

### Slice A: database scalar editor

Start with well-labelled, bounded values such as selected weapon damage, force,
dispersion, cadence, inventory counts, object health, mass/damping metadata,
material flags/colors, and effect timing values.

Each field needs units/meaning/confidence documentation. Avoid presenting broad
numeric grids without context.

### Slice B: reference replacement

Allow selection from compatible resolved resources rather than free-typing where
possible. Show every affected use and expected resource kind. Preserve original
slash/case convention unless the user explicitly normalizes it.

### Slice C: spatial mission edits

Add gizmos and numeric entry for actor/player/nav-point positions and established
angles. Preview changes against the map while keeping source and staged states
visually distinguishable.

Changing a navigation point must rerun connection/group validation. Moving an
actor does not imply changing scripts, routes, cells, or sectors automatically.

### Slice D: constrained script edits

Initially support only high-confidence operations, such as enabling/disabling a
script or replacing a typed actor/path/zone/animation reference. Generic tree
editing may follow, but a visual full-language authoring environment is not a
prerequisite.

## Work breakdown

### P06-01: no-op serializer

- Serialize header, flat entries, identifiers, strings, and trailing bytes.
- Achieve byte-identical output for valid unmodified synthetic and corpus files.
- Document any exceptional non-identical case before proceeding.

### P06-02: scalar and string edits

- Add fixed-width scalar mutation through typed views.
- Add safe string-table updates and index preservation/rebuild.
- Reopen and compare intended changes.

### P06-03: edit transactions and history

- Add command-based undo/redo and dirty state.
- Add change list with source, old/new value, validation, and user-visible meaning.
- Make document/mission switching prompt or retain staged state predictably.

### P06-04: database authoring

- Expose a small documented field set from Armas/Objetos/Materiales/Efectos BDD.
- Add per-field constraints and compatible-reference pickers.
- Add semantic diff/report output.

### P06-05: spatial authoring

- Add actor/player/nav-point transform editing.
- Rebuild only required CSFFBS values/tables.
- Rerun mission spatial/reference validation and show before/after overlays.

### P06-06: constrained script authoring

- Add supported typed operand changes and enable flags.
- Update proven cross-references transactionally.
- Preserve unsupported actions and branches byte-for-byte.

### P06-07: staging save and manifest

- Save under an explicit external staging root while preserving relative paths.
- Reopen and validate every output.
- Emit a manifest of source identity, output path, hashes, changed fields, and
  validation results without copying unchanged copyrighted assets by default.

## Test plan

### Serializer tests

1. Byte-identical empty/minimal document round trip.
2. Byte-identical nested document with duplicate tables and unknown header bytes.
3. Byte-identical raw non-ASCII string bytes.
4. Fixed-width integer/float edit changes only expected bytes.
5. Same-length and different-length string replacement.
6. Shared string-table entry edit versus copy-on-write behavior is explicit.
7. Added string entry and updated reference.
8. Container count/link rebuild when structural editing is enabled.
9. Trailing and unsupported data preserved.
10. Serialize/reopen failure leaves previous destination untouched.
11. Undo/redo restores exact bytes and typed values.
12. Change manifest is deterministic and complete.

### Semantic edit tests

- range and enum rejection;
- finite-coordinate enforcement;
- compatible/incompatible resource replacement;
- actor transform edit without unintended sibling changes;
- navigation edit reruns connectivity validation;
- reference rename updates the declared supported use set only;
- unsupported fields remain read-only and byte-identical.

### Corpus gates

- No-op serialize every structurally valid CSFFBS file to a temporary directory
  outside the repository and compare hashes.
- Never write beside the resource corpus.
- Group and investigate all differences before enabling general save.
- Reopen every staged result and run document plus mission validation.

## Performance targets

- Scalar edits and undo/redo are immediate.
- Serialization is linear in document size and does not require mission-wide
  resource payloads to be resident.
- Validation reruns incrementally where dependencies are known, with an explicit
  full-validation command before staging.
- Temporary output uses the destination filesystem so final publication can be
  atomic where supported.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Generic parse tree loses serializer-only details | Keep raw flat records/tables authoritative from Phase 01 |
| Shared string edits unintentionally affect other records | Show use count and use copy-on-write unless global replacement is explicit |
| Structurally valid mod crashes game | Separate structural, semantic, and runtime validation claims |
| Unknown data changes during canonical rewrite | Require byte-identical no-op round trip and preserve raw regions |
| Undo targets shift after structural edits | Gate structural editing and maintain explicit identity remapping |
| User mistakes staging for source overwrite | Display both roots and disallow implicit input-path output |

## Definition of done

- Unmodified valid CSFFBS documents serialize byte-identically across the corpus,
  or every documented exception has a justified preservation strategy.
- Supported scalar and reference edits serialize, reopen, and validate.
- Undo/redo restores exact prior document state.
- Unsupported/unknown fields remain read-only and preserved.
- Database and selected spatial/script fields expose contextual validation rather
  than unrestricted raw numeric editing.
- Save copy and staging output never overwrite source resources.
- Each output has a deterministic change/validation manifest.
- Failed serialization or validation cannot replace a previous valid staged file.
- All automated tests and prior read-only workflows continue to pass.

