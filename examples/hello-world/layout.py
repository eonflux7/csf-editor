"""The hello-world map layout, shared by terrain.py (inside Blender) and
project.py: the terrain's shape and the placements of Convoy's house, trees
and plants. Positions are game units (cm): x, z on the ground.
"""

from __future__ import annotations

import math

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

# Convoy's house: the donor World triangles in this box (lit by the EDIFICIO_1*
# lightmaps), set 4 cm into its pad.
HOUSE_BOX = (-5650.0, 1020.0, -6850.0, -4120.0, 2500.0, -5440.0)
HOUSE_AT, HOUSE_SINK = (-2500.0, -2500.0), -4.0
TREE_SINK, PLANT_SINK = -10.0, -5.0
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
