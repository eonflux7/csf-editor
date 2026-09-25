"""Build the hello-world terrain headless and export it as `.csfworld`.

    blender --background --factory-startup --python tools/hello_world/terrain.py -- out.csfworld [out.blend]

A 100 m square (game -5000..5000 cm in X and Z) with gentle undulation, a low
hill in the middle, hills closing the edges, flat pads under the house, the
camp and the start, and a dirt path from the start past the camp to the house
(shape in layout.py). Faces get grass, path or rock (steep slopes) from
Convoy's ground textures. The terrain object is tagged as the project's
`terrain` asset; with out.blend the scene is also saved for editing. The house,
trees and plants are project placements (project.py), not part of the export.
"""

from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout import CELLS, PATH_HALF_WIDTH, SIZE, height, path_frame  # noqa: E402

ROCK_SLOPE = math.radians(24.0)
MATERIALS = {  # name: (texture, surface)
    "grass": ("FFLRA11B", "Tierra"),
    "path": ("FFLRA14I", "Tierra"),
    "rock": ("FDET_05B", "Piedra"),
}
UV_TILE = 400.0  # cm per texture repeat
PATH_UV_WIDTH = 600.0  # cm across FFLRA14I (dirt band in the middle, grass at the sides)


def blender(x: float, z: float, y: float) -> tuple[float, float, float]:
    return x / 100, -z / 100, y / 100


def build():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    step = SIZE / CELLS
    verts, game = [], []
    for j in range(CELLS + 1):
        for i in range(CELLS + 1):
            x, z = -SIZE / 2 + i * step, -SIZE / 2 + j * step
            y = height(x, z)
            verts.append(blender(x, z, y))
            game.append((x, y, z))
    faces, kinds = [], []
    for j in range(CELLS):
        for i in range(CELLS):
            a = j * (CELLS + 1) + i
            # game +z is Blender -y, so this order is counter-clockwise seen from +Z
            for tri in ((a, a + CELLS + 1, a + 1), (a + 1, a + CELLS + 1, a + CELLS + 2)):
                faces.append(tri)
                p = [game[v] for v in tri]
                cx, cz = sum(q[0] for q in p) / 3, sum(q[2] for q in p) / 3
                ux = [p[1][k] - p[0][k] for k in range(3)]
                vx = [p[2][k] - p[0][k] for k in range(3)]
                n = [ux[1] * vx[2] - ux[2] * vx[1], ux[2] * vx[0] - ux[0] * vx[2], ux[0] * vx[1] - ux[1] * vx[0]]
                slope = math.acos(abs(n[1]) / math.sqrt(sum(c * c for c in n)))
                if abs(path_frame(cx, cz)[0]) < PATH_HALF_WIDTH:
                    kinds.append("path")
                elif slope > ROCK_SLOPE:
                    kinds.append("rock")
                else:
                    kinds.append("grass")
    mesh = bpy.data.meshes.new("Terrain")
    mesh.from_pydata(verts, [], faces)
    order = list(MATERIALS)
    for name in order:
        texture, surface = MATERIALS[name]
        material = bpy.data.materials.new(name)
        material["csf_texture"] = texture
        material["csf_surface"] = surface
        mesh.materials.append(material)
    uv = mesh.uv_layers.new(name="UVMap")
    for polygon, kind in zip(mesh.polygons, kinds):
        polygon.material_index = order.index(kind)
        for loop in polygon.loop_indices:
            x, y, z = game[mesh.loops[loop].vertex_index]
            if kind == "path":
                across, along = path_frame(x, z)
                uv.uv[loop].vector = (0.5 + across / PATH_UV_WIDTH, along / UV_TILE)
            else:
                uv.uv[loop].vector = (x / UV_TILE, -z / UV_TILE)
    mesh.shade_smooth()
    obj = bpy.data.objects.new("Terrain", mesh)
    obj["csf_asset_id"] = "terrain"  # the project's terrain asset (csf_authoring add-on)
    obj["csf_asset_kind"] = "terrain"
    bpy.context.scene.collection.objects.link(obj)
    if any(face_normal_down(mesh)):
        raise SystemExit("terrain faces point down")


def face_normal_down(mesh):
    return (polygon.normal.z < 0 for polygon in mesh.polygons)


def main(argv: list[str]) -> int:
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    if len(args) not in (1, 2):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    build()
    spec = importlib.util.spec_from_file_location(
        "export_csf_world", Path(__file__).resolve().parents[1] / "blender" / "export_csf_world.py")
    exporter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(exporter)
    stats = exporter.export(args[0])
    if len(args) == 2:
        bpy.ops.wm.save_as_mainfile(filepath=str(Path(args[1]).resolve()))
    print(f"csfworld\t{args[0]}\t{stats['faces']} faces\t{stats['materials']} materials")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
