#!/usr/bin/env python3
"""Print the `csf-mod mission-edit` operations that turn Convoy's scene into the
hello-world scene (docs/plans/hello-world-mission.md, phases 6 and 7).

    scene_ops.py <decompiled Convoy.scn text> <script-ids> <actor-ids>

Keeps the Sniper (1), the Spy (2) and guard GE_02 (6, patrolling nav points
5/3 and 5/4 through script 23); keeps scripts 1 (weapons), 5 (campaign),
10 (strings), 23 (patrol) and rewrites 33 (register zone 6, objective 1) and
35 (enter zone 6: objective done, mission won). The house areas (occluders,
rain exclusion) move with the house piece; zone 6 surrounds the new house;
TOTAL covers the terrain. Scene effects (.EFECTOS) whose dummy lies over the
terrain are moved 30 m under the ground (they floated in the air at Convoy's
heights; 2026-09-25). Other areas keep their shipped shapes: they are
inert once the scripts that use them are gone, and collapsing them into
degenerate polygons crashed the game on load (2026-09-25).
"""
import re
import sys

HOUSE_BOX = (-5650, -6850, -4120, -5440)          # donor x0, z0, x1, z1 (build_world.sh)
HOUSE_ANCHOR = (-4885.0, 1020.0, -6145.0)         # bottom centre of the piece box
HOUSE_TARGET = (-2500.0, -4.0, -2500.0)
HOUSE_AREAS = {"OCLUSOR_01", "OCLUSOR_02", "OCLUSOR_03", "OCLUSOR_04", "LLUVIA_CASA_01", "LLUVIA_CASA_02"}
KEEP_SCRIPTS = {1, 5, 10, 23, 33, 35}
KEEP_ACTORS = {1, 2, 6}
ZONE = 6
ZONE_RECT = [(-3400, -3350), (-1600, -3350), (-1600, -1650), (-3400, -1650)]
TERRAIN = 5000


def areas(text):
    block = text[text.index(".AREAS ("):]
    for m in re.finditer(r"\.ID (\d+)\s+\.FLAGS \d+.*?\.NOMBRE (\S+).*?\.PUNTOS \((.*?)\n\s*\)\n", block, re.S):
        points = [tuple(map(float, p)) for p in
                  re.findall(r"\.POS \(([-\d.e]+) ([-\d.e]+) ([-\d.e]+)\)", m.group(3))]
        yield int(m.group(1)), m.group(2), points
        if m.group(2) == "TOTAL":
            break


def effect_dummies(text):
    """(dummy id, x, z) of each .EFECTOS entry."""
    effects = text[text.index(".EFECTOS ("):]
    effects = effects[:effects.index("\n  .", 10)]
    ids = {int(d) for d in re.findall(r"\.DUMMY (\d+)", effects)}
    dummies = text[text.index(".DUMMIES ("):]
    for m in re.finditer(r"\.ID (\d+)\s+\.NOMBRE \S+\s+\.POS \(([-\d.e]+) ([-\d.e]+) ([-\d.e]+)\)", dummies):
        if int(m.group(1)) in ids:
            ids.discard(int(m.group(1)))
            yield int(m.group(1)), float(m.group(2)), float(m.group(4))


def main():
    text = open(sys.argv[1], encoding="utf-8").read()
    scripts = [int(v) for v in sys.argv[2].split(",") if v]
    actors = [int(v) for v in sys.argv[3].split(",") if v]
    ops = ["--force"]
    ops += [f"--delete-script {s}" for s in scripts if s not in KEEP_SCRIPTS]
    ops += [f"--delete-actor {a}" for a in actors if a not in KEEP_ACTORS]
    ops += ["--set-script 33 tools/hello_world/init_zonas.txt",
            "--set-script 35 tools/hello_world/ejecution_01.txt",
            "--move-actor 1 3500 0 -3500", "--move-actor 2 3700 0 -3300",
            "--move-actor 6 1000 0 4000", "--move-nav-point 5 4 -4000 0 4000"]
    delta = [HOUSE_TARGET[i] - HOUSE_ANCHOR[i] for i in range(3)]
    for area, name, points in areas(text):
        xs = [p[0] for p in points]
        zs = [p[2] for p in points]
        if name in HOUSE_AREAS:
            new = [(p[0] + delta[0], p[1] + delta[1], p[2] + delta[2]) for p in points]
        elif area == ZONE:
            new = [(x, 0.0, z) for x, z in ZONE_RECT]
            ops.append(f"--area-height {ZONE} 400")
        elif name == "TOTAL":
            new = [(-5500, -500, -5500), (5500, -500, -5500), (5500, -500, 5500), (-5500, -500, 5500)]
        else:
            continue
        if len(new) != len(points):
            raise SystemExit(f"area {area} point count changed")
        for i, (x, y, z) in enumerate(new):
            ops.append(f"--move-area-point {area} {i} {x:g} {y:g} {z:g}")
    for dummy, x, z in effect_dummies(text):
        if abs(x) < TERRAIN + 500 and abs(z) < TERRAIN + 500:
            ops.append(f"--move-dummy {dummy} {x:g} -3000 {z:g}")
    print("\n".join(ops))


if __name__ == "__main__":
    main()
