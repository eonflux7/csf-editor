"""Build the Country terrain headless and export it as `.csfworld`.

    blender --background --factory-startup --python tools/country/terrain.py -- out.csfworld [out.blend]

The 200 m square of layout.py: grass, dirt roads (across-road UVs on
Convoy's FFLRA14I), rock on steep slopes, and the two fields in FR01's
ploughed (FFLR_16A) and stubble (FFLR_35A) textures, which the project
imports as its own textures. A second UV set maps the whole square onto the
placeholder lightmap COUNTRY_Lm (lightmaps.py). The object is tagged as the
project's `terrain` asset; with out.blend the scene is saved for editing.
"""

from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout import CELLS, SIZE, field_at, height, road_frame  # noqa: E402

LIGHTMAP = "COUNTRY_Lm"
ROCK_SLOPE = math.radians(32.0)
MATERIALS = {  # name: (texture, surface)
    "grass": ("FFLRA11B", "Tierra"),
    "road": ("FFLRA14I", "Tierra"),
    "rock": ("FDET_05B", "Piedra"),
    "stubble": ("FFLR_35A", "Tierra"),
    "ploughed": ("FFLR_16A", "Tierra"),
}
FIELD_KIND = {"FFLR_35A": "stubble", "FFLR_16A": "ploughed"}
UV_TILE = 400.0        # cm per texture repeat
ROAD_UV_WIDTH = 700.0  # cm across FFLRA14I (dirt band in the middle, grass at the sides)


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
                across, _, half = road_frame(cx, cz)
                if abs(across) < half:
                    kinds.append("road")
                elif slope > ROCK_SLOPE:
                    kinds.append("rock")
                elif field_at(cx, cz):
                    kinds.append(FIELD_KIND[field_at(cx, cz)])
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
        material["csf_lightmap"] = LIGHTMAP
        mesh.materials.append(material)
    uv = mesh.uv_layers.new(name="UVMap")
    lightmap_uv = mesh.uv_layers.new(name="CSF_Lightmap")
    for polygon, kind in zip(mesh.polygons, kinds):
        polygon.material_index = order.index(kind)
        for loop in polygon.loop_indices:
            x, y, z = game[mesh.loops[loop].vertex_index]
            if kind == "road":
                across, along, _ = road_frame(x, z)
                uv.uv[loop].vector = (0.5 + across / ROAD_UV_WIDTH, along / UV_TILE)
            else:
                uv.uv[loop].vector = (x / UV_TILE, -z / UV_TILE)
            # The exporter flips V: game v = 1 - this, so row 0 of the image is the north edge.
            lightmap_uv.uv[loop].vector = ((x + SIZE / 2) / SIZE, (z + SIZE / 2) / SIZE)
    mesh.shade_smooth()
    obj = bpy.data.objects.new("Terrain", mesh)
    obj["csf_asset_id"] = "terrain"  # the project's terrain asset (csf_authoring add-on)
    obj["csf_asset_kind"] = "terrain"
    bpy.context.scene.collection.objects.link(obj)
    if any(polygon.normal.z < 0 for polygon in mesh.polygons):
        raise SystemExit("terrain faces point down")


def exporter():
    spec = importlib.util.spec_from_file_location(
        "export_csf_world", Path(__file__).resolve().parents[1] / "blender" / "export_csf_world.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main(argv: list[str]) -> int:
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    if len(args) not in (1, 2):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    build()
    stats = exporter().export(args[0])
    if len(args) == 2:
        bpy.ops.wm.save_as_mainfile(filepath=str(Path(args[1]).resolve()))
    print(f"csfworld\t{args[0]}\t{stats['faces']} faces\t{stats['materials']} materials")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
