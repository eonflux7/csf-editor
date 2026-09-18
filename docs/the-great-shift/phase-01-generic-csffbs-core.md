# Phase 01: generic CSFFBS core

## Outcome

Add a safe, lossless, read-only-first parser for the generic CSFFBS document
container and expose it through a new command-line inspection tool. At the end of
this phase, every CSFFBS file in the reference corpus can be opened as a typed tree
or rejected with a precise bounded diagnostic; no input file is renamed,
overwritten, or converted in place.

This phase deliberately implements the container before assigning gameplay
semantics to SCN, GSC, CSC, or BDD records.

## Why this is first

One generic format underlies at least 1,262 supplied resources:

- 21 `.scn` scene documents;
- 21 `.gsc` mission programs;
- 21 `.csc` cutscene programs;
- 108 `.bdd` databases;
- 947 `.sp` particle definitions;
- 126 `.fbs` UI documents;
- 18 `.txt` icon documents.

A trustworthy container parser therefore unlocks more of the game than any one
additional RWS chunk decoder. It also establishes the source identities and
round-trip discipline required for later editing.

## Dependencies

- Existing C++20/CMake project and test infrastructure.
- No dependency on the GUI, OpenGL, ImGui, or RenderWare parsing.
- Public decoder source may be used as format evidence, subject to its license,
  but production code should follow this project's bounds, diagnostics, and
  ownership conventions.

## In scope

- Content sniffing for the `CSFFBS` magic independent of extension.
- Bounds-checked parsing of header counts and all tables.
- Fixed-size entry records and six observed entry types.
- Identifier and string tables with original byte encoding retained.
- Reconstruction of group/array nesting from element counts.
- Optional identifier references on groups and arrays.
- Stable source identities: file, entry index, byte offset, table index.
- Preservation of entry order, duplicate identifiers/strings, empty strings,
  unknown header values, and trailing data.
- Structured diagnostics with severity, offset, and affected record.
- A generic immutable document model in a new CSF-focused core layer.
- Text and JSON export to explicit new paths or standard output.
- A `csf-info` CLI for one-file inspection and validation.
- A content-sniffing `csf-corpus` mode or equivalent CLI command.
- Synthetic parser tests and read-only corpus validation.

## Explicitly out of scope

- Typed SCN actors, navigation, or routes.
- GSC/CSC opcode semantics or execution.
- Typed BDD records.
- Editing and serialization of modified trees.
- In-place conversion to text.
- Automatic “repair” of malformed input.
- GUI integration beyond an optional developer tree view after the core is stable.
- Parsing non-CSFFBS files merely because they share an extension with one.

## Proposed core model

Names are illustrative; implementation may adapt them to project conventions.

```cpp
namespace csf {

enum class ValueKind : std::uint16_t {
    identifier = 0,
    group = 1,
    array = 2,
    integer = 3,
    real = 4,
    string = 5,
};

struct SourceRange {
    std::uint64_t offset{};
    std::uint64_t size{};
};

struct Entry {
    ValueKind kind{};
    std::uint32_t raw_next_entry{};
    std::uint32_t raw_value_or_size{};
    std::int16_t raw_identifier_index{};
    std::uint16_t raw_type{};
    std::uint32_t entry_index{};
    SourceRange source{};
};

struct Node {
    Entry entry;
    std::optional<std::uint32_t> identifier_index;
    std::variant<std::monostate, std::int32_t, float,
                 std::uint32_t> scalar;
    std::vector<Node> children;
};

struct Document {
    Header header;
    std::vector<Entry> entries;
    std::vector<RawString> identifiers;
    std::vector<RawString> strings;
    std::vector<Node> roots;
    std::vector<Diagnostic> diagnostics;
    std::vector<std::byte> trailing_bytes;
};

}
```

The raw flat tables are authoritative. The nested tree is a validated view over
them, not a replacement. This avoids losing information if a malformed element
count prevents complete tree reconstruction.

## Parsing rules

### Header

Validate before allocating:

- at least 24 bytes are available for the observed header;
- the first six bytes equal `CSFFBS`;
- all computed table offsets use checked arithmetic;
- entry count times 12 fits the file;
- identifier and string counts are bounded by remaining bytes and a sane
  implementation limit;
- unknown/reserved header bytes are retained verbatim.

Never allocate directly from an untrusted count without checking the maximum
possible records in the remaining file.

### Entry table

Each observed entry is 12 bytes:

```text
u32 next-entry/raw link
u32 value or container size
i16 identifier index or -1
u16 type
```

Retain all raw fields even where the current semantic view does not use them.
Reject or preserve an unknown type according to whether its fixed record can still
be bounded; do not reinterpret it as a known scalar.

### String tables

Observed strings use a 32-bit byte length including a terminating null. Empty
string values may use zero length. The parser must:

- validate length before reading;
- preserve the raw bytes and terminator state;
- expose a display conversion separately;
- diagnose embedded nulls or a missing final null without discarding bytes;
- avoid assuming UTF-8; the corpus contains Western European byte strings.

Use a deterministic display policy such as Windows-1252 for the current game
corpus while keeping raw bytes authoritative.

### Tree reconstruction

Container sizes express a number of contained values according to the observed
grammar. Reconstruct with an explicit stack, not recursion controlled solely by
input, and cap nesting depth for diagnostic safety.

