#!/usr/bin/env python3
"""Read-only decoder for CSFFBS documents.

CSFFBS is the generic serialized-document container used by Commandos: Strike
Force for mission scenes (``.scn``), mission scripts (``.gsc``/``.csc``),
gameplay databases (``.bdd``), particle systems (``.sp``), UI forms (``.fbs``)
and some text tables.  This module implements the container grammar and a typed
mission-scene projection.  It mirrors the reference C++ implementation in
``src/csf_document.cpp`` and ``src/csf_mission_scene.cpp`` and the prose grammar
in ``docs/game-knowledge/csffbs-format.md``.

Properties:

* standard library only;
* never mutates or rewrites its input;
* preserves unknown entry types, unknown fields and trailing bytes;
* keeps the flat entry table authoritative even when container counts are
  malformed.

Command line::

    python3 csffbs.py FILE [--summary] [--tree] [--find NAME] [--json OUT]
                            [--scene] [--scene-json OUT]
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import struct
import sys
from dataclasses import dataclass, field
from enum import IntEnum
from pathlib import Path
from typing import Iterator, Optional


MAGIC = b"CSFFBS"
HEADER_SIZE = 24
ENTRY_SIZE = 12
MAX_NESTING_DEPTH = 256
MAX_TABLE_RECORDS = 16 * 1024 * 1024


class ValueKind(IntEnum):
    IDENTIFIER = 0
    GROUP = 1
    ARRAY = 2
    INTEGER = 3
    REAL = 4
    STRING = 5


KIND_NAMES = {k.value: k.name.lower() for k in ValueKind}


# C1 control bytes 0x80..0x9F as decoded by Windows-1252.  Bytes that are
# undefined in the WHATWG cp1252 table map to their C1 codepoint, matching the
# reference C++ decoder rather than Python's stricter ``cp1252`` codec.
_W1252_CONTROLS = (
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
)


def _w1252_codepoint(value: int) -> int:
    if 0x80 <= value <= 0x9F:
        return _W1252_CONTROLS[value - 0x80]
    return value


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _i32_from_u32(value: int) -> int:
    return value - 0x1_0000_0000 if value >= 0x8000_0000 else value


def _f32_from_u32(value: int) -> float:
    return struct.unpack("<f", struct.pack("<I", value))[0]


def _range_fits(offset: int, size: int, total: int) -> bool:
    return offset <= total and size <= total - offset


@dataclass
class RawString:
    table_index: int
    declared_length: int
    offset: int
    size: int = 0
    data: bytes = b""
    complete: bool = True
    has_final_null: bool = False
    has_embedded_null: bool = False

    def display(self) -> str:
        content = self.data[:-1] if (self.has_final_null and self.data) else self.data
        return "".join(chr(_w1252_codepoint(b)) for b in content)


@dataclass
class Diagnostic:
    severity: str  # "warning" | "error"
    offset: int
    entry_index: Optional[int]
    message: str


@dataclass
class Entry:
    index: int
    raw_next_entry: int
    raw_value_or_size: int
    identifier_index: int
    raw_type: int
    offset: int
    size: int = ENTRY_SIZE

    def kind(self) -> Optional[ValueKind]:
        if self.raw_type <= int(ValueKind.STRING):
            return ValueKind(self.raw_type)
        return None


@dataclass
class Node:
    entry_index: int
    label_entry_index: Optional[int] = None
    identifier_index: Optional[int] = None
    # scalar_kind is one of None, "int", "real", "string"; scalar is the value.
    scalar_kind: Optional[str] = None
    scalar: object = None
    children: list["Node"] = field(default_factory=list)

    def set_int(self, value: int) -> None:
        self.scalar_kind, self.scalar = "int", value

    def set_real(self, value: float) -> None:
        self.scalar_kind, self.scalar = "real", value

    def set_string(self, index: int) -> None:
        self.scalar_kind, self.scalar = "string", index


class Document:
    """Parsed, immutable-by-convention CSFFBS document."""

    def __init__(self, data: bytes, source: Optional[Path] = None) -> None:
        self.bytes = data
        self.source_path = source
        self.magic = b""
        self.reserved = b""
        self.version = 0
        self.entry_count = 0
        self.identifier_count = 0
        self.string_count = 0
        self.entries: list[Entry] = []
        self.identifiers: list[RawString] = []
        self.strings: list[RawString] = []
        self.roots: list[Node] = []
        self.diagnostics: list[Diagnostic] = []
        self.trailing_offset = 0
        self.trailing_size = 0
        self.state = "partial"
        self._structural_error = False
        self._unsupported_entry = False
        self.parse()

    # -- loading ---------------------------------------------------------
    @classmethod
    def load(cls, path: Path | str) -> "Document":
        path = Path(path)
        return cls(path.read_bytes(), path)

    @staticmethod
    def sniff(data: bytes) -> bool:
        return data.startswith(MAGIC)

    # -- diagnostics -----------------------------------------------------
    def _error(self, offset: int, message: str, entry_index: Optional[int] = None) -> None:
        self.diagnostics.append(Diagnostic("error", offset, entry_index, message))
        self._structural_error = True
        if self.state != "non_csffbs":
            self.state = "partial"

    def _warning(self, offset: int, message: str, entry_index: Optional[int] = None) -> None:
        self.diagnostics.append(Diagnostic("warning", offset, entry_index, message))

    @property
    def has_errors(self) -> bool:
        return any(d.severity == "error" for d in self.diagnostics)

    def identifier(self, index: int) -> Optional[RawString]:
        return self.identifiers[index] if 0 <= index < len(self.identifiers) else None

    def string(self, index: int) -> Optional[RawString]:
        return self.strings[index] if 0 <= index < len(self.strings) else None

    def label(self, node: Node) -> str:
        if node.identifier_index is None:
            return ""
        value = self.identifier(node.identifier_index)
        return value.display() if value else ""

    # -- parsing ---------------------------------------------------------
    def parse(self) -> None:
        data = self.bytes
        total = len(data)
        if not self.sniff(data):
            self.state = "non_csffbs"
            self.diagnostics.append(
                Diagnostic("error", 0, None, "File does not begin with CSFFBS magic")
            )
            return
        if total < HEADER_SIZE:
            self._error(total, "Truncated CSFFBS header: expected 24 bytes")
            return

        self.magic = data[0:6]
        self.reserved = data[6:8]
        self.version = _u32(data, 8)
        self.entry_count = _u32(data, 12)
        self.identifier_count = _u32(data, 16)
        self.string_count = _u32(data, 20)

        if (self.entry_count > MAX_TABLE_RECORDS
                or self.identifier_count > MAX_TABLE_RECORDS
                or self.string_count > MAX_TABLE_RECORDS):
            self._error(12, "A table count exceeds the implementation safety limit")
            return

        available = (total - HEADER_SIZE) // ENTRY_SIZE
        if self.entry_count > available:
            self._error(
                12,
                f"Entry table is truncated: declared {self.entry_count}, "
                f"at most {available} complete records fit",
            )
            for index in range(available):
                offset = HEADER_SIZE + index * ENTRY_SIZE
                self.entries.append(self._read_entry(index, offset))
            self.build_tree()
            return

        for index in range(self.entry_count):
            offset = HEADER_SIZE + index * ENTRY_SIZE
            self.entries.append(self._read_entry(index, offset))

        cursor = HEADER_SIZE + self.entry_count * ENTRY_SIZE
        minimum = self.identifier_count + self.string_count
        if minimum > (total - cursor) // 4:
            self._error(cursor, "Identifier and string counts exceed the remaining table space")
            self.build_tree()
            return

        if not self._parse_strings(cursor, self.identifier_count, self.identifiers, "identifier"):
            self.build_tree()
            return
        cursor += sum(4 + value.declared_length for value in self.identifiers)

        if not self._parse_strings(cursor, self.string_count, self.strings, "string"):
            self.build_tree()
            return
        cursor += sum(4 + value.declared_length for value in self.strings)

        if cursor < total:
            self.trailing_offset = cursor
            self.trailing_size = total - cursor
            self._warning(cursor, "Trailing bytes after the declared tables are preserved")

        self.build_tree()
        if not self._structural_error:
            self.state = "unsupported" if self._unsupported_entry else "exact"

    def _read_entry(self, index: int, offset: int) -> Entry:
        data = self.bytes
        raw_identifier = _u16(data, offset + 8)
        return Entry(
            index=index,
            raw_next_entry=_u32(data, offset),
            raw_value_or_size=_u32(data, offset + 4),
            identifier_index=raw_identifier - 0x1_0000 if raw_identifier >= 0x8000
            else raw_identifier,
            raw_type=_u16(data, offset + 10),
            offset=offset,
        )

    def _parse_strings(
        self, cursor: int, count: int, output: list[RawString], table_name: str
    ) -> bool:
        data = self.bytes
        total = len(data)
        for index in range(count):
            if not _range_fits(cursor, 4, total):
                self._error(cursor, f"Truncated {table_name} length at index {index}")
                return False
            record_offset = cursor
            length = _u32(data, cursor)
            cursor += 4
            value = RawString(index, length, record_offset)
            if not _range_fits(cursor, length, total):
                available = total - cursor
                value.data = data[cursor:total]
                value.size = 4 + available
                value.complete = False
                output.append(value)
                self._error(
                    record_offset,
                    f"Truncated {table_name} payload at index {index}: declared "
                    f"{length} bytes, only {available} remain",
                )
                return False
            value.data = data[cursor:cursor + length]
            value.size = 4 + length
            value.complete = True
            value.has_final_null = length > 0 and value.data[-1] == 0
            content_end = len(value.data) - 1 if value.has_final_null else len(value.data)
            value.has_embedded_null = 0 in value.data[:content_end]
            if length > 0 and not value.has_final_null:
                self._warning(record_offset, f"{table_name} index {index} has no final null byte")
            if value.has_embedded_null:
                self._warning(
                    record_offset, f"{table_name} index {index} contains an embedded null byte"
                )
            output.append(value)
            cursor += length
        return True

    def build_tree(self) -> None:
        stack: list[dict] = []
        pending: Optional[Entry] = None

        def consume_parent(entry: Entry) -> None:
            if not stack:
                return
            if stack[-1]["remaining"] == 0:
                self._error(entry.offset, "Container element count underflow", entry.index)
            else:
                stack[-1]["remaining"] -= 1

        def close_complete() -> None:
            while stack and stack[-1]["remaining"] == 0:
                stack.pop()

        for entry in self.entries:
            close_complete()
            kind = entry.kind()
            if kind is None:
                self._error(
                    entry.offset + 10,
                    f"Unknown entry type {entry.raw_type}",
                    entry.index,
                )
                self._unsupported_entry = True
                if pending is not None:
                    self._error(
                        pending.offset,
                        "Identifier before an unknown entry cannot be attached safely",
                        pending.index,
                    )
                    pending = None
                node = Node(entry_index=entry.index)
                (stack[-1]["children"] if stack else self.roots).append(node)
                consume_parent(entry)
                continue

            if kind == ValueKind.IDENTIFIER:
                if entry.identifier_index < 0 or entry.identifier_index >= len(self.identifiers):
                    self._error(
                        entry.offset + 8,
                        "Identifier entry references an invalid identifier index",
                        entry.index,
                    )
                if pending is not None:
                    self._error(
                        pending.offset,
                        "Identifier is not followed by a scalar value",
                        pending.index,
                    )
                pending = entry
                continue

            node = Node(entry_index=entry.index)
            if kind in (ValueKind.GROUP, ValueKind.ARRAY):
                if pending is not None:
                    self._error(
                        pending.offset,
                        "Identifier before a container cannot be attached safely",
                        pending.index,
                    )
                    pending = None
                if entry.identifier_index >= 0:
                    node.identifier_index = entry.identifier_index
                    if entry.identifier_index >= len(self.identifiers):
                        self._error(
                            entry.offset + 8,
                            "Container references an invalid identifier index",
                            entry.index,
                        )
                elif entry.identifier_index != -1:
                    self._error(
                        entry.offset + 8,
                        "Container identifier index is negative but not -1",
                        entry.index,
                    )
            else:
                if pending is not None:
                    node.label_entry_index = pending.index
                    if pending.identifier_index >= 0:
                        node.identifier_index = pending.identifier_index
                    pending = None
                if kind == ValueKind.INTEGER:
                    node.set_int(_i32_from_u32(entry.raw_value_or_size))
                elif kind == ValueKind.REAL:
                    node.set_real(_f32_from_u32(entry.raw_value_or_size))
                elif kind == ValueKind.STRING:
                    node.set_string(entry.raw_value_or_size)
                    if entry.raw_value_or_size >= len(self.strings):
                        self._error(
                            entry.offset + 4,
                            "String entry references an invalid string index",
                            entry.index,
                        )

            children = stack[-1]["children"] if stack else self.roots
            child_index = len(children)
            children.append(node)
            consume_parent(entry)

            if kind in (ValueKind.GROUP, ValueKind.ARRAY):
                if entry.raw_value_or_size > len(self.entries) - entry.index - 1:
                    self._error(
                        entry.offset + 4,
                        "Container declares more values than remaining entries can hold",
                        entry.index,
                    )
                if entry.raw_value_or_size != 0:
                    if len(stack) >= MAX_NESTING_DEPTH:
                        self._error(
                            entry.offset, "Maximum container nesting depth exceeded", entry.index
                        )
                        continue
                    stack.append(
                        {
                            "children": node.children,
                            "remaining": entry.raw_value_or_size,
                            "entry_index": entry.index,
                        }
                    )

        if pending is not None:
            self._error(
                pending.offset,
                "Identifier at end of entry table has no scalar value",
                pending.index,
            )
        for frame in stack:
            if frame["remaining"] != 0:
                entry = self.entries[frame["entry_index"]]
                self._error(
                    entry.offset,
                    f"Container is unclosed with {frame['remaining']} value(s) missing",
                    entry.index,
                )

    # -- generic helpers -------------------------------------------------
    def walk(self) -> Iterator[Node]:
        stack = list(reversed(self.roots))
        while stack:
            node = stack.pop()
            yield node
            stack.extend(reversed(node.children))

    def root_field(self, name: str) -> Optional[Node]:
        def descendant(node: Node) -> Optional[Node]:
            if self.label(node) == name:
                return node
            for child in node.children:
                found = descendant(child)
                if found is not None:
                    return found
            return None

        for root in self.roots:
            found = descendant(root)
            if found is not None:
                return found
        return None

    def find(self, needle: str, limit: int = 100) -> list[tuple[Node, str]]:
        results: list[tuple[Node, str]] = []
        for node in self.walk():
            text = self.label(node)
            if node.scalar_kind == "string":
                value = self.string(int(node.scalar))
                text = value.display() if value else text
            if needle.lower() in text.lower():
                results.append((node, text))
                if len(results) >= limit:
                    break
        return results

    def tree_stats(self) -> dict:
        max_depth = 0
        node_count = 0
        leaf_scalars = 0
        containers = 0
        stack = [(node, 1) for node in self.roots]
        while stack:
            node, depth = stack.pop()
            node_count += 1
            max_depth = max(max_depth, depth)
            if node.children:
                containers += 1
                stack.extend((child, depth + 1) for child in node.children)
            else:
                leaf_scalars += 1
        return {
            "roots": len(self.roots),
            "nodes": node_count,
            "containers": containers,
            "leaves": leaf_scalars,
            "max_depth": max_depth,
        }


# ---------------------------------------------------------------------------
# Typed mission-scene projection
# ---------------------------------------------------------------------------


@dataclass
class Vec3:
    x: float
    y: float
    z: float


@dataclass
class MissionActor:
    entry_index: int
    offset: int
    name: Optional[str] = None
    id: Optional[int] = None
    class_id: Optional[int] = None
    position: Optional[Vec3] = None
    heading: Optional[float] = None
    pitch: Optional[float] = None
    script: Optional[str] = None
    script_ids: list[int] = field(default_factory=list)
    group: Optional[int] = None
    cell: Optional[int] = None
    collision: Optional[int] = None
    flags: Optional[int] = None
    faction: Optional[str] = None
    portrait: Optional[str] = None
    animations: list[tuple[Optional[int], Optional[str]]] = field(default_factory=list)
    unknown_fields: list[str] = field(default_factory=list)


@dataclass
class NavPoint:
    entry_index: int
    group_id: Optional[int] = None
    id: Optional[int] = None
    name: Optional[str] = None
    position: Optional[Vec3] = None


@dataclass
class NavConnection:
    entry_index: int
    origin_group: Optional[int] = None
    origin_point: Optional[int] = None
    destination_group: Optional[int] = None
    destination_point: Optional[int] = None
    valid: bool = False
    invalid_reason: str = ""


@dataclass
class NavGroup:
    entry_index: int
    id: Optional[int] = None
    name: Optional[str] = None
    type: Optional[int] = None
    points: list[NavPoint] = field(default_factory=list)
    connections: list[NavConnection] = field(default_factory=list)


@dataclass
class MissionDummy:
    entry_index: int
    id: Optional[int] = None
    name: Optional[str] = None
    position: Optional[Vec3] = None


@dataclass
class MissionArea:
    entry_index: int
    id: Optional[int] = None
    name: Optional[str] = None
    flags: Optional[int] = None
    height: Optional[float] = None
    points: int = 0


@dataclass
class MissionLight:
    entry_index: int
    id: Optional[int] = None
    name: Optional[str] = None
    position: Optional[Vec3] = None
    color: Optional[int] = None
    radius: Optional[float] = None


@dataclass
class MissionEffect:
    entry_index: int
    id: Optional[int] = None
    name: Optional[str] = None
    class_id: Optional[int] = None
    dummy_id: Optional[int] = None


@dataclass
class MissionScene:
    """Typed projection of an SCN.  Semantics live here, not in the container."""

    source_path: Optional[Path] = None
    active_player: Optional[int] = None
    commando_start: Optional[int] = None
    sniper_start: Optional[int] = None
    spy_start: Optional[int] = None
    environment: list[tuple[str, object]] = field(default_factory=list)
    actors: list[MissionActor] = field(default_factory=list)
    navigation: list[NavGroup] = field(default_factory=list)
    cross_group: list[NavConnection] = field(default_factory=list)
    dummies: list[MissionDummy] = field(default_factory=list)
    areas: list[MissionArea] = field(default_factory=list)
    lights: list[MissionLight] = field(default_factory=list)
    effects: list[MissionEffect] = field(default_factory=list)
    folders: list[tuple[str, list[int]]] = field(default_factory=list)
    diagnostics: list[Diagnostic] = field(default_factory=list)

    # -- generic field readers ------------------------------------------
    @staticmethod
    def _child(document: Document, parent: Node, name: str) -> Optional[Node]:
        for child in parent.children:
            if document.label(child) == name:
                return child
        return None

    @staticmethod
    def _integer(node: Optional[Node]) -> Optional[int]:
        if node is not None and node.scalar_kind == "int":
            return int(node.scalar)
        return None

    @staticmethod
    def _real(node: Optional[Node]) -> Optional[float]:
        if node is not None and node.scalar_kind == "real":
            return float(node.scalar)
        return None

    @staticmethod
    def _string(document: Document, node: Optional[Node]) -> Optional[str]:
        if node is not None and node.scalar_kind == "string":
            value = document.string(int(node.scalar))
            return value.display() if value else None
        return None

    @staticmethod
    def _vec3(node: Optional[Node]) -> Optional[Vec3]:
        if node is None or len(node.children) < 3:
            return None
        values = [MissionScene._real(child) for child in node.children[:3]]
        if any(value is None or not math.isfinite(value) for value in values):
            return None
        return Vec3(*values)  # type: ignore[arg-type]

    @staticmethod
    def _connection(
        document: Document, node: Node, local_group: Optional[int] = None
    ) -> NavConnection:
        def labeled(name: str) -> Optional[int]:
            for item in node.children:
                if document.label(item) == name and item.scalar_kind == "int":
                    return int(item.scalar)
            return None

        origin_group = labeled(".GRUPO_ORI")
        destination_group = labeled(".GRUPO_DST")
        if origin_group is None:
            origin_group = local_group
        if destination_group is None:
            destination_group = local_group
        return NavConnection(
            entry_index=node.entry_index,
            origin_group=origin_group,
            origin_point=labeled(".PUNTO_ORI"),
            destination_group=destination_group,
            destination_point=labeled(".PUNTO_DST"),
        )

    @classmethod
    def project(cls, document: Document) -> "MissionScene":
        scene = cls(source_path=document.source_path)
        if document.state == "non_csffbs":
            scene.diagnostics.append(
                Diagnostic("error", 0, None, "SCN typed projection requires a valid CSFFBS document")
            )
            return scene

        for diagnostic in document.diagnostics:
            scene.diagnostics.append(diagnostic)

        scene.active_player = cls._integer(document.root_field(".PLAYER"))
        scene.commando_start = cls._integer(document.root_field(".INICIO_COMMANDO"))
        scene.sniper_start = cls._integer(document.root_field(".INICIO_SNIPER"))
        scene.spy_start = cls._integer(document.root_field(".INICIO_SPY"))

        world = document.root_field(".MUNDOVIS")
        if world is not None:
            for item in world.children:
                name = document.label(item)
                if item.scalar_kind == "int":
                    scene.environment.append((name, int(item.scalar)))
                elif item.scalar_kind == "real":
                    scene.environment.append((name, float(item.scalar)))
                elif item.scalar_kind == "string":
                    scene.environment.append((name, cls._string(document, item)))

        actors = document.root_field(".BICHOS")
        if actors is not None:
            for record in actors.children:
                actor = MissionActor(record.entry_index, document.entries[record.entry_index].offset)
                actor.name = cls._string(document, cls._child(document, record, ".NOMBRE"))
                actor.id = cls._integer(cls._child(document, record, ".ID"))
                actor.class_id = cls._integer(cls._child(document, record, ".CLASSID"))
                actor.position = cls._vec3(cls._child(document, record, ".POS"))
                actor.heading = cls._real(cls._child(document, record, ".ANGULO"))
                actor.pitch = cls._real(cls._child(document, record, ".ANGULO_X"))
                scripts = cls._child(document, record, ".SCRIPT")
                if scripts is not None:
                    actor.script = cls._string(document, scripts)
                    value = cls._integer(scripts)
                    if value is not None:
                        actor.script_ids.append(value)
                    for item in scripts.children:
                        inner = cls._integer(item)
                        if inner is not None:
                            actor.script_ids.append(inner)
                actor.collision = cls._integer(cls._child(document, record, ".COLISION"))
                actor.flags = cls._integer(cls._child(document, record, ".FLAGS"))
                actor.faction = cls._string(document, cls._child(document, record, ".BANDO"))
                actor.portrait = cls._string(document, cls._child(document, record, ".PORTRAIT"))
                animations = cls._child(document, record, ".ANIMACIONES")
                if animations is not None:
                    for item in animations.children:
                        actor.animations.append(
                            (
                                cls._integer(cls._child(document, item, ".ID")),
                                cls._string(document, cls._child(document, item, ".TIPO")),
                            )
                        )
                cell = cls._child(document, record, ".CELDA")
                if cell is not None:
                    actor.group = cls._integer(cls._child(document, cell, ".GRUPO"))
                    actor.cell = cls._integer(cls._child(document, cell, ".PUNTO"))
                scene.actors.append(actor)

        effects = document.root_field(".EFECTOS")
        if effects is not None:
            for record in effects.children:
                scene.effects.append(
                    MissionEffect(
                        entry_index=record.entry_index,
                        id=cls._integer(cls._child(document, record, ".ID")),
                        name=cls._string(document, cls._child(document, record, ".NOMBRE")),
                        class_id=cls._integer(cls._child(document, record, ".CLASSID")),
                        dummy_id=cls._integer(cls._child(document, record, ".DUMMY")),
                    )
                )

        nav = document.root_field(".MALLA_NAVEGACION")
        if nav is not None:
            groups = cls._child(document, nav, ".GRUPOS")
            if groups is not None:
                for record in groups.children:
                    group = NavGroup(entry_index=record.entry_index)
                    group.id = cls._integer(cls._child(document, record, ".ID"))
                    group.name = cls._string(document, cls._child(document, record, ".NOMBRE"))
                    group.type = cls._integer(cls._child(document, record, ".TIPO"))
                    points = cls._child(document, record, ".PUNTOS")
                    if points is not None:
                        for item in points.children:
                            group.points.append(
                                NavPoint(
                                    entry_index=item.entry_index,
                                    group_id=group.id,
                                    id=cls._integer(cls._child(document, item, ".ID")),
                                    name=cls._string(
                                        document, cls._child(document, item, ".NOMBRE")
                                    ),
                                    position=cls._vec3(cls._child(document, item, ".POS")),
                                )
                            )
                    links = cls._child(document, record, ".CONEXIONES")
                    if links is not None:
                        for item in links.children:
                            group.connections.append(cls._connection(document, item, group.id))
                    scene.navigation.append(group)
            for item in nav.children:
                if document.label(item) == ".GRUPOS":
                    continue
                if "CONEX" in document.label(item):
                    for record in item.children:
                        scene.cross_group.append(cls._connection(document, record))

        mesh = document.root_field(".MALLA_DUMMIES")
        if mesh is not None:
            records = cls._child(document, mesh, ".DUMMIES")
            if records is not None:
                for record in records.children:
                    scene.dummies.append(
                        MissionDummy(
                            entry_index=record.entry_index,
                            id=cls._integer(cls._child(document, record, ".ID")),
                            name=cls._string(document, cls._child(document, record, ".NOMBRE")),
                            position=cls._vec3(cls._child(document, record, ".POS")),
                        )
                    )
            cls._collect_folders(document, cls._child(document, mesh, ".CARPETAS"), "", scene.folders)

        mesh = document.root_field(".MALLA_AREAS")
        if mesh is not None:
            records = cls._child(document, mesh, ".AREAS")
            if records is not None:
                for record in records.children:
                    points = cls._child(document, record, ".PUNTOS")
                    count = sum(
                        1
                        for item in (points.children if points is not None else [])
                        if cls._vec3(cls._child(document, item, ".POS")) is not None
                    )
                    scene.areas.append(
                        MissionArea(
                            entry_index=record.entry_index,
                            id=cls._integer(cls._child(document, record, ".ID")),
                            name=cls._string(document, cls._child(document, record, ".NOMBRE")),
                            flags=cls._integer(cls._child(document, record, ".FLAGS")),
                            height=cls._real(cls._child(document, record, ".HEIGHT")),
                            points=count,
                        )
                    )

        mesh = document.root_field(".MALLA_LUCES")
        if mesh is not None:
            records = cls._child(document, mesh, ".LIGHTS")
            if records is not None:
                for record in records.children:
                    scene.lights.append(
                        MissionLight(
                            entry_index=record.entry_index,
                            id=cls._integer(cls._child(document, record, ".ID")),
                            name=cls._string(document, cls._child(document, record, ".NOMBRE")),
                            position=cls._vec3(cls._child(document, record, ".POS")),
                            color=cls._integer(cls._child(document, record, ".COLOR")),
                            radius=cls._real(cls._child(document, record, ".RADIO")),
                        )
                    )
            cls._collect_folders(document, cls._child(document, mesh, ".CARPETAS"), "", scene.folders)

        return scene

    @staticmethod
    def _collect_folders(
        document: Document,
        node: Optional[Node],
        parent: str,
        output: list[tuple[str, list[int]]],
    ) -> None:
        if node is None:
            return
        name = MissionScene._string(document, MissionScene._child(document, node, ".NOMBRE")) or ""
        if not parent:
            path = name
        else:
            path = parent if not name else f"{parent}/{name}"
        elements = MissionScene._child(document, node, ".ELEMENTOS")
        if elements is not None:
            ids = [int(item.scalar) for item in elements.children if item.scalar_kind == "int"]
            output.append((path, ids))
        for item in node.children:
            label = document.label(item)
            if item.children and label not in (".ELEMENTOS", ".NOMBRE"):
                MissionScene._collect_folders(document, item, path, output)

    # -- aggregate statistics -------------------------------------------
    def navigation_stats(self) -> dict:
        from collections import Counter

        group_counts = Counter(g.id for g in self.navigation if g.id is not None)
        point_counts: Counter = Counter()
        for group in self.navigation:
            for point in group.points:
                if group.id is not None and point.id is not None:
                    point_counts[(group.id, point.id)] += 1

        links = [link for group in self.navigation for link in group.connections]
        links += self.cross_group
        invalid = 0
        adjacency: dict[tuple[int, int], list[tuple[int, int]]] = {}
        for (group_id, point_id), count in point_counts.items():
            if count == 1 and group_counts[group_id] == 1:
                adjacency[(group_id, point_id)] = []
        for link in links:
            if None in (link.origin_group, link.origin_point,
                        link.destination_group, link.destination_point):
                link.invalid_reason = "missing-identity"
            else:
                origin = (link.origin_group, link.origin_point)
                destination = (link.destination_group, link.destination_point)
                if (group_counts[origin[0]] != 1 or group_counts[destination[0]] != 1
                        or point_counts[origin] != 1 or point_counts[destination] != 1):
                    link.invalid_reason = (
                        "ambiguous-identity"
                        if (group_counts[origin[0]] > 1 or group_counts[destination[0]] > 1
                            or point_counts[origin] > 1 or point_counts[destination] > 1)
                        else "missing-endpoint"
                    )
            link.valid = not link.invalid_reason
            if not link.valid:
                invalid += 1
                continue
            a = (link.origin_group, link.origin_point)
            b = (link.destination_group, link.destination_point)
            adjacency[a].append(b)
            adjacency[b].append(a)

        visited: set[tuple[int, int]] = set()
        components = 0
        orphans = 0
        for point, neighbors in adjacency.items():
            if not neighbors:
                orphans += 1
            if point in visited:
                continue
            components += 1
            queue = [point]
            visited.add(point)
            while queue:
                here = queue.pop()
                for nxt in adjacency[here]:
                    if nxt not in visited:
                        visited.add(nxt)
                        queue.append(nxt)

        return {
            "groups": len(self.navigation),
            "points": sum(len(g.points) for g in self.navigation),
            "connections": len(links),
            "valid_connections": len(links) - invalid,
            "invalid_connections": invalid,
            "connected_components": components,
            "orphan_points": orphans,
            "duplicate_group_ids": sum(1 for c in group_counts.values() if c > 1),
            "duplicate_point_ids": sum(1 for c in point_counts.values() if c > 1),
        }


# ---------------------------------------------------------------------------
# Serialization
# ---------------------------------------------------------------------------


def node_to_json(document: Document, node: Node) -> dict:
    kind = None
    value: object = None
    if node.scalar_kind == "int":
        kind, value = "int", node.scalar
    elif node.scalar_kind == "real":
        kind = "real"
        value = node.scalar if math.isfinite(node.scalar) else None
    elif node.scalar_kind == "string":
        kind = "string"
        raw = document.string(int(node.scalar))
        value = raw.display() if raw else None

    entry = document.entries[node.entry_index]
    result: dict = {
        "entry": node.entry_index,
        "offset": entry.offset,
        "type": KIND_NAMES.get(entry.raw_type, f"unknown({entry.raw_type})"),
        "label": document.label(node) or None,
        "kind": kind,
        "value": value,
    }
    if node.children:
        result["children"] = [node_to_json(document, child) for child in node.children]
    return result


def scene_to_json(scene: "MissionScene") -> dict:
    """JSON-safe view of a typed scene. Non-finite floats become null."""

    def sanitize(value: object) -> object:
        if isinstance(value, float):
            return value if math.isfinite(value) else None
        if isinstance(value, Path):
            return str(value)
        if isinstance(value, dict):
            return {key: sanitize(item) for key, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [sanitize(item) for item in value]
        if dataclasses.is_dataclass(value) and not isinstance(value, type):
            return sanitize(dataclasses.asdict(value))
        return value

    payload = dataclasses.asdict(scene)
    payload["navigation_stats"] = scene.navigation_stats()
    return sanitize(payload)  # type: ignore[return-value]


def document_to_json(document: Document, typed: bool = False) -> dict:
    payload: dict = {
        "schema": "csffbs-python-1",
        "source": str(document.source_path) if document.source_path else None,
        "state": document.state,
        "header": {
            "magic": document.magic.decode("ascii", "replace"),
            "reserved": document.reserved.hex(),
            "version": document.version,
            "entry_count": document.entry_count,
            "identifier_count": document.identifier_count,
            "string_count": document.string_count,
        },
        "diagnostics": [
            {"severity": d.severity, "offset": d.offset, "entry": d.entry_index, "message": d.message}
            for d in document.diagnostics
        ],
        "tree": [node_to_json(document, root) for root in document.roots],
    }
    if typed:
        payload["scene"] = scene_to_json(MissionScene.project(document))
    return payload


def render_tree(document: Document, node: Node, prefix: str = "", last: bool = True) -> list[str]:
    connector = "`- " if last else "|- "
    label = document.label(node)
    if node.scalar_kind == "int":
        detail = f" = {node.scalar}"
    elif node.scalar_kind == "real":
        detail = f" = {node.scalar:g}"
    elif node.scalar_kind == "string":
        raw = document.string(int(node.scalar))
        detail = f' = "{raw.display()}"' if raw else " = <bad string>"
    else:
        entry = document.entries[node.entry_index]
        detail = f" ({len(node.children)})" if node.children else f" ({KIND_NAMES.get(entry.raw_type, '?')})"
    lines = [f"{prefix}{connector}{label or '<unnamed>'}{detail}"]
    child_prefix = prefix + ("   " if last else "|  ")
    for index, child in enumerate(node.children):
        lines.extend(
            render_tree(document, child, child_prefix, index == len(node.children) - 1)
        )
    return lines


def _summary(document: Document) -> str:
    from collections import Counter

    types = Counter()
    labels = Counter()
    bad_strings = 0
    max_depth = 0
    for node in document.walk():
        entry = document.entries[node.entry_index]
        types[KIND_NAMES.get(entry.raw_type, f"unknown({entry.raw_type})")] += 1
        name = document.label(node)
        if name:
            labels[name] += 1
    for value in document.identifiers + document.strings:
        if not value.complete or not value.has_final_null or value.has_embedded_null:
            bad_strings += 1

    def depth(node: Node) -> int:
        return 1 + (max((depth(c) for c in node.children), default=0))

    max_depth = max((depth(root) for root in document.roots), default=0)
    stats = document.tree_stats()

    lines = [
        f"source      : {document.source_path}",
        f"state       : {document.state}",
        f"version     : {document.version}  reserved: {document.reserved.hex()}",
        f"entries     : {document.entry_count}  identifiers: {document.identifier_count}  strings: {document.string_count}",
        f"tree        : {stats['nodes']} nodes, {stats['containers']} containers, "
        f"{stats['leaves']} leaves, {stats['roots']} roots, depth {max_depth}",
        f"diagnostics : {len(document.diagnostics)} "
        f"({sum(1 for d in document.diagnostics if d.severity == 'error')} errors, "
        f"{sum(1 for d in document.diagnostics if d.severity == 'warning')} warnings)",
        f"trailing    : {document.trailing_size} bytes",
        f"odd strings : {bad_strings}",
        "entry types : " + ", ".join(f"{k}={v}" for k, v in sorted(types.items())),
    ]
    top = labels.most_common(15)
    lines.append("top labels  : " + ", ".join(f"{name}({count})" for name, count in top))
    return "\n".join(lines)


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Decode a CSFFBS document (read-only).")
    parser.add_argument("file", help="path to a CSFFBS file")
    parser.add_argument("--summary", action="store_true", help="print container summary")
    parser.add_argument("--tree", action="store_true", help="print the decoded node tree")
    parser.add_argument("--find", metavar="NAME", help="print nodes whose label/value contains NAME")
    parser.add_argument("--scene", action="store_true", help="print typed mission-scene statistics")
    parser.add_argument("--json", metavar="OUT", help="write generic tree JSON")
    parser.add_argument("--scene-json", metavar="OUT", help="write typed scene JSON")
    args = parser.parse_args(argv)

    document = Document.load(args.file)
    if document.state == "non_csffbs":
        print(f"{args.file}: not a CSFFBS document", file=sys.stderr)
        return 2

    printed = False
    if args.summary or not any([args.tree, args.find, args.scene, args.json, args.scene_json]):
        print(_summary(document))
        printed = True
    if args.tree:
        for root in document.roots:
            print("\n".join(render_tree(document, root)))
        printed = True
    if args.find:
        for node, text in document.find(args.find):
            entry = document.entries[node.entry_index]
            print(f"entry {node.entry_index:>7}  offset {entry.offset:>9}  {text}")
        printed = True
    if args.scene:
        scene = MissionScene.project(document)
        stats = scene.navigation_stats()
        print("player      : " + repr(
            (scene.active_player, scene.commando_start, scene.sniper_start, scene.spy_start)
        ))
        print(f"environment : {len(scene.environment)} fields")
        print(f"actors      : {len(scene.actors)}")
        print(f"dummies     : {len(scene.dummies)}")
        print(f"areas       : {len(scene.areas)}")
        print(f"lights      : {len(scene.lights)}")
        print(f"effects     : {len(scene.effects)}")
        print("navigation  : " + ", ".join(f"{k}={v}" for k, v in stats.items()))
        printed = True
    if args.json:
        Path(args.json).write_text(json.dumps(document_to_json(document), indent=2), encoding="utf-8")
        print(f"wrote {args.json}", file=sys.stderr)
    if args.scene_json:
        Path(args.scene_json).write_text(
            json.dumps(document_to_json(document, typed=True), indent=2), encoding="utf-8"
        )
        print(f"wrote {args.scene_json}", file=sys.stderr)
    return 0 if printed or args.json or args.scene_json else 0


if __name__ == "__main__":
    raise SystemExit(main())
