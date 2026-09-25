"""Headless test of the csf_authoring add-on against a copy of an authoring project.

    CSF_MOD=build/Release/csf-mod blender PROJECT/sources/world/terrain.blend --background \
        --factory-startup --python tools/blender/csf_authoring_test.py -- PROJECT

PROJECT must be a disposable copy (tools/hello_world/build.sh output): the test
saves the .blend, rewrites the terrain export, adds a building asset and raises
the terrain by 0.5 m. It loads the reference, checks it is never exported, and
checks each export; `csf-mod project-heights` must then report the raise.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csf_authoring  # noqa: E402
from csf_authoring import csfworld  # noqa: E402


def lines_without_comments(path: Path) -> list[str]:
    return [line for line in path.read_text(encoding="utf-8").splitlines() if not line.startswith("#")]


def check(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAILED: {message}")
    print(f"ok\t{message}")


def main(argv: list[str]) -> int:
    project = Path(argv[argv.index("--") + 1]).resolve()
    csf_authoring.register()
    context = bpy.context
    context.scene.csf_project = str(project)
    terrain_export = project / "sources" / "world" / "terrain.csfworld"
    before = lines_without_comments(terrain_export)

    # Reference: locked, complete, and invisible to the exporter.
    summary = csf_authoring.load_reference(context)
    reference = bpy.data.collections[csf_authoring.REFERENCE]
    check(reference.hide_select and len(reference.all_objects) > 50, f"reference loaded ({summary})")
    check(all(csfworld.is_reference(obj) for obj in reference.all_objects), "every reference object is marked")
    check(not any(csfworld.is_reference(obj) for obj in csfworld.exportable(context.scene)),
          "the exporter skips the reference")
    check(len(bpy.data.collections["CSF actors"].objects) == 24, "24 actors")

    # Sending the unchanged terrain rewrites the same export.
    csf_authoring.send(context)
    check(lines_without_comments(terrain_export) == before, "unchanged terrain sends the same export")

    # A building is exported about its origin and registered in the project.
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(10.0, 10.0, 1.0))
    hut = context.active_object
    hut.name = "Hut"
    bpy.ops.csf.tag_asset(kind="building")
    check(hut["csf_asset_id"] == "hut", "the building is tagged 'hut'")
    csf_authoring.send(context)
    assets = csf_authoring.project_assets(project)
    check(assets.get("hut", {}).get("kind") == "building", "the building is registered")
    xs = [float(line.split()[1]) for line in lines_without_comments(project / assets["hut"]["export"])
          if line.startswith("v ")]
    check(min(xs) == -100.0 and max(xs) == 100.0, "the building is exported about its origin")

    # Raising the terrain shows up in rws-man's height report.
    terrain = next(obj for obj in bpy.data.objects if obj.get("csf_asset_id") == "terrain")
    for vertex in terrain.data.vertices:
        vertex.co.z += 0.5
    csf_authoring.send(context)
    report = subprocess.run([os.environ["CSF_MOD"], "project-heights", str(project)], capture_output=True,
                            text=True, check=True).stdout
    check("44 height findings" in report, "the raise is reported for 30 placements and 14 actors")
    print("csf_authoring test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
