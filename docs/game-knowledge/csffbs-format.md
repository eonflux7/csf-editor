# CSFFBS container format

This note records the generic, read-only container grammar currently implemented
by `csf_core`. It deliberately does not assign SCN, GSC, CSC, BDD, SP, or FBS
gameplay semantics.

All integers are little-endian. Offsets below are absolute from the start of the
file.

## Header

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 6 | ASCII magic `CSFFBS` |
| `0x06` | 2 | Reserved/version bytes, observed as `00 7f` |
| `0x08` | 4 | Version, observed as `1` |
| `0x0c` | 4 | Entry count |
| `0x10` | 4 | Identifier count |
| `0x14` | 4 | String count |

The reserved bytes and version are retained even when they do not match the
currently observed values. Counts are checked against the remaining file size and
an implementation safety limit before allocation.

## Entry table

The entry table begins at `0x18` and contains fixed 12-byte records:

| Record offset | Size | Meaning |
| ---: | ---: | --- |
| `0x00` | 4 | Entry index of the element's next sibling, 0 for the last child (see below) |
| `0x04` | 4 | Scalar bits, string-table index, or container element count |
| `0x08` | 2 | Signed identifier-table index; containers may use `-1` |
| `0x0a` | 2 | Entry type |

Observed entry types are identifier `0`, group `1`, array `2`, signed integer
`3`, IEEE-754 binary32 `4`, and string-table reference `5`. Unknown types remain
in the authoritative flat entry table and produce a bounded diagnostic.

An identifier entry labels the following scalar entry and does not itself consume
a container element. A group or array is one element of its parent and its value
field declares the number of semantic child values. Containers can instead carry
their own identifier-table index. `csf_core` reconstructs this view with an
explicit stack capped at 256 levels; the flat table remains authoritative if the
counts are malformed.

## Identifier and string tables

The identifier table immediately follows the entry table, followed by the string
table. Each record starts with a `u32` byte length. A nonzero length normally
includes a final null byte. String-table entries may encode an empty value with a
zero length.

The parser retains the complete record bytes, declared length, terminator state,
embedded-null state, table index, and source range. Display conversion uses
Windows-1252 because that matches the studied PC corpus; it never replaces the raw
bytes.

## Validation evidence and uncertainty

The grammar was cross-checked against the public CSFFBS decoder and the local,
read-only reference corpus. The current parser identifies 1,262 files by magic
and parses all of them without structural errors: 108 BDD, 21 CSC, 126 FBS, 21
GSC, 21 SCN, 947 SP, and 18 TXT documents.

The same scan observes only the six known entry types:

| Type | Meaning | Entries |
| ---: | --- | ---: |
| 0 | Identifier | 603,447 |
| 1 | Group | 431,413 |
| 2 | Array | 128,286 |
| 3 | Integer | 450,595 |
| 4 | Float | 334,639 |
| 5 | String reference | 523,568 |

All 1,262 documents use reserved bytes `00 7f` and version `1`. These are corpus
observations rather than parser requirements.

The next-entry field links siblings: each element's first entry (the identifier
entry of a labelled scalar, whose value field is `0xFFFFFFFF`) stores the entry
index where the next sibling starts, or 0 for the last child, and the value entry
of a labelled scalar stores 0. This holds for all 1,867,239 container elements of
the 1,262 shipped files. With identifier and string tables interned in first-use
order, the canonical writer `csf::Tree` rebuilds every shipped file byte-for-byte
from its tree, which is what makes structural edits possible (see
[mission-editor.md](../guides/mission-editor.md)). Reserved header values and trailing bytes
are preserved rather than normalized. The `--export-text` and JSON exports are
deterministic inspection formats; `csf-mod decompile` / `compile` use the
recompilable source text instead. File export is restricted to new paths and
never overwrites its input.
