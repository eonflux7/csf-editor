"""Headless integration check for the RWS Lightmaps add-on.

Usage:
  blender --background --factory-startup --python smoke_test.py -- scene.gltf scene.manifest.json Textures
"""

import sys
from pathlib import Path

import bpy


arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
async_bake = len(arguments) == 4 and arguments[3] == "--async-bake"
if len(arguments) not in ({4} if async_bake else {3}):
    raise SystemExit(
        "Expected: <scene.gltf> <scene.manifest.json> <Textures directory> [--async-bake]")

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rws_lightmaps

rws_lightmaps.register()
scene = bpy.context.scene
bpy.ops.import_scene.gltf(filepath=str(Path(arguments[0]).resolve()))
scene.rws_lightmap_texture_directory = ""
scene.rws_lightmap_manifest = str(Path(arguments[1]).resolve())
assert Path(scene.rws_lightmap_texture_directory).resolve() == Path(arguments[2]).resolve()
result = bpy.ops.rws_lightmaps.configure()
assert result == {"FINISHED"}, result
assert scene.rws_lightmap_material_count > 0
assert scene.rws_lightmap_texture_count > 0
configured_materials = [
    material for material in bpy.data.materials
    if material.get("rws_lightmaps_configured") and material.node_tree
]
assert configured_materials
for material in configured_materials:
    nodes = material.node_tree.nodes
    assert nodes.get("RWS_TRANSPARENT")
    assert nodes.get("RWS_ALPHA_MIX")
    assert nodes.get("RWS_ALPHA_MIX").outputs["Shader"].is_linked
for material in configured_materials:
    if material.get("rws_uses_base_alpha"):
        alpha_mix = material.node_tree.nodes["RWS_ALPHA_MIX"]
        assert alpha_mix.inputs[0].is_linked
    if Path(material.get("rws_base_texture", "")).stem.upper().startswith("FFLR"):
        assert not material.get("rws_uses_base_alpha")
for mode in ("BASE", "LIGHTMAP", "COMBINED"):
    scene.rws_lightmap_view = mode
    assert mode.replace("_", " ").title() in scene.rws_lightmap_status
    expected_strength = 1.0 if mode == "BASE" else scene.rws_lightmap_intensity
    for material in configured_materials:
        assert material.node_tree.nodes["RWS_EMISSION"].inputs["Strength"].default_value == expected_strength
loaded_base = next(
    (material.node_tree.nodes["RWS_BASE"].image for material in configured_materials
     if material.node_tree.nodes["RWS_BASE"].image), None)
assert loaded_base and loaded_base.colorspace_settings.name == "Non-Color"

# Exercise world selection and conservative coincident-floor-shell detection.
assert bpy.ops.rws_lightmaps.select_world_sectors() == {"FINISHED"}
assert "coincident terrain shells" in scene.rws_lightmap_status

# Validate the non-destructive bake setup on one lightmapped mesh without
# performing an expensive render during the integration test.
bpy.ops.object.select_all(action="DESELECT")
bake_object = next(
    obj for obj in bpy.data.objects if obj.type == "MESH" and any(
        slot.material and slot.material.get("rws_lightmap_path") for slot in obj.material_slots))
bake_object.select_set(True)
bpy.context.view_layer.objects.active = bake_object
previous_uv = bake_object.data.uv_layers.active_index
scene.rws_bake_resolution_scale = "2"
assert bpy.ops.rws_lightmaps.prepare_bake() == {"FINISHED"}
prepared = [slot.material for slot in bake_object.material_slots
            if slot.material and slot.material.get("rws_bake_prepared")]
assert prepared
passive = [material for material in bpy.data.materials
           if material.get("rws_bake_passive")]
assert passive
for material in passive:
    nodes = material.node_tree.nodes
    passive_shader = nodes.get("RWS_BAKE_PASSIVE")
    alpha_mix = nodes.get("RWS_ALPHA_MIX")
    assert passive_shader and alpha_mix
    assert alpha_mix.inputs[2].links[0].from_node == passive_shader
    assert passive_shader.inputs["Base Color"].links[0].from_node == nodes["RWS_BASE"]
