"""The Country map layout (examples/country/README.md), shared by terrain.py
(inside Blender), lightmaps.py, project.py and ops.py. Positions are game
units (cm): x east, z north, y up.

A 150 m square in Convoy's slot: a village of Ransom's (FR01) buildings in
the middle, a farm with a barn and a cow paddock to the north-east, a tank
workshop in an open shed to the south-east, a ploughed and a stubble field
to the south, a water tower to the west, and wooded hills closing the edges.
The players come up the south road through a checkpoint; the Gestapo officer
drives in along the east road.
"""

from __future__ import annotations

import math
import random

SIZE, CELLS = 15000.0, 100  # 1.5 m cells
EDGE = (2000.0, 650.0)      # width, height of the hills closing the map
LIFT = 4.0                  # a building's lowest floor stands this far above our ground

# ---- Buildings and props cut from other maps' Worlds ---------------------------
# A piece is anchored at the bottom centre of its box. `ref` is the donor
# height that lands on our ground: for a building the lower of its lowest
# floor (less LIFT) and the donor's ground around it, so the ground never
# shows through a floor (Ransom's own ground stands level with its floors);
# for a yard prop the donor ground under it. The village keeps FR01's own
# arrangement, moved by VILLAGE_SHIFT; the others are placed on their own.
# All stay at yaw 0 so their doors, floors and furniture move by a plain
# translation (`moved`, `y_of`).
VILLAGE_SHIFT = (-7900.0, -11800.0)
FR01_VILLAGE_GROUND = 1080.0


class Piece:
    def __init__(self, ident, groups, box, ref, at=None, donor="fr01", yaw=0.0):
        self.ident, self.groups, self.box, self.ref, self.donor, self.yaw = ident, groups, box, ref, donor, yaw
        x0, _, z0, x1, _, z1 = box
        centre = ((x0 + x1) / 2, (z0 + z1) / 2)
        self.at = at or (centre[0] + VILLAGE_SHIFT[0], centre[1] + VILLAGE_SHIFT[1])

    @property
    def sink(self) -> float:
        return self.box[1] - self.ref

    def footprint(self, margin: float = 0.0):
        """x0, z0, x1, z1 where the piece stands in Country (yaw 0)."""
        x0, _, z0, x1, _, z1 = self.box
        hx, hz = (x1 - x0) / 2 + margin, (z1 - z0) / 2 + margin
        return self.at[0] - hx, self.at[1] - hz, self.at[0] + hx, self.at[1] + hz

    def moved(self, x: float, z: float) -> tuple[float, float]:
        """A donor point of this piece (a door, a table) in Country."""
        x0, _, z0, x1, _, z1 = self.box
        return x - (x0 + x1) / 2 + self.at[0], z - (z0 + z1) / 2 + self.at[1]

    def y_of(self, y: float) -> float:
        """A donor height of this piece (a floor, a table top) in Country, over flat ground."""
        return y - self.ref


def building(ident, name, box, floor, ground, at=None, interior=True, props=True):
    groups = [name] + ([name + "_INTERIOR"] if interior else []) + (["ATREZZO_" + name] if props else [])
    return Piece(ident, groups, box, min(floor - LIFT, ground), at)


def prop(ident, groups, box, sink=5.0, at=None, donor="fr01", yaw=0.0):
    """A yard prop (a single object of a group, examples/country/catalog.py objects), sunk a little."""
    return Piece(ident, groups, box, box[1] + sink, at, donor, yaw)


