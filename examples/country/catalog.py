#!/usr/bin/env python3
"""A first asset catalogue over the unpacked game (examples/country/README.md):
the tables Country was assembled from, as TSV, for an asset registry to grow from.

    examples/country/catalog.py classes CORPUS OUT.tsv
    examples/country/catalog.py anims CORPUS OUT.tsv
    examples/country/catalog.py scripts CORPUS OUT.tsv
    examples/country/catalog.py map CORPUS MISSION MAP.rws OUT.tsv   (e.g. Ransom Maps/FR01/FR01.rws)
    examples/country/catalog.py objects CORPUS MISSION MAP.rws OUT.tsv

- classes: every mission's Objetos.bdd class (ID, name, type, rank, behaviour, model).
- anims: every Anims.bdd clip (IDs are the same clip in every mission), its
  file, loop flag, weapon stance (SF rifle, SM SMG, SP any class: weapon
  away), the item model held, and the missions that carry it.
- scripts: every mission program script (ID, name, folder, trigger flag,
  events, the animations it plays on THIS, whether it runs a cutscene).
- map: a map's World by lightmap group (a building is its group: EDIFICIO_n,
  its _INTERIOR and ATREZZO_ furniture), with bounds, triangle counts, the
  floor height and FR01-style ground height around it, for `piece donor=
  lightmaps=` records.
- objects: each group split into its separate objects (a rock, a lamp post,
  a sandbag nest, a hay bale) with their boxes, for single-object pieces.

Run from the repository root after ./build.sh (uses csf-mod decompile and
rws-info --export-scene-gltf).
"""

from __future__ import annotations

import collections
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

CSF_MOD, RWS_INFO = "./build/Release/csf-mod", "./build/Release/rws-info"


def decompile(path: Path) -> str:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "out.txt"
        subprocess.run([CSF_MOD, "decompile", str(path), str(out)], check=True, capture_output=True)
        return out.read_text(encoding="utf-8", errors="replace")


def records(text: str):
    """Top-level list records of a decompiled database or program, as {field: value}."""
    for block in re.findall(r"^    \[\n(.*?)^    \]\n", text, re.M | re.S):
        fields = dict(re.findall(r"^      \.(\w+) (.*)$", block, re.M))
        yield {k: v.strip().strip('"') for k, v in fields.items()}, block


def missions(corpus: Path):
    return sorted(p for p in corpus.iterdir() if (p / "BDD").is_dir())


def classes(corpus: Path, out: Path) -> None:
    rows = ["mission\tid\tname\ttype\trank\tbehaviour\tmodel"]
    for mission in missions(corpus):
        for fields, _ in records(decompile(mission / "BDD" / "Objetos.bdd")):
            rows.append("\t".join([mission.name] + [fields.get(k, "") for k in
                                                    ("ID", "NOMBRE", "TIPO", "GRADO", "COMPOR", "MODELO")]))
    out.write_text("\n".join(rows) + "\n", encoding="utf-8")


def stance(name: str) -> str:
    return {"SF": "rifle", "SM": "smg", "SP": "any"}.get(name[:2], "")


def anims(corpus: Path, out: Path) -> None:
    table: dict[int, dict] = {}
    for mission in missions(corpus):
        for fields, block in records(decompile(mission / "BDD" / "Anims.bdd")):
            if "ID" not in fields:
                continue
            entry = table.setdefault(int(fields["ID"]), {
                "name": fields.get("NOMBRE", ""), "loop": fields.get("LOOP", ""),
                "file": (re.search(r'\.FILE "(.*?)"', block) or [None, ""])[1],
                "item": fields.get("MODEL3D_ITEM", ""), "missions": set()})
            entry["missions"].add(mission.name)
    rows = ["id\tname\tfile\tloop\tstance\titem\tmissions"]
    for ident, e in sorted(table.items()):
        rows.append(f"{ident}\t{e['name']}\t{e['file']}\t{e['loop']}\t{stance(e['name'])}\t{e['item']}\t"
                    + ",".join(sorted(e["missions"])))
    out.write_text("\n".join(rows) + "\n", encoding="utf-8")


def scripts(corpus: Path, out: Path) -> None:
    rows = ["mission\tid\tname\tfolder\ttrigger\tevents\tanims\tcutscene"]
    for mission in missions(corpus):
        for program in sorted(mission.glob("Maps/**/*.gsc")):
            if program.stem != mission.name:
                continue
            for fields, block in records(decompile(program)):
                events = re.search(r"\.EVENTOS \((.*?)\n      \)", block, re.S)
                rows.append("\t".join([
                    mission.name, fields.get("ID", ""), fields.get("NOMBRE", ""), fields.get("CARPETA", ""),
                    (re.search(r"\.TRIGGER (\d)", block) or [None, ""])[1],
                    " ".join(re.findall(r"\((\w+)\)", events[1])) if events else "",
                    ",".join(sorted(set(re.findall(r"PLAY_ANMBDD\w* \(THIS\) \(ANM_BDD (\d+)\)", block)))),
                    "1" if "CUTSCENE_EXE" in block else ""]))
    out.write_text("\n".join(rows) + "\n", encoding="utf-8")


