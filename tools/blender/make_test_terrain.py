"""Build the hello-world test terrain headless and export it as `.csfworld`.

    blender --background --factory-startup --python tools/blender/make_test_terrain.py -- \
        out.csfworld [--size 100] [--cells 50] [--hill-height 4] [--hill-radius 15] \
        [--center X,Y] [--height Z] [--texture FFLR_33A] [--surface Tierra] \
        [--uv-tile 4] [--save scene.blend]

All lengths are Blender metres (game centimetres / 100, Blender Y = -game Z).
The terrain is a square grid of `--size` metres with a smooth cosine hill of
`--hill-height` metres at its centre; it is generated, never committed.
"""

from __future__ import annotations

import argparse
import importlib.util
import math
import sys
from pathlib import Path

import bpy


def parse(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="make_test_terrain.py")
    ap.add_argument("output")
    ap.add_argument("--size", type=float, default=100.0)
    ap.add_argument("--cells", type=int, default=50)
    ap.add_argument("--hill-height", type=float, default=4.0)
    ap.add_argument("--hill-radius", type=float, default=15.0)
    ap.add_argument("--center", default="0,0", help="terrain centre X,Y in Blender metres")
    ap.add_argument("--height", type=float, default=0.0, help="base height (Blender Z) in metres")
    ap.add_argument("--texture", default="FFLR_33A")
    ap.add_argument("--surface", default="Tierra")
    ap.add_argument("--uv-tile", type=float, default=4.0, help="metres per texture repeat")
    ap.add_argument("--save")
    return ap.parse_args(argv[argv.index("--") + 1:] if "--" in argv else [])


def build(args: argparse.Namespace):
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    cx, cy = (float(v) for v in args.center.split(","))
    n = args.cells
    step = args.size / n
    verts, faces, uvs = [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            x = -args.size / 2 + i * step
            y = -args.size / 2 + j * step
            r = math.hypot(x, y)
            z = args.height
            if r < args.hill_radius:
                z += args.hill_height * 0.5 * (1 + math.cos(math.pi * r / args.hill_radius))
            verts.append((cx + x, cy + y, z))
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            faces.append((a, a + 1, a + n + 2, a + n + 1))  # counter-clockwise seen from +Z
    mesh = bpy.data.meshes.new("Terrain")
    mesh.from_pydata(verts, [], faces)
    uv = mesh.uv_layers.new(name="UVMap")
    for loop in mesh.loops:
        co = mesh.vertices[loop.vertex_index].co
        uv.uv[loop.index].vector = ((co.x - cx) / args.uv_tile, (co.y - cy) / args.uv_tile)
    mesh.shade_smooth()
    material = bpy.data.materials.new(args.texture)
    material["csf_texture"] = args.texture
    material["csf_surface"] = args.surface
    mesh.materials.append(material)
    obj = bpy.data.objects.new("Terrain", mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def main(argv: list[str]) -> int:
    args = parse(argv)
    build(args)
    if args.save:
        bpy.ops.wm.save_as_mainfile(filepath=str(Path(args.save).resolve()))
    spec = importlib.util.spec_from_file_location(
        "export_csf_world", Path(__file__).with_name("export_csf_world.py"))
    exporter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(exporter)
    stats = exporter.export(args.output)
    print(f"csfworld\t{args.output}\t{stats['faces']} faces\t{stats['materials']} materials")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
