"""Headless test of a map edited without a project (import, export, build).

    CSF_MOD=build/Release/csf-mod blender --background --factory-startup \
        --python tools/blender/standalone_map_test.py -- MAP.rws OUT_DIR

MAP is a shipped map (e.g. ../CSF_unpacks/Convoy/Maps/FR03/FR03.rws, read
only). The test decompiles it with `csf-mod world-source`, imports it through
the add-on's Import map, checks every face, material and collision shade came
in, exports the scene unchanged and checks the export against the source
(positions to 0.01 cm, UVs to 1e-4), then raises one visual vertex and builds the map with
`csf-mod world-build --keep-props`. It also checks that a glTF-imported
material falls back to its rws_* properties and that glTF props are skipped.
Everything is written under OUT_DIR.
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


def check(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAILED: {message}")
    print(f"ok\t{message}")


def faces_of(path: Path) -> list:
    """Each face in file order: (role, texture, surface, shade, lightmap), corners."""
    materials, vertices, faces = [], [], []
    for line in path.read_text(encoding="latin-1").splitlines():
        parts = csfworld.fields(line)
        if not parts or parts[0].startswith("#"):
            continue
        if parts[0] == "material":
            materials.append((parts[1], parts[2], int(parts[3]), parts[4] if len(parts) > 4 else ""))
        elif parts[0] == "v":
            values = [float(v) for v in parts[1:]]
            vertices.append((values[:3], values[6:]))  # position, UVs
        elif parts[0] == "f":
            texture, surface, shade, lightmap = materials[int(parts[4])]
            if parts[5] == "visual":
                shade = None  # only collision keeps it
            faces.append(((parts[5], texture, surface, shade, lightmap), [vertices[int(k)] for k in parts[1:4]]))
    return faces


def same_faces(before: list, after: list) -> bool:
    """The same faces in the same order, corners within 0.01 cm, UVs within 1e-4."""
    def close(corner, other) -> bool:
        (position, uvs), (other_position, other_uvs) = corner, other
        return (all(abs(p - q) <= 0.01 for p, q in zip(position, other_position))
                and (not uvs or not other_uvs or all(abs(p - q) <= 1e-4 for p, q in zip(uvs, other_uvs))))
    return len(before) == len(after) and all(
        key == other_key and all(close(c, d) for c, d in zip(corners, other))
        for (key, corners), (other_key, other) in zip(before, after))


def csf_mod(*arguments) -> str:
    result = subprocess.run([os.environ["CSF_MOD"], *map(str, arguments)], capture_output=True, text=True,
                            errors="replace")
    if result.returncode != 0:
        raise SystemExit(f"FAILED: csf-mod {arguments[0]}: {result.stderr or result.stdout}")
    return result.stdout


def main(argv: list[str]) -> int:
    map_path, out = (Path(a).resolve() for a in argv[argv.index("--") + 1:][:2])
    out.mkdir(parents=True, exist_ok=True)
    csf_authoring.register()
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)

    source = out / f"{map_path.stem}.csfworld"
    csf_mod("world-source", map_path, source, "--overwrite")
    stats = csfworld.load(str(source))
    visual = bpy.data.objects.get(f"{map_path.stem} visual")
    collision = bpy.data.objects.get(f"{map_path.stem} collision")
    check(visual is not None and collision is not None and stats["objects"] == 2, "a visual and a collision object")
    check(visual["csf_role"] == "visual" and collision["csf_role"] == "collision", "roles are set")
    check(len(visual.data.polygons) + len(collision.data.polygons) == stats["faces"], "every face imported")
    check(collision.data.attributes["csf_shade"].domain == "FACE", "collision shade is a face attribute")
    check(len(visual.data.uv_layers) == 2, "visual has base and lightmap UVs")
    check(all("csf_texture" in m and "csf_surface" in m for m in visual.data.materials), "materials carry names")
    textured = [m for m in visual.data.materials if any(n.type == "TEX_IMAGE" for n in m.node_tree.nodes)]
    check(len(textured) > 0, f"textures found ({len(textured)} of {len(visual.data.materials)} materials)")

    exported = out / "unchanged.csfworld"
    csfworld.export(str(exported), bpy.context.scene, precise=True)
    before = faces_of(source)
    check(same_faces(before, faces_of(exported)), f"unchanged export keeps all {len(before)} faces, materials and shades")

    # Raise one visual vertex by 1 m, export and build with the map as donor.
    visual.data.vertices[0].co.z += 1.0
    edited = out / "edited.csfworld"
    csfworld.export(str(edited), bpy.context.scene, precise=True)
    built = out / "map" / map_path.name
    report = csf_mod("world-build", edited, map_path, built, "--keep-props", "--overwrite")
    check("visual\t" in report and "collision\t" in report and built.is_file(), "world-build compiles the edit")
    check(any(line.startswith("visual\t") and f"\t{len(visual.data.polygons)} triangles" in line
              for line in report.splitlines()), "the built map has every visual triangle")

    # glTF path: rws_* material properties, props skipped.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    bpy.ops.mesh.primitive_plane_add(size=2)
    ground = bpy.context.active_object
    material = bpy.data.materials.new("world_0x0_material_3")
    material["rws_base_texture"] = "FTREA06A"
    material["rws_surface_name"] = "Hierba"
    material["rws_lightmap_texture"] = "TERRENO_Lm"
    ground.data.materials.append(material)
    bpy.ops.mesh.primitive_cube_add(size=1)
    bpy.context.active_object["rws_kind"] = "clump_atomic"
    gltf = out / "gltf.csfworld"
    stats = csfworld.export(str(gltf), bpy.context.scene)
    check(stats["faces"] == 2, "glTF props (rws_kind) are not exported")
    check("material FTREA06A Hierba 228 TERRENO_Lm" in gltf.read_text(), "rws_* properties name the material")

    # The operators: Import map on the .rws itself (csf-mod from CSF_MOD), Export.
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj)
    check(bpy.ops.csf.import_map(filepath=str(map_path)) == {"FINISHED"}, "Import map reads the .rws")
    check(bpy.context.scene.get("csf_source_map") == str(map_path), "the scene remembers the map")
    operator_export = out / "operator.csfworld"
    check(bpy.ops.csf.export_world(filepath=str(operator_export)) == {"FINISHED"}, "Export writes a .csfworld")
    check(same_faces(before, faces_of(operator_export)), "the operators round-trip the map")
    print("standalone map test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
