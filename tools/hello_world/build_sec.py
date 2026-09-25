#!/usr/bin/env python3
"""Write a `.sec` sector map for the walkable faces of a `.csfworld`.

    tools/hello_world/build_sec.py hello.csfworld Convoy.sec

Format (KB-scn-6, KB-scn-8): u32 version 0, u32 vertex count, vec3 vertices,
u32 sector count, then per sector u32 entry count, u32 sector index, u32 0,
a 0x2c plane {point, unit normal, cached normal, d, u8 flag, 3 pad} and one
12-byte directed half-edge {A, B, neighbour sector or 0xFFFFFFFF} per polygon
vertex, in loop order. Containment is an XZ crossing test over the A vertices;
with flag 1 the game recomputes d from the point and normal.

Every collision face of the source becomes part of a sector: adjacent
triangles are paired into convex quads (the shipped maps have at most about
3000 sectors), and neighbour links join sectors across shared edges.
"""

from __future__ import annotations

import math
import struct
import sys


def read_faces(path: str) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    positions: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    with open(path, encoding="utf-8") as source:
        for line in source:
            fields = line.split()
            if not fields or fields[0].startswith("#"):
                continue
            if fields[0] == "v":
                positions.append((float(fields[1]), float(fields[2]), float(fields[3])))
            elif fields[0] == "f" and fields[5] in ("collision", "both"):
                faces.append((int(fields[1]), int(fields[2]), int(fields[3])))
    # One vertex per distinct position, so neighbouring faces share edges.
    index: dict[tuple[int, int, int], int] = {}
    vertices: list[tuple[float, float, float]] = []
    remap = []
    for p in positions:
        key = tuple(round(c * 10) for c in p)
        if key not in index:
            index[key] = len(vertices)
            vertices.append(p)
        remap.append(index[key])
    faces = [(remap[a], remap[b], remap[c]) for a, b, c in faces]
    return vertices, [f for f in faces if len(set(f)) == 3]


def convex_xz(vertices, loop) -> bool:
    sign = 0
    n = len(loop)
    for k in range(n):
        a, b, c = (vertices[loop[(k + j) % n]] for j in range(3))
        cross = (b[0] - a[0]) * (c[2] - b[2]) - (b[2] - a[2]) * (c[0] - b[0])
        if abs(cross) < 1e-6:
            return False
        s = 1 if cross > 0 else -1
        if sign and s != sign:
            return False
        sign = s
    return True


def pair_triangles(vertices, faces) -> list[list[int]]:
    edge_face: dict[tuple[int, int], int] = {}
    for i, (a, b, c) in enumerate(faces):
        for e in ((a, b), (b, c), (c, a)):
            edge_face[e] = i
    used = [False] * len(faces)
    polygons = []
    for i, face in enumerate(faces):
        if used[i]:
            continue
        used[i] = True
        merged = None
        for k in range(3):
            a, b, c = face[k], face[(k + 1) % 3], face[(k + 2) % 3]
            j = edge_face.get((b, a))
            if j is None or used[j]:
                continue
            other = [v for v in faces[j] if v not in (a, b)]
            if len(other) != 1:
                continue
            loop = [a, other[0], b, c]  # the shared edge a-b is replaced by a-d-b
            if convex_xz(vertices, loop):
                used[j] = True
                merged = loop
                break
        polygons.append(merged or list(face))
    return polygons


def plane(vertices, loop) -> bytes:
    p0, p1, p2 = (vertices[v] for v in loop[:3])
    u = [p1[i] - p0[i] for i in range(3)]
    w = [p2[i] - p0[i] for i in range(3)]
    n = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]]
    length = math.sqrt(sum(c * c for c in n)) or 1.0
    n = [c / length for c in n]
    d = -sum(n[i] * p0[i] for i in range(3))
    return struct.pack("<3f3f3ffB3x", *p0, *n, *n, d, 1)


def write_sec(path: str, vertices, polygons) -> None:
    owner: dict[tuple[int, int], int] = {}
    for s, loop in enumerate(polygons):
        for k in range(len(loop)):
            owner[(loop[k], loop[(k + 1) % len(loop)])] = s
    out = bytearray(struct.pack("<II", 0, len(vertices)))
    for p in vertices:
        out += struct.pack("<3f", *p)
    out += struct.pack("<I", len(polygons))
    for s, loop in enumerate(polygons):
        out += struct.pack("<III", len(loop), s, 0)
        out += plane(vertices, loop)
        for k in range(len(loop)):
            a, b = loop[k], loop[(k + 1) % len(loop)]
            out += struct.pack("<III", a, b, owner.get((b, a), 0xFFFFFFFF))
    with open(path, "wb") as target:
        target.write(out)


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    vertices, faces = read_faces(argv[1])
    polygons = pair_triangles(vertices, faces)
    write_sec(argv[2], vertices, polygons)
    print(f"sec\t{argv[2]}\t{len(vertices)} vertices\t{len(polygons)} sectors")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
