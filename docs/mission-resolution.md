# Mission package resolution

Phase 02 adds a read-only mission graph above the generic CSFFBS parser. The
graph is metadata: opening a mission does not keep every referenced animation,
texture, model, or RenderWare stream in memory and never changes the extracted
resource tree.

## Resolution contract

References preserve their original bytes-as-text spelling and also receive an
ASCII case-folded lookup key with `\\` converted to `/`. Absolute paths and any
reference containing a `..` component are rejected as outside the configured
roots.

Resolution proceeds in this order:

1. exact spelling under the package root;
2. a unique case-insensitive package match;
3. same-path `.dff` to `.rpc` mapping in the package;
4. configured shared roots in declaration order;
5. a unique logical-path candidate below known resource anchors such as `Maps`,
   `Models`, `Anims`, `Gfx`, or `Sounds`;
6. missing or ambiguous, retaining all candidates.

The default shared root is the selected package's parent resource directory.
Passing `--root` supplies an explicit root; a root may either be a direct resource tree
or the parent of package trees because logical-path indexing is separate from
resolved filesystem identity.

## Graph identity and evidence

Resolved nodes keep their actual path, original reference, normalized key, byte
size, inferred resource kind, load state, optional RenderWare root type, and an
optional FNV-1a content signature. Missing, rejected, and ambiguous references
also have nodes, so no dependency disappears merely because it cannot load.

Every edge records its source node, dependency kind, resolution rule, original
reference, candidate paths, and an evidence location containing source file,
byte offset, byte length, optional table/line index, and adapter name. Node IDs
and edge order are deterministic for the same filesystem snapshot. JSON graph
exports identify their schema as `csf-mission-graph-1`.

## Adapter evidence

- VIS begins with four unsigned 32-bit length-prefixed byte strings: visual map,
  collision map, texture directory, and sky model. Remaining bytes are retained.
- TXL is line-oriented. Empty lines are ignored; duplicate non-empty paths and
  their original line numbers remain separate edges.
- M3D records currently validate as a 32-bit length, path bytes, and four raw
  bytes. A final four-byte terminator/tail is retained rather than named.
- AND records currently validate as a 32-bit length, path bytes, and two raw
  bytes. A final four-byte terminator/tail is retained rather than named.
- PHD is not treated as a flat string list. The adapter finds only bounded
  length-prefixed values with known model/Physics extensions, records them as
  candidates, and retains the entire file as unknown schema data.
- SCN, GSC, CSC, and BDD contribute only string-table values that contain a path
  separator and a recognized resource extension. This is intentionally narrower
  than full typed record interpretation, which belongs to Phase 03.

## Reference-corpus validation

The 2026-09-19 read-only scan built graphs for all 21 SCNs without a fatal
failure. Across those graphs it recorded 29,572 exact resolutions, 725 unique
case-mismatched resolutions, 4,711 DFF-to-RPC mappings, 266 shared-root
resolutions, 803 ambiguous references, and 1,781 missing references. Missing is a
graph state rather than a parse failure; many source databases name audio or
optional assets absent from a particular extracted package.

With the explicit `--duplicates` option, the selected SCN is compared by size
and FNV-1a signature against the other SCNs under the corpus root. Keeping this
scan opt-in avoids recursively scanning and hashing the corpus during normal
interactive mission loads. Identical content remains represented by distinct
paths and package identities and is reported as `duplicate-content`; nothing is
renamed, merged, or deleted.

## CLI

`csf-info mission` supports `--summary`, `--dependencies`, `--missing`, and
`--graph <new-path>`, `--package-root <directory>`, and repeated
`--root <directory>` options. `--duplicates` enables the explicit duplicate-SCN
scan. `csf-info uses` builds one shared resource index for its reverse-use
report across SCNs, and `csf-info compare` compares stable
normalized edge sets and SCN content signatures. Graph output uses an adjacent
temporary file and a no-overwrite copy, so an existing destination is never
replaced.
