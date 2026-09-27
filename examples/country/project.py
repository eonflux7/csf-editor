#!/usr/bin/env python3
"""Write Country's project.csfproj (docs/reference/project-format.md).

    examples/country/project.py PROJECT_DIR

PROJECT_DIR comes from `csf-mod project-new --slot Convoy --name Country`
(its text range and local.csfproj are kept) and holds the sources that
terrain.py, assets.py, lightmaps.py and textures.py wrote. Records: FR01
(Ransom's map) as a second donor; the terrain and the two imported models
(fence, tank) as assets; FR01's buildings, yard props, hedgehogs and street
lamps and Convoy's own logs as pieces with their lightmap groups; fence
sections and the tank as buildings; Convoy's trees and bushes as props; the
placeholder lightmaps; the textures of its own (FR01's field textures, the
tank's); the mission strings.
"""

from __future__ import annotations

import hashlib
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout import BUSH, PIECES, TANK_AT, TANK_YAW, bushes, fence_sections, height, trees  # noqa: E402

TREE_SINK, BUSH_SINK = -10.0, -5.0
STRINGS = {  # offsets in the project's reserved text range (ops.py uses the same)
    0: "Kill the Gestapo officer.",
    1: "The Gestapo officer is dead.",
    2: "Sabotage the radio in the half-timbered house.",
    3: "The radio is silenced.",
    4: "Sabotage radio",
    5: "Cut the telephone line in the ruined farm by the silo.",
    6: "The telephone line is cut.",
    7: "Cut telephone line",
    8: "A body has been found! The alarm is sounding.",
    9: "Guards who find a body will sound the alarm. Hide the bodies.",
    10: "A Gestapo officer is expected in the village tonight.",
    11: "A car and a lorry are coming up the east road.",
    12: "The officer has arrived in the village square.",
}


def number(value: float) -> str:
    return f"{value:.0f}"


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    project = Path(argv[1])
    current = (project / "project.csfproj").read_text(encoding="utf-8")
    texts = re.search(r"^texts .*$", current, re.M)[0]  # the range project-new reserved
    first = int(texts.split()[3])
    exports = {name: hashlib.sha256((project / path).read_bytes()).hexdigest() for name, path in
               (("terrain", "sources/world/terrain.csfworld"), ("fence", "sources/assets/fence.csfworld"),
                ("tank", "sources/assets/tank.csfworld"))}
    lines = [
        "csfproj 1",
        "name Country",
        "slot Convoy Maps/FR03/Convoy.scn maps/Convoy.pak",
        texts,
        "donor-map Maps/FR03/FR03.rws Maps/FR03/FR03_col.rws",
        "donor fr01 Ransom Maps/FR01/FR01.rws Maps/FR01/FR01_col.rws",
        "",
        "# World geometry from Blender (examples/country/terrain.py, assets.py)",
        "asset terrain terrain sources/world/terrain.blend sources/world/terrain.csfworld",
        f"asset-export terrain export_csf_world.py 1 sha256:{exports['terrain']}",
        "asset fence building sources/assets/fence.blend sources/assets/fence.csfworld",
        f"asset-export fence export_csf_world.py 1 sha256:{exports['fence']}",
        "asset tank building sources/assets/tank.blend sources/assets/tank.csfworld",
        f"asset-export tank export_csf_world.py 1 sha256:{exports['tank']}",
        "",
        "# Pieces of Worlds by lightmap group: Ransom's (FR01) buildings, yard props, hedgehogs",
        "# and street lamps; Convoy's own (FR03) logs",
    ]
    for piece in PIECES:
        x, z = piece.at
        donor = f"donor={piece.donor} " if piece.donor else ""
        lines.append(f"piece {piece.ident} {donor}lightmaps={','.join(piece.groups)} "
                     + " ".join(number(v) for v in piece.box)
                     + f" {number(x)} {number(height(x, z) + piece.sink)} {number(z)} {number(piece.yaw)}"
                     + f" ground {number(piece.sink)}")
    lines += ["", "# The imported models, placed: the paddock fence and the tank in the workshop"]
    for k, (x, z, yaw) in enumerate(fence_sections(), 1):
        lines.append(f"building fence-{k} fence {number(x)} {number(height(x, z) - 5)} {number(z)} {number(yaw)} ground -5")
    lines.append(f"building tank-1 tank {number(TANK_AT[0])} {number(height(*TANK_AT))} {number(TANK_AT[1])} "
                 f"{number(TANK_YAW)} ground 0")
    lines += ["", "# Convoy's trees (trunk and canopy instances) and bushes"]
    for k, (ids, x, z, yaw) in enumerate(trees(), 1):
        lines.append(f"prop tree-{k} {ids} {number(x)} {number(height(x, z) + TREE_SINK)} {number(z)} {yaw}"
                     f" ground {number(TREE_SINK)}")
    for k, (x, z, yaw) in enumerate(bushes(), 1):
        lines.append(f"prop bush-{k} {BUSH} {number(x)} {number(height(x, z) + BUSH_SINK)} {number(z)} {yaw}"
                     f" ground {number(BUSH_SINK)}")
    lines += ["", "# Placeholder lightmaps (examples/country/lightmaps.py)",
              "lightmap COUNTRY_Lm sources/lightmaps/COUNTRY_Lm.png",
              "lightmap COUNTRY_PROPS_Lm sources/lightmaps/COUNTRY_PROPS_Lm.png",
              "", "# Textures of their own: FR01's fields, the tank's hull and tracks",
              "texture FFLR_16A sources/textures/FFLR_16A.dds FFLRA11B",
              "texture FFLR_35A sources/textures/FFLR_35A.dds FFLRA11B",
              "texture PZ3_HULL sources/textures/PZ3_HULL.png FFLRA11B",
              "texture PZ3_TRACK sources/textures/PZ3_TRACK.dds FFLRA11B",
              "", "# Mission text (GlobalEK)"]
    lines += [f'text {first + offset:04d} "{value}"' for offset, value in STRINGS.items()]
    (project / "project.csfproj").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"project\t{project / 'project.csfproj'}\t{len(PIECES)} pieces, {len(fence_sections())} fence sections, "
          f"{len(trees())} trees, {len(bushes())} bushes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