# Boxes: the groups' bounds from examples/country/catalog.py map (30 cm wider)
# and objects; floors: the lowest walkable floor (the _INTERIOR group's).
FARMHOUSE = building("farmhouse", "EDIFICIO_1", (7423, 1070, 12685, 10757, 2123, 14820), 1080, FR01_VILLAGE_GROUND)
RUIN = building("ruin", "EDIFICIO_2", (3845, 967, 11716, 6858, 2714, 16062), 1037, FR01_VILLAGE_GROUND)
HOUSE = building("house", "EDIFICIO_4", (4075, 1053, 8377, 6534, 2542, 10999), 1079, FR01_VILLAGE_GROUND)
RADIO_HOUSE = building("radio-house", "EDIFICIO_5", (9379, 1073, 10480, 11797, 2811, 12334), 1078, FR01_VILLAGE_GROUND)
WALLS = Piece("yard-walls", ["MURO"], (5214, 1062, 12092, 11698, 1748, 15222), FR01_VILLAGE_GROUND)
WELL = Piece("well", ["ATREZZO_EXTERIOR_1"], (6917, 940, 11088, 7462, 1406, 11666), FR01_VILLAGE_GROUND)
WOODSHED = Piece("woodshed", ["ATREZZO_EXTERIOR_1"], (8508, 1000, 10127, 9132, 1342, 10739), 1036)  # on a slope in FR01
BARN = building("barn", "EDIFICIO_3", (499, 649, 12511, 1476, 1551, 13974), 686, 653, at=(5100.0, 2150.0),
                interior=False, props=False)
OPEN_SHED = building("open-shed", "EDIFICIO_9", (4921, 458, 4762, 5875, 993, 6119), 478, 505, at=(5100.0, -3500.0),
                     interior=False, props=False)
COW_SHED = building("cow-shed", "EDIFICIO_7", (1229, 490, 6272, 2513, 1026, 7039), 511, 511, at=(1900.0, 5150.0),
                    interior=False, props=False)
WATER_TOWER = building("water-tower", "EDIFICIO_8", (4504, 996, -4289, 5979, 2977, -2680), 1122, 1050,
                       at=(-4800.0, -2500.0), interior=False, props=False)
BUILDINGS = [FARMHOUSE, RUIN, HOUSE, RADIO_HOUSE, BARN, OPEN_SHED, COW_SHED, WATER_TOWER]

SQUARE = (200.0, -2300.0)  # where the south, east and west roads meet
HAND_CART = prop("hand-cart", ["ATREZZO_EXTERIOR_1"], (5110, 471, 4933, 5509, 690, 5163), 10.0, at=(4850.0, 1250.0))
PUMP = prop("pump", ["ATREZZO_EXTERIOR_1"], (6406, 1048, 12869, 6471, 1166, 12924), 3.0, at=(-200.0, -1900.0))
BALE_BOX = (380, 396, 3118, 616, 587, 3398)  # one of FR01's round hay bales
BALES = [prop(f"bale-{k + 1}", ["ATREZZO_EXTERIOR_2"], BALE_BOX, 33.0, at=at) for k, at in enumerate(
    [(2000.0, -4800.0), (3300.0, -5300.0), (4300.0, -4700.0), (6300.0, 3700.0), (6500.0, 4100.0)])]
# Czech hedgehogs (FR01's anti-tank obstacles) at the south checkpoint, either side of the road.
HEDGEHOG_BOXES = [(-4598, 641, 6078, -4370, 852, 6272), (-4426, 569, 6687, -4216, 783, 6898),
                  (-4043, 657, 8140, -3878, 869, 8349), (-3978, 551, 7176, -3769, 749, 7394)]
HEDGEHOGS = [prop(f"hedgehog-{k + 1}", ["ATREZZO_EXTERIOR_1"], box, 25.0, at=at) for k, (box, at) in enumerate(zip(
    HEDGEHOG_BOXES + HEDGEHOG_BOXES[:2],
    [(-700.0, -4300.0), (-1100.0, -4050.0), (1100.0, -4250.0), (1500.0, -4000.0), (-1600.0, -3900.0),
     (1900.0, -3800.0)]))]
# Street lamps (FR01's FAROLAS): two where Ransom has them in the village, more along the streets.
LAMP_BOX = (-6283, 722, 6349, -6187, 1175, 6448)
LAMPS = [prop("lamp-1", ["FAROLAS"], (6661, 1046, 12921, 6757, 1499, 13021), 5.0),
         prop("lamp-2", ["FAROLAS"], (7233, 1067, 11274, 7291, 1520, 11383), 5.0)] + [
    prop(f"lamp-{k + 3}", ["FAROLAS"], LAMP_BOX, 5.0, at=at, yaw=yaw) for k, (at, yaw) in enumerate(
        [((650.0, -3300.0), 0.0), ((-450.0, -1150.0), 180.0), ((3500.0, -1450.0), 90.0), ((4200.0, -2700.0), 270.0)])]
