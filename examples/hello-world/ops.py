#!/usr/bin/env python3
"""Write hello world as mission operations (include/csf/mission_ops.hpp).

    examples/hello-world/ops.py TERRAIN.csfworld CORPUS_ROOT OUT_DIR

Writes OUT_DIR/hello.ops from scene.py's own data: every record and script
comes from an editor operation or preset, none from raw script text. examples/hello-world/parity.sh runs it with `csf-mod mission-ops` and
checks the result against the scene.py build, file for file.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scene as hw  # noqa: E402


def position(x: float, z: float, lift: float = 0.0) -> str:
    """scene.py's rounding: POS with one decimal."""
    return hw.pos(x, z, lift).strip("()").replace(" ", ",")


def rotation(degrees: float) -> str:
    return f"{math.radians(degrees):.6f}"


def points(values) -> str:
    return ";".join(values)


def quote(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    hw.TERRAIN = hw.Terrain(argv[1])
    corpus, out = Path(argv[2]).resolve(), Path(argv[3])
    out.mkdir(parents=True, exist_ok=True)
    ops = ["# hello world through the editor's operations (examples/hello-world/ops.py)", "new-mission"]
    ops += [f"import-class donor={quote(str(corpus / 'Ransom'))} class={hw.DOG_CLASS}",
            f"import-anim donor={quote(str(corpus / 'Ransom'))} anim=2383",
            f"import-anim donor={quote(str(corpus / 'Ransom'))} anim=2384",
            f"import-class donor={quote(str(corpus / 'Escape'))} class={hw.GHOST_CLASS}",
            f"import-class donor={quote(str(corpus / 'Escape'))} class=383"]

    # Navigation: the walking grid and the players' start points.
    grid, grid_links = hw.walk_grid()
    ops.append(f"nav-group id={hw.G_WALK} name=MALLA type=0 "
               f"points={points(position(x, z) for _, x, z in grid)} "
               f"links={','.join(f'{a}-{b}' for a, b in grid_links)}")
    start = [(1, 3700.0, -3300.0, 112.5), (2, 3500.0, -3500.0, 7.3)]
    ops.append(f"nav-group id={hw.G_START} name=COMMANDOS type=0 "
               f"points={points(f'{position(x, z)},{rotation(h)}' for _, x, z, h in start)}")
    ops.append(f"cover-group id={hw.G_COVER} name=Parapeto_Campamento "
               f"points={points(f'{position(x, z)},{rotation(90.0)}' for x, z in hw.COVER)}")
    # Scripts come in ID order (the program lists them so): each recipe adds its own.
    # Starting equipment, mission tips and the three objectives (recipes).
    ops += ["kit actor=1 weapons=16,102@100/100,4@100/100,64,37 select=4",
            "kit actor=2 weapons=50,102@100/100,45,44,37,70 disguise=23",
            f"equipment script={hw.S_WEAPONS} script-name=COMMANDOS_INI",
            f"tips tips=g200,g199 pos=0.115,0.25 script={hw.S_INIT} script-name=INIT_MISSION",
            f"objective n=1 kind=zone target={hw.ZONE_HOUSE} label=0900 done=0902 script={hw.S_ZONE} "
            "script-name=CASA_ALCANZADA",
            f"objective n=2 kind=kill target={hw.OFFICER} label=0901 done=0903 script={hw.S_OFFICER_DEAD} "
            "script-name=OFICIAL_MUERTO",
            f"objective n=3 kind=use target={hw.RADIO_GHOST} label=0904 done=0905 prompt=0906 secondary=1 "
            f"script={hw.S_RADIO} script-name=RADIO_SABOTEADA",
            f"objectives setup={hw.S_OBJECTIVES} setup-name=INIT_OBJETIVOS success=g014 pause=4"]

    actors = {record[0]: record for record in hw.ACTORS}
    _, name, cls, x, z, heading, _, _, extra = actors[hw.OFFICER]
    portrait = extra[0].split('"')[1].replace("\\\\", "\\")
    ops.append(f"guard-idle id={hw.OFFICER} name={name} class={cls} pos={position(x, z)} heading={heading} "
               f"portrait={quote(portrait)} cover={hw.G_COVER} script={hw.S_OFFICER_IDLE} "
               f"script-name=OFICIAL_FUMAR loop=1881:2.0-4.0,1385")
    for ident, route_id, route_name, route, pause, script_id, script_name in (
            (hw.GUARD_CAMP, hw.G_CAMP_ROUTE, "RUTA_CAMPAMENTO", hw.CAMP_ROUTE, 3.0, hw.S_CAMP_PATROL,
             "GE_RUTA_CAMPAMENTO"),
            (hw.GUARD_NORTH, hw.G_NORTH_ROUTE, "RUTA_NORTE", hw.NORTH_ROUTE, 4.0, hw.S_NORTH_PATROL, "GE_RUTA_NORTE")):
        _, name, cls, _, _, heading, _, _, _ = actors[ident]
        ops.append(f"guard-patrol id={ident} name={name} class={cls} heading={heading} route={route_id} "
                   f"route-name={route_name} points={points(position(px, pz) for px, pz in route)} "
                   f"pause={pause} cover={hw.G_COVER} script={script_id} script-name={script_name}")
    _, name, cls, x, z, heading, _, _, _ = actors[hw.GUARD_HOUSE]
    ops.append(f"guard-idle id={hw.GUARD_HOUSE} name={name} class={cls} pos={position(x, z)} heading={heading} "
               f"cover={hw.G_COVER} script={hw.S_HOUSE_IDLE} script-name=GE_RADIO loop=1782")
    _, name, cls, x, z, heading, _, _, _ = actors[hw.DOG]
    ops.append(f"animal-patrol id={hw.DOG} name={name} class={cls} pos={position(x, z)} heading={heading} "
               f"route={hw.G_DOG_ROUTE} route-name=RUTA_PERRO "
               f"points={points(position(px, pz) for px, pz in hw.DOG_ROUTE)} walk={hw.DOG_WALK} "
               f"script={hw.S_DOG} script-name=PERRO_RUTA")
    # The travelling-camera intro (scene.py's SHOTS), with its exact rounded
    # angles and speeds.
    for k, (_, camera, target, seconds) in enumerate(hw.SHOTS):
        x, z, height = camera
        dx, dz = hw.SHOT_TRAVEL[k]
        heading = math.degrees(math.atan2(dx, dz))
        end_lift = hw.ground(x, z) + height - hw.ground(x + dx, z + dz)
        yaw, pitch = hw.look_at(camera, target)
        speed = math.hypot(dx, dz) / seconds
        ops.append(f"shot camera={position(x, z, height)} end={position(x + dx, z + dz, end_lift)} "
                   f"target={position(*target)} seconds={seconds} aim={yaw:.6f},{pitch:.6f} "
                   f"heading={rotation(heading)} speed={speed:.6f}")
    ops.append(f"intro class={hw.CAMERA_CLASS} dummy={hw.SHOTS[0][0]} actor=100 group=100 script={hw.S_INTRO} "
               "script-name=CUTSCENE_INICIO cutscene=1,2,3,28 cutscene-name=CUT_INICIO")

    # Players on their start points, the radio, the camp props and the camera helpers.
    for ident, point in ((hw.SNIPER, 2), (hw.SPY, 1)):
        _, name, cls, x, z, heading, _, _, _ = actors[ident]
        ops.append(f"actor id={ident} name={name} class={cls} pos={position(x, z)} heading={heading} "
                   f"cell={hw.G_START}/{point}")
    _, name, cls, x, z, heading, _, _, _ = actors[hw.RADIO_GHOST]
    ops.append(f"prop id={hw.RADIO_GHOST} name={name} class={cls} pos={position(x, z, hw.RADIO_LIFT)} "
               f"heading={heading}")
    for ident, name, cls, x, z, heading, _, _, _ in hw.prop_actors():
        ops.append(f"prop id={ident} name={name} class={cls} pos={position(x, z)} heading={heading}")
    ops.append(f"look actor={hw.RADIO_GHOST} class=211")

    # Routes join the walking grid; zones.
    for group in (hw.G_START, hw.G_CAMP_ROUTE, hw.G_NORTH_ROUTE):
        ops.append(f"link-nearest group={group} target={hw.G_WALK}")
    ops.append(f"area id={hw.ZONE_HOUSE} name=ZONA_CASA height=400.0 "
               "points=-3400.0,0.0,-3350.0;-1600.0,0.0,-3350.0;-1600.0,0.0,-1650.0;-3400.0,0.0,-1650.0")
    ops.append(f"area id={hw.AREA_TOTAL} name=TOTAL height=3000.0 "
               "points=-5500.0,-500.0,-5500.0;5500.0,-500.0,-5500.0;5500.0,-500.0,5500.0;-5500.0,-500.0,5500.0")
    (out / "hello.ops").write_text("\n".join(ops) + "\n", encoding="utf-8")
    print(f"ops\t{out / 'hello.ops'}\t{len(ops)} lines")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
