"""Export the visible mesh objects of a Blender scene to a `.csfworld` file.

`csf-mod world-build` (or an authoring project's build) compiles the result
into a map `.rws` and `_col.rws` (format: include/rws/world_source.hpp).

Conventions
-----------
* Blender metres, Z up  ->  game centimetres, source Y up: game = 100 * (x, z, -y).
  This is the inverse of the glTF path (csf-editor exports Y-up glTF at 0.01 and
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
* A face attribute ``csf_shade`` (integer, per face) overrides the material's
  shade for that face: the collision byte a decompiled map keeps per triangle.
* Materials a glTF import made from ``rws-info --export-scene-gltf`` fall back
  to their ``rws_base_texture``, ``rws_surface_name`` and
  ``rws_lightmap_texture`` properties; that glTF's props (Clumps and scene
  instances, ``rws_kind``) are never exported, as the map keeps them.
* Names with spaces are written in double quotes; ``-`` is an untextured
  material (``-#RRGGBBAA`` of that colour). Names are the game's bytes, read
  and written as Latin-1 (shipped names use cp1252 letters such as Ñ);
  comments are UTF-8.

`load()` is the other direction: a `.csfworld` (``csf-mod world-source`` of a
shipped map) as one object per role, with the names above as properties, the
per-triangle collision shade as ``csf_shade`` and, when found, the textures.
"""

from __future__ import annotations

import os

import bpy
import mathutils

DEFAULT_SURFACE = "Tierra"
DEFAULT_SHADE = 228
ROLES = ("both", "visual", "collision")


PROP_KINDS = ("clump_atomic", "csf_instance")


def material_texture(material) -> str:
    if material is None:
        return "default"
    if "csf_texture" in material:
        return str(material["csf_texture"])
    if material.get("rws_base_texture"):
        return str(material["rws_base_texture"])
    if material.use_nodes and material.node_tree:
        for node in material.node_tree.nodes:
            if node.type == "TEX_IMAGE" and node.image:
                return os.path.splitext(node.image.name)[0]
    return material.name


def material_surface(material) -> str:
    if material is None:
        return DEFAULT_SURFACE
    return str(material.get("csf_surface") or material.get("rws_surface_name") or DEFAULT_SURFACE)


def material_lightmap(material) -> str:
    if material is None:
        return ""
    return str(material["csf_lightmap"] if "csf_lightmap" in material else material.get("rws_lightmap_texture", ""))


def quoted(name: str) -> str:
    return f'"{name}"' if any(c.isspace() for c in name) else name


def is_reference(obj) -> bool:
    """Objects the add-on loaded for context (never exported)."""
    return bool(obj.get("csf_reference")) or any(c.get("csf_reference") for c in obj.users_collection)


def exportable(scene) -> list:
    return [obj for obj in scene.objects
            if obj.type == "MESH" and not obj.hide_get() and not obj.hide_render and not is_reference(obj)
            and obj.get("rws_kind") not in PROP_KINDS]