# Convoy's own (FR03) logs: a pile of cut logs by the workshop, trunks lying in the woods.
LOGS = [prop("log-pile", ["ATREZZO_EXTERIOR_1"], (6584, 520, -609, 7640, 677, 245), 8.0, at=(6300.0, -3600.0),
             donor=None),
        prop("log-1", ["ATREZZO_EXTERIOR_1"], (2562, 967, 3459, 3986, 1250, 4102), 40.0, at=(-5600.0, 3000.0),
             donor=None, yaw=60.0),
        prop("log-2", ["ATREZZO_EXTERIOR_1"], (4338, 886, 2416, 4792, 1111, 3041), 25.0, at=(3600.0, 6300.0),
             donor=None),
        prop("log-3", ["ATREZZO_EXTERIOR_1"], (4338, 886, 2416, 4792, 1111, 3041), 25.0, at=(-2600.0, -6300.0),
             donor=None, yaw=130.0),
        prop("log-4", ["ATREZZO_EXTERIOR_1"], (2562, 967, 3459, 3986, 1250, 4102), 40.0, at=(6300.0, 800.0),
             donor=None, yaw=20.0)]
PIECES = [FARMHOUSE, RUIN, HOUSE, RADIO_HOUSE, WALLS, WELL, WOODSHED, BARN, OPEN_SHED, COW_SHED, WATER_TOWER,
          HAND_CART, PUMP, *BALES, *HEDGEHOGS, *LAMPS, *LOGS]

# ---- Places inside the buildings (FR01 coordinates; examples/country/catalog.py objects) --
# The radio house's ground floor (1094): a 3 m table with benches along it
# (object 17, top 1175); its first floor (1506) has a writing desk (object 27).
RADIO_FLOOR, RADIO_UPSTAIRS = RADIO_HOUSE.y_of(1094.0), RADIO_HOUSE.y_of(1506.0)
RADIO_TABLE_TOP = RADIO_HOUSE.y_of(1175.0)
RADIO_ON_TABLE = RADIO_HOUSE.moved(10650.0, 10965.0)  # the table's east end
RADIO_OPERATOR = RADIO_HOUSE.moved(10819.0, 10960.0)  # standing at the end, facing west
RADIO_READER = RADIO_HOUSE.moved(10259.0, 11000.0)    # the far end, reading the map on the table
RADIO_WINDOW = RADIO_HOUSE.moved(10700.0, 11400.0)    # upstairs, a clear room
RADIO_DOOR = RADIO_HOUSE.moved(9745.0, 11289.0)       # Ransom's door actor P_FR01_07
# The ruin (walkable floor 1045): a table with a chair by the silo (object 15, top 1119).
RUIN_FLOOR = RUIN.y_of(1045.0)
PHONE_TABLE_TOP = RUIN.y_of(1119.0)
PHONE_ON_TABLE = RUIN.moved(6150.0, 13700.0)
PHONE_OPERATOR = RUIN.moved(6145.0, 13515.0)          # south of the table, facing north
# The farmhouse store (floor 1080) and the house (floor 1079): clear floor between crates and hay
# (free cells of an occupancy grid of each floor, 40 cm from any wall or furniture).
FARMHOUSE_FLOOR, HOUSE_FLOOR = FARMHOUSE.y_of(1080.0), HOUSE.y_of(1079.0)
FARMHOUSE_SPOTS = [FARMHOUSE.moved(9302.0, 13525.0), FARMHOUSE.moved(8902.0, 13705.0)]  # the cows stand at x 9640-9820
HOUSE_SPOTS = [HOUSE.moved(4995.0, 9797.0), HOUSE.moved(4715.0, 9297.0)]

