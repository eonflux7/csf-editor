#!/usr/bin/env python3
"""Write the hello-world scene and scripts from scratch.

    scene.py <donor scene source text> <hello.csfworld> <out dir>

Writes <out dir>/Convoy.scn.txt and Convoy.csc.txt (compile them with Convoy's
files as templates), anchors.csfproj (the actors' height relations, for the
project) and
<out dir>/scripts/<id>.txt, one `csf-mod mission-edit --add-script` file per
script. Only the donor's `.MUNDOVIS` environment (sky, fog, textures, minimap)
is kept; actors, navigation, dummies, areas and lights are all written here
(docs/plans/hello-world-mission.md, "Extended hello world").

Coordinates are game units (cm, Y up); heights come from the terrain faces of
the `.csfworld` that build_world.sh exported (tools/hello_world/terrain.py).
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

class Terrain:
    """Ground height at (x, z), sampled from the walkable faces of a `.csfworld`."""

    CELL = 500.0

    def __init__(self, path: str):
        vertices, self.cells = [], {}
        with open(path, encoding="utf-8") as source:
            for line in source:
                fields = line.split()
                if fields and fields[0] == "v":
                    vertices.append((float(fields[1]), float(fields[2]), float(fields[3])))
                elif fields and fields[0] == "f" and fields[5] in ("collision", "both"):
                    tri = [vertices[int(k)] for k in fields[1:4]]
                    xs, zs = [p[0] for p in tri], [p[2] for p in tri]
                    for i in range(int(min(xs) // self.CELL), int(max(xs) // self.CELL) + 1):
                        for j in range(int(min(zs) // self.CELL), int(max(zs) // self.CELL) + 1):
                            self.cells.setdefault((i, j), []).append(tri)

    def height(self, x: float, z: float) -> float:
        best = None
        for a, b, c in self.cells.get((int(x // self.CELL), int(z // self.CELL)), []):
            d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
            if abs(d) < 1e-9:
                continue
            u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
            v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
            if min(u, v, 1 - u - v) >= -1e-6:
                y = u * a[1] + v * b[1] + (1 - u - v) * c[1]
                best = y if best is None else max(best, y)
        if best is None:
            raise ValueError(f"no terrain under ({x}, {z})")
        return best


TERRAIN: Terrain


def ground(x: float, z: float) -> float:
    return TERRAIN.height(x, z)


def pos(x: float, z: float, lift: float = 0.0) -> str:
    return f"({x:.1f} {ground(x, z) + lift:.1f} {z:.1f})"


# ---- Content ------------------------------------------------------------------

HOUSE = (-3300.0, -3250.0, -1700.0, -1750.0)  # footprint x0 z0 x1 z1 (Convoy's house piece)
ZONE_HOUSE = 6
AREA_TOTAL = 16

# Actor ids. Players keep Convoy's ids 1 and 2 (the kept weapon script names them).
SNIPER, SPY = 1, 2
OFFICER, GUARD_CAMP, GUARD_NORTH, GUARD_HOUSE, DOG, RADIO_GHOST = 10, 11, 12, 13, 14, 15
GHOST_CLASS = 484  # Escape's visible telephone ghost; build_mission gives it radio class 211's look.
RADIO_LIFT = 90.55355  # Escape class 383 BBOX.SUP.y minus radio class 211 BBOX.INF.y.
DOG_CLASS, DOG_WALK = 431, 2383  # Ransom's Doberman and DobermanAndar (imported)

# Nav groups.
G_WALK, G_START, G_CAMP_ROUTE, G_NORTH_ROUTE, G_DOG_ROUTE, G_COVER = 1, 2, 5, 6, 7, 20

# Script ids (all Convoy scripts are deleted first).
S_WEAPONS, S_INIT, S_OBJECTIVES, S_ZONE, S_OFFICER_DEAD, S_RADIO = 9001, 9002, 9003, 9004, 9005, 9006
S_OFFICER_IDLE, S_CAMP_PATROL, S_NORTH_PATROL, S_HOUSE_IDLE, S_DOG = 9010, 9011, 9012, 9013, 9014
S_INTRO = 9020
CAMERA_CLASS = 197  # Ambush's invisible, physics-free travelling-camera actor

# The camp between the players' start and the house: props and the cover
# points behind them (on the side away from the players, who come from +x).
PROPS = [  # class, name, x, z, heading degrees
    (93, "CAMP_TRUCK", 600.0, -2700.0, 90.0),     # burnt Mercedes
    (335, "CAMP_WOOD_1", 150.0, -3450.0, 20.0),   # wood pile
    (335, "CAMP_WOOD_2", 950.0, -3550.0, -35.0),
    (383, "RADIO_CRATE", 350.0, -3050.0, 0.0),   # Escape's static wooden crate
    (306, "SCRAP_TRUCK", -900.0, 3300.0, 160.0),  # scrap truck near the north route
    (105, "PICKUP_MP40", 2600.0, -3300.0, 0.0),   # weapon pickup near the start
]
COVER = [  # x, z: west of each prop, facing east (heading 90)
    (330.0, -2800.0), (330.0, -2550.0), (0.0, -3450.0), (780.0, -3600.0), (-1150.0, 3300.0),
]
CAMP_ROUTE = [(1600.0, -2400.0), (-400.0, -2400.0), (-400.0, -3900.0), (1600.0, -3900.0)]
NORTH_ROUTE = [(1000.0, 4000.0), (-4000.0, 4000.0)]
DOG_ROUTE = [(1750.0, -2250.0), (-550.0, -2250.0), (-550.0, -4050.0), (1750.0, -4050.0)]  # around the camp loop

ACTORS = [  # id, name, class, x, z, heading, nav (group, point) or None, scripts, extra lines
    (SNIPER, "Sniper", 54, 3500.0, -3500.0, 7.3, (G_START, 2), [], []),
    (SPY, "Spy", 55, 3700.0, -3300.0, 112.5, (G_START, 1), [], []),
    (OFFICER, "OFICIAL", 36, 500.0, -3150.0, -90.0, None, [S_OFFICER_IDLE],
     ['.PORTRAIT "Menus\\\\Retratos\\\\FotoOficial.fbs"']),
    (GUARD_CAMP, "GE_CAMP", 13, *CAMP_ROUTE[0], -90.0, (G_CAMP_ROUTE, 1), [S_CAMP_PATROL], []),
    (GUARD_NORTH, "GE_NORTH", 21, *NORTH_ROUTE[0], -101.5, (G_NORTH_ROUTE, 1), [S_NORTH_PATROL], []),
    (GUARD_HOUSE, "GE_HOUSE", 3, -1400.0, -2000.0, 90.0, None, [S_HOUSE_IDLE], []),
    (DOG, "DOBERMAN", DOG_CLASS, *DOG_ROUTE[0], -90.0, None, [S_DOG], []),
    (RADIO_GHOST, "CAMP_RADIO", GHOST_CLASS, 350.0, -3050.0, 0.0, None, [], []),
]

def prop_actors() -> list[tuple]:
    """Props are actors of decorative (and pickup) classes, without a nav cell (KB-scn-12)."""
    return [(30 + k, name, cls, x, z, heading, None, [], [])
            for k, (cls, name, x, z, heading) in enumerate(PROPS)]


# Intro shots: camera dummy id, camera (x, z, height over ground), target
# (x, z, height over ground), seconds. A camera dummy looks along
# (sin .ROT, cos .ROT) in XZ and a positive .ROT_X tilts it down (radians,
# CAMARA_EN_DUMMY 0x00465310; checked against Ransom's intro cameras 108/109).
SHOTS = [
    (101, (2800.0, -800.0, 1100.0), (500.0, -3000.0, 100.0), 5.0),     # the camp from above
    (102, (2400.0, -1600.0, 180.0), (1500.0, -2350.0, 60.0), 4.5),     # the dog and the camp guard
    (103, (1100.0, -3650.0, 200.0), (500.0, -3150.0, 130.0), 3.5),     # the officer
    (104, (-1000.0, 2800.0, 350.0), (-1500.0, 4000.0, 100.0), 3.5),    # the north patrol
    (105, (-500.0, -700.0, 550.0), (-2500.0, -2500.0, 250.0), 4.0),    # the building
]
# Short straight travelling shots, in game centimetres. The cuts between
# subjects stay; each view moves while holding its own look-at target.
SHOT_TRAVEL = [(-220.0, -120.0), (-180.0, -80.0), (-100.0, 100.0),
               (-220.0, 0.0), (-160.0, -120.0)]


def camera_helpers():
    """(actor record, height above terrain), matching Ambush's class 197 rigs.

    KB-scripting-44: CREATE_VIEWPOINT binds a dummy to a cameraman and target;
    IR_A_PATHPOINT moves the cameraman on an independent type-0 nav group.
    """
    for k, (_, camera, target, _) in enumerate(SHOTS):
        cameraman, aim, group = 100 + 2 * k, 101 + 2 * k, 100 + k
        x, z, height = camera
        yield ((cameraman, f"INTRO_CAMERA_{k + 1}", CAMERA_CLASS, x, z, 0.0,
                (group, 1), [], []), height)
        x, z, height = target
        yield ((aim, f"INTRO_TARGET_{k + 1}", CAMERA_CLASS, x, z, 0.0,
                None, [], []), height)


def look_at(camera, target) -> tuple[float, float]:
    (cx, cz, ch), (tx, tz, th) = camera, target
    dx, dz = tx - cx, tz - cz
    dy = (ground(tx, tz) + th) - (ground(cx, cz) + ch)
    return math.atan2(dx, dz), math.atan2(-dy, math.hypot(dx, dz))


def walk_grid() -> tuple[list[tuple[int, float, float]], list[tuple[int, int]]]:
    """Points every 10 m over the map, minus the house and the camp props."""
    blocked = [(x, z) for _, _, x, z, _ in PROPS]
    points, index = [], {}
    for j in range(-4, 5):
        for i in range(-4, 5):
            x, z = i * 1000.0 + 500.0 * (j % 2 == 0), j * 1000.0
            if HOUSE[0] - 200 <= x <= HOUSE[2] + 200 and HOUSE[1] - 200 <= z <= HOUSE[3] + 200:
                continue
            if any(math.hypot(x - bx, z - bz) < 300 for bx, bz in blocked):
                continue
            index[(i, j)] = len(points) + 1
            points.append((len(points) + 1, x, z))
    links = []
    for (i, j), a in index.items():
        for di, dj in ((1, 0), (0, 1)):
            if (b := index.get((i + di, j + dj))) is not None:
                links.append((a, b))
    return points, links


# ---- Scene text ---------------------------------------------------------------


def block(lines: list[str], indent: int) -> str:
    return "".join(" " * indent + line + "\n" for line in lines)


def mundovis(donor: str) -> str:
    lines = donor.splitlines()
    start = next(i for i, line in enumerate(lines) if line == "  .MUNDOVIS [")
    end = next(i for i in range(start + 1, len(lines)) if lines[i] == "  ]")
    return "\n".join(lines[start:end + 1]) + "\n"


def actor(record, lift: float = 0.0) -> str:
    ident, name, cls, x, z, heading, nav, scripts, extra = record
    group, point = nav if nav else (-1, -1)
    lines = [
        "[", f"  .NOMBRE {name}", f"  .ID {ident}", f"  .CLASSID {cls}",
        f"  .POS {pos(x, z, RADIO_LIFT if ident == RADIO_GHOST else lift)}",
        f"  .ANGULO {heading}", "  .ANGULO_X 0.0", "  .COLISION 1", "  .FLAGS 0",
        *("  " + line for line in extra), "  .SEGUNDA_EXPLOSION 0",
    ]
    if scripts:
        lines.append(f"  .SCRIPT ({' '.join(map(str, scripts))})")
    lines += ["  .CELDA [", f"    .GRUPO {group}", f"    .PUNTO {point}", "  ]", "]"]
    return block(lines, 4)


def nav_point(ident: int, x: float, z: float, heading_degrees: float = 0.0,
              lift: float = 0.0) -> list[str]:
    return ["[", f"  .ID {ident}", '  .NOMBRE ""', f"  .POS {pos(x, z, lift)}",
            f"  .ROT {math.radians(heading_degrees):.6f}", "  .ROT_X 0.0", "]"]


def nav_group(ident: int, name: str, kind: int, points, links) -> str:
    lines = ["[", f"  .ID {ident}", f"  .NOMBRE {name}", f"  .TIPO {kind}", "  .PUNTOS ("]
    for point in points:
        lines += ["    " + line for line in nav_point(*point)]
    lines.append("  )")
    if links:
        lines.append("  .CONEXIONES (")
        for a, b in links:
            lines += ["    [", f"      .PUNTO_ORI {a}", f"      .PUNTO_DST {b}", "    ]"]
        lines.append("  )")
    else:
        lines.append("  .CONEXIONES ()")
    lines.append("]")
    return block(lines, 6)


def navigation() -> str:
    grid, grid_links = walk_grid()
    loop = lambda n: [(k + 1, (k + 1) % n + 1) for k in range(n)]
    start = [(1, 3700.0, -3300.0, 112.5), (2, 3500.0, -3500.0, 7.3)]
    camp = [(k + 1, x, z) for k, (x, z) in enumerate(CAMP_ROUTE)]
    north = [(k + 1, x, z) for k, (x, z) in enumerate(NORTH_ROUTE)]
    cover = [(k + 1, x, z, 90.0) for k, (x, z) in enumerate(COVER)]
    text = "  .MALLA_NAVEGACION [\n    .GRUPOS (\n"
    text += nav_group(G_WALK, "MALLA", 0, grid, grid_links)
    text += nav_group(G_START, "COMMANDOS", 0, start, [])
    text += nav_group(G_CAMP_ROUTE, "RUTA_CAMPAMENTO", 0, camp, loop(len(camp)))
    text += nav_group(G_NORTH_ROUTE, "RUTA_NORTE", 0, north, [(1, 2)])
    dog = [(k + 1, x, z) for k, (x, z) in enumerate(DOG_ROUTE)]
    text += nav_group(G_DOG_ROUTE, "RUTA_PERRO", 0, dog, loop(len(dog)))
    text += nav_group(G_COVER, "Parapeto_Campamento", 3, cover, [])
    # Camera paths deliberately have no links into the walking graph. Hold
    # world height constant instead of following terrain height along the shot.
    for k, (_, (x, z, height), _, _) in enumerate(SHOTS):
        dx, dz = SHOT_TRAVEL[k]
        heading = math.degrees(math.atan2(dx, dz))
        end_lift = ground(x, z) + height - ground(x + dx, z + dz)
        text += nav_group(100 + k, f"Rutas_Cutscene_Inicio_Camara_{k + 1}", 0,
                          [(1, x, z, heading, height),
                           (2, x + dx, z + dz, heading, end_lift)], [(1, 2)])
    text += "    )\n"
    # Every route and start point joins the nearest walk-grid point.
    links = []
    for group, points in ((G_START, start), (G_CAMP_ROUTE, camp), (G_NORTH_ROUTE, north)):
        for ident, x, z, *_ in points:
            nearest = min(grid, key=lambda p: math.hypot(p[1] - x, p[2] - z))
            links.append((group, ident, G_WALK, nearest[0]))
    text += "    .CONEXIONES (\n"
    for ga, pa, gb, pb in links:
        text += block(["[", f"  .GRUPO_ORI {ga}", f"  .PUNTO_ORI {pa}",
                       f"  .GRUPO_DST {gb}", f"  .PUNTO_DST {pb}", "]"], 6)
    text += "    )\n  ]\n"
    return text


def dummies() -> str:
    lines = ["  .MALLA_DUMMIES [", "    .DUMMIES ("]
    for ident, camera, target, _ in SHOTS:
        yaw, pitch = look_at(camera, target)
        lines += ["      " + line for line in
                  ["[", f"  .ID {ident}", f"  .NOMBRE CAMARA_{ident}", f"  .POS {pos(*camera)}",
                   f"  .ROT {yaw:.6f}", f"  .ROT_X {pitch:.6f}", "]"]]
    lines += ["    )", "  ]"]
    return "\n".join(lines) + "\n"


def area(ident: int, name: str, height: float, corners, y: float) -> str:
    lines = ["[", f"  .ID {ident}", "  .FLAGS 1", "  .OCLUSION 1", f"  .NOMBRE {name}",
             f"  .HEIGHT {height}", "  .REVERB 0", "  .LIMITREVERB 0", "  .PUNTOS ("]
    for x, z in corners:
        lines += ["    [", f"      .POS ({x} {y} {z})", "    ]"]
    lines += ["  )", "]"]
    return block(lines, 6)


def areas() -> str:
    text = "  .MALLA_AREAS [\n    .AREAS (\n"
    text += area(ZONE_HOUSE, "ZONA_CASA", 400.0,
                 [(-3400.0, -3350.0), (-1600.0, -3350.0), (-1600.0, -1650.0), (-3400.0, -1650.0)], 0.0)
    text += area(AREA_TOTAL, "TOTAL", 3000.0,
                 [(-5500.0, -5500.0), (5500.0, -5500.0), (5500.0, 5500.0), (-5500.0, 5500.0)], -500.0)
    return text + "    )\n  ]\n"


def scene(donor: str) -> str:
    return (
        "[\n  .VERSION 17\n  .PLAYER 2\n" + mundovis(donor)
        + "  .BICHOS (\n" + "".join(actor(record) for record in ACTORS + prop_actors())
        + "".join(actor(record, lift) for record, lift in camera_helpers()) + "  )\n"
        + "  .EFECTOS ()\n  .AGUAS ()\n"
        + "  .MULTIPLAYER [\n    .MPDEATHMATCH 0\n    .MPTDEATHMATCH 0\n    .MPPOSTMEN 0\n"
          "    .MPPOSTMEN_IDPOSTMAN -1\n    .MPFRANCOS 0\n  ]\n"
        + "  .PUNTUACION_MAXIMA 1850\n  .PUNTUACION_MINIMA 1200\n"
        + navigation()
        + '  .MAPA_SECTORES "Maps\\\\Secs\\\\Convoy.sec"\n'
        + dummies() + areas()
        + "  .MALLA_LUCES [\n    .LIGHTS ()\n  ]\n"
        + "  .MALLA_SCENE_OBJS [\n    .SCENEOBJS ()\n  ]\n"
        + "  .BRIDGES ()\n]\n"
    )


# ---- Scripts ------------------------------------------------------------------


def script(ident: int, name: str, trigger: int, events: list[str], actions: list[str],
           conditions: list[str] | None = None) -> str:
    lines = ["[", f"  .ID {ident}", f"  .NOMBRE {name}", '  .CARPETA ""',
             "  .FLAGS [", f"    .TRIGGER {trigger}", "    .ENABLED 1", "    .VALIDO 1", "  ]"]
    if events:
        lines += ["  .EVENTOS (", *(f"    ({event})" for event in events), "  )"]
    if conditions:
        lines += ["  .CONDICIONES {", *("    " + c for c in conditions), "  }"]
    lines += ["  .ACCIONES {", *("    " + a for a in actions), "  }", "]"]
    return "\n".join(lines) + "\n"


CHECK_BOTH = [
    "IF (AND (OBJETIVO_COMPLETADO (NUMERO 1.0)) (OBJETIVO_COMPLETADO (NUMERO 2.0)))",
    "  TIMED_STRING_V2 (FLI g014) (NUMERO 5.0) (NUMERO 4.0)",
    "  PAUSE (NUMERO 4.0)",
    "  SET_MISSION_SUCCESS (BOOL TRUE)",
    "ENDIF",
]


def patrol(route_group: int, count: int, pause: float) -> list[str]:
    body = []
    for k in range(count):
        body += [f"  IR_A_PATHPOINT (THIS) (PATHPOINT {route_group} {k + 1})", f"  PAUSE (NUMERO {pause})"]
    return ["WHILE (BOOL TRUE)", *body, "WEND"]


def scripts() -> dict[int, str]:
    # Escape's INIT scripts configure the AI mode as well as the cover group
    # (e.g. SELECT_GRUPO_PARAPETO 127 + MOVIL_A_PARAPETO). A group alone
    # leaves the actor's default combat behavior in place.
    cover = [
        f"SELECT_GRUPO_PARAPETO (THIS) (GRUPO_PATHPOINT {G_COVER})",
        "SET_IA_ALERTA (THIS) (IA_ALERTA MOVIL_A_PARAPETO)",
        "SET_IA_COMBATE (THIS) (IA_COMBATE MOVIL_A_PARAPETO)",
    ]
    return {
        S_WEAPONS: script(S_WEAPONS, "COMMANDOS_INI", 1, ["START_GAME"], [
            "ADD_ARMA (BICHO 1) (ARMA_CLASSID 16)",
            "ADD_ARMA (BICHO 1) (ARMA_CLASSID 102)",
            "SET_MUNICION_ARMA (BICHO 1) (ARMA_CLASSID 102) (NUMERO 100.0) (NUMERO 100.0)",
            "ADD_ARMA (BICHO 1) (ARMA_CLASSID 4)",
            "SET_MUNICION_ARMA (BICHO 1) (ARMA_CLASSID 4) (NUMERO 100.0) (NUMERO 100.0)",
            "ADD_ARMA (BICHO 1) (ARMA_CLASSID 64)",
            "ADD_ARMA (BICHO 1) (ARMA_CLASSID 37)",
            "SELECT_ARMA (BICHO 1) (ARMA_CLASSID 4)",
            "ADD_ARMA (BICHO 2) (ARMA_CLASSID 50)",
            "ADD_ARMA (BICHO 2) (ARMA_CLASSID 102)",
            "SET_MUNICION_ARMA (BICHO 2) (ARMA_CLASSID 102) (NUMERO 100.0) (NUMERO 100.0)",
            "ADD_ARMA (BICHO 2) (ARMA_CLASSID 45)",
            "ADD_ARMA (BICHO 2) (ARMA_CLASSID 44)",
            "ADD_ARMA (BICHO 2) (ARMA_CLASSID 37)",
            "ADD_ARMA (BICHO 2) (ARMA_CLASSID 70)",
            "DISFRAZAR (BICHO 2) (CLASSID 23)",
        ]),
        S_INIT: script(S_INIT, "INIT_MISSION", 1, ["START_GAME"], [
            "TIMED_STRING_INITPOS (NUMERO 0.115) (NUMERO 0.25)",
            "ADD_TIP_MISSION (FLI g200)",
            "ADD_TIP_MISSION (FLI g199)",
        ]),
        S_OBJECTIVES: script(S_OBJECTIVES, "INIT_OBJETIVOS", 1, ["START_GAME"], [
            f"ACT_BICHO_EVENT_ZONA (PLAYER) (ZONA {ZONE_HOUSE}) (BOOL TRUE)",
            "SET_OBJETIVO (NUMERO 1.0) (BOOL FALSE) (NUMERO 2.0)",
            'SET_OBJETIVO_LABEL (NUMERO 1.0) (FLI "0900")',
            "SET_OBJETIVO (NUMERO 2.0) (BOOL FALSE) (NUMERO 2.0)",
            'SET_OBJETIVO_LABEL (NUMERO 2.0) (FLI "0901")',
            # Secondary objective: use the visible radio ghost on the crate.
            "SET_OBJETIVO (NUMERO 3.0) (BOOL TRUE) (NUMERO 1.0)",
            'SET_OBJETIVO_LABEL (NUMERO 3.0) (FLI "0904")',
            f'BICHO_SET_CONTEXT_LABEL (BICHO {RADIO_GHOST}) (FLI "0906")',
            f"HABILITAR_GHOST (BICHO {RADIO_GHOST}) (BOOL TRUE)",
            f"SET_CONTEXTUAL (BICHO {RADIO_GHOST}) (BOOL TRUE)",
            f"ENABLE_GHOST_ILUM (BICHO {RADIO_GHOST}) (BOOL TRUE)",
        ]),
        S_RADIO: script(S_RADIO, "RADIO_SABOTEADA", 1, ["EVT_GHOST_USADO"], [
            f"HABILITAR_GHOST (BICHO {RADIO_GHOST}) (BOOL FALSE)",
            f"SET_CONTEXTUAL (BICHO {RADIO_GHOST}) (BOOL FALSE)",
            f"ENABLE_GHOST_ILUM (BICHO {RADIO_GHOST}) (BOOL FALSE)",
            "SET_OBJETIVO_SUCCESS (NUMERO 3.0) (BOOL TRUE)",
            'TIMED_STRING_V2 (FLI "0905") (NUMERO 5.0) (NUMERO 4.0)',
        ], [f"CMP_OP_BICHO (EVT_BICHO2) (OP_BOOLEAN 0) (BICHO {RADIO_GHOST})"]),
        S_ZONE: script(S_ZONE, "CASA_ALCANZADA", 1, ["BICHO_ENT_ZONA"], [
            f"ACT_BICHO_EVENT_ZONA (PLAYER) (ZONA {ZONE_HOUSE}) (BOOL FALSE)",
            "SET_OBJETIVO_SUCCESS (NUMERO 1.0) (BOOL TRUE)",
            'TIMED_STRING_V2 (FLI "0902") (NUMERO 5.0) (NUMERO 4.0)',
            *CHECK_BOTH,
        ], [f"CMP_OP_ZONA (EVT_ZONA) (OP_BOOLEAN 0) (ZONA {ZONE_HOUSE})"]),
        S_OFFICER_DEAD: script(S_OFFICER_DEAD, "OFICIAL_MUERTO", 1, ["MORIBUNDO", "MUERTO"], [
            "SET_OBJETIVO_SUCCESS (NUMERO 2.0) (BOOL TRUE)",
            'TIMED_STRING_V2 (FLI "0903") (NUMERO 5.0) (NUMERO 4.0)',
            *CHECK_BOTH,
        ], [f"AND (CMP_OP_BICHO (EVT_BICHO1) (OP_BOOLEAN 0) (BICHO {OFFICER})) "
            "(NOT (OBJETIVO_COMPLETADO (NUMERO 2.0)))"]),
        S_OFFICER_IDLE: script(S_OFFICER_IDLE, "OFICIAL_FUMAR", 0, ["INIT"], [
            *cover,
            "WHILE (BOOL TRUE)",
            "  PLAY_ANMBDD_CICLOS (THIS) (ANM_BDD 1881) (RANDOM (NUMERO 2.0) (NUMERO 4.0))",
            "  PLAY_ANMBDD (THIS) (ANM_BDD 1385)",
            "WEND",
        ]),
        S_CAMP_PATROL: script(S_CAMP_PATROL, "GE_RUTA_CAMPAMENTO", 0, ["INIT"],
                              [*cover, *patrol(G_CAMP_ROUTE, len(CAMP_ROUTE), 3.0)]),
        S_NORTH_PATROL: script(S_NORTH_PATROL, "GE_RUTA_NORTE", 0, ["INIT"],
                               [*cover, *patrol(G_NORTH_ROUTE, len(NORTH_ROUTE), 4.0)]),
        S_DOG: script(S_DOG, "PERRO_RUTA", 0, ["INIT"], [
            "WHILE (BOOL TRUE)",
            *(f"  IR_A_PATHPOINT_ANIM (THIS) (PATHPOINT {G_DOG_ROUTE} {k + 1}) (BOOL TRUE) (ANM_BDD {DOG_WALK})"
              for k in range(len(DOG_ROUTE))),
            "WEND",
        ]),
        # The Convoy/Ransom intro pattern: fade in, non-interactive mode, the
        # camera cutscene from the .csc, fade out and back to play.
        S_INTRO: script(S_INTRO, "CUTSCENE_INICIO", 1, ["START_GAME"], [
            "FX_FADE (NUMERO 1.0) (BOOL FALSE) (VECTOR 0.0 0.0 0.0)",
            "CUTSCENE_NO_INTERACTIVA (BOOL TRUE)",
            "PLAYER_TERCERA (BOOL TRUE)",
            # INIT is a mission event, not a built-in one (KB-scripting-14): the
            # actor scripts listening for it (patrols, idles, cover) start here,
            # as Convoy's intro script sends it.
            "SEND_EVENT (EVENT INIT)",
            # Shipped Ambush travelling-camera pattern (KB-scripting-44).
            # Register viewpoints in .gsc; the .csc activates them by dummy ID.
            *(f"CREATE_VIEWPOINT (DUMMY {ident}) (BICHO {100 + 2 * k}) "
              f"(BICHO {101 + 2 * k}) (NUMERO 0.0)"
              for k, (ident, _, _, _) in enumerate(SHOTS)),
            "CUTSCENE_EXE (CUTSCENE 1)",
            "FX_FADE (NUMERO 1.0) (BOOL TRUE) (VECTOR 0.0 0.0 0.0)",
            "PAUSE (NUMERO 1.5)",
            "CUTSCENE_NO_INTERACTIVA (BOOL FALSE)",
            "PLAYER_TERCERA (BOOL FALSE)",
            *(f"NAVEGACION_STOP (BICHO {100 + 2 * k})" for k in range(len(SHOTS))),
            "FX_FADE (NUMERO 1.0) (BOOL FALSE) (VECTOR 0.0 0.0 0.0)",
        ]),
        S_HOUSE_IDLE: script(S_HOUSE_IDLE, "GE_RADIO", 0, ["INIT"], [
            *cover,
            "WHILE (BOOL TRUE)",
            "  PLAY_ANMBDD (THIS) (ANM_BDD 1782)",
            "WEND",
        ]),
    }


def cutscene_records() -> list[str]:
    """Convoy.csc's structure (CUT_INICIO runs its INIT, camera and END scripts)
    with the hello-world camera script, one script record each."""
    camera = []
    for k, (ident, _, _, seconds) in enumerate(SHOTS):
        speed = math.hypot(*SHOT_TRAVEL[k]) / seconds
        camera += ["PAUSE (NUMERO 0.01)", f"CAMARA_EN_DUMMY (DUMMY {ident})",
                   'CAM_SETFILTRO (CADENA "<NINGUNO>")',
                   f"SET_WANTED_VEL (BICHO {100 + 2 * k}) (NUMERO {speed:.6f})",
                   f"CONTINUE (IR_A_PATHPOINT (BICHO {100 + 2 * k}) (PATHPOINT {100 + k} 2))",
                   f"PAUSE (NUMERO {seconds})"]
    empty = lambda ident, name: "\n".join(script(ident, name, 1, [], ["X"]).split("\n")[:9]) + "\n]\n"
    records = [
        script(1, "CUT_INICIO", 1, [], ["CUTSCENE_EXE (CUTSCENE 2)",
                                        "CONTINUE (CUTSCENE_EXE (CUTSCENE 28))",
                                        "CUTSCENE_EXE (CUTSCENE 3)"]),
        empty(2, "CUT_INICIOINIT"),
        script(3, "CUT_INICIOEND", 1, [], ["WAIT_CONDICION (CUTSCENE_FINISHED (CUTSCENE 28))"]),
        script(28, "CUT_INICIO_GENERAL_Camara", 1, [], camera),
    ]
    return records


def cutscene() -> str:
    body = "".join("\n".join("    " + line for line in record.rstrip("\n").split("\n")) + "\n"
                   for record in cutscene_records())
    return "[\n  .SCRIPTS (\n" + body + "  )\n  .POOL ()\n]\n"


def anchors() -> str:
    """Project `anchor` records (docs/plans/editor-project-format.md): every
    actor and prop actor stands on the ground, the radio on its crate. Camera
    helpers keep their absolute heights."""
    crate = next(30 + k for k, (_, name, *_rest) in enumerate(PROPS) if name == "RADIO_CRATE")
    lines = ["", "# Height relations of mission actors (scene.py)"]
    for ident, *_ in ACTORS + prop_actors():
        rule = f"on actor {crate} {RADIO_LIFT}" if ident == RADIO_GHOST else "ground 0"
        lines.append(f"anchor actor {ident} {rule}")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    global TERRAIN
    donor = Path(argv[1]).read_text(encoding="utf-8")
    TERRAIN = Terrain(argv[2])
    out = Path(argv[3])
    (out / "scripts").mkdir(parents=True, exist_ok=True)
    (out / "Convoy.scn.txt").write_text(scene(donor), encoding="utf-8")
    (out / "Convoy.csc.txt").write_text(cutscene(), encoding="utf-8")
    for ident, text in scripts().items():
        (out / "scripts" / f"{ident}.txt").write_text(text, encoding="utf-8")
    (out / "anchors.csfproj").write_text(anchors(), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
