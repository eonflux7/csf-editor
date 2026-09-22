#!/usr/bin/env python3
"""Self-tests for the CSFFBS decoder.

Two independent checks:

1. A synthetic CSFFBS document is built byte by byte and decoded, exercising
   the header, entry table, identifier table and string table without needing
   any game data.
2. If the reference corpus is present, all documented aggregate totals are
   re-derived and compared.  These are the same numbers the C++ implementation
   produces.

Run with ``python3 test_csffbs.py``.
"""

from __future__ import annotations

import struct
import sys
from collections import Counter
from pathlib import Path

import csffbs


def build_entry(raw_next: int, value_or_size: int, identifier: int, kind: int) -> bytes:
    return struct.pack("<IIhH", raw_next, value_or_size, identifier, kind)


def build_synthetic() -> bytes:
    identifiers = [b".VERSION\x00", b".NAME\x00"]
    strings = [b"Test\x00"]
    entries = [
        build_entry(0, 0, 0, 0),   # identifier -> .VERSION
        build_entry(0, 17, -1, 3),  # integer 17
        build_entry(0, 0, 1, 0),   # identifier -> .NAME
        build_entry(0, 0, -1, 5),  # string index 0
    ]
    header = struct.pack(
        "<6s2sIIII", b"CSFFBS", b"\x00\x7f", 1, len(entries), len(identifiers), len(strings)
    )
    body = b"".join(entries)
    for value in identifiers + strings:
        body += struct.pack("<I", len(value)) + value
    return header + body


def test_synthetic() -> None:
    document = csffbs.Document(build_synthetic())
    assert document.state == "exact", document.state
    assert not document.diagnostics, document.diagnostics
    assert document.version == 1
    assert document.entry_count == 4
    assert len(document.roots) == 2, len(document.roots)

    version, name = document.roots
    assert document.label(version) == ".VERSION"
    assert version.scalar_kind == "int" and version.scalar == 17
    assert document.label(name) == ".NAME"
    assert name.scalar_kind == "string"
    assert document.string(name.scalar).display() == "Test"

    payload = csffbs.document_to_json(document)
    assert payload["state"] == "exact"
    assert payload["tree"][0]["label"] == ".VERSION"
    print("synthetic document: ok")


def test_non_csffbs() -> None:
    document = csffbs.Document(b"not a csffbs file")
    assert document.state == "non_csffbs"
    print("non-CSFFBS rejection: ok")


def test_corpus(root: Path) -> None:
    files = sorted(p for p in root.rglob("*") if p.suffix.lower() == ".scn")
    if not files:
        print(f"corpus not present at {root}; skipping")
        return
    totals: Counter = Counter()
    total_bytes = 0
    for path in files:
        document = csffbs.Document.load(path)
        assert document.state == "exact", (path, document.state)
        assert not document.diagnostics, (path, document.diagnostics)
        scene = csffbs.MissionScene.project(document)
        stats = scene.navigation_stats()
        totals["actors"] += len(scene.actors)
        totals["navigation_groups"] += stats["groups"]
        totals["points"] += stats["points"]
        totals["valid_connections"] += stats["valid_connections"]
        totals["dummies"] += len(scene.dummies)
        totals["areas"] += len(scene.areas)
        totals["lights"] += len(scene.lights)
        total_bytes += path.stat().st_size

    expected = {
        "actors": 3564,
        "navigation_groups": 1714,
        "points": 17201,
        "valid_connections": 8070,
        "dummies": 5295,
        "areas": 1404,
        "lights": 1382,
    }
    for key, value in expected.items():
        assert totals[key] == value, (key, totals[key], value)
    assert total_bytes == 7_163_617, total_bytes
    print(f"corpus totals ({len(files)} files, {total_bytes:,} bytes): ok")


def main() -> int:
    test_synthetic()
    test_non_csffbs()
    test_corpus(Path("/mnt/e/dev/re-csf/CSF_unpacks"))
    print("all tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