# ---- Roads, fields, the paddock, the workshop -------------------------------------
EAST_GATE = (SIZE / 2 - 100.0, -1900.0)
ROADS = [  # polylines; width in cm
    ([(300.0, -SIZE / 2), (300.0, -4600.0), SQUARE], 480.0),                                     # south road
    ([EAST_GATE, (5800.0, -1900.0), (3900.0, -2000.0), SQUARE], 440.0),                          # east road
    ([(3900.0, -2000.0), (4300.0, 0.0), (4300.0, 1300.0), (4200.0, 3000.0)], 380.0),             # to the farm
    ([SQUARE, (-900.0, -4000.0), (-3800.0, -4000.0), (-4600.0, -3400.0)], 380.0),                # to the water tower
]
FIELDS = [  # x0, z0, x1, z1, own texture (edges on the 1.5 m grid)
    (1200.0, -5700.0, 4800.0, -4350.0, "FFLR_35A"),   # stubble, hay bales
    (-4800.0, -6000.0, -1200.0, -4800.0, "FFLR_16A"),  # ploughed
]
PADDOCK = (5500.0, 3300.0, 7000.0, 5000.0)  # x0 z0 x1 z1, fenced
PADDOCK_GATE = (5500.0, 4150.0)  # a gap in the west fence (x, z)
VILLAGE = (-4300.0, -3700.0, 4100.0, 4500.0)  # flat ground: x0 z0 x1 z1
WORKSHOP = (2600.0, -4300.0, 6700.0, -2500.0)  # the tank yard in front of the open shed
TANK_AT, TANK_YAW = (3500.0, -3350.0), 90.0      # the Panzer III, nose east

# ---- Where the officer's car and the lorry drive in -------------------------------
# The lorry starts at point 1, the car ahead of it at point 2; both invisible
# until they set off, on the hills beyond the east edge of the fields.
ARRIVAL_ROUTE = [(EAST_GATE[0] - 100.0, -1900.0), (EAST_GATE[0] - 900.0, -1900.0), (5800.0, -1900.0),
                 (3900.0, -2000.0), (2600.0, -2100.0), (900.0, -2250.0)]
LORRY_START, KUBEL_START, LORRY_PARK, KUBEL_PARK = 1, 2, 5, 6  # route points (1-based)


