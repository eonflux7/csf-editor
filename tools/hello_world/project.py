#!/usr/bin/env python3
"""Write the hello-world authoring project files (docs/plans/editor-project-format.md).

    tools/hello_world/project.py PROJECT_DIR CORPUS_ROOT

Expects PROJECT_DIR/sources/world/terrain.csfworld (terrain.py). Writes
project.csfproj (Convoy's slot, the objective text range, the terrain asset
and the house, tree and plant placements from layout.py, standing on the
ground at the sinks v13 used) and local.csfproj (the corpus path). Heights are
resolved here from layout.height, as v13's terrain.py did, so the project
builds the same World.
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout import (HOUSE_AT, HOUSE_BOX, HOUSE_SINK, PLANT, PLANT_SINK, PLANTS, TREE_SINK,  # noqa: E402
                    TREES, height)

TERRAIN = "sources/world/terrain.csfworld"


def number(value: float) -> str:
    return f"{value:.0f}"


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    project, corpus = Path(argv[1]), Path(argv[2]).resolve()
    export_hash = hashlib.sha256((project / TERRAIN).read_bytes()).hexdigest()
    lines = [
        "csfproj 1",
        'name "Hello world"',
        "slot Convoy Maps/FR03/Convoy.scn maps/Convoy.pak",
        "texts GlobalEK.pak Texts/Convoy.fli 900 999",
        "donor-map Maps/FR03/FR03.rws Maps/FR03/FR03_col.rws",
        "",
        "# World geometry from Blender",
        f"asset terrain terrain sources/world/terrain.blend {TERRAIN}",
        f"asset-export terrain export_csf_world.py 1 sha256:{export_hash}",
        "",
        "# Buildings, donor pieces and props placed by the editor",
        "piece house " + " ".join(number(v) for v in HOUSE_BOX)
        + f" {number(HOUSE_AT[0])} {number(HOUSE_SINK)} {number(HOUSE_AT[1])} 0 ground {number(HOUSE_SINK)}",
    ]
    for k, (ids, x, z, yaw) in enumerate(TREES, 1):
        lines.append(f"prop tree-{k} {ids} {number(x)} {number(height(x, z) + TREE_SINK)} {number(z)} {yaw}"
                     f" ground {number(TREE_SINK)}")
    for k, (x, z) in enumerate(PLANTS, 1):
        lines.append(f"prop plant-{k} {PLANT} {number(x)} {number(height(x, z) + PLANT_SINK)} {number(z)}"
                     f" {(k - 1) * 47 % 360} ground {number(PLANT_SINK)}")
    (project / "project.csfproj").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    (project / "local.csfproj").write_text(f'csfproj-local 1\ncorpus "{corpus.as_posix()}"\n',
                                           encoding="utf-8", newline="\n")
    print(f"project\t{project / 'project.csfproj'}\t{1 + len(TREES) + len(PLANTS)} placements")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