def export(path: str, scene=None, objects=None, local: bool = False, precise: bool = False) -> dict:
    """Write `objects` (default: every visible, non-reference mesh of `scene`).

    With `local`, vertices are in each object's own space (scaled, not rotated
    or moved): a building asset, which the editor places. Numbers have six
    significant digits (a project's builds are pinned to that); `precise`
    writes nine, which keeps Blender's floats exactly, as an edited shipped map
    (coordinates to 100000 cm) needs.
    """
    number = "{:.9g}" if precise else "{:.6g}"
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
            shades = mesh.attributes.get("csf_shade")
            if shades is not None and (shades.domain != "FACE" or shades.data_type != "INT"):
                shades = None
            for triangle in mesh.loop_triangles:
                slot = obj.material_slots[triangle.material_index].material if obj.material_slots else None
                shade = int(slot.get("csf_shade", DEFAULT_SHADE)) if slot else DEFAULT_SHADE
                if shades is not None:
                    shade = min(max(shades.data[triangle.polygon_index].value, 0), 255)
                key = (material_texture(slot), material_surface(slot), shade, material_lightmap(slot))
                if key not in materials:
                    materials[key] = len(materials)
                    material_lines.append(f"material {quoted(key[0])} {quoted(key[1])} {key[2]}" +
                                          (f" {quoted(key[3])}" if key[3] else ""))
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
                    vertex_lines.append("v " + " ".join(number.format(value) for value in fields))
                face_lines.append(f"f {first} {first + 1} {first + 2} {materials[key]} {role}")
        finally:
            evaluated.to_mesh_clear()
    if not face_lines:
        raise ValueError("no visible mesh faces to export")
    try:
        body = "\n".join(material_lines + vertex_lines + face_lines).encode("latin-1")
    except UnicodeEncodeError as error:
        raise ValueError(f"a material name has a character the game cannot hold: {error.object[error.start:error.end]!r}")
    with open(path, "wb") as out:
        out.write(b"csfworld 1\n")
        out.write(f"# exported from {bpy.data.filepath or 'an unsaved scene'} by export_csf_world.py\n".encode("utf-8"))
        out.write(body + b"\n")
    return {"materials": len(material_lines), "vertices": len(vertex_lines), "faces": len(face_lines)}


# ---- Import ----------------------------------------------------------------------


def fields(line: str) -> list[str]:
    """Tokens of a .csfworld line (a double-quoted name may hold spaces)."""
    out, i = [], 0
    while i < len(line):
        if line[i].isspace():
            i += 1
        elif line[i] == '"':
            end = line.find('"', i + 1)
            end = len(line) if end < 0 else end
            out.append(line[i + 1:end])
            i = end + 1
        else:
            j = i
            while j < len(line) and not line[j].isspace():
                j += 1
            out.append(line[i:j])
            i = j
    return out


def find_texture(directory: str, name: str) -> str | None:
    """<directory>/<name>.dds or .png, matched without regard to case."""
    if not directory or not os.path.isdir(directory):
        return None
    wanted = {f"{name}.dds".lower(), f"{name}.png".lower()}
    for entry in os.listdir(directory):
        if entry.lower() in wanted:
            return os.path.join(directory, entry)
    return None


def surface_color(name: str) -> tuple:
    """A stable colour per collision surface."""
    import colorsys
    hue = (sum(ord(c) * 31 ** i for i, c in enumerate(name.lower())) % 360) / 360.0
    return (*colorsys.hsv_to_rgb(hue, 0.55, 0.9), 1.0)


def make_material(key: tuple, textures: str, collision: bool):
    texture, surface, lightmap = key
    name = f"{surface} (collision)" if collision else texture + (f" [{lightmap}]" if lightmap else "")
    material = bpy.data.materials.new(name)
    material["csf_texture"] = texture
    material["csf_surface"] = surface
    if lightmap:
        material["csf_lightmap"] = lightmap
    material.use_nodes = True
    shader = material.node_tree.nodes.get("Principled BSDF")
    color = surface_color(surface) if collision else (0.8, 0.8, 0.8, 1.0)
    if not collision and texture.startswith("-#") and len(texture) == 10:
        color = tuple(int(texture[2 + 2 * k:4 + 2 * k], 16) / 255.0 for k in range(4))
    material.diffuse_color = color
    if shader:
        shader.inputs["Base Color"].default_value = color
        image_path = None if collision or texture.startswith("-") else find_texture(textures, texture)
        if image_path:
            node = material.node_tree.nodes.new("ShaderNodeTexImage")
            node.image = bpy.data.images.load(image_path, check_existing=True)
            node.location = (-300, 300)
            material.node_tree.links.new(node.outputs["Color"], shader.inputs["Base Color"])
    return material