def smoothstep(edge0: float, edge1: float, x: float) -> float:
    t = min(max((x - edge0) / (edge1 - edge0), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def box_distance(x: float, z: float, box) -> float:
    x0, z0, x1, z1 = box
    dx, dz = max(x0 - x, 0.0, x - x1), max(z0 - z, 0.0, z - z1)
    return math.hypot(dx, dz)


KNOLLS = [  # x, z, radius, height: meadow knolls and rocky outcrops (steep sides turn to rock)
    (-5200.0, 4600.0, 2200.0, 220.0), (2600.0, 6200.0, 1500.0, 140.0), (-2400.0, -6400.0, 1400.0, 160.0),
    (6300.0, -5600.0, 1100.0, 260.0), (-6200.0, -5800.0, 900.0, 320.0), (5600.0, 6300.0, 800.0, 300.0),
]


def natural_height(x: float, z: float) -> float:
    h = 90.0 * math.sin(0.0011 * x + 0.7) * math.cos(0.0010 * z) + 40.0 * math.sin(0.0029 * x + 0.0021 * z)
    h += 18.0 * math.sin(0.0071 * x - 0.3) * math.sin(0.0063 * z + 1.1)  # small bumps
    for kx, kz, radius, rise in KNOLLS:
        h += rise * math.exp(-((x - kx) ** 2 + (z - kz) ** 2) / radius ** 2)
    # A dry ditch along the north-west, under the hedges.
    ditch = abs((z - 2600.0) - 0.45 * (x + 7500.0))
    if x < -1500.0:
        h -= 110.0 * smoothstep(350.0, 0.0, ditch)
    for distance in (SIZE / 2 - x, SIZE / 2 + x, SIZE / 2 - z, SIZE / 2 + z):
        rise = smoothstep(EDGE[0], 0.0, distance)
        h = h * (1 - rise) + rise * (EDGE[1] + 200.0 * math.sin(0.0023 * x + 1.3) * math.cos(0.0019 * z + 0.4))
    return h


def flatness(x: float, z: float) -> float:
    flat = smoothstep(1500.0, 0.0, box_distance(x, z, VILLAGE))
    flat = max(flat, smoothstep(900.0, 0.0, box_distance(x, z, WORKSHOP)))
    for piece in BUILDINGS:
        flat = max(flat, smoothstep(900.0, 0.0, box_distance(x, z, piece.footprint(300.0))))
    return flat


def height(x: float, z: float) -> float:
    """Flat village, workshop and building pads (height 0) blended into the natural ground."""
    return natural_height(x, z) * (1.0 - flatness(x, z))


def road_frame(x: float, z: float) -> tuple[float, float, float]:
    """(distance across the nearest road, distance along it, its half width)."""
    best = (math.inf, 0.0, 0.0)
    for points, width in ROADS:
        along = 0.0
        for (ax, az), (bx, bz) in zip(points, points[1:]):
            dx, dz = bx - ax, bz - az
            length = math.hypot(dx, dz)
            t = min(max(((x - ax) * dx + (z - az) * dz) / (length * length), 0.0), 1.0)
            px, pz = ax + t * dx, az + t * dz
            d = math.hypot(x - px, z - pz)
            if d < abs(best[0]):
                side = math.copysign(1.0, dx * (z - az) - dz * (x - ax))
                best = (side * d, along + t * length, width / 2)
            along += length
    return best


def field_at(x: float, z: float):
    for x0, z0, x1, z1, texture in FIELDS:
        if x0 <= x <= x1 and z0 <= z <= z1:
            return texture
    return None


# ---- Fences (the imported picket fence, 4.2 m sections) --------------------------
FENCE_SECTION = 420.0  # the imported fence is 4.28 m long


def fence_sections() -> list[tuple[float, float, float]]:
    """(x, z, yaw degrees) of each fence section round the paddock, leaving the gate."""
    x0, z0, x1, z1 = PADDOCK
    corners = [(x0, z0), (x1, z0), (x1, z1), (x0, z1), (x0, z0)]
    sections = []
    for (ax, az), (bx, bz) in zip(corners, corners[1:]):
        length = math.hypot(bx - ax, bz - az)
        count = round(length / FENCE_SECTION)
        yaw = math.degrees(math.atan2(bx - ax, bz - az)) - 90.0  # the section's length runs along its local x
        for k in range(count):
            t = (k + 0.5) / count
            x, z = ax + t * (bx - ax), az + t * (bz - az)
            if math.hypot(x - PADDOCK_GATE[0], z - PADDOCK_GATE[1]) < 300.0:
                continue
            sections.append((x, z, yaw))
    return sections


# ---- Trees and bushes (Convoy's own prop instances) -----------------------------------
# Donor instance pairs (trunk, canopy) of FR03's species, by the radius of the canopy.
TREE_SPECIES = {
    "fir": (["1,2", "7,8", "15,16"], 1050.0),        # 26 m firs
    "small-fir": (["37,38", "31,32", "23,24"], 650.0),  # 16 m firs
    "oak": (["83,84", "93,94", "105,106"], 1250.0),   # 28 m broadleaf
    "poplar": (["129,130", "139,140", "145,146"], 1200.0),
    "small-oak": (["154,155", "164,165", "176,177"], 600.0),
}
BUSH, BUSH_RADIUS = "319", 200.0  # Convoy's ground plant (prototype 1040)


def keep_clear(x: float, z: float, margin: float) -> bool:
    """True where nothing may grow: buildings, roads, fields, the paddock, the yards."""
    if abs(x) > SIZE / 2 - 150 or abs(z) > SIZE / 2 - 150:
        return True
    across, _, half = road_frame(x, z)
    if abs(across) < half + margin:
        return True
    for piece in PIECES:
        if box_distance(x, z, piece.footprint()) < margin + 150.0:
            return True
    for box in ([f[:4] for f in FIELDS] + [PADDOCK, WORKSHOP, (SQUARE[0] - 1500, SQUARE[1] - 1200,
                                                                 SQUARE[0] + 1500, SQUARE[1] + 900)]):
        if box_distance(x, z, box) < margin:
            return True
    return box_distance(x, z, VILLAGE) == 0.0  # the village keeps its own yards


def trees() -> list[tuple[str, float, float, int]]:
    """(donor instances, x, z, yaw) of each tree: woods on the hills, groves and single trees."""
    rng = random.Random(1944)
    placed: list[tuple[float, float, float]] = []
    result = []

    def try_place(kind: str, x: float, z: float) -> None:
        ids, radius = TREE_SPECIES[kind]
        trunk_room = radius * 0.55
        if keep_clear(x, z, trunk_room * 0.6 + 250.0):
            return
        if any(math.hypot(x - px, z - pz) < (trunk_room + pr) * 0.8 for px, pz, pr in placed):
            return
        placed.append((x, z, trunk_room))
        result.append((rng.choice(ids), x, z, rng.randrange(360)))

    # Groves: north-west meadow, south-west corner, north of the farm.
    for cx, cz, spread in ((-5000.0, 4400.0, 1500.0), (-5600.0, -5200.0, 900.0), (3000.0, 6300.0, 900.0),
                           (-2800.0, 5800.0, 900.0)):
        for _ in range(120):
            try_place(rng.choice(["oak", "small-oak", "poplar", "small-oak", "small-oak"]), cx + rng.gauss(0, spread),
                      cz + rng.gauss(0, spread))
    # Woods on the hills: firs, a few oaks.
    for _ in range(900):
        side = rng.randrange(4)
        along, depth = rng.uniform(-SIZE / 2, SIZE / 2), rng.uniform(200.0, EDGE[0] + 400.0)
        x, z = [(along, SIZE / 2 - depth), (along, -SIZE / 2 + depth), (SIZE / 2 - depth, along),
                (-SIZE / 2 + depth, along)][side]
        if side == 1 and abs(x - 300.0) < 900.0:  # leave the south road's view open
            continue
        try_place(rng.choice(["fir", "fir", "small-fir", "oak", "small-fir"]), x, z)
    # Single trees along the roads and between the fields.
    for x, z, kind in ((-1800.0, -5200.0, "poplar"), (900.0, -6300.0, "small-oak"), (-400.0, -5900.0, "small-oak"),
                       (5300.0, -500.0, "poplar"), (6700.0, -900.0, "small-oak"), (2600.0, -1500.0, "small-oak"),
                       (4900.0, 1000.0, "small-oak"), (-3400.0, -1000.0, "poplar"), (-5600.0, -900.0, "small-oak"),
                       (-5300.0, 800.0, "oak"), (5200.0, -6000.0, "small-oak"), (-3300.0, 6200.0, "poplar")):
        try_place(kind, x, z)
    return result


def bushes() -> list[tuple[float, float, int]]:
    """(x, z, yaw) of each bush: hedges along the roads, the ditch and the fields, clumps by the woods."""
    rng = random.Random(1940)
    placed: list[tuple[float, float]] = []
    result = []

    def try_place(x: float, z: float) -> None:
        if keep_clear(x, z, 250.0) or any(math.hypot(x - px, z - pz) < 330.0 for px, pz in placed):
            return
        placed.append((x, z))
        result.append((x, z, rng.randrange(360)))

    for points, width in ROADS:  # hedges along the roads, with gaps
        for (ax, az), (bx, bz) in zip(points, points[1:]):
            length = math.hypot(bx - ax, bz - az)
            nx, nz = -(bz - az) / length, (bx - ax) / length
            for k in range(int(length / 450.0)):
                if rng.random() < 0.45:
                    continue
                t = (k + rng.random()) * 450.0 / length
                side = rng.choice((-1, 1))
                off = side * (width / 2 + 420.0 + rng.uniform(0, 150.0))
                try_place(ax + t * (bx - ax) + nx * off, az + t * (bz - az) + nz * off)
    for x0, z0, x1, z1, _ in FIELDS:  # field edges
        for k in range(18):
            t = rng.random()
            x, z = [(x0 + t * (x1 - x0), z0 - 330.0), (x0 + t * (x1 - x0), z1 + 330.0), (x0 - 330.0, z0 + t * (z1 - z0)),
                    (x1 + 330.0, z0 + t * (z1 - z0))][k % 4]
            try_place(x, z)
    for k in range(40):  # along the ditch
        x = -7000.0 + k * 140.0
        try_place(x + rng.uniform(-150, 150), 2600.0 + 0.45 * (x + 7500.0) + rng.uniform(-300.0, 300.0))
    for _ in range(260):  # clumps where the woods begin, and scattered
        x, z = rng.uniform(-SIZE / 2 + 400, SIZE / 2 - 400), rng.uniform(-SIZE / 2 + 400, SIZE / 2 - 400)
        edge = min(SIZE / 2 - abs(x), SIZE / 2 - abs(z))
        if edge > EDGE[0] + 800.0 and rng.random() < 0.75:
            continue
        try_place(x, z)
    return result