Validate:

- identifier/string table references;
- container sizes against remaining entries;
- stack underflow/overflow;
- unclosed or over-closed containers;
- scalar placement after identifiers;
- entry-link values as preserved evidence, even before their semantics are used.

A malformed suffix must not erase an already validated prefix. Return a partial
tree plus diagnostics when source boundaries remain known.

## CLI design

Suggested commands:

```powershell
csf-info file.scn --summary
csf-info file.scn --tree
csf-info file.gsc --validate
csf-info file.bdd --strings
csf-info file.scn --export-text output.scn.txt
csf-info file.scn --export-json output.scn.json
csf-info file.scn --find RUTA_Garita
csf-info corpus E:\dev\re-csf\CSF_unpacks
```

Required behavior:

- input extension does not determine the parser;
- export never uses the input path unless a future explicit overwrite flag is
  separately designed and confirmed;
- standard output is usable in scripts;
- diagnostics go to standard error or structured JSON as appropriate;
- validation returns a nonzero status for structural errors;
- summary distinguishes exact, partial, unsupported, and non-CSFFBS inputs.

The text exporter should use stable indentation and escaping, but it is initially
an inspection format rather than guaranteed recompilable source.

## Work breakdown

### P01-01: byte reader and header

- Add checked little-endian reads and overflow-safe range helpers to `csf_core`.
- Parse and retain the complete observed header.
- Add magic sniffing independent of extension.

### P01-02: flat tables

- Parse the 12-byte entry table.
- Parse raw identifier and string tables.
- Add table-reference accessors that return diagnostics rather than unchecked
  indexing.

### P01-03: tree reconstruction

- Build validated group/array nesting.
- Preserve partial results on malformed suffixes.
- Add stable entry-index and source-offset identity.

### P01-04: exporters

- Add canonical text output with escaped byte strings.
- Add JSON including raw indices/offsets and semantic values.
- Make all exports explicit and non-destructive.

### P01-05: CLI and corpus scan

- Add `csf-info` without GUI dependencies.
- Add summary, tree, strings, find, validate, and export modes.
- Add a recursive corpus report grouped by magic, extension, version/header,
  entry type, identifier, diagnostic, and file.

### P01-06: documentation and regression

- Document the confirmed grammar with offsets and uncertainty.
- Add README commands only after the target exists.
- Run core-only and full build/test workflows.

## Test plan

### Synthetic parser tests

Create byte builders inside the existing test infrastructure or a CSF-specific
test target. Cover at minimum:

1. Empty valid document.
2. One integer, float, and string value.
3. Nested named and anonymous groups/arrays.
4. Empty string encoded with zero length.
5. Duplicate identifier and string table entries.
6. Western European non-ASCII bytes preserved exactly.
7. Missing string terminator.
8. Truncated header, entry, length, and string payload.
9. Count multiplication/addition overflow.
10. Invalid identifier and string indices.
11. Unknown entry type.
12. Excessive nesting depth.
13. Container size underflow and overflow.
14. Unclosed containers with a usable prefix.
15. Trailing bytes preserved and reported.
16. Text/JSON output determinism.
17. Parsing and inspection leave input bytes unchanged.

### Corpus checks

Against the read-only local corpus:

- sniff exactly the expected CSFFBS population or explain any updated count;
- parse all structurally valid documents without a fatal process failure;
- group failures by reproducible offset and reason;
- confirm all observed entry types are accounted for;
- compare identifier/string counts and selected text output with the public
  decoder on copies outside the repository, never on source resources;
- record new format evidence in the resource report or a dedicated schema note.

### Build commands

```powershell
.\build.ps1 -CoreOnly
.\test.ps1 -CoreOnly
.\build.ps1 -Target rws-man
.\test.ps1
.\build.ps1 -Config Debug
.\test.ps1 -Config Debug
```

## Performance targets

- Parse a typical sub-megabyte SCN/GSC in effectively interactive time.
- Avoid copying the entire byte buffer for every node or displayed string.
- Bound all allocations from file counts before allocation.
- Corpus scanning should parse files once and stream report aggregation.
- Tree rendering, when added, must virtualize large documents rather than format
  every node on every frame.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Container size semantics differ in an unobserved variant | Keep the flat entry table authoritative and return a partial tree |
| Non-ASCII strings are mis-decoded | Preserve raw bytes; make display encoding an explicit policy |
| Huge counts cause allocation or arithmetic failure | Check against remaining bytes and implementation limits first |
| Text export is mistaken for round-trip source | Label it inspection output until Phase 06 serializer guarantees exist |
| Existing public decoder behavior is copied blindly | Validate every rule against local samples and record discrepancies |
| Extension-based dispatch misses CSFFBS `.txt` | Sniff magic before extension |

## Definition of done

- A reusable `csf_core` target parses the generic CSFFBS grammar without GUI
  dependencies.
- Every returned node has stable source identity and bounded raw backing data.
- Malformed data produces offset-bearing diagnostics and never an unchecked read.
- `csf-info` can summarize, validate, search, and export text/JSON.
- No operation renames or overwrites an input resource.
- A recursive scan covers all CSFFBS documents by magic, including `.txt`.
- Synthetic parser tests cover valid, malformed, partial, encoding, and overflow
  cases.
- Release and Debug tests pass and existing RWS behavior is unchanged.

