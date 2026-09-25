"""CSF authoring: Blender side of an rws-man authoring project.

Blender owns mesh sources; rws-man owns placements and mission records
(docs/plans/editor-blender-authoring.md). The sidebar tab "CSF" (3D view, N):

* Project: the authoring project folder (with project.csfproj) and csf-mod.
* Asset: tag selected meshes as the project's terrain or as a building (a
  building is exported about its own origin; rws-man places it).
* Bake lighting: bakes each tagged asset's light (Cycles, no colour) into its
  own lightmap, saved into the project; Send then uses it.
* Send to rws-man: saves the .blend, exports every tagged asset into the
  project and registers new ones. An open rws-man rebuilds the map by itself.
* Load reference: the built map, actor models and the mission's placements,
  navigation routes, areas and dummies in a locked "CSF reference" collection.
  It is view-only and never exported.
* Import donor model: a vanilla .rpc as an editable mesh, to start an asset.
"""

from __future__ import annotations

import json
import math
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import bpy
from bpy.props import EnumProperty, StringProperty

from . import csfworld

bl_info = {
    "name": "CSF authoring",
    "author": "rws-man",
    "version": (0, 1, 0),
    "blender": (4, 2, 0),
    "location": "3D View > Sidebar > CSF",
    "description": "Terrain and building assets for rws-man authoring projects",
    "category": "Import-Export",
}

REFERENCE = "CSF reference"
KINDS = (("terrain", "Terrain", "Built into the World where it is modelled"),
         ("building", "Building", "Exported about its origin; rws-man places it"))


# ---- Project file ----------------------------------------------------------------


def tokens(line: str) -> list[str]:
    """Fields of a project.csfproj line (double-quoted strings with \\ escapes)."""
    out, i = [], 0
    while i < len(line):
        if line[i] in " \t\r":
            i += 1
            continue
        if line[i] == '"':
            i += 1
            text = []
            while i < len(line) and line[i] != '"':
                if line[i] == "\\" and i + 1 < len(line):
                    i += 1
                text.append(line[i])
                i += 1
            out.append("".join(text))
            i += 1
        else:
            j = i
            while j < len(line) and line[j] not in " \t\r":
                j += 1
            out.append(line[i:j])
            i = j
    return out


def project_assets(project: Path) -> dict[str, dict]:
    assets = {}
    for line in (project / "project.csfproj").read_text(encoding="utf-8").splitlines():
        fields = tokens(line)
        if len(fields) == 5 and fields[0] == "asset":
            assets[fields[1]] = {"kind": fields[2], "blend": fields[3], "export": fields[4]}
    return assets


def to_blender(position) -> tuple[float, float, float]:
    x, y, z = position
    return x / 100.0, -z / 100.0, y / 100.0


# ---- Settings --------------------------------------------------------------------


class CsfPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__
    csf_mod: StringProperty(name="csf-mod", subtype="FILE_PATH",
                            description="The csf-mod executable (rws-man's build/Release/csf-mod); empty: from PATH")

    def draw(self, context):
        self.layout.prop(self, "csf_mod")


def csf_mod(context) -> str:
    configured = context.preferences.addons[__package__].preferences.csf_mod if __package__ in \
        context.preferences.addons else ""
    path = bpy.path.abspath(configured) if configured else (os.environ.get("CSF_MOD") or shutil.which("csf-mod"))
    if not path:
        raise RuntimeError("Set the csf-mod executable in the add-on preferences")
    return path


def project_dir(context) -> Path:
    path = Path(bpy.path.abspath(context.scene.csf_project))
    if not (path / "project.csfproj").is_file():
        raise RuntimeError("Choose the authoring project folder (with project.csfproj)")
    return path