def load(path: str, textures: str | None = None, collection=None) -> dict:
    """Import a .csfworld as one mesh object per role (visual, collision, both).

    `textures` is the folder of the map's DDS textures; by default the
    Textures folder next to the map a `# world-source <map>` comment names.
    """
    materials, vertices, faces = [], [], []
    source_map = None
    with open(path, "rb") as source:
        for raw in source:
            if raw.startswith(b"#"):
                if raw.startswith(b"# world-source "):
                    source_map = raw[len(b"# world-source "):].decode("utf-8", "replace").strip()
                continue
            parts = fields(raw.decode("latin-1"))
            if not parts or parts[0].startswith("#") or parts[0] == "csfworld":
                continue
            if parts[0] == "material":
                materials.append((parts[1], parts[2], int(parts[3]) if len(parts) > 3 else DEFAULT_SHADE,
                                  parts[4] if len(parts) > 4 else ""))
            elif parts[0] == "v":
                values = [float(v) for v in parts[1:]]
                vertices.append(values if len(values) == 10 else values + [None, None])
            elif parts[0] == "f":
                faces.append((int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]), parts[5]))
            elif parts[0] in ("prop", "piece"):
                raise ValueError(f"{path}: {parts[0]} lines are not imported (a project's source)")
    if textures is None and source_map:
        textures = os.path.join(os.path.dirname(source_map), "Textures")
    collection = collection or bpy.context.scene.collection
    stem = os.path.splitext(os.path.basename(path))[0]
    created = []
    for role in ROLES:
        role_faces = [f for f in faces if f[4] == role]
        if not role_faces:
            continue
        collision = role == "collision"
        # Welded by exact position so edits do not tear sector seams; UVs,
        # normals and shades stay per corner or per face.
        index: dict[tuple, int] = {}
        positions, loops, loop_uv0, loop_uv1, loop_normals = [], [], [], [], []  # loops: per face
        slots: dict[tuple, int] = {}
        face_slots, face_shades = [], []
        has_uv1 = any(vertices[f[k]][8] is not None for f in role_faces for k in range(3))
        for a, b, c, m, _ in role_faces:
            texture, surface, shade, lightmap = materials[m]
            key = (texture, surface, lightmap)
            face_slots.append(slots.setdefault(key, len(slots)))
            face_shades.append(shade)
            corners = [vertices[k] for k in (a, b, c)]
            points = [(x / 100.0, -z / 100.0, y / 100.0) for x, y, z, *_ in corners]
            welded = [index.get(point) for point in points]
            if len(set(points)) < 3:
                welded = [None] * 3  # degenerate: its own vertices, never welded
            for k, point in enumerate(points):
                if welded[k] is None:
                    welded[k] = len(positions)
                    positions.append(point)
                    if len(set(points)) == 3:
                        index[point] = welded[k]
            loops.append(tuple(welded))
            for x, y, z, nx, ny, nz, u0, v0, u1, v1 in corners:
                loop_uv0.append((u0, 1.0 - v0))
                loop_uv1.append((0.5, 0.5) if u1 is None else (u1, 1.0 - v1))
                loop_normals.append((nx, -nz, ny))
        mesh = bpy.data.meshes.new(f"{stem} {role}")
        mesh.from_pydata(positions, [], loops)
        mesh.polygons.foreach_set("material_index", face_slots)
        if not collision:
            for name, values in (("UVMap", loop_uv0), ("Lightmap", loop_uv1))[:2 if has_uv1 else 1]:
                layer = mesh.uv_layers.new(name=name)
                layer.data.foreach_set("uv", [c for uv in values for c in uv])
        if role != "visual":
            shades = mesh.attributes.new("csf_shade", "INT", "FACE")
            shades.data.foreach_set("value", face_shades)
        mesh.update()
        if not collision and any(any(n) for n in loop_normals):
            mesh.normals_split_custom_set(loop_normals)
        for key in sorted(slots, key=slots.get):
            mesh.materials.append(make_material(key, textures, collision))
        obj = bpy.data.objects.new(f"{stem} {role}", mesh)
        obj["csf_role"] = role
        if collision:
            obj.display_type = "WIRE"
        collection.objects.link(obj)
        created.append(obj)
    if source_map:
        bpy.context.scene["csf_source_map"] = source_map
    return {"objects": len(created), "faces": len(faces), "materials": len(materials), "textures": textures or ""}
