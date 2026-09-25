"""Build the hello-world terrain headless and export it as `.csfworld`.

    blender --background --factory-startup --python tools/hello_world/terrain.py -- out.csfworld

A 100 m square (game -5000..5000 cm in X and Z) with gentle undulation, a low
hill in the middle, hills closing the edges, flat pads under the house, the
camp and the start, and a dirt path from the start past the camp to the house.
Faces get grass, path or rock (steep slopes) from Convoy's ground textures.
After the terrain, the house piece and the tree and plant props are appended
at the terrain height. Positions here are game units (cm): x, z on the ground.
"""

from __future__ import annotations

import importlib.util
import math
import sys
from pathlib import Path

import bpy

SIZE, CELLS = 10000.0, 50
HILL = ((0.0, 0.0), 1500.0, 400.0)  # centre, radius, height
EDGE = (1600.0, 480.0)  # width, height of the hills closing the map
EDGE_SOUTH = (800.0, 330.0)
PADS = [  # centre, flat radius, blend width: the house, the camp and the start
    ((-2500.0, -2500.0), 1150.0, 900.0),
    ((550.0, -3100.0), 950.0, 700.0),
    ((3500.0, -3400.0), 500.0, 700.0),
]
PATH = [(3400.0, -3400.0), (2200.0, -2800.0), (1000.0, -2150.0), (-800.0, -2150.0), (-1600.0, -2500.0)]
PATH_HALF_WIDTH = 230.0
ROCK_SLOPE = math.radians(24.0)
MATERIALS = {  # name: (texture, surface)
    "grass": ("FFLRA11B", "Tierra"),
    "path": ("FFLRA14I", "Tierra"),
    "rock": ("FDET_05B", "Piedra"),
}
UV_TILE = 400.0  # cm per texture repeat
PATH_UV_WIDTH = 600.0  # cm across FFLRA14I (dirt band in the middle, grass at the sides)

HOUSE_PIECE = "piece -5650 1020 -6850 -4120 2500 -5440 -2500 {y} -2500"
TREES = [  # donor instance ids, x, z, yaw
    ("1,2", 2500.0, -2000.0, 0), ("7,8", 2000.0, 2500.0, 90), ("15,16", -3600.0, 1200.0, 30),
    ("1,2", -4200.0, -800.0, 200), ("7,8", -1200.0, 4300.0, 140), ("15,16", 3000.0, 4200.0, 310),
    ("1,2", 4300.0, 1500.0, 60), ("7,8", 4200.0, -1200.0, 250), ("15,16", -4300.0, -4300.0, 10),
    ("1,2", -700.0, -4500.0, 170), ("7,8", 2600.0, -4500.0, 80), ("15,16", -4400.0, 3600.0, 290),
    ("1,2", 1800.0, 900.0, 120), ("7,8", -2200.0, 700.0, 330),
]
PLANT = "319"  # Convoy's ground plant (prototype 1040)
PLANTS = [(-600.0, -1600.0), (1900.0, -3700.0), (-1400.0, -3700.0), (3900.0, -2600.0), (200.0, 1900.0),
          (-3000.0, 2600.0), (3300.0, 800.0), (-800.0, 2600.0), (2400.0, 3300.0), (-3900.0, -2000.0),
          (1200.0, -1300.0), (-200.0, -3900.0), (4000.0, 3000.0), (-2600.0, 4000.0), (600.0, 3800.0)]


def smoothstep(edge0: float, edge1: float, x: float) -> float:
    t = min(max((x - edge0) / (edge1 - edge0), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def natural_height(x: float, z: float) -> float:
    h = 30.0 * math.sin(0.0013 * x + 0.7) * math.cos(0.0011 * z) + 18.0 * math.sin(0.0031 * x + 0.0023 * z)
    (hx, hz), radius, height = HILL
    r = math.hypot(x - hx, z - hz)
    if r < radius:
        h += height * 0.5 * (1 + math.cos(math.pi * r / radius))
    # The south side (game -z) holds the house, the camp and the start, so its
    # hills are narrower and lower than the others.
    for distance, (width, rise_height) in ((SIZE / 2 - x, EDGE), (SIZE / 2 + x, EDGE), (SIZE / 2 - z, EDGE),
                                           (SIZE / 2 + z, EDGE_SOUTH)):
        rise = smoothstep(width, 0.0, distance)
        h = max(h, h * (1 - rise) + rise * (rise_height + 150.0 * math.sin(0.0021 * x + 1.3)
                                            * math.cos(0.0017 * z + 0.4)))
    return h


def height(x: float, z: float) -> float:
    h = natural_height(x, z)
    for (px, pz), radius, blend in PADS:
        w = smoothstep(radius + blend, radius, math.hypot(x - px, z - pz))
        h = h * (1 - w)  # pads are at game height 0
    return h


def path_frame(x: float, z: float) -> tuple[float, float]:
    """Signed distance across the path and distance along it."""
    best_distance, best, along = math.inf, (math.inf, 0.0), 0.0
    for (ax, az), (bx, bz) in zip(PATH, PATH[1:]):
        dx, dz = bx - ax, bz - az
        length = math.hypot(dx, dz)
        t = min(max(((x - ax) * dx + (z - az) * dz) / (length * length), 0.0), 1.0)
        distance = math.hypot(x - (ax + t * dx), z - (az + t * dz))
        if distance < best_distance:
            side = (x - ax) * dz - (z - az) * dx
            best_distance, best = distance, (math.copysign(distance, side), along + t * length)
        along += length
    return best


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
    bpy.context.scene.collection.objects.link(obj)
    if any(face_normal_down(mesh)):
        raise SystemExit("terrain faces point down")


def face_normal_down(mesh):
    return (polygon.normal.z < 0 for polygon in mesh.polygons)


def props() -> list[str]:
    lines = ["# Convoy's house (the World triangles lit by the EDIFICIO_1* lightmaps), on its pad",
             HOUSE_PIECE.format(y=-4)]
    lines.append("# Convoy trees (two scene instances per placement) and ground plants")
    for ids, x, z, yaw in TREES:
        lines.append(f"prop {ids} {x:.0f} {height(x, z) - 10:.0f} {z:.0f} {yaw}")
    for k, (x, z) in enumerate(PLANTS):
        lines.append(f"prop {PLANT} {x:.0f} {height(x, z) - 5:.0f} {z:.0f} {k * 47 % 360}")
    return lines


def main(argv: list[str]) -> int:
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    if len(args) != 1:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    build()
    spec = importlib.util.spec_from_file_location(
        "export_csf_world", Path(__file__).resolve().parents[1] / "blender" / "export_csf_world.py")
    exporter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(exporter)
    stats = exporter.export(args[0])
    with open(args[0], "a", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(props()) + "\n")
    print(f"csfworld\t{args[0]}\t{stats['faces']} faces\t{stats['materials']} materials")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