assert bake_object.data.uv_layers.active_index == 1
assert bake_object.data.uv_layers[1].active_render
for material in prepared:
    nodes = material.node_tree.nodes
    assert nodes.get("RWS_BAKE_TARGET") and nodes["RWS_BAKE_TARGET"].image
    source_size = tuple(nodes["RWS_LIGHTMAP"].image.size)
    assert tuple(nodes["RWS_BAKE_TARGET"].image.size) == tuple(value * 2 for value in source_size)
    assert nodes["RWS_BAKE_TARGET"].image.get("rws_resolution_scale") == 2
    assert nodes.active == nodes["RWS_BAKE_TARGET"]
    assert nodes.get("RWS_BAKE_DIFFUSE")
    assert nodes["RWS_BAKE_DIFFUSE"].inputs["Base Color"].links[0].from_node == nodes["RWS_BASE"]
    assert nodes["RWS_ALPHA_MIX"].inputs[2].links[0].from_node == nodes["RWS_BAKE_DIFFUSE"]
assert bpy.ops.rws_lightmaps.add_preview_sun() == {"FINISHED"}
preview_sun = bpy.data.objects.get("RWS Preview Sun")
assert preview_sun and preview_sun.type == "LIGHT" and preview_sun.data.type == "SUN"
assert preview_sun.data.energy == 4.0
assert bake_object.get("rws_bake_uv_prepared")
assert bpy.ops.rws_lightmaps.diagnose_lighting() == {"FINISHED"}
assert f"shaders {len(prepared)}/{len(prepared)}" in scene.rws_lightmap_status
assert bpy.ops.rws_lightmaps.optimize_cycles() == {"FINISHED"}
assert scene.cycles.device == "GPU"
assert scene.cycles.samples == 32
assert scene.cycles.diffuse_bounces == 2
for material in prepared:
    material.node_tree.nodes["RWS_BAKE_TARGET"].image.scale(32, 32)


def verify_bake_and_restore():
    for material in prepared:
        target = material.node_tree.nodes["RWS_BAKE_TARGET"].image
        low, high = rws_lightmaps._bake_image_sample_range(target)
        assert high - low > 1.0e-5, (target.name, low, high)
        assert target.get("rws_bake_completed_by") == "rws-lightmaps-batch-v1"
    assert bpy.ops.rws_lightmaps.restore_preview() == {"FINISHED"}
    assert bake_object.data.uv_layers.active_index == previous_uv
    for material in prepared:
        nodes = material.node_tree.nodes
        assert nodes["RWS_ALPHA_MIX"].inputs[2].links[0].from_node == nodes["RWS_EMISSION"]
    for material in passive:
        nodes = material.node_tree.nodes
        assert not material.get("rws_bake_passive")
        assert nodes["RWS_ALPHA_MIX"].inputs[2].links[0].from_node == nodes["RWS_EMISSION"]
    print(
        "RWS_ADDON_ASYNC_BAKE_OK" if async_bake else "RWS_ADDON_SMOKE_OK",
        f"materials={scene.rws_lightmap_material_count}",
        f"base={scene.rws_lightmap_base_count}",
        f"lightmaps={scene.rws_lightmap_lightmap_count}",
        f"alpha={scene.rws_lightmap_alpha_count}",
        f"missing_materials={scene.rws_lightmap_missing_material_count}",
        f"missing_uv2={scene.rws_lightmap_missing_uv_count}",
        flush=True,
    )


if async_bake:
    assert bpy.ops.rws_lightmaps.bake_selected("INVOKE_DEFAULT") == {"RUNNING_MODAL"}
    assert scene.rws_bake_running

    def wait_for_async_bake():
        if scene.rws_bake_running:
            return 0.1
        verify_bake_and_restore()
        bpy.ops.wm.quit_blender()
        return None

    bpy.app.timers.register(wait_for_async_bake, first_interval=0.1)
else:
    assert bpy.ops.rws_lightmaps.bake_selected() == {"FINISHED"}
    verify_bake_and_restore()
