#!/usr/bin/env python3
"""Write the Country mission as editor operations (include/csf/mission_ops.hpp).

    examples/country/ops.py CORPUS_ROOT CONVOY_GSC_TEXT RANSOM_GSC_TEXT OUT_DIR [FIRST_TEXT]

The two texts are `csf-mod decompile` output of Convoy's and Ransom's mission
programs (Ransom's cow script is pulled from it); FIRST_TEXT is the first ID
of the project's text range (default 1000), which project.py's strings are
offsets into. Writes OUT_DIR/country.ops
and the hand-written scripts beside it; run it with `csf-mod mission-ops
<workspace> <Convoy.scn> country.ops --components --ground <terrain.csfworld>`.

The mission (examples/country/README.md):
- 35 German soldiers: guards standing or working at their posts (radio and
  telephone operators at their tables, men inside the farmhouse and the
  house, mechanics at the Panzer, a lookout, an MG nest, a barrier guard),
  seven patrols and a dog.
- The Gestapo officer arrives 45 s after INIT (about 30 s after the intro)
  by car with his driver and escort, a lorry with four soldiers behind them,
  shown in a picture-in-picture view (Convoy's VIEWPORT_* pattern); they get
  out in the square and take their posts.
- A cutscene of the officer when the player first reaches the village,
  armed only once he stands in the square (`arm=`).
- The alarm sounds when a living German sees a dead one (a body found), not
  when a guard is alerted: there is no "body found" event, so a watcher
  checks every second (ESTA_VIVO, VEO_BICHO).

What comes from where:
- Convoy (the slot): soldiers 3/13/21/23/142, the Kubelwagen 95, the lorry 42
  (with its headlights and engine sound), the camp radio model 211, the MP40
  pickup 105, the woodpile 335, the invisible camera actor 197; smoking, map,
  pissing, telephone and attention idles.
- Escape: the Gestapo officer 20, the telephone ghost 484 (the radio's use
  point, with 211's look, and the telephone), the crate 383, the map 198.
- Gestapo: the Gestapo agent 19, the barrel 473, the barrier 201, the MG
  mount 200, the motorcycle 264; the radio operator, drunk, hammering,
  clipboard, sweeping, map-on-table, dozing, flashlight, mechanic,
  crate-search and warming-at-the-fire idles.
- Ransom: cows 239 with their idles and INIT_VACAS script, the doberman 431,
  the tractor 290, the sandbag nests 199 and 337, the fire barrel 213, the
  barbed wire 278, the binocular lookout idle.
- Assault: yawning, rifle talking pair, talking; Bridge: the horse 30 and the
  small crate 384; Panzers: the field kitchen 153.
"""

from __future__ import annotations

import math
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout import (ARRIVAL_ROUTE, BARN, FARMHOUSE, FARMHOUSE_FLOOR, FARMHOUSE_SPOTS, HOUSE_FLOOR,  # noqa: E402
                    HOUSE_SPOTS, KUBEL_PARK, KUBEL_START, LORRY_PARK, LORRY_START, PADDOCK,
                    PHONE_ON_TABLE, PHONE_OPERATOR, PHONE_TABLE_TOP, PIECES, RADIO_DOOR, RADIO_FLOOR, RADIO_ON_TABLE,
                    RADIO_OPERATOR, RADIO_READER, RADIO_TABLE_TOP, RADIO_UPSTAIRS, RADIO_WINDOW, RUIN_FLOOR, SIZE,
                    TANK_AT, WATER_TOWER, height)

# ---- IDs -------------------------------------------------------------------------
SNIPER, SPY = 1, 2  # Convoy's players keep their IDs
# The officer's party (in the car and the lorry until they arrive).
OFFICER, KUBEL_DRIVER, ESCORT, LORRY_DRIVER, LORRY_A, LORRY_B, LORRY_C = 10, 11, 12, 13, 14, 15, 16
# Posts: in the square, inside the buildings, at the checkpoint and the workshop, round the farm.
WELL_A, WELL_B, RADIO_OP, RADIO_MAP, RADIO_TORCH, DOOR_GUARD, PHONE_OP = 17, 18, 19, 20, 21, 22, 23
STORE_A, STORE_B, HOUSE_A, HOUSE_B, FIRE_GUARD = 24, 25, 26, 27, 28
LOOKOUT, MG_GUARD, BARRIER_GUARD, MECHANIC_A, MECHANIC_B, FOREMAN = 29, 30, 31, 32, 33, 34
PISSER, SENTRY, DRUNK = 35, 36, 37
PATROLS = list(range(40, 47))
DOG = 47
COWS = [50, 51, 52, 53, 54, 55]
HORSE = 56
KUBEL, LORRY, TRACTOR, RADIO, PHONE, PICKUP, MAP_PROP = 60, 61, 62, 63, 64, 65, 66
FIRST_PROP = 70
CAMERA_ACTORS, CUTSCENE_ACTORS = 100, 120  # two per shot
GERMANS = [OFFICER, KUBEL_DRIVER, ESCORT, LORRY_DRIVER, LORRY_A, LORRY_B, LORRY_C, WELL_A, WELL_B, RADIO_OP, RADIO_MAP,
           RADIO_TORCH, DOOR_GUARD, PHONE_OP, STORE_A, STORE_B, HOUSE_A, HOUSE_B, FIRE_GUARD, LOOKOUT, MG_GUARD,
           BARRIER_GUARD, MECHANIC_A, MECHANIC_B, FOREMAN, PISSER, SENTRY, DRUNK, *PATROLS]

