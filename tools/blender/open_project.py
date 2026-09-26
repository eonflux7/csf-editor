"""Opens an rws-man authoring project in Blender (rws-man's Edit in Blender).

    blender [BLEND] --python tools/blender/open_project.py -- PROJECT BLEND EXPORT CSF_MOD

Loads the csf_authoring add-on (installed, or from beside this script), sets
the scene's project and the csf-mod the add-on runs. When BLEND does not exist
yet, it starts one from the asset's export (rws-man's starter terrain): one
mesh tagged as the project's terrain, its materials carrying the texture and
surface names, saved as BLEND. Send (CSF tab) then exports it back.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import addon_utils
import bpy


def load_addon() -> None:
    try:
        addon_utils.enable("csf_authoring", default_set=True)
    except Exception:  # noqa: BLE001 - not installed
        pass
    if "csf_authoring" in bpy.context.preferences.addons:
        return
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import csf_authoring  # noqa: PLC0415

    csf_authoring.register()


def starter_scene(export: Path, blend: Path) -> None:
    """The export as one mesh tagged as the terrain (game cm, Y up -> Blender m, Z up)."""
    materials, vertices, uvs, faces, face_materials = [], [], [], [], []
    for line in export.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if fields[0] == "material":
            materials.append((fields[1], fields[2]))
        elif fields[0] == "v":
            x, y, z = (float(v) for v in fields[1:4])
            vertices.append((x / 100, -z / 100, y / 100))
            uvs.append((float(fields[7]), 1.0 - float(fields[8])))  # the exporter flips V
        elif fields[0] == "f":
            faces.append(tuple(int(v) for v in fields[1:4]))
            face_materials.append(int(fields[4]))
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    mesh = bpy.data.meshes.new("terrain")
    mesh.from_pydata(vertices, [], faces)
    for texture, surface in materials:
        material = bpy.data.materials.new(texture)
        material["csf_texture"] = texture
        material["csf_surface"] = surface
        mesh.materials.append(material)
    for polygon, index in zip(mesh.polygons, face_materials):
        polygon.material_index = index
    layer = mesh.uv_layers.new(name="UVMap")
    for loop in mesh.loops:
        layer.data[loop.index].uv = uvs[loop.vertex_index]
    mesh.update()
    obj = bpy.data.objects.new("terrain", mesh)
    obj["csf_asset_kind"] = "terrain"
    obj["csf_asset_id"] = "terrain"
    bpy.context.scene.collection.objects.link(obj)
    blend.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))


def main(argv: list[str]) -> int:
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    if len(args) != 4:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    project, blend, export, csf_mod = (Path(a) for a in args)
    os.environ["CSF_MOD"] = str(csf_mod)
    load_addon()
    if not blend.is_file() and export.is_file():
        starter_scene(export, blend)
    bpy.context.scene.csf_project = str(project)
    if "csf_authoring" in bpy.context.preferences.addons:
        bpy.context.preferences.addons["csf_authoring"].preferences.csf_mod = str(csf_mod)
    print(f"csf-project\t{project}\t{blend}")
    return 0


main(sys.argv)
