# CSFFBS tooling (Python)

A small, self-contained, **read-only** decoder for CSFFBS documents and a corpus
analysis of the `.scn` mission scenes. It is a companion to the C++ core in
`src/csf_document.cpp` and `src/csf_mission_scene.cpp` and to the prose grammar
in [`../csffbs-format.md`](../csffbs-format.md).

Nothing here mutates the game corpus. Files are opened for reading only, and the
typed projection is a view over the immutable generic tree.

## Files

| File | Purpose |
| --- | --- |
| `csffbs.py` | Decoder library and CLI |
| `analyze_scn.py` | Scans a corpus of `.scn` files and writes the report |
| `test_csffbs.py` | Synthetic self-test plus a corpus cross-check |
| `analysis.md` | Generated analysis of all 21 reference SCNs |

## Quick start

```bash
# Decode one document (summary by default)
python3 csffbs.py /path/to/Mission.scn

# Tree, search, and typed statistics
python3 csffbs.py /path/to/Mission.scn --tree
python3 csffbs.py /path/to/Mission.scn --find ".RUTA_"
python3 csffbs.py /path/to/Mission.scn --scene

# Export generic or typed JSON
python3 csffbs.py /path/to/Mission.scn --json out.json
python3 csffbs.py /path/to/Mission.scn --scene-json out.scene.json

# Analyze a whole corpus
python3 analyze_scn.py --root /path/to/CSF_unpacks --out analysis.md

# Self-tests
python3 test_csffbs.py
```

The decoder is generic: the same commands work on `.gsc`, `.csc`, `.bdd`,
`.sp`, `.fbs`, and CSFFBS `.txt` documents. Only the `--scene` projection is
SCN-specific.

## Design

The decoder mirrors the reference C++ parser exactly:

- **Header** (24 bytes): `CSFFBS` magic, reserved bytes, version, and the three
  table counts.
- **Entry table**: fixed 12-byte records (`raw_next_entry`, `raw_value_or_size`,
  signed identifier index, type).
- **Identifier and string tables**: `u32` length followed by bytes, decoded as
  Windows-1252 for display only.
- **Tree**: identifier entries label the following value and do not consume a
  parent element; groups/arrays declare their child count. The flat entry table
  stays authoritative if counts are malformed, and the stack is capped at 256.

Typed semantics live in `MissionScene.project`, which reads the Spanish field
labels (`.BICHOS`, `.MALLA_NAVEGACION`, `.MALLA_DUMMIES`, `.MALLA_AREAS`,
`.MALLA_LUCES`, `.EFECTOS`, `.MUNDOVIS`, `.PLAYER`, `.INICIO_*`). Every typed
field is optional: a missing field is distinguishable from a source value of
zero, and `raw_next_entry` is preserved but not interpreted.

## Validation

`test_csffbs.py` first decodes a synthetic document constructed byte by byte,
then, when the reference corpus is available, re-derives the aggregate totals
documented in `docs/the-great-shift/resource-report.md` and
`docs/mission-explorer.md`:

| quantity | value |
| --- | ---: |
| actors | 3,564 |
| navigation groups | 1,714 |
| points | 17,201 |
| valid connections | 8,070 |
| dummies | 5,295 |
| areas | 1,404 |
| lights | 1,382 |
| total `.scn` bytes | 7,163,617 |

All match the C++ implementation. See `analysis.md` for the full report.

## Scope and limitations

- The meaning of the per-entry `raw_next_entry` link field is not confirmed.
- Some top-level SCN systems (sector map, minimap, multiplayer, save-game
  configuration) are decoded into the generic tree but are not part of the
  typed projection yet.
- Data here describes the local reference corpus only; it is not a format
  specification.