def run(context, *arguments) -> str:
    result = subprocess.run([csf_mod(context), *map(str, arguments)], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError((result.stderr or result.stdout).strip() or f"csf-mod {arguments[0]} failed")
    return result.stdout


# ---- Operators -------------------------------------------------------------------


class CSF_OT_tag_asset(bpy.types.Operator):
    """Tag the selected meshes as a project asset"""
    bl_idname = "csf.tag_asset"
    bl_label = "Tag as asset"
    bl_options = {"REGISTER", "UNDO"}
    kind: EnumProperty(items=KINDS)

    def execute(self, context):
        used = {obj.get("csf_asset_id") for obj in bpy.data.objects}
        for obj in context.selected_objects:
            if obj.type != "MESH" or csfworld.is_reference(obj):
                continue
            obj["csf_asset_kind"] = self.kind
            if not obj.get("csf_asset_id"):
                base = re.sub(r"[^A-Za-z0-9_-]+", "-", obj.name).strip("-").lower() or self.kind
                ident, n = base, 2
                while ident in used:
                    ident, n = f"{base}-{n}", n + 1
                obj["csf_asset_id"] = ident
                used.add(ident)
        return {"FINISHED"}


class CSF_OT_untag_asset(bpy.types.Operator):
    """Stop exporting the selected meshes as project assets"""
    bl_idname = "csf.untag_asset"
    bl_label = "Untag"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        for obj in context.selected_objects:
            for key in ("csf_asset_kind", "csf_asset_id"):
                if key in obj:
                    del obj[key]
        return {"FINISHED"}


def tagged_assets(scene) -> dict[str, tuple[str, list]]:
    """Asset ID -> (kind, objects) for the tagged, visible, non-reference meshes."""
    assets: dict[str, tuple[str, list]] = {}
    for obj in csfworld.exportable(scene):
        ident, kind = obj.get("csf_asset_id"), obj.get("csf_asset_kind")
        if not ident or kind not in ("terrain", "building"):
            continue
        known_kind, objects = assets.setdefault(str(ident), (str(kind), []))
        if known_kind != kind:
            raise RuntimeError(f"Asset '{ident}' mixes terrain and building objects")
        objects.append(obj)
    return assets


def send(context) -> list[str]:
    project = project_dir(context)
    assets = tagged_assets(context.scene)
    if not assets:
        raise RuntimeError("No tagged assets: tag the terrain (and buildings) first")
    if bpy.data.filepath:
        bpy.ops.wm.save_mainfile()
    blend = Path(bpy.data.filepath) if bpy.data.filepath else None
    known = project_assets(project)
    lines = []
    for ident, (kind, objects) in sorted(assets.items()):
        if ident in known and known[ident]["kind"] != kind:
            raise RuntimeError(f"Asset '{ident}' is a {known[ident]['kind']} in the project")
        export = known[ident]["export"] if ident in known else \
            f"sources/{'world' if kind == 'terrain' else 'buildings'}/{ident}.csfworld"
        target = project / export
        target.parent.mkdir(parents=True, exist_ok=True)
        # Written beside the target, then renamed: rws-man never sees half a file.
        with tempfile.NamedTemporaryFile(dir=target.parent, suffix=".tmp", delete=False) as handle:
            temporary = Path(handle.name)
        stats = csfworld.export(str(temporary), context.scene, objects, local=kind == "building")
        temporary.replace(target)
        blend_field = blend.relative_to(project).as_posix() if blend and blend.is_relative_to(project) else \
            (blend.as_posix() if blend else "")
        if ident not in known or known[ident]["blend"] != blend_field:
            run(context, "project-asset", project, ident, kind, blend_field or "-", export)
        lines.append(f"{ident}: {stats['faces']} faces")
    return lines


LIGHTMAP_UV = "CSF_Lightmap"


def lightmap_name(ident: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]+", "_", ident).upper() + "_Lm"


def ensure_lightmap_uv(obj) -> None:
    """A second UV layer for the lightmap, unwrapped when the object has none."""
    mesh = obj.data
    if len(mesh.uv_layers) >= 2:
        return
    if not mesh.uv_layers:
        mesh.uv_layers.new(name="UVMap")
    layer = mesh.uv_layers.new(name=LIGHTMAP_UV)
    mesh.uv_layers.active = layer
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(island_margin=0.02)
    bpy.ops.object.mode_set(mode="OBJECT")
    mesh.uv_layers.active = mesh.uv_layers[0]


def own_materials(objects, ident: str) -> list:
    """The asset's materials, copied when another object uses them too, so each
    asset's materials name its own lightmap."""
    owned = []
    members = set(objects)
    for obj in objects:
        for slot in obj.material_slots:
            material = slot.material
            if material is None:
                continue
            others = [o for o in bpy.data.objects if o not in members and any(
                s.material == material for s in getattr(o, "material_slots", []))]
            if others:
                material = material.copy()
                material.name = f"{slot.material.name}.{ident}"
                slot.material = material
            if material not in owned:
                owned.append(material)
    return owned


def bake_lightmaps(context, size: int, samples: int) -> list[str]:
    """Bake each tagged asset's diffuse lighting (direct and indirect, no colour)
    into <ASSET>_Lm and save it into the project for rws-man to build."""
    import numpy as np
    project = project_dir(context)
    assets = tagged_assets(context.scene)
    if not assets:
        raise RuntimeError("No tagged assets to bake")
    scene = context.scene
    if not any(obj.type == "LIGHT" for obj in scene.objects) and scene.world is None:
        raise RuntimeError("The scene has no lights; add a Sun (its rotation sets the light direction)")
    scene.render.engine = "CYCLES"
    scene.cycles.samples = samples
    scene.render.bake.use_pass_direct = True
    scene.render.bake.use_pass_indirect = True
    scene.render.bake.use_pass_color = False
    scene.render.bake.margin = 4
    out_dir = project / "sources" / "lightmaps"
    out_dir.mkdir(parents=True, exist_ok=True)
    lines = []
    for ident, (_, objects) in sorted(assets.items()):
        name = lightmap_name(ident)
        for obj in objects:
            ensure_lightmap_uv(obj)
        image = bpy.data.images.get(name) or bpy.data.images.new(name, size, size, float_buffer=True, alpha=False)
        if tuple(image.size) != (size, size):
            image.scale(size, size)
        image.colorspace_settings.name = "Non-Color"
        materials = own_materials(objects, ident)
        nodes = []
        for material in materials:
            material.use_nodes = True
            node = material.node_tree.nodes.new("ShaderNodeTexImage")
            node.image = image
            material.node_tree.nodes.active = node
            nodes.append((material, node))
            material["csf_lightmap"] = name
        active_uv = {}
        for obj in objects:
            active_uv[obj] = obj.data.uv_layers.active_index
            obj.data.uv_layers.active_index = 1
        try:
            bpy.ops.object.select_all(action="DESELECT")
            for obj in objects:
                obj.select_set(True)
            context.view_layer.objects.active = objects[0]
            bpy.ops.object.bake(type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"}, margin=4, use_clear=True)
        finally:
            for obj, index in active_uv.items():
                obj.data.uv_layers.active_index = index
            for material, node in nodes:
                material.node_tree.nodes.remove(node)
        # CSF modulates lightmaps 2x: store half the light (the lightmap add-on's CSF RGB Scale).
        pixels = np.empty(size * size * 4, dtype=np.float32)
        image.pixels.foreach_get(pixels)
        pixels[0::4] *= 0.5
        pixels[1::4] *= 0.5
        pixels[2::4] *= 0.5
        pixels[3::4] = 1.0
        np.clip(pixels, 0.0, 1.0, out=pixels)
        png = bpy.data.images.new(name + "_png", size, size, alpha=False)
        png.colorspace_settings.name = "Non-Color"
        png.pixels.foreach_set(pixels)
        target = out_dir / f"{name}.png"
        png.filepath_raw = str(target)
        png.file_format = "PNG"
        png.save()
        bpy.data.images.remove(png)
        run(context, "project-lightmap", project, name, target.relative_to(project).as_posix())
        lines.append(f"{name} ({size}x{size})")
    return lines


class CSF_OT_bake(bpy.types.Operator):
    """Bake the tagged assets' lighting into lightmaps for the project (Cycles, light only)"""
    bl_idname = "csf.bake"
    bl_label = "Bake lighting"
    size: EnumProperty(items=(("512", "512", ""), ("1024", "1024", ""), ("2048", "2048", "")), default="1024")
    samples: bpy.props.IntProperty(name="Samples", default=64, min=1, max=4096)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        try:
            lines = bake_lightmaps(context, int(self.size), self.samples)
        except (RuntimeError, OSError, ValueError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        self.report({"INFO"}, "Baked " + "; ".join(lines) + ". Send to rws-man to use them.")
        return {"FINISHED"}


class CSF_OT_send(bpy.types.Operator):
    """Export the tagged assets into the project; rws-man rebuilds the map"""
    bl_idname = "csf.send"
    bl_label = "Send to rws-man"

    def execute(self, context):
        try:
            lines = send(context)
        except (RuntimeError, OSError, ValueError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        self.report({"INFO"}, "Sent " + "; ".join(lines))
        return {"FINISHED"}


def reference_collection(context, clear: bool):
    collection = bpy.data.collections.get(REFERENCE)
    if collection and clear:
        for obj in list(collection.all_objects):
            bpy.data.objects.remove(obj, do_unlink=True)
        for child in list(collection.children_recursive):
            bpy.data.collections.remove(child)
    if collection is None:
        collection = bpy.data.collections.new(REFERENCE)
        context.scene.collection.children.link(collection)
    collection["csf_reference"] = True
    collection.hide_select = True
    return collection


def child_collection(parent, name: str):
    collection = bpy.data.collections.new(name)
    collection["csf_reference"] = True
    parent.children.link(collection)
    return collection


def import_gltf(path: Path, into) -> list:
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(path))
    imported = [obj for obj in bpy.data.objects if obj not in before]
    for obj in imported:
        for collection in list(obj.users_collection):
            collection.objects.unlink(obj)
        into.objects.link(obj)
        obj["csf_reference"] = True
    return imported


def wire_object(name: str, vertices, edges, into):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(vertices, edges, [])
    obj = bpy.data.objects.new(name, mesh)
    obj.display_type = "WIRE"
    obj.show_in_front = True
    obj["csf_reference"] = True
    into.objects.link(obj)
    return obj


def marker(name: str, position, heading: float, into, display: str = "SINGLE_ARROW", size: float = 0.5):
    obj = bpy.data.objects.new(name, None)
    obj.empty_display_type = display
    obj.empty_display_size = size
    obj.location = to_blender(position)
    obj.rotation_euler = (0.0, 0.0, math.radians(heading))
    obj.show_name = True
    obj["csf_reference"] = True
    into.objects.link(obj)
    return obj


def load_reference(context) -> str:
    project = project_dir(context)
    out = project / "build" / "reference"
    run(context, "project-reference", project, out)
    markers = json.loads((out / "markers.json").read_text(encoding="utf-8"))
    root = reference_collection(context, clear=True)
    world = child_collection(root, "CSF world")
    for obj in import_gltf(out / "world.gltf", world):
        if obj.name.startswith("clump_"):
            # Clump prototypes at their donor positions; the placed props are
            # the prototype_*_instance_* objects.
            bpy.data.objects.remove(obj, do_unlink=True)
        elif obj.name.startswith("world_"):
            # The built World (terrain and pieces) as wire: it would otherwise
            # z-fight with the terrain being edited.
            obj.display_type = "WIRE"
    # Each actor model once, instanced at every actor of its class.
    models = child_collection(root, "CSF models")
    models.hide_viewport = True
    model_collections = {}
    for actor in markers["actors"]:
        name = actor.get("model")
        if name and name not in model_collections:
            collection = child_collection(models, f"class {actor['class']}")
            import_gltf(out / name, collection)
            model_collections[name] = collection
    actors = child_collection(root, "CSF actors")
    for actor in markers["actors"]:
        obj = marker(f"{actor['name']} ({actor['id']})", actor["position"], actor["heading"], actors)
        if actor.get("model") in model_collections:
            obj.instance_type = "COLLECTION"
            obj.instance_collection = model_collections[actor["model"]]
    placements = child_collection(root, "CSF placements")
    for placement in markers["placements"]:
        marker(placement["id"], placement["position"], placement["yaw"], placements, "PLAIN_AXES", 0.3)
    navigation = child_collection(root, "CSF navigation")
    for group in markers["navigation"]:
        index = {p["id"]: k for k, p in enumerate(group["points"])}
        vertices = [to_blender(p["position"]) for p in group["points"]]
        edges = [(index[a], index[b]) for a, b in group["links"] if a in index and b in index]
        wire_object(f"{group['name'] or 'group'} ({group['id']})", vertices, edges, navigation)
    areas = child_collection(root, "CSF areas")
    for area in markers["areas"]:
        ring = [to_blender(p) for p in area["points"]]
        top = [(x, y, z + area["height"] / 100.0) for x, y, z in ring]
        n = len(ring)
        edges = [(k, (k + 1) % n) for k in range(n)] + [(n + k, n + (k + 1) % n) for k in range(n)] + \
                [(k, n + k) for k in range(n)]
        wire_object(f"{area['name'] or 'area'} ({area['id']})", ring + top, edges, areas)
    dummies = child_collection(root, "CSF dummies")
    for dummy in markers["dummies"]:
        marker(f"{dummy['name']} ({dummy['id']})", dummy["position"], dummy["heading"], dummies, "ARROWS", 0.4)
    return (f"{len(markers['actors'])} actors, {len(markers['placements'])} placements, "
            f"{len(markers['navigation'])} routes, {len(markers['areas'])} areas")


class CSF_OT_load_reference(bpy.types.Operator):
    """Load the built map and the mission's records as a locked, view-only reference"""
    bl_idname = "csf.load_reference"
    bl_label = "Load reference"

    def execute(self, context):
        try:
            summary = load_reference(context)
        except (RuntimeError, OSError, ValueError, KeyError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        self.report({"INFO"}, "Reference: " + summary)
        return {"FINISHED"}


class CSF_OT_clear_reference(bpy.types.Operator):
    """Remove the reference collection"""
    bl_idname = "csf.clear_reference"
    bl_label = "Clear reference"

    def execute(self, context):
        collection = bpy.data.collections.get(REFERENCE)
        if collection:
            reference_collection(context, clear=True)
            bpy.data.collections.remove(collection)
        return {"FINISHED"}


class CSF_OT_import_model(bpy.types.Operator):
    """Import a vanilla model (.rpc) as an editable mesh"""
    bl_idname = "csf.import_model"
    bl_label = "Import donor model"
    filepath: StringProperty(subtype="FILE_PATH")
    filter_glob: StringProperty(default="*.rpc;*.rws;*.dff", options={"HIDDEN"})

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        try:
            rws_info = Path(csf_mod(context)).with_name("rws-info")
            with tempfile.TemporaryDirectory() as directory:
                out = Path(directory) / "model.gltf"
                result = subprocess.run([str(rws_info), self.filepath, "--export-scene-gltf", str(out)],
                                        capture_output=True, text=True)
                if result.returncode != 0 or not out.is_file():
                    raise RuntimeError((result.stderr or result.stdout).strip() or "rws-info failed")
                before = set(bpy.data.objects)
                bpy.ops.import_scene.gltf(filepath=str(out))
                imported = [obj for obj in bpy.data.objects if obj not in before]
        except (RuntimeError, OSError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        self.report({"INFO"}, f"Imported {len(imported)} objects")
        return {"FINISHED"}


# ---- Panel -----------------------------------------------------------------------


class CSF_PT_authoring(bpy.types.Panel):
    bl_label = "CSF authoring"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "CSF"

    def draw(self, context):
        layout = self.layout
        layout.prop(context.scene, "csf_project", text="Project")
        box = layout.box()
        box.label(text="Asset")
        obj = context.active_object
        if obj and obj.type == "MESH" and not csfworld.is_reference(obj):
            if obj.get("csf_asset_id"):
                box.label(text=f"{obj['csf_asset_kind']}: {obj['csf_asset_id']}")
                box.prop(obj, '["csf_asset_id"]', text="ID")
            row = box.row(align=True)
            row.operator("csf.tag_asset", text="Terrain").kind = "terrain"
            row.operator("csf.tag_asset", text="Building").kind = "building"
            row.operator("csf.untag_asset", text="", icon="X")
        else:
            box.label(text="Select a mesh to tag it")
        layout.operator("csf.bake", icon="LIGHT_SUN")
        layout.operator("csf.send", icon="EXPORT")
        row = layout.row(align=True)
        row.operator("csf.load_reference", icon="LINKED")
        row.operator("csf.clear_reference", text="", icon="TRASH")
        layout.operator("csf.import_model", icon="IMPORT")


CLASSES = (CsfPreferences, CSF_OT_tag_asset, CSF_OT_untag_asset, CSF_OT_bake, CSF_OT_send, CSF_OT_load_reference,
           CSF_OT_clear_reference, CSF_OT_import_model, CSF_PT_authoring)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.csf_project = StringProperty(name="Project", subtype="DIR_PATH",
                                                 description="The authoring project folder (with project.csfproj)")


def unregister():
    del bpy.types.Scene.csf_project
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
