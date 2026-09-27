"""Turn Country's two downloaded CC0 models into project building assets.

    blender --background --factory-startup --python tools/country/assets.py -- DOWNLOADS OUT_DIR

DOWNLOADS holds `picketfence_0.blend` ("Basic Wooden Fence" by WeaponGuy,
OpenGameArt, CC0) and `pzIIIs.blend` ("panzerkampfwagen III" by konserwa,
OpenGameArt, CC0; 7k triangles, untextured). For each, OUT_DIR gets
`<id>.blend` and `<id>.csfworld` (exported about the origin, as buildings
are): the objects joined, transforms applied, scaled to game size, standing
on z = 0.

- fence: re-textured with Convoy's plank wood FWOD_04A (box-projected UVs),
  surface Madera.
- tank: a Panzer III 5.6 m long, re-textured with two textures of the
  project's own (`texture` records): PZ3_TRACK (Panzers' tank track, on the
  running gear: wheels and the low sides) and PZ3_HULL (a painted steel plate
  made by textures.py) everywhere else; box-projected UVs, surface Metal.

Both use the flat placeholder lightmap COUNTRY_PROPS_Lm (lightmaps.py).
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import bpy
import mathutils

PROPS_LIGHTMAP = "COUNTRY_PROPS_Lm"
FENCE_SCALE = 0.62    # 2.13 m posts as modelled
TANK_SCALE = 3.17     # 1.77 m long as modelled; a Panzer III is 5.6 m


def exporter():
    spec = importlib.util.spec_from_file_location(
        "export_csf_world", Path(__file__).resolve().parents[1] / "blender" / "export_csf_world.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def join_and_ground(objects, name: str, scale: float):
    """One mesh object from `objects`, transforms applied, scaled, its bottom
    centre at the origin."""
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    if len(objects) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.name = obj.data.name = name
    obj.scale = [s * scale for s in obj.scale]
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    corners = [v.co for v in obj.data.vertices]
    lo = mathutils.Vector([min(c[i] for c in corners) for i in range(3)])
    hi = mathutils.Vector([max(c[i] for c in corners) for i in range(3)])
    shift = mathutils.Vector(((lo.x + hi.x) / 2, (lo.y + hi.y) / 2, lo.z))
    for vertex in obj.data.vertices:
        vertex.co -= shift
    return obj, hi - lo


def material(name: str, texture: str, surface: str):
    result = bpy.data.materials.new(name)
    result["csf_texture"] = texture
    result["csf_surface"] = surface
    result["csf_lightmap"] = PROPS_LIGHTMAP
    return result


def tag(obj, ident: str):
    obj["csf_asset_id"] = ident  # the csf_authoring add-on's asset identity
    obj["csf_asset_kind"] = "building"


def box_project():
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.cube_project(cube_size=1.0)
    bpy.ops.object.mode_set(mode="OBJECT")


def tank(downloads: Path):
    bpy.ops.wm.open_mainfile(filepath=str(downloads / "pzIIIs.blend"))
    meshes = [o for o in bpy.data.objects if o.type == "MESH"]
    # The running gear, before joining: the hull's materials 1-7 are its road
    # wheels and sprockets; its material 0 below the mudguards is the tracks.
    gear = set()
    for o in meshes:
        slots = [s.material.name if s.material else "" for s in o.material_slots]
        for p in o.data.polygons:
            name = slots[p.material_index] if p.material_index < len(slots) else ""
            low = max(o.data.vertices[v].co.z for v in p.vertices) < 0.21
            wheel = o.name == "Plane.004" and 1 <= p.material_index <= 7
            if wheel or (o.name == "Plane.004" and p.material_index == 0 and low):
                gear.add((o.name, p.index))
            p.use_smooth = False
        o["gear"] = [i for n, i in gear if n == o.name]
    marks = {o.name: set(o["gear"]) for o in meshes}
    for o in meshes:  # keep the running gear through the join as material 1
        o.data.materials.clear()
        o.data.materials.append(material("pz3_hull", "PZ3_HULL", "Metal"))
        o.data.materials.append(material("pz3_track", "PZ3_TRACK", "Metal"))
        for p in o.data.polygons:
            p.material_index = 1 if p.index in marks[o.name] else 0
    obj, size = join_and_ground(meshes, "tank", TANK_SCALE)
    box_project()
    tag(obj, "tank")
    return obj, size


def fence(downloads: Path):
    bpy.ops.wm.open_mainfile(filepath=str(downloads / "picketfence_0.blend"))
    obj, size = join_and_ground([o for o in bpy.data.objects if o.type == "MESH"], "fence", FENCE_SCALE)
    obj.data.materials.clear()
    obj.data.materials.append(material("fence_wood", "FWOD_04A", "Madera"))
    for polygon in obj.data.polygons:
        polygon.material_index = 0
    box_project()  # one plank texture repeat per metre
    tag(obj, "fence")
    return obj, size


def main(argv: list[str]) -> int:
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    if len(args) != 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    downloads, out = Path(args[0]).resolve(), Path(args[1]).resolve()
    out.mkdir(parents=True, exist_ok=True)
    for make in (fence, tank):
        obj, size = make(downloads)
        ident = obj["csf_asset_id"]
        # Keep only the asset in the saved scene.
        for other in [o for o in bpy.data.objects if o is not obj]:
            bpy.data.objects.remove(other)
        stats = exporter().export(str(out / f"{ident}.csfworld"), objects=[obj], local=True)
        bpy.ops.wm.save_as_mainfile(filepath=str(out / f"{ident}.blend"))
        print(f"asset\t{ident}\t{stats['faces']} faces\tsize {size.x:.2f} x {size.y:.2f} x {size.z:.2f} m")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