G_WALK, G_START, G_ROUTE, G_ARRIVE, G_COVER = 1, 2, 3, 4, 20
ZONE_VILLAGE, AREA_TOTAL = 6, 16
PIP_DUMMIES = (50, 51, 52, 53)  # the road (camera, target), the square (camera, target)
# Hand-written scripts (the recipes number theirs from 9530 on).
S_ALARM, S_COWS, S_SETUP, S_ARRIVAL = 9500, 9501, 9502, 9503
S_PARTY = 9510  # one per member of the officer's party
ARRIVED, IN_SQUARE = "OFICIAL_LLEGA", "OFICIAL_EN_PLAZA"

GHOST_CLASS, CRATE_CLASS, RADIO_LOOK = 484, 383, 211
KUBEL_CLASS, LORRY_CLASS = 95, 42


TEXT_BASE = 1000


def fli(offset: int) -> str:
    """A project string's ID (project.py's STRINGS offsets)."""
    return f"{TEXT_BASE + offset:04d}"


def quote(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def at(x: float, z: float, lift: float = 0.0) -> str:
    return f"{x:.1f},{height(x, z) + lift:.1f},{z:.1f}"


def inside(x: float, z: float, floor: float) -> str:
    return f"{x:.1f},{floor:.1f},{z:.1f}"


def route(points) -> str:
    return ";".join(at(x, z) for x, z in points)


def facing(x: float, z: float, tx: float, tz: float) -> float:
    """Heading in degrees looking from (x, z) at (tx, tz): forward is (sin, cos)."""
    return round(math.degrees(math.atan2(tx - x, tz - z)), 1)


def nav_point(x: float, z: float, heading: float = 0.0) -> str:
    return f"{at(x, z)},{math.radians(heading):.6f}"


def pull_script(text: str, script_id: int, new_id: int, edit=lambda s: s) -> str:
    """A script record of a decompiled program, renumbered (its own TRIGGER_OFF too)."""
    for block in re.findall(r"^    \[\n(.*?)^    \]\n", text, re.M | re.S):
        if re.search(rf"^      \.ID {script_id}\n", block, re.M):
            record = "[\n" + "\n".join(line[4:] for line in block.splitlines()) + "\n]\n"
            record = record.replace(f".ID {script_id}\n", f".ID {new_id}\n", 1)
            record = record.replace(f"(TRIGGER {script_id})", f"(TRIGGER {new_id})")
            return edit(record)
    raise SystemExit(f"script {script_id} not found")


def script(ident: int, name: str, actions: list[str], events: list[str] | None = None, trigger: bool = True,
           variables: list[tuple[int, str, str, str]] | None = None) -> str:
    """A script record as `csf-mod decompile` writes one; `actions` are lines,
    indented two spaces per IF/WHILE level already."""
    lines = ["[", f"  .ID {ident}", f"  .NOMBRE {name}", '  .CARPETA ""', "  .FLAGS [",
             f"    .TRIGGER {1 if trigger else 0}", "    .ENABLED 1", "    .VALIDO 1", "  ]"]
    if events:
        lines += ["  .EVENTOS ("] + [f"    ({event})" for event in events] + ["  )"]
    if variables:
        lines.append("  .VARIABLES (")
        for vid, kind, vname, value in variables:
            lines += ["    [", f"      .ID {vid}", f"      .TYPE {kind}", f"      .NOMBRE {vname}",
                      f"      .VALOR {value}", "    ]"]
        lines.append("  )")
    lines += ["  .ACCIONES {"] + [f"    {line}" for line in actions] + ["  }", "]", ""]
    return "\n".join(lines)


def bicho(ident: int) -> str:
    return f"(BICHO {ident})"


# ---- The alarm: a body found -----------------------------------------------------
def alarm_script() -> str:
    """Every second, for each dead German, whether a living one sees him."""
    actions = ["WHILE (NOT (VAR 90))", "  PAUSE (NUMERO 1.0)"]
    for dead in GERMANS:
        actions.append(f"  IF (NOT (ESTA_VIVO {bicho(dead)}))")
        for finder in GERMANS:
            if finder == dead:
                continue
            actions += [f"    IF (AND (ESTA_VIVO {bicho(finder)}) (VEO_BICHO {bicho(finder)} {bicho(dead)}))",
                        "      SET (VAR 90) (BOOL TRUE)", f"      SET (VAR 91) {bicho(finder)}", "    ENDIF"]
        actions.append("  ENDIF")
    actions += ["WEND",
                "CREA_ESTIMULO_ACUSTICO (REACTIVIDAD AMENAZA_DIRECTA) (GET_PATHPOINT (VAR 91)) (NUMERO 20000.0) "
                "(NUMERO 5.0)",
                "ACTIVAR_ALARMA (NUMERO 60.0)",
                f'TIMED_STRING_V2 (FLI "{fli(8)}") (NUMERO 5.0) (NUMERO 4.0)']
    return script(S_ALARM, "CADAVER_ENCONTRADO", actions, ["INIT"],
                  variables=[(90, "BOOL", "Encontrado", "FALSE"), (91, "BICHO", "Descubridor", "0")])


# ---- The officer's arrival ---------------------------------------------------------
PARTY = [  # actor, class, vehicle, seat
    (KUBEL_DRIVER, 19, KUBEL, 0), (OFFICER, 20, KUBEL, 1), (ESCORT, 19, KUBEL, 2),
    (LORRY_DRIVER, 3, LORRY, 0), (LORRY_A, 13, LORRY, 3), (LORRY_B, 21, LORRY, 4), (LORRY_C, 23, LORRY, 5),
]
# Where each gets to after getting out (G_ARRIVE points), facing, and what he does there.
OFFICER_SPOT = (-650.0, -1700.0)
ARRIVE_POINTS = {
    OFFICER: (OFFICER_SPOT, facing(*OFFICER_SPOT, -200.0, -1900.0)),
    ESCORT: ((-420.0, -1420.0), facing(-420.0, -1420.0, *OFFICER_SPOT)),
    KUBEL_DRIVER: ((900.0, -1900.0), 180.0),
    LORRY_DRIVER: ((2600.0, -1750.0), 180.0),
    LORRY_A: ((2200.0, -2650.0), facing(2200.0, -2650.0, 2450.0, -2800.0)),
    LORRY_B: ((2450.0, -2800.0), facing(2450.0, -2800.0, 2200.0, -2650.0)),
    LORRY_C: ((-1200.0, -2700.0), 90.0),
}
SQUARE_BEAT = (1500.0, -2750.0)  # LORRY_C walks between his point and this one
PARTY_LOOPS = {
    OFFICER: ["PLAY_ANMBDD_CICLOS (THIS) (ANM_BDD 1941) (RANDOM (NUMERO 2.0) (NUMERO 3.0))", "PLAY_ANMBDD (THIS) (ANM_BDD 1881)"],
    ESCORT: ["PLAY_ANMBDD (THIS) (ANM_BDD 2316)"],
    KUBEL_DRIVER: ["PLAY_ANMBDD_CICLOS (THIS) (ANM_BDD 1881) (RANDOM (NUMERO 2.0) (NUMERO 4.0))",
                   "PLAY_ANMBDD (THIS) (ANM_BDD 1385)"],
    LORRY_DRIVER: ["PLAY_ANMBDD_CICLOS (THIS) (ANM_BDD 1934) (RANDOM (NUMERO 3.0) (NUMERO 5.0))",
                   "PLAY_ANMBDD (THIS) (ANM_BDD 1385)"],
    LORRY_A: ["PLAY_ANMBDD (THIS) (ANM_BDD 2046)"],
    LORRY_B: ["PLAY_ANMBDD (THIS) (ANM_BDD 2047)"],
    LORRY_C: [f"IR_A_PATHPOINT (THIS) (PATHPOINT {G_ARRIVE} 8)", "PAUSE (NUMERO 4.0)",
              f"IR_A_PATHPOINT (THIS) (PATHPOINT {G_ARRIVE} 7)", "PAUSE (NUMERO 4.0)"],
}
ARRIVE_ORDER = [OFFICER, ESCORT, KUBEL_DRIVER, LORRY_DRIVER, LORRY_A, LORRY_B, LORRY_C]  # point numbers 1..7


def setup_script() -> str:
    """At the start: the picture-in-picture view (hidden), everyone in his seat, all out of sight."""
    actions = ["VIEWPORT_CREATE (NUMERO 1.0) (VECTOR 0.7 0.25 0.0) (VECTOR 0.925 0.475 0.0)",
               "VIEWPORT_CAM_SETFOV (NUMERO 1.0) (NUMERO 70.0)", "VIEWPORT_SHOW (NUMERO 1.0) (BOOL FALSE)"]
    for actor, _, vehicle, seat in PARTY:
        actions.append(f"LINK_A_HABITACULO {bicho(actor)} {bicho(vehicle)} (NUMERO {seat:.1f})")
    for ident in [KUBEL, LORRY] + [actor for actor, *_ in PARTY]:
        actions.append(f"SET_INVISIBLE {bicho(ident)} (BOOL TRUE)")
    return script(S_SETUP, "LLEGADA_INI", actions, ["START_GAME"])


def arrival_script() -> str:
    """After INIT and a wait: the car and the lorry drive in, seen in the corner of the screen."""
    road_cam, road_target, square_cam, square_target = PIP_DUMMIES
    actions = ["PAUSE (NUMERO 45.0)"]
    for ident in [KUBEL, LORRY] + [actor for actor, *_ in PARTY]:
        actions.append(f"SET_INVISIBLE {bicho(ident)} (BOOL FALSE)")
    actions += [f"EFFECTO_DUMMY_BICHO {bicho(LORRY)} (DUMMY_BICHO 220 Glow01) (EFECTO_CLASSID 166)",
                f"EFFECTO_DUMMY_BICHO {bicho(LORRY)} (DUMMY_BICHO 221 Glow02) (EFECTO_CLASSID 166)",
                f"EFFECTO_DUMMY_BICHO {bicho(LORRY)} (DUMMY_BICHO 222 luz) (EFECTO_CLASSID 161)",
                f'TIMED_STRING_V2 (FLI "{fli(11)}") (NUMERO 5.0) (NUMERO 4.0)',
                f"VIEWPORT_CAM_SETPOS (NUMERO 1.0) (DUMMY {road_cam}) (DUMMY {road_target})",
                "VIEWPORT_SETICON (NUMERO 1.0) (NUMERO 4.0)",
                "VIEWPORT_SHOW (NUMERO 1.0) (BOOL TRUE)",
                f"CONTINUE (IR_A_PATHPOINT {bicho(KUBEL)} (PATHPOINT {G_ROUTE} {KUBEL_PARK}))",
                "PAUSE (NUMERO 2.5)",
                f"PLAY_SONIDOID_BICHO (SONIDO_BDD 255) {bicho(LORRY)} (NUMERO 0.0) (NUMERO 0.0)",
                f"CONTINUE (IR_A_PATHPOINT {bicho(LORRY)} (PATHPOINT {G_ROUTE} {LORRY_PARK}))",
                "PAUSE (NUMERO 9.0)",
                f"VIEWPORT_CAM_SETPOS (NUMERO 1.0) (DUMMY {square_cam}) (DUMMY {square_target})",
                f"WHILE (ESTA_YENDO_PUNTO {bicho(KUBEL)})", "  PAUSE (NUMERO 0.5)", "WEND",
                f"SEND_EVENT (EVENT {ARRIVED})",
                "PAUSE (NUMERO 8.0)",
                f'TIMED_STRING_V2 (FLI "{fli(12)}") (NUMERO 5.0) (NUMERO 4.0)',
                "VIEWPORT_SHOW (NUMERO 1.0) (BOOL FALSE)"]
    return script(S_ARRIVAL, "LLEGADA_OFICIAL", actions, ["INIT"])


def party_script(k: int, actor: int, vehicle: int) -> str:
    """One member of the party: out when his vehicle stops, to his post, then his loop."""
    point = ARRIVE_ORDER.index(actor) + 1
    actions = [f"WHILE (ESTA_YENDO_PUNTO {bicho(vehicle)})", "  PAUSE (NUMERO 0.5)", "WEND",
               f"PAUSE (NUMERO {0.4 * k:.1f})", "BAJAR_DE_HABITACULO (THIS)", "PAUSE (NUMERO 1.0)",
               f"IR_A_PATHPOINT_ORIENT (THIS) (PATHPOINT {G_ARRIVE} {point}) (BOOL TRUE)"]
    if actor == OFFICER:
        actions.append(f"SEND_EVENT (EVENT {IN_SQUARE})")
    actions += [f"SELECT_GRUPO_PARAPETO (THIS) (GRUPO_PATHPOINT {G_COVER})",
                "SET_IA_ALERTA (THIS) (IA_ALERTA MOVIL_A_PARAPETO)", "SET_IA_COMBATE (THIS) (IA_COMBATE MOVIL_A_PARAPETO)",
                "WHILE (BOOL TRUE)"] + [f"  {line}" for line in PARTY_LOOPS[actor]] + ["WEND"]
    names = {OFFICER: "OFICIAL", ESCORT: "ESCOLTA", KUBEL_DRIVER: "CONDUCTOR_KUBEL", LORRY_DRIVER: "CONDUCTOR_CAMION",
             LORRY_A: "CAMION_SOLDADO_1", LORRY_B: "CAMION_SOLDADO_2", LORRY_C: "CAMION_SOLDADO_3"}
    return script(S_PARTY + k, f"LLEGADA_{names[actor]}", actions, [ARRIVED], trigger=False)


def look(camera, target) -> tuple[float, float]:
    dx, dy, dz = target[0] - camera[0], target[1] - camera[1], target[2] - camera[2]
    return math.atan2(dx, dz), math.atan2(-dy, math.hypot(dx, dz))


def main(argv: list[str]) -> int:
    global TEXT_BASE
    if len(argv) not in (5, 6):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    corpus, out = Path(argv[1]).resolve(), Path(argv[4])
    TEXT_BASE = int(argv[5]) if len(argv) == 6 else 1000
    ransom = Path(argv[3]).read_text(encoding="utf-8")
    out.mkdir(parents=True, exist_ok=True)

    # Hand-written and pulled scripts. Ransom's cows are also usable (for its
    # cow cutscene); ours only graze.
    scripts = {"alarm.gsc.txt": alarm_script(), "setup.gsc.txt": setup_script(), "arrival.gsc.txt": arrival_script(),
               "cows.gsc.txt": pull_script(ransom, 93, S_COWS, lambda s: s.replace(
                   "    HABILITAR_GHOST (THIS) (BOOL TRUE)\n", ""))}
    for k, (actor, _, vehicle, _) in enumerate(PARTY):
        scripts[f"party{k}.gsc.txt"] = party_script(k, actor, vehicle)
    for name, text in scripts.items():
        (out / name).write_text(text, encoding="utf-8")

    def donor(mission: str) -> str:
        return quote(str(corpus / mission))

    # No new-mission: `csf-mod project-new` already emptied the slot's mission.
    ops = ["# Country (examples/country/ops.py): Convoy's slot, assembled from other missions"]
    # Classes and animations from other missions.
    for mission, classes in (("Escape", [20, GHOST_CLASS, CRATE_CLASS, 198]), ("Gestapo", [19, 473, 201, 200, 264]),
                             ("Ransom", [239, 431, 290, 199, 337, 213, 278]), ("Bridge", [30, 384]),
                             ("Panzers", [153])):
        ops += [f"import-class donor={donor(mission)} class={c}" for c in classes]
    for mission, anims in (("Gestapo", [1949, 1950, 1763, 1361, 1437, 1900, 1429, 1425, 1973, 1831, 1832, 1333, 1366]),
                           ("Ransom", [2034, 2030, 2383, 2384, 1374]), ("Assault", [1421, 2046, 2047, 2316])):
        ops += [f"import-anim donor={donor(mission)} anim={a}" for a in anims]

    # Hand-written scripts first, with their own IDs.
    ops += [f"script file={name}" for name in scripts]

    # Navigation: a walking grid round the buildings, the players' start, cover.
    solid = [piece for piece in PIECES if (piece.box[3] - piece.box[0]) * (piece.box[5] - piece.box[2]) > 40000]
    excluded = ";".join(",".join(f"{v:.0f}" for v in piece.footprint(150.0)) for piece in solid)
    ops.append(f"walk-grid id={G_WALK} name=MALLA spacing=800 x={-SIZE / 2 + 1000:.0f}..{SIZE / 2 - 1000:.0f} "
               f"z={-SIZE / 2 + 1000:.0f}..{SIZE / 2 - 1000:.0f} exclude={excluded}")
    start = [(200.0, -7000.0, 0.0), (500.0, -7150.0, 0.0)]
    ops.append(f"nav-group id={G_START} name=COMMANDOS type=0 points={';'.join(nav_point(x, z, h) for x, z, h in start)}")
    ops.append(f"nav-group id={G_ROUTE} name=RUTA_LLEGADA type=0 "
               f"points={';'.join(nav_point(x, z, -90.0) for x, z in ARRIVAL_ROUTE)} links=chain")
    arrive = [ARRIVE_POINTS[a] for a in ARRIVE_ORDER] + [(SQUARE_BEAT, 270.0)]
    ops.append(f"nav-group id={G_ARRIVE} name=PUESTOS_LLEGADA type=0 "
               f"points={';'.join(nav_point(x, z, h) for (x, z), h in arrive)}")

    # Props (actors of shipped classes): sandbag nests, crates, barrels, the workshop's gear.
    props = [  # class, name, x, z, heading
        (199, "NIDO_CONTROL", -500.0, -4750.0, 180.0), (200, "MG42_CONTROL", -500.0, -4550.0, 180.0),
        (201, "BARRERA_CONTROL", 300.0, -4450.0, 0.0), (337, "SACOS_CONTROL", 1250.0, -4500.0, 175.0),
        (278, "ALAMBRADA_1", -1500.0, -4650.0, 10.0), (278, "ALAMBRADA_2", 2300.0, -4250.0, 350.0),
        (199, "NIDO_TALLER", 6300.0, -2600.0, 90.0), (337, "SACOS_PLAZA", -1800.0, -2500.0, 90.0),
        (337, "SACOS_GRANJA", 4700.0, 3500.0, 0.0),
        (CRATE_CLASS, "CAJA_TALLER_1", 4000.0, -3950.0, 15.0), (CRATE_CLASS, "CAJA_TALLER_2", 4110.0, -3960.0, 40.0),
        (384, "CAJA_TALLER_3", 4060.0, -4080.0, 5.0), (384, "CAJA_TALLER_4", 2800.0, -2750.0, 30.0),
        (473, "BIDON_TALLER_1", 4700.0, -2620.0, 0.0), (473, "BIDON_TALLER_2", 4790.0, -2650.0, 0.0),
        (473, "BIDON_TALLER_3", 4745.0, -2560.0, 0.0), (473, "BIDON_TANQUE_1", 3000.0, -3850.0, 0.0),
        (473, "BIDON_TANQUE_2", 3080.0, -3900.0, 0.0), (153, "COCINA_CAMPANA", 6300.0, -2750.0, 270.0),
        (213, "HOGUERA_PLAZA", -700.0, -2750.0, 0.0), (213, "HOGUERA_TALLER", 2700.0, -3100.0, 0.0),
        (335, "LENA_GRANJA", 4300.0, 1900.0, 90.0), (264, "MOTO_PLAZA", -1500.0, -2100.0, 200.0),
        (CRATE_CLASS, "CAJA_PLAZA_1", 1300.0, -2750.0, 20.0), (384, "CAJA_PLAZA_2", 1400.0, -2850.0, 70.0),
        (473, "BIDON_GRANJA", 4400.0, 2600.0, 0.0), (CRATE_CLASS, "CAJA_DEPOSITO", -4000.0, -1500.0, 10.0),
    ]
    for k, (cls, name, x, z, heading) in enumerate(props):
        ops.append(f"prop id={FIRST_PROP + k} name={name} class={cls} pos={at(x, z)} heading={heading}")
    # Cover: behind the sandbags and in the square.
    cover = []
    for cls, _, x, z, heading in props:
        if cls in (199, 337):
            back = math.radians(heading + 180.0)
            for side in (-120.0, 120.0):
                along = math.radians(heading + 90.0)
                cx = x + 170.0 * math.sin(back) + side * math.sin(along)
                cz = z + 170.0 * math.cos(back) + side * math.cos(along)
                cover.append((cx, cz, heading))
    ops.append(f"cover-group id={G_COVER} name=Parapeto_Pueblo "
               f"points={';'.join(nav_point(x, z, h) for x, z, h in cover)}")

    # Mission text, tips, kits and objectives.
    ops += ["kit actor=1 weapons=16,102@100/100,4@100/100,64,37 select=4",
            "kit actor=2 weapons=50,102@100/100,45,44,37,70 disguise=23",
            "equipment script-name=COMMANDOS_INI",
            f"tips tips={fli(9)},{fli(10)} script-name=INIT_MISSION",
            f"objective n=1 kind=kill target={OFFICER} label={fli(0)} done={fli(1)} script-name=OFICIAL_GESTAPO_MUERTO",
            f"objective n=2 kind=use target={RADIO} label={fli(2)} done={fli(3)} prompt={fli(4)} "
            "script-name=RADIO_SABOTEADA",
            f"objective n=3 kind=use target={PHONE} label={fli(5)} done={fli(6)} prompt={fli(7)} secondary=1 "
            "script-name=TELEFONO_CORTADO",
            "objectives setup-name=INIT_OBJETIVOS success=g014 pause=4"]

    # The officer's party and their vehicles, on the road beyond the east fields.
    kubel_at, lorry_at = ARRIVAL_ROUTE[KUBEL_START - 1], ARRIVAL_ROUTE[LORRY_START - 1]
    ops.append(f"actor id={KUBEL} name=KUBEL_OFICIAL class={KUBEL_CLASS} pos={at(*kubel_at)} heading=-90 "
               f"cell={G_ROUTE}/{KUBEL_START}")
    ops.append(f"actor id={LORRY} name=CAMION_ESCOLTA class={LORRY_CLASS} pos={at(*lorry_at)} heading=-90 "
               f"cell={G_ROUTE}/{LORRY_START}")
    names = {OFFICER: "OFICIAL_GESTAPO", KUBEL_DRIVER: "GESTAPO_CONDUCTOR", ESCORT: "GESTAPO_ESCOLTA",
             LORRY_DRIVER: "GE_CONDUCTOR_CAMION", LORRY_A: "GE_CAMION_1", LORRY_B: "GE_CAMION_2", LORRY_C: "GE_CAMION_3"}
    for k, (actor, cls, vehicle, _) in enumerate(PARTY):
        x, z = kubel_at if vehicle == KUBEL else lorry_at
        portrait = f" portrait={quote('Menus\\Retratos\\FotoGestapo.fbs')}" if actor == OFFICER else ""
        ops.append(f"actor id={actor} name={names[actor]} class={cls} pos={at(x, z + 250.0 + 60.0 * k)} "
                   f"heading=-90{portrait} scripts={S_PARTY + k}")

    # The square: two guards talking at the well, one warming his hands at the fire barrel.
    well_a, well_b = (-1150.0, -900.0), (-900.0, -1050.0)
    ops.append(f"guard-idle id={WELL_A} name=GE_POZO_1 class=13 pos={at(*well_a)} heading={facing(*well_a, *well_b)} "
               f"cover={G_COVER} script-name=POZO_CHARLA_1 loop=2046")
    ops.append(f"guard-idle id={WELL_B} name=GE_POZO_2 class=21 pos={at(*well_b)} heading={facing(*well_b, *well_a)} "
               f"cover={G_COVER} script-name=POZO_CHARLA_2 loop=2047")
    fire = (-700.0, -2610.0)
    ops.append(f"guard-idle id={FIRE_GUARD} name=GE_HOGUERA class=13 pos={at(*fire)} heading=180 cover={G_COVER} "
               "script-name=HOGUERA_CALIENTA loop=1366")

    # The radio house: the radio on the ground floor's long table, its operator at
    # the end, a man reading the map at the other end, a torch upstairs, a smoker at the door.
    ops.append(f"prop id={RADIO} name=RADIO class={GHOST_CLASS} pos={inside(*RADIO_ON_TABLE, RADIO_TABLE_TOP)} heading=90")
    ops.append(f"look actor={RADIO} class={RADIO_LOOK}")
    ops.append(f"prop id={MAP_PROP} name=MAPA_MESA class=198 "
               f"pos={inside(RADIO_READER[0] + 180.0, RADIO_READER[1] - 30.0, RADIO_TABLE_TOP)} heading=80")
    ops.append(f"guard-idle id={RADIO_OP} name=GE_RADIO class=142 pos={inside(*RADIO_OPERATOR, RADIO_FLOOR)} "
               "heading=-90 script-name=OPERADOR_RADIO loop=1949:3.0-6.0,1950")
    ops.append(f"guard-idle id={RADIO_MAP} name=GE_RADIO_MAPA class=23 pos={inside(*RADIO_READER, RADIO_FLOOR)} "
               "heading=90 script-name=RADIO_MAPA loop=1429")
    ops.append(f"guard-idle id={RADIO_TORCH} name=GE_RADIO_ARRIBA class=3 pos={inside(*RADIO_WINDOW, RADIO_UPSTAIRS)} "
               "heading=200 script-name=RADIO_LINTERNA loop=1973")
    door_guard = (RADIO_DOOR[0] - 250.0, RADIO_DOOR[1] - 100.0)
    ops.append(f"guard-idle id={DOOR_GUARD} name=GE_PUERTA_RADIO class=23 pos={at(*door_guard)} heading=-90 "
               f"cover={G_COVER} script-name=PUERTA_RADIO_FUMA loop=1881:2.0-4.0,1385")
    # The ruin by the silo: the telephone on its table and a man on the line.
    ops.append(f"prop id={PHONE} name=TELEFONO class={GHOST_CLASS} pos={inside(*PHONE_ON_TABLE, PHONE_TABLE_TOP)} "
               "heading=180")
    ops.append(f"guard-idle id={PHONE_OP} name=GE_TELEFONO class=142 pos={inside(*PHONE_OPERATOR, RUIN_FLOOR)} "
               "heading=0 script-name=OPERADOR_TELEFONO loop=1782")
    # The farmhouse store and the house: searching the crates, counting them, sweeping, dozing.
    ops.append(f"guard-idle id={STORE_A} name=GE_ALMACEN_1 class=3 pos={inside(*FARMHOUSE_SPOTS[0], FARMHOUSE_FLOOR)} "
               "heading=90 script-name=ALMACEN_BUSCA loop=1333")
    ops.append(f"guard-idle id={STORE_B} name=GE_ALMACEN_2 class=23 pos={inside(*FARMHOUSE_SPOTS[1], FARMHOUSE_FLOOR)} "
               "heading=60 script-name=ALMACEN_CUENTA loop=1437")
    ops.append(f"guard-idle id={HOUSE_A} name=GE_CASA_DORMIDO class=21 pos={inside(*HOUSE_SPOTS[0], HOUSE_FLOOR)} "
               "heading=150 script-name=CASA_DORMITA loop=1425:3.0-6.0,1421")
    ops.append(f"guard-idle id={HOUSE_B} name=GE_CASA_BARRE class=3 pos={inside(*HOUSE_SPOTS[1], HOUSE_FLOOR)} "
               "heading=30 script-name=CASA_BARRE loop=1900")

    # The south checkpoint: a lookout with binoculars, the MG nest, a smoker at the barrier.
    lookout = (1000.0, -4750.0)
    ops.append(f"guard-idle id={LOOKOUT} name=GE_PRISMATICOS class=13 pos={at(*lookout)} heading=180 "
               f"cover={G_COVER} script-name=VIGIA_PRISMATICOS loop=1374")
    ops.append(f"guard-idle id={MG_GUARD} name=GE_NIDO class=21 pos={at(-500.0, -4380.0)} heading=180 "
               f"cover={G_COVER} script-name=NIDO_GUARDIA loop=1934")
    ops.append(f"guard-idle id={BARRIER_GUARD} name=GE_BARRERA class=23 pos={at(-150.0, -4300.0)} heading=150 "
               f"cover={G_COVER} script-name=BARRERA_FUMA loop=1881:2.0-4.0,1385")
    # The workshop: two mechanics at the Panzer, a foreman with his clipboard.
    tx, tz = TANK_AT
    ops.append(f"guard-idle id={MECHANIC_A} name=GE_MECANICO_1 class=3 pos={at(tx + 100.0, tz + 330.0)} heading=180 "
               "script-name=TANQUE_MECANICO loop=1831:2.0-4.0,1832")
    ops.append(f"guard-idle id={MECHANIC_B} name=GE_MECANICO_2 class=3 pos={at(tx - 400.0, tz - 60.0)} heading=90 "
               "script-name=TANQUE_MARTILLO loop=1361")
    foreman = (tx + 500.0, tz + 650.0)
    ops.append(f"guard-idle id={FOREMAN} name=GE_CAPATAZ class=23 pos={at(*foreman)} heading={facing(*foreman, tx, tz)} "
               f"cover={G_COVER} script-name=TALLER_CAPATAZ loop=1437")
    # Behind the barn, the water tower sentry, the drunk by the woodshed.
    bx0, bz0, bx1, bz1 = BARN.footprint()
    pisser = (bx1 + 300.0, (bz0 + bz1) / 2)
    ops.append(f"guard-idle id={PISSER} name=GE_MEANDO class=3 pos={at(*pisser)} heading=90 "
               f"script-name=GRANERO_MEA loop=1402")
    wx0, wz0, wx1, wz1 = WATER_TOWER.footprint()
    sentry = ((wx0 + wx1) / 2 + 200.0, wz1 + 300.0)
    ops.append(f"guard-idle id={SENTRY} name=GE_DORMIDO class=21 pos={at(*sentry)} heading=20 "
               f"cover={G_COVER} script-name=DEPOSITO_DORMITA loop=1425:3.0-6.0,1421")
    drunk = (900.0, -850.0)
    ops.append(f"guard-idle id={DRUNK} name=GE_BORRACHO class=3 pos={at(*drunk)} heading=200 "
               f"script-name=BORRACHO loop=1763")

    # Seven patrols, then the dog round the farmyard.
    px0, pz0, px1, pz1 = PADDOCK
    patrols = [
        ("GE_CONTROL", 21, [(800.0, -4800.0), (800.0, -5600.0), (-200.0, -5600.0), (-200.0, -4850.0)], "RUTA_CONTROL", 4.0),
        ("GE_PUEBLO", 13, [(-600.0, -2950.0), (-150.0, -700.0), (1100.0, 50.0), (1250.0, -950.0)], "RUTA_PUEBLO", 3.0),
        ("GE_GRANJA", 21, [(4300.0, -800.0), (4300.0, 1200.0), (4100.0, 3800.0), (400.0, 4600.0), (4100.0, 3800.0),
                           (4300.0, 1200.0)], "RUTA_GRANJA", 4.0),
        ("GE_DEPOSITO", 13, [(-900.0, -4000.0), (-3700.0, -4050.0), (-4600.0, -1300.0), (-4600.0, 1800.0),
                             (-4600.0, -1300.0), (-3700.0, -4050.0)], "RUTA_DEPOSITO", 5.0),
        ("GE_PRADO", 21, [(px0 - 300.0, pz0 - 300.0), (px1 + 300.0, pz0 - 300.0), (px1 + 300.0, pz1 + 300.0),
                          (px0 - 300.0, pz1 + 300.0)], "RUTA_PRADO", 3.0),
        ("GE_CAMPOS", 23, [(1000.0, -4250.0), (5000.0, -4450.0), (5100.0, -5900.0), (1000.0, -5900.0)], "RUTA_CAMPOS", 4.0),
        ("GE_TALLER", 3, [(2700.0, -2650.0), (6600.0, -2450.0), (6600.0, -4450.0), (2700.0, -4400.0)], "RUTA_TALLER", 3.0),
    ]
    first_route = 5
    for k, (name, cls, points, route_name, pause) in enumerate(patrols):
        ops.append(f"guard-patrol id={PATROLS[k]} name={name} class={cls} heading={facing(*points[0], *points[1])} "
                   f"route={first_route + k} route-name={route_name} points={route(points)} pause={pause} "
                   f"cover={G_COVER} script-name={name}_PATRULLA")
    dog_route = [(4600.0, 600.0), (6300.0, 600.0), (6300.0, 3000.0), (5900.0, 3050.0)]
    dog_group = first_route + len(patrols)
    ops.append(f"animal-patrol id={DOG} name=DOBERMAN class=431 pos={at(*dog_route[0])} heading=90 route={dog_group} "
               f"route-name=RUTA_PERRO points={route(dog_route)} walk=2383 script-name=PERRO_GRANJA")

    # The farm: cows in the paddock and two in the farmhouse stable (where Ransom keeps
    # them), a horse, the tractor; an MP40 by the south road.
    cows = [(px0 + 400, pz0 + 500, 40), (px0 + 900, pz0 + 1200, 200), (px1 - 500, pz0 + 400, 120),
            (px1 - 450, pz1 - 500, 300)]
    stable = [(*FARMHOUSE.moved(fx, fz), heading) for fx, fz, heading in
              ((9641.66, 13825.9, -161.8), (9818.44, 13724.1, -122.6))]
    for k, (x, z, heading) in enumerate(cows + stable):
        pos = at(x, z) if k < len(cows) else inside(x, z, FARMHOUSE_FLOOR)
        ops.append(f"actor id={COWS[k]} name=Vaca_{k + 1} class=239 pos={pos} heading={heading} scripts={S_COWS}")
    ops += [f"prop id={HORSE} name=Caballo class=30 pos={at(px0 + 700, pz1 - 700)} heading=70",
            f"prop id={TRACTOR} name=Tractor class=290 pos={at(3900.0, 3300.0)} heading=200",
            f"prop id={PICKUP} name=PICKUP_MP40 class=105 pos={at(900.0, -6200.0)} heading=0"]
    for ident, point in ((SNIPER, 2), (SPY, 1)):
        x, z, _ = start[point - 1]
        ops.append(f"actor id={ident} name={'Sniper' if ident == SNIPER else 'Spy'} "
                   f"class={54 if ident == SNIPER else 55} pos={at(x, z)} heading=0 cell={G_START}/{point}")
    for group in (G_START, G_ARRIVE, *range(first_route, dog_group + 1)):
        ops.append(f"link-nearest group={group} target={G_WALK}")

    # Zones: the village's south edge (the officer's cutscene) and the whole map.
    ops.append(f"area id={ZONE_VILLAGE} name=ZONA_PUEBLO height=800.0 "
               "points=-4200.0,0.0,-3700.0;4000.0,0.0,-3700.0;4000.0,0.0,-3000.0;-4200.0,0.0,-3000.0")
    half = SIZE / 2 - 200
    ops.append(f"area id={AREA_TOTAL} name=TOTAL height=3000.0 "
               f"points={-half},-500.0,{-half};{half},-500.0,{-half};{half},-500.0,{half};{-half},-500.0,{half}")

    # The picture-in-picture views: the east road, then the square.
    road_cam, road_target = (5000.0, 900.0, -900.0), (6800.0, 150.0, -1900.0)
    square_cam, square_target = (-800.0, 600.0, -3600.0), (600.0, 100.0, -2100.0)
    for ident, name, (camera, target) in ((PIP_DUMMIES[0], "PIP_CARRETERA", (road_cam, road_target)),
                                          (PIP_DUMMIES[2], "PIP_PLAZA", (square_cam, square_target))):
        yaw, pitch = look(camera, target)
        for dummy, point in ((ident, camera), (ident + 1, target)):
            ops.append(f"dummy id={dummy} name={name}_{'CAMARA' if dummy == ident else 'OBJETIVO'} "
                       f"pos={at(point[0], point[2], point[1])} rot={yaw:.6f} pitch={pitch:.6f}")

    # The intro: the checkpoint from the south road, the tank workshop, the radio house.
    for camera, end, target, seconds in (
            ((300.0, -7300.0, 1400.0), (300.0, -6300.0, 1100.0), (300.0, -2500.0, 200.0), 5.0),
            ((1800.0, -5000.0, 500.0), (2300.0, -4700.0, 450.0), (tx, tz, 120.0), 4.5),
            ((-300.0, -3600.0, 450.0), (0.0, -3300.0, 420.0), (*RADIO_DOOR, 250.0), 4.0)):
        ops.append(f"shot camera={at(camera[0], camera[1], camera[2])} end={at(end[0], end[1], end[2])} "
                   f"target={at(target[0], target[1], target[2])} seconds={seconds}")
    ops.append(f"intro actor={CAMERA_ACTORS} script-name=CUTSCENE_INICIO cutscene-name=CUT_INICIO")
    # Entering the village once the officer is there: him and his escort, then the Panzer.
    ox, oz = OFFICER_SPOT
    for camera, end, target, seconds in (
            ((ox - 700.0, oz - 1100.0, 220.0), (ox - 350.0, oz - 900.0, 200.0), (ox, oz, 150.0), 4.0),
            ((tx - 1200.0, tz + 900.0, 350.0), (tx - 800.0, tz + 1000.0, 330.0), (tx, tz, 120.0), 3.5)):
        ops.append(f"shot camera={at(camera[0], camera[1], camera[2])} end={at(end[0], end[1], end[2])} "
                   f"target={at(target[0], target[1], target[2])} seconds={seconds}")
    ops.append(f"intro actor={CUTSCENE_ACTORS} zone={ZONE_VILLAGE} script-name=CUT_OFICIAL "
               f"setup-name=CUT_OFICIAL_INI cutscene-name=CUT_OFICIAL arm={IN_SQUARE}")

    (out / "country.ops").write_text("\n".join(ops) + "\n", encoding="utf-8")
    print(f"ops\t{out / 'country.ops'}\t{len(ops)} lines, {len(GERMANS)} soldiers")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