def world_triangles(corpus: Path, mission: str, map_path: str):
    """A map's World triangles (game cm) by lightmap texture."""
    import numpy as np

    with tempfile.TemporaryDirectory() as tmp:
        gltf_path = Path(tmp) / "map.gltf"
        subprocess.run([RWS_INFO, str(corpus / mission / map_path), "--export-scene-gltf", str(gltf_path)],
                       check=True, capture_output=True)
        gltf = json.loads(gltf_path.read_text())
        data = gltf_path.with_suffix(".bin").read_bytes()

    def accessor(index: int):
        a = gltf["accessors"][index]
        view = gltf["bufferViews"][a["bufferView"]]
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[a["type"]]
        kind = {5126: np.float32, 5125: np.uint32, 5123: np.uint16}[a["componentType"]]
        values = np.frombuffer(data, dtype=kind, count=a["count"] * width,
                               offset=view.get("byteOffset", 0) + a.get("byteOffset", 0))
        return values.reshape(-1, width) if width > 1 else values

    groups = collections.defaultdict(list)  # lightmap -> triangles (game cm)
    for node in gltf["nodes"]:
        if "mesh" not in node or node.get("name", "").startswith("clump") or \
                node.get("extras", {}).get("rws_kind") == "csf_instance":
            continue
        offset = np.array(node.get("translation", [0, 0, 0]))
        scale = np.array(node.get("scale", [1, 1, 1]))
        for primitive in gltf["meshes"][node["mesh"]]["primitives"]:
            extras = gltf["materials"][primitive["material"]].get("extras", {})
            positions = (accessor(primitive["attributes"]["POSITION"]) * scale + offset) * 100.0
            groups[extras.get("rws_lightmap_texture") or "-"].append(positions[accessor(primitive["indices"])].reshape(-1, 3, 3))
    return {name: np.concatenate(parts) for name, parts in groups.items()}


def world_map(corpus: Path, mission: str, map_path: str, out: Path) -> None:
    import numpy as np

    triangles = world_triangles(corpus, mission, map_path)
    ground = triangles.get("SUELO_Lm")
    rows = ["group\ttriangles\tx0\ty0\tz0\tx1\ty1\tz1\tfloor\tground"]
    for name, tris in sorted(triangles.items()):
        lo, hi = tris.reshape(-1, 3).min(0), tris.reshape(-1, 3).max(0)
        low = np.round(tris.reshape(-1, 3)[:, 1])
        values, counts = np.unique(low[low < lo[1] + 150], return_counts=True)
        floor = values[counts.argmax()] if len(values) else lo[1]
        around = ""
        if ground is not None:
            centres = ground.mean(1)
            ring = ((centres[:, 0] > lo[0] - 300) & (centres[:, 0] < hi[0] + 300) & (centres[:, 2] > lo[2] - 300) &
                    (centres[:, 2] < hi[2] + 300))
            inside = (centres[:, 0] > lo[0]) & (centres[:, 0] < hi[0]) & (centres[:, 2] > lo[2]) & (centres[:, 2] < hi[2])
            if (ring & ~inside).any():
                around = f"{np.median(centres[ring & ~inside][:, 1]):.0f}"
        rows.append(f"{name.removesuffix('_Lm')}\t{len(tris)}\t" + "\t".join(f"{v:.0f}" for v in (*lo, *hi))
                    + f"\t{floor:.0f}\t{around}")
    out.write_text("\n".join(rows) + "\n", encoding="utf-8")


def world_objects(corpus: Path, mission: str, map_path: str, out: Path) -> None:
    """Each lightmap group split into its objects: triangles joined by shared
    vertices (to 1 cm), then objects whose boxes overlap merged, so a lamp's
    post and its glass or a rock's faces stay one object."""
    import numpy as np

    rows = ["group\tobject\ttriangles\tx0\ty0\tz0\tx1\ty1\tz1"]
    for name, tris in sorted(world_triangles(corpus, mission, map_path).items()):
        keys = np.round(tris.reshape(-1, 3)).astype(np.int64)
        _, vertex = np.unique(keys, axis=0, return_inverse=True)
        vertex = vertex.reshape(-1, 3)
        parent = list(range(vertex.max() + 1))

        def find(a):
            while parent[a] != a:
                parent[a] = parent[parent[a]]
                a = parent[a]
            return a

        for a, b, c in vertex:
            for u, v in ((a, b), (a, c)):
                ru, rv = find(u), find(v)
                if ru != rv:
                    parent[ru] = rv
        label = np.array([find(v[0]) for v in vertex])
        boxes = []
        for root in np.unique(label):
            part = tris[label == root].reshape(-1, 3)
            boxes.append([part.min(0), part.max(0), int((label == root).sum())])
        merged = True
        while merged:  # merge overlapping boxes
            merged = False
            for i in range(len(boxes)):
                for j in range(i + 1, len(boxes)):
                    (lo1, hi1, n1), (lo2, hi2, n2) = boxes[i], boxes[j]
                    if (lo1 <= hi2 + 5).all() and (lo2 <= hi1 + 5).all():
                        boxes[i] = [np.minimum(lo1, lo2), np.maximum(hi1, hi2), n1 + n2]
                        del boxes[j]
                        merged = True
                        break
                if merged:
                    break
        for k, (lo, hi, count) in enumerate(sorted(boxes, key=lambda b: (b[0][0], b[0][2])), 1):
            rows.append(f"{name.removesuffix('_Lm')}\t{k}\t{count}\t" + "\t".join(f"{v:.0f}" for v in (*lo, *hi)))
    out.write_text("\n".join(rows) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    commands = {"classes": (classes, 4), "anims": (anims, 4), "scripts": (scripts, 4), "map": (world_map, 6),
                "objects": (world_objects, 6)}
    if len(argv) < 2 or argv[1] not in commands or len(argv) != commands[argv[1]][1]:
        print(__doc__.strip().split("\n\n")[1], file=sys.stderr)
        return 2
    function, _ = commands[argv[1]]
    function(Path(argv[2]), *argv[3:-1], Path(argv[-1]))
    print(f"{argv[1]}\t{argv[-1]}\t{sum(1 for _ in open(argv[-1])) - 1} rows")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
