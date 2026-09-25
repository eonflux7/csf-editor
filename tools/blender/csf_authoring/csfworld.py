"""Export the visible mesh objects of a Blender scene to a `.csfworld` file.

`csf-mod world-build` (or an authoring project's build) compiles the result
into a map `.rws` and `_col.rws` (format: include/rws/world_source.hpp).

Conventions
-----------
* Blender metres, Z up  ->  game centimetres, source Y up: game = 100 * (x, z, -y).
  This is the inverse of the glTF path (rws-man exports Y-up glTF at 0.01 and
  Blender's importer turns it Z-up), so an exported map re-exports in place.
* Faces keep Blender's counter-clockwise front, which is the game's too.
* UV V is flipped (Blender's origin is bottom-left, the game's top-left).
* Per material, custom properties override the defaults:
  ``csf_texture`` (default: the first Image Texture node's image name without
  extension, else the material name), ``csf_surface`` (default ``Tierra``) and
  ``csf_shade`` (0-255, default 228). ``csf_lightmap`` names a baked lightmap
  of the project (the add-on's Bake lighting sets it); the second UV layer then
  maps it.
* Per object, ``csf_role`` is ``both`` (default), ``visual`` or ``collision``.
"""

from __future__ import annotations

import os

import bpy
import mathutils

DEFAULT_SURFACE = "Tierra"
DEFAULT_SHADE = 228
ROLES = ("both", "visual", "collision")


def material_texture(material) -> str:
    if material is None:
        return "default"
    if "csf_texture" in material:
        return str(material["csf_texture"])
    if material.use_nodes and material.node_tree:
        for node in material.node_tree.nodes:
            if node.type == "TEX_IMAGE" and node.image:
                return os.path.splitext(node.image.name)[0]
    return material.name


def is_reference(obj) -> bool:
    """Objects the add-on loaded for context (never exported)."""
    return bool(obj.get("csf_reference")) or any(c.get("csf_reference") for c in obj.users_collection)


def exportable(scene) -> list:
    return [obj for obj in scene.objects
            if obj.type == "MESH" and not obj.hide_get() and not obj.hide_render and not is_reference(obj)]


def export(path: str, scene=None, objects=None, local: bool = False) -> dict:
    """Write `objects` (default: every visible, non-reference mesh of `scene`).

    With `local`, vertices are in each object's own space (scaled, not rotated
    or moved): a building asset, which the editor places.
    """
    scene = scene or bpy.context.scene
    depsgraph = bpy.context.evaluated_depsgraph_get()
    materials: dict[tuple, int] = {}
    material_lines: list[str] = []
    vertex_lines: list[str] = []
    face_lines: list[str] = []
    for obj in (exportable(scene) if objects is None else objects):
        role = str(obj.get("csf_role", "both"))
        if role not in ROLES:
            raise ValueError(f"{obj.name}: csf_role must be one of {ROLES}")
        evaluated = obj.evaluated_get(depsgraph)
        mesh = evaluated.to_mesh()
        try:
            matrix = mathutils.Matrix.Diagonal((*obj.scale, 1.0)) if local else obj.matrix_world
            mesh.transform(matrix)
            if matrix.is_negative:
                mesh.flip_normals()
            mesh.calc_loop_triangles()
            normals = mesh.corner_normals
            uv_layers = list(mesh.uv_layers)[:2]
            for triangle in mesh.loop_triangles:
                slot = obj.material_slots[triangle.material_index].material if obj.material_slots else None
                key = (material_texture(slot),
                       str(slot.get("csf_surface", DEFAULT_SURFACE)) if slot else DEFAULT_SURFACE,
                       int(slot.get("csf_shade", DEFAULT_SHADE)) if slot else DEFAULT_SHADE,
                       str(slot.get("csf_lightmap", "")) if slot else "")
                if key not in materials:
                    materials[key] = len(materials)
                    material_lines.append(f"material {key[0]} {key[1]} {key[2]}" + (f" {key[3]}" if key[3] else ""))
                first = len(vertex_lines)
                for loop in triangle.loops:
                    co = mesh.vertices[mesh.loops[loop].vertex_index].co
                    n = normals[loop].vector
                    fields = [co.x * 100, co.z * 100, -co.y * 100, n.x, n.z, -n.y]
                    for layer in uv_layers:
                        uv = layer.uv[loop].vector
                        fields += [uv.x, 1.0 - uv.y]
                    if not uv_layers:
                        fields += [0.0, 0.0]
                    vertex_lines.append("v " + " ".join(f"{value:.6g}" for value in fields))
                face_lines.append(f"f {first} {first + 1} {first + 2} {materials[key]} {role}")
        finally:
            evaluated.to_mesh_clear()
    if not face_lines:
        raise ValueError("no visible mesh faces to export")
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("csfworld 1\n")
        out.write(f"# exported from {bpy.data.filepath or 'an unsaved scene'} by export_csf_world.py\n")
        out.write("\n".join(material_lines + vertex_lines + face_lines) + "\n")
    return {"materials": len(material_lines), "vertices": len(vertex_lines), "faces": len(face_lines)}
