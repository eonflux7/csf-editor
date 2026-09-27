bl_info = {
    "name": "RWS Lightmaps",
    "author": "eonflux7",
    "version": (0, 15, 0),
    "blender": (4, 0, 0),
    "location": "3D View > Sidebar > RWS Lightmaps",
    "description": "Configure CSF Mission Editor glTF materials for base and lightmap preview",
    "category": "Material",
}

import json
import math
import shutil
import struct
import subprocess
import time
from pathlib import Path

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, StringProperty
from mathutils.kdtree import KDTree


NODE_PREFIX = "RWS_"
SUPPORTED_IMAGES = {".dds", ".png", ".tga", ".jpg", ".jpeg"}
_NVTT_CACHE = {}
def _format_elapsed(seconds):
    seconds = max(0, int(seconds))
    hours, remainder = divmod(seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    return f"{hours:02d}:{minutes:02d}:{seconds:02d}"


def _resolve_path(value):
    return Path(bpy.path.abspath(value)).expanduser().resolve()


def _texture_index(directory):
    result = {}
    if not directory.is_dir():
        return result
    for path in directory.iterdir():
        if path.is_file() and path.suffix.lower() in SUPPORTED_IMAGES:
            result.setdefault(path.stem.casefold(), path)
            result.setdefault(path.name.casefold(), path)
    return result


def _find_texture(index, name):
    if not name:
        return None
    key = Path(name).name.casefold()
    return index.get(key) or index.get(Path(key).stem)


def _scene_material_indexes():
    """Index manifest aliases and representative UV layers in one scene pass."""
    tagged = {}
    ambiguous = set()
    for material in bpy.data.materials:
        name = material.get("rws_manifest_name")
        if not name:
            continue
        if name in tagged:
            ambiguous.add(name)
        else:
            tagged[name] = material
    for name in ambiguous:
        tagged.pop(name, None)

    uv_names = {}
    for obj in bpy.data.objects:
        if obj.type != "MESH" or not obj.data or not obj.data.uv_layers:
            continue
        names = [layer.name for layer in obj.data.uv_layers]
        for slot in obj.material_slots:
            if slot.material and slot.material not in uv_names:
                uv_names[slot.material] = names
    return tagged, uv_names


def _material_by_manifest_name(name, tagged):
    exact = bpy.data.materials.get(name)
    if exact:
        return exact
    return tagged.get(name)


def _new_node(nodes, node_type, name, x, y):
    node = nodes.new(node_type)
    node.name = name
    node.label = name.replace(NODE_PREFIX, "RWS ").replace("_", " ").title()
    node.location = (x, y)
    return node


def _load_image(path, non_color):
    image = bpy.data.images.load(str(path), check_existing=True)
    try:
        image.colorspace_settings.name = "Non-Color" if non_color else "sRGB"
    except (TypeError, ValueError):
        pass
    return image


def _enable_alpha_rendering(material):
    """Enable hashed/dithered transparency across Blender 4.x API variants."""
    if hasattr(material, "surface_render_method"):
        try:
            material.surface_render_method = "DITHERED"
        except (TypeError, ValueError):
            pass
    if hasattr(material, "blend_method"):
        try:
            material.blend_method = "HASHED"
        except (TypeError, ValueError):
            pass
    if hasattr(material, "shadow_method"):
        try:
            material.shadow_method = "HASHED"
        except (TypeError, ValueError):
            pass


def _configure_material(material, entry, texture_lookup, uv_names,
                        base_non_color, lightmap_non_color):
    base_name = entry.get("base_texture", "")
    lightmap_name = entry.get("lightmap_texture", "")
    base_path = _find_texture(texture_lookup, base_name)
    lightmap_path = _find_texture(texture_lookup, lightmap_name)
    base_uv_name = uv_names[0] if uv_names else "UVMap"
    lightmap_uv_name = uv_names[1] if len(uv_names) > 1 else "UVMap.001"
    fallback_color = tuple(material.diffuse_color)

    # Blender 5 always enables material nodes and deprecates this compatibility flag.
    if bpy.app.version < (5, 0, 0):
        material.use_nodes = True
    tree = material.node_tree
    nodes = tree.nodes
    links = tree.links
    for node in list(nodes):
        if node.name.startswith(NODE_PREFIX):
            nodes.remove(node)

    outputs = [node for node in nodes if node.type == "OUTPUT_MATERIAL"]
    output = outputs[0] if outputs else nodes.new("ShaderNodeOutputMaterial")
    output.name = "RWS_Output"
    output.label = "RWS Material Output"
    output.location = (820, 0)
    for link in list(output.inputs["Surface"].links):
        links.remove(link)

    uv_base = _new_node(nodes, "ShaderNodeUVMap", "RWS_UV_BASE", -900, 180)
    uv_base.uv_map = base_uv_name
    uv_lightmap = _new_node(nodes, "ShaderNodeUVMap", "RWS_UV_LIGHTMAP", -900, -220)
    uv_lightmap.uv_map = lightmap_uv_name

    base = _new_node(nodes, "ShaderNodeTexImage", "RWS_BASE", -620, 180)
    base.interpolation = "Linear"
    if base_path:
        try:
            # CSF is a legacy fixed-function pipeline: its DDS samples are used as
            # stored gamma values rather than sRGB-decoded linear values.
            base.image = _load_image(base_path, base_non_color)
        except RuntimeError:
            base_path = None
    if not base_path:
        base.image = None
        base.outputs["Color"].default_value = fallback_color
    links.new(uv_base.outputs["UV"], base.inputs["Vector"])

    lightmap = _new_node(nodes, "ShaderNodeTexImage", "RWS_LIGHTMAP", -620, -220)
    lightmap.interpolation = "Linear"
    if lightmap_path:
        try:
            lightmap.image = _load_image(lightmap_path, lightmap_non_color)
        except RuntimeError:
            lightmap_path = None
    if not lightmap_path:
        lightmap.image = None
        lightmap.outputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    links.new(uv_lightmap.outputs["UV"], lightmap.inputs["Vector"])

    multiply = _new_node(nodes, "ShaderNodeMixRGB", "RWS_MULTIPLY", -260, 20)
    multiply.blend_type = "MULTIPLY"
    multiply.inputs[0].default_value = 1.0
    links.new(base.outputs["Color"], multiply.inputs[1])
    links.new(lightmap.outputs["Color"], multiply.inputs[2])

    # The DDS RGB beneath transparent foliage texels is commonly black. Keep the
    # base texture's alpha as the opacity mask even in Lightmap-only mode, instead
    # of displaying that RGB through the rectangular foliage cards.
    use_base_alpha = bool(base_path) and not Path(base_name).stem.upper().startswith("FFLR")
    transparent = _new_node(nodes, "ShaderNodeBsdfTransparent", "RWS_TRANSPARENT", 260, -160)
    emission = _new_node(nodes, "ShaderNodeEmission", "RWS_EMISSION", 260, 40)
    emission.inputs["Strength"].default_value = 1.0
    alpha_mix = _new_node(nodes, "ShaderNodeMixShader", "RWS_ALPHA_MIX", 540, 0)
    links.new(transparent.outputs["BSDF"], alpha_mix.inputs[1])
    links.new(emission.outputs["Emission"], alpha_mix.inputs[2])
    if use_base_alpha:
        links.new(base.outputs["Alpha"], alpha_mix.inputs[0])
        _enable_alpha_rendering(material)
    else:
        alpha_mix.inputs[0].default_value = 1.0
    links.new(alpha_mix.outputs["Shader"], output.inputs["Surface"])

    material["rws_lightmaps_configured"] = True
    material["rws_manifest_name"] = entry.get("name", material.name)
    material["rws_base_texture"] = base_name
    material["rws_lightmap_texture"] = lightmap_name
    material["rws_base_path"] = str(base_path) if base_path else ""
    material["rws_lightmap_path"] = str(lightmap_path) if lightmap_path else ""
    material["rws_base_uv"] = base_uv_name
    material["rws_lightmap_uv"] = lightmap_uv_name
    material["rws_uses_base_alpha"] = use_base_alpha
    material["rws_bake_prepared"] = False
    material["rws_bake_passive"] = False
    return base_path is not None, lightmap_path is not None, len(uv_names) > 1, use_base_alpha


def _apply_view_mode(scene):
    mode = scene.rws_lightmap_view
    source_name = {
        "BASE": "RWS_BASE",
        "LIGHTMAP": "RWS_LIGHTMAP",
        "COMBINED": "RWS_MULTIPLY",
    }.get(mode, "RWS_MULTIPLY")
    configured = 0
    for material in bpy.data.materials:
        if not material.get("rws_lightmaps_configured") or not material.node_tree:
            continue
        nodes = material.node_tree.nodes
        links = material.node_tree.links
        source = nodes.get(source_name)
        emission = nodes.get("RWS_EMISSION")
        if not source or not emission:
            continue
        color_input = emission.inputs.get("Color")
        if not color_input:
            continue
        for link in list(color_input.links):
            links.remove(link)
        color_output = source.outputs.get("Color")
        if color_output:
            links.new(color_output, color_input)
            emission.inputs["Strength"].default_value = (
                1.0 if mode == "BASE" else scene.rws_lightmap_intensity)
            configured += 1
    scene.rws_lightmap_status = f"{configured} materials showing {mode.replace('_', ' ').title()}"


def _selected_mesh_objects(context):
    return [obj for obj in context.selected_objects if obj.type == "MESH" and obj.data]


def _selected_configured_materials(context):
    result = []
    seen = set()
    for obj in _selected_mesh_objects(context):
        for slot in obj.material_slots:
            material = slot.material
            if (material and material.name not in seen and
                    material.get("rws_lightmaps_configured") and material.node_tree):
                seen.add(material.name)
                result.append(material)
    return result


def _configured_materials_on_objects(objects):
    result = []
    seen = set()
    for obj in objects:
        for slot in obj.material_slots:
            material = slot.material
            if (material and material.name not in seen and
                    material.get("rws_lightmaps_configured") and material.node_tree):
                seen.add(material.name)
                result.append(material)
    return result


def _is_floor_material(material):
    return bool(material and
                Path(material.get("rws_base_texture", "")).stem.upper().startswith("FFLR"))


def _hide_duplicate_terrain_shells(context, world_objects, tolerance=0.055):
    """Hide clump-only floor shells that reproduce world vertices with a tiny offset."""
    vertex_count = sum(len(obj.data.vertices) for obj in world_objects)
    if not vertex_count:
        return []
    tree = KDTree(vertex_count)
    index = 0
    for obj in world_objects:
        matrix = obj.matrix_world
        for vertex in obj.data.vertices:
            tree.insert(matrix @ vertex.co, index)
            index += 1
    tree.balance()

    hidden = []
    for obj in context.scene.objects:
        if obj.type != "MESH" or not obj.data or not obj.name.startswith("clump_"):
            continue
        materials = [slot.material for slot in obj.material_slots]
        if not materials or not all(_is_floor_material(material) for material in materials):
            continue
        matrix = obj.matrix_world
        if any(tree.find(matrix @ vertex.co)[2] > tolerance for vertex in obj.data.vertices):
            continue
        obj["rws_duplicate_terrain_shell"] = True
        obj.hide_render = True
        obj.hide_set(True)
        hidden.append(obj)
    return hidden


def _show_scene_lighting(context):
    """Make Blender display the lights that the prepared Cycles shader will bake."""
    for window in context.window_manager.windows:
        for area in window.screen.areas:
            if area.type != "VIEW_3D":
                continue
            for space in area.spaces:
                if space.type != "VIEW_3D":
                    continue
                shading = space.shading
                if hasattr(shading, "use_scene_lights"):
                    shading.use_scene_lights = True
                if hasattr(shading, "use_scene_world"):
                    shading.use_scene_world = True
            area.tag_redraw()
    if context.area and context.area.type == "VIEW_3D" and context.space_data:
        context.space_data.shading.type = "RENDERED"


def _refresh_prepared_scene(context, objects, materials):
    for material in materials:
        material.node_tree.update_tag()
        material.update_tag()
    for obj in objects:
        obj.data.update()
        obj.data.update_tag()
    context.view_layer.update()


def _bake_target_for_material(material, resolution_scale):
    lightmap_name = material.get("rws_lightmap_texture", "")
    nodes = material.node_tree.nodes
    lightmap = nodes.get("RWS_LIGHTMAP")
    if not lightmap_name or not lightmap or not lightmap.image:
        return None
    source_width, source_height = lightmap.image.size
    if source_width <= 0 or source_height <= 0:
        return None
    width = source_width * resolution_scale
    height = source_height * resolution_scale
    image_name = "RWS_BAKE_" + Path(lightmap_name).stem
    if resolution_scale != 1:
        image_name += f"_{resolution_scale}x"
    image = bpy.data.images.get(image_name)
    if image and tuple(image.size) != (width, height):
        if image.source == "GENERATED":
            image.generated_width = width
            image.generated_height = height
        else:
            image.scale(width, height)
        image["rws_bake_completed"] = False
        if "rws_bake_completed_by" in image:
            del image["rws_bake_completed_by"]
    if not image:
        image = bpy.data.images.new(
            image_name, width=width, height=height, alpha=False, float_buffer=True)
        image.generated_color = (0.5, 0.5, 0.5, 1.0)
    try:
        image.colorspace_settings.name = "Non-Color"
    except (TypeError, ValueError):
        pass
    image["rws_lightmap_texture"] = lightmap_name
    image["rws_source_lightmap_path"] = material.get("rws_lightmap_path", "")
    image["rws_source_image"] = lightmap.image.name
    image["rws_resolution_scale"] = resolution_scale
    return image


def _bake_image_sample_range(image, sample_count=1024):
    """Cheaply reject untouched/empty targets without copying a multi-gigapixel atlas."""
    pixel_count = int(image.size[0]) * int(image.size[1])
    if pixel_count <= 0:
        return 0.0, 0.0
    pixels = image.pixels
    count = min(sample_count, pixel_count)
    low = float("inf")
    high = float("-inf")
    for sample in range(count):
        pixel = (sample * (pixel_count - 1) // max(1, count - 1)) * 4
        for channel in range(3):
            value = float(pixels[pixel + channel])
            low = min(low, value)
            high = max(high, value)
    return low, high


def _unpack_565(value):
    r = (value >> 11) & 31
    g = (value >> 5) & 63
    b = value & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def _pack_565(color):
    r = max(0, min(31, round(color[0] * 31 / 255)))
    g = max(0, min(63, round(color[1] * 63 / 255)))
    b = max(0, min(31, round(color[2] * 31 / 255)))
    return (r << 11) | (g << 5) | b


def _encode_color_block(pixels):
    pixels = [tuple(int(channel) for channel in pixel) for pixel in pixels]
    ranges = [max(pixel[channel] for pixel in pixels) - min(pixel[channel] for pixel in pixels)
              for channel in range(3)]
    axis = max(range(3), key=lambda channel: ranges[channel])
    low = min(pixels, key=lambda pixel: pixel[axis])
    high = max(pixels, key=lambda pixel: pixel[axis])
    color0 = _pack_565(high)
    color1 = _pack_565(low)
    if color0 < color1:
        color0, color1 = color1, color0
    if color0 == color1:
        if color0 < 0xFFFF:
            color0 += 1
        elif color1 > 0:
            color1 -= 1
    endpoint0 = _unpack_565(color0)
    endpoint1 = _unpack_565(color1)
    palette = (
        endpoint0,
        endpoint1,
        tuple((2 * endpoint0[channel] + endpoint1[channel]) // 3 for channel in range(3)),
        tuple((endpoint0[channel] + 2 * endpoint1[channel]) // 3 for channel in range(3)),
    )
    indices = 0
    for index, pixel in enumerate(pixels):
        choice = min(range(4), key=lambda candidate: sum(
            (pixel[channel] - palette[candidate][channel]) ** 2 for channel in range(3)))
        indices |= choice << (index * 2)
    return struct.pack("<HHI", color0, color1, indices)


def _downsample_rgba(pixels, width, height):
    import numpy as np

    next_width = max(1, width // 2)
    next_height = max(1, height // 2)
    y0 = np.arange(next_height) * 2
    y1 = np.minimum(height - 1, y0 + 1)
    x0 = np.arange(next_width) * 2
    x1 = np.minimum(width - 1, x0 + 1)
    values = pixels.astype(np.uint16, copy=False)
    result = (values[y0[:, None], x0] + values[y0[:, None], x1] +
              values[y1[:, None], x0] + values[y1[:, None], x1]) // 4
    return result.astype(np.uint8), next_width, next_height


def _encode_dds_level(pixels, width, height, fourcc):
    array_pixels = getattr(pixels, "ndim", 1) == 3
    output = bytearray()
    for block_y in range((height + 3) // 4):
        for block_x in range((width + 3) // 4):
            block = [pixels[min(height - 1, block_y * 4 + py),
                            min(width - 1, block_x * 4 + px)]
                     if array_pixels else
                     pixels[min(height - 1, block_y * 4 + py) * width +
                            min(width - 1, block_x * 4 + px)]
                     for py in range(4) for px in range(4)]
            if fourcc == b"DXT3":
                alpha = 0
                for index, pixel in enumerate(block):
                    alpha |= max(0, min(15, round(int(pixel[3]) * 15 / 255))) << (index * 4)
                output.extend(struct.pack("<Q", alpha))
            output.extend(_encode_color_block(block))
    return output


def _encode_dds_level_chunks(pixels, width, height, fourcc):
    """Yield one compressed block-row at a time so Blender can repaint the UI."""
    array_pixels = getattr(pixels, "ndim", 1) == 3
    block_columns = (width + 3) // 4
    for block_y in range((height + 3) // 4):
        output = bytearray()
        for block_x in range(block_columns):
            block = [pixels[min(height - 1, block_y * 4 + py),
                            min(width - 1, block_x * 4 + px)]
                     if array_pixels else
                     pixels[min(height - 1, block_y * 4 + py) * width +
                            min(width - 1, block_x * 4 + px)]
                     for py in range(4) for px in range(4)]
            if fourcc == b"DXT3":
                alpha = 0
                for index, pixel in enumerate(block):
                    alpha |= max(0, min(15, round(int(pixel[3]) * 15 / 255))) << (index * 4)
                output.extend(struct.pack("<Q", alpha))
            output.extend(_encode_color_block(block))
        yield bytes(output), block_columns


def _image_rgba_array(image, width, height, scale, alpha_image=None):
    import numpy as np

    values = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(values)
    rgba = values.reshape((height, width, 4))
    pixels = np.empty((height, width, 4), dtype=np.uint8)
    # Blender stores image rows bottom-up; legacy DDS files store blocks top-down.
    pixels[:, :, :3] = np.clip(
        np.rint(rgba[::-1, :, :3] * scale * 255.0), 0, 255).astype(np.uint8)
    if alpha_image and alpha_image.size[0] > 0 and alpha_image.size[1] > 0:
        alpha_width, alpha_height = tuple(alpha_image.size)
        alpha_values = np.empty(alpha_width * alpha_height * 4, dtype=np.float32)
        alpha_image.pixels.foreach_get(alpha_values)
        alpha = alpha_values.reshape((alpha_height, alpha_width, 4))[::-1, :, 3]
        y_indices = np.minimum(alpha_height - 1,
                               np.arange(height) * alpha_height // height)
        x_indices = np.minimum(alpha_width - 1,
                               np.arange(width) * alpha_width // width)
        pixels[:, :, 3] = np.clip(
            np.rint(alpha[y_indices[:, None], x_indices[None, :]] * 255.0),
            0, 255).astype(np.uint8)
    else:
        pixels[:, :, 3] = 255
    return pixels


def _dds_export_info(image, source_path):
    with source_path.open("rb") as stream:
        header = stream.read(128)
    if (len(header) < 128 or header[:4] != b"DDS " or
            header[4:8] != struct.pack("<I", 124)):
        raise ValueError(f"Invalid legacy DDS header: {source_path}")
    source_height, source_width = struct.unpack_from("<II", header, 12)
    source_mips = max(1, struct.unpack_from("<I", header, 28)[0])
    fourcc = header[84:88]
    if fourcc not in (b"DXT1", b"DXT3"):
        raise ValueError(f"Unsupported DDS format {fourcc!r}: {source_path.name}")
    width, height = tuple(image.size)
    if width <= 0 or height <= 0 or source_width <= 0 or source_height <= 0:
        raise ValueError(f"Invalid bake resolution for {image.name}")
    width_scale = width / source_width
    height_scale = height / source_height
    if width_scale != height_scale or width_scale not in (1, 2, 4):
        raise ValueError(
            f"Bake target {image.name} is {width}x{height}; expected 1x, 2x, or 4x "
            f"the source {source_width}x{source_height}")
    mip_count = min(source_mips + int(math.log2(width_scale)),
                    int(math.log2(max(width, height))) + 1)
    level_width, level_height = width, height
    total_blocks = 0
    for _level in range(mip_count):
        total_blocks += ((level_width + 3) // 4) * ((level_height + 3) // 4)
        level_width = max(1, level_width // 2)
        level_height = max(1, level_height // 2)
    return header, fourcc, width, height, mip_count, total_blocks


def _raw_bgra8_work_units(width, height, mip_count):
    """Count pixels written across a complete mip chain for export progress."""
    total = 0
    for _level in range(mip_count):
        total += width * height
        width = max(1, width // 2)
        height = max(1, height // 2)
    return total


def _raw_bgra8_header(width, height, mip_count):
    """Build a legacy A8R8G8B8 DDS header (BGRA bytes, no DX10 extension)."""
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)       # DDS_HEADER.dwSize
    flags = 0x00000001 | 0x00000002 | 0x00000004 | 0x00000008 | 0x00001000
    if mip_count > 1:
        flags |= 0x00020000                       # DDSD_MIPMAPCOUNT
    struct.pack_into("<I", header, 8, flags)
    struct.pack_into("<I", header, 12, height)
    struct.pack_into("<I", header, 16, width)
    struct.pack_into("<I", header, 20, width * 4)  # top-level row pitch
    struct.pack_into("<I", header, 28, mip_count)
    struct.pack_into("<I", header, 76, 32)       # DDS_PIXELFORMAT.dwSize
    struct.pack_into("<I", header, 80, 0x41)     # DDPF_RGB | DDPF_ALPHAPIXELS
    struct.pack_into("<I", header, 88, 32)
    struct.pack_into("<I", header, 92, 0x00FF0000)
    struct.pack_into("<I", header, 96, 0x0000FF00)
    struct.pack_into("<I", header, 100, 0x000000FF)
    struct.pack_into("<I", header, 104, 0xFF000000)
    caps = 0x00001000                             # DDSCAPS_TEXTURE
    if mip_count > 1:
        caps |= 0x00000008 | 0x00400000           # COMPLEX | MIPMAP
    struct.pack_into("<I", header, 108, caps)
    return header


def _raw_rows_per_chunk(width, target_pixels=1024 * 1024):
    """Keep each Blender pixel read bounded while avoiding excessive timer ticks."""
    return max(1, min(256, target_pixels // max(1, width)))


def _read_bake_rgba8_rows(image, width, height, row, row_end, scale, alpha_image=None):
    """Read a top-down row range without copying the complete Blender float image."""
    import numpy as np

    # Blender exposes rows bottom-up. The requested top-down rows form one
    # contiguous bottom-up slice, which is reversed after conversion.
    blender_row = height - row_end
    values = np.asarray(
        image.pixels[blender_row * width * 4:(height - row) * width * 4],
        dtype=np.float32).reshape((row_end - row, width, 4))[::-1]
    pixels = np.empty((row_end - row, width, 4), dtype=np.uint8)
    pixels[:, :, :3] = np.clip(
        np.rint(values[:, :, :3] * scale * 255.0), 0, 255).astype(np.uint8)
    if alpha_image and alpha_image.size[0] > 0 and alpha_image.size[1] > 0:
        alpha_width, alpha_height = tuple(alpha_image.size)
        y_indices = np.minimum(
            alpha_height - 1, np.arange(row, row_end) * alpha_height // height)
        alpha_row = int(y_indices[0])
        alpha_row_end = int(y_indices[-1]) + 1
        blender_alpha_row = alpha_height - alpha_row_end
        alpha_values = np.asarray(
            alpha_image.pixels[
                blender_alpha_row * alpha_width * 4:
                (alpha_height - alpha_row) * alpha_width * 4],
            dtype=np.float32).reshape(
                (alpha_row_end - alpha_row, alpha_width, 4))[::-1]
        x_indices = np.minimum(
            alpha_width - 1, np.arange(width) * alpha_width // width)
        pixels[:, :, 3] = np.clip(
            np.rint(alpha_values[y_indices - alpha_row][:, x_indices, 3] * 255.0),
            0, 255).astype(np.uint8)
    else:
        pixels[:, :, 3] = 255
    return pixels


def _downsample_rgba_rows(pixels, width, height, row, row_end):
    """Build a bounded row range of the next mip from a top-down RGBA array."""
    import numpy as np

    next_width = max(1, width // 2)
    y0 = np.arange(row, row_end) * 2
    y1 = np.minimum(height - 1, y0 + 1)
    x0 = np.arange(next_width) * 2
    x1 = np.minimum(width - 1, x0 + 1)
    # Convert only the rows used by this chunk. Converting the complete level to
    # uint16 was the largest transient allocation in the original raw writer.
    top = pixels[y0].astype(np.uint16)
    bottom = pixels[y1].astype(np.uint16)
    result = (top[:, x0] + top[:, x1] + bottom[:, x0] + bottom[:, x1]) // 4
    return result.astype(np.uint8)


def _write_bake_dds_raw32_steps(image, source_path, output_path, scale):
    """Stream an opaque legacy A8R8G8B8 DDS without full-image RAM copies."""
    import numpy as np

    _source_header, _source_fourcc, width, height, mip_count, _source_blocks = \
        _dds_export_info(image, source_path)
    total_units = _raw_bgra8_work_units(width, height, mip_count)

    # Lightmaps use RGB only. Keep alpha opaque, matching the proven 32-bit test
    # asset rather than copying legacy DXT3 payload data into a raw surface.
    header = _raw_bgra8_header(width, height, mip_count)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_suffix(output_path.suffix + ".tmp")
    mip_paths = [output_path.parent / f".{output_path.name}.mip-{level}.tmp"
                 for level in range(mip_count)]
    completed_units = 0
    current_pixels = None
    next_pixels = None
    try:
        with temporary.open("wb") as stream:
            stream.write(header)
            level_width, level_height = width, height
            for level in range(mip_count):
                keep_level = level + 1 < mip_count
                next_pixels = None
                if keep_level:
                    next_pixels = np.memmap(
                        mip_paths[level], dtype=np.uint8, mode="w+",
                        shape=(level_height, level_width, 4))
                rows_per_chunk = _raw_rows_per_chunk(level_width)
                for row in range(0, level_height, rows_per_chunk):
                    row_end = min(level_height, row + rows_per_chunk)
                    if level == 0:
                        chunk = _read_bake_rgba8_rows(
                            image, level_width, level_height, row, row_end, scale)
                    else:
                        chunk = _downsample_rgba_rows(
                            current_pixels, current_pixels.shape[1],
                            current_pixels.shape[0], row, row_end)
                    if next_pixels is not None:
                        next_pixels[row:row_end] = chunk
                    # The masks describe A8R8G8B8 packed little-endian, so the
                    # on-disk byte order is B, G, R, A.
                    stream.write(chunk[:, :, [2, 1, 0, 3]].tobytes())
                    completed_units += (row_end - row) * level_width
                    yield (completed_units, total_units,
                           f"Streaming raw mip {level + 1}/{mip_count}")
                if next_pixels is not None:
                    next_pixels.flush()
                if current_pixels is not None:
                    del current_pixels
                    current_pixels = None
                    mip_paths[level - 1].unlink()
                current_pixels = next_pixels
                next_pixels = None
                level_width = max(1, level_width // 2)
                level_height = max(1, level_height // 2)
        temporary.replace(output_path)
    finally:
        if current_pixels is not None:
            del current_pixels
        if next_pixels is not None:
            del next_pixels
        if temporary.exists():
            temporary.unlink()
        for path in mip_paths:
            if path.exists():
                path.unlink()
    return "A8R8G8B8", width, height, mip_count, total_units


def _write_bake_dds_steps(image, source_path, output_path, scale):
    """Incrementally encode a DDS and yield (completed blocks, total blocks, phase)."""
    import numpy as np

    source_header, fourcc, width, height, mip_count, total_blocks = \
        _dds_export_info(image, source_path)

    alpha_image = bpy.data.images.get(image.get("rws_source_image", ""))
    block_bytes = 8 if fourcc == b"DXT1" else 16
    linear_size = ((width + 3) // 4) * ((height + 3) // 4) * block_bytes
    header = bytearray(source_header)
    struct.pack_into("<I", header, 12, height)
    struct.pack_into("<I", header, 16, width)
    struct.pack_into("<I", header, 20, linear_size)
    struct.pack_into("<I", header, 28, mip_count)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = output_path.with_suffix(output_path.suffix + ".tmp")
    mip_paths = [output_path.parent / f".{output_path.name}.bc-mip-{level}.tmp"
                 for level in range(mip_count)]
    completed_blocks = 0
    current_pixels = None
    level_pixels = None
    try:
        with temporary.open("wb") as stream:
            stream.write(header)
            level_width, level_height = width, height
            for level in range(mip_count):
                level_pixels = np.memmap(
                    mip_paths[level], dtype=np.uint8, mode="w+",
                    shape=(level_height, level_width, 4))
                rows_per_chunk = _raw_rows_per_chunk(level_width)
                for row in range(0, level_height, rows_per_chunk):
                    row_end = min(level_height, row + rows_per_chunk)
                    if level == 0:
                        chunk = _read_bake_rgba8_rows(
                            image, level_width, level_height, row, row_end,
                            scale, alpha_image)
                        phase = "Reading bake pixels"
                    else:
                        chunk = _downsample_rgba_rows(
                            current_pixels, current_pixels.shape[1],
                            current_pixels.shape[0], row, row_end)
                        phase = f"Building mip {level + 1}/{mip_count}"
                    level_pixels[row:row_end] = chunk
                    yield completed_blocks, total_blocks, phase
                level_pixels.flush()
                if current_pixels is not None:
                    del current_pixels
                    current_pixels = None
                    mip_paths[level - 1].unlink()
                current_pixels = level_pixels
                level_pixels = None
                for payload, block_count in _encode_dds_level_chunks(
                        current_pixels, level_width, level_height, fourcc):
                    stream.write(payload)
                    completed_blocks += block_count
                    yield completed_blocks, total_blocks, f"Compressing mip {level + 1}/{mip_count}"
                level_width = max(1, level_width // 2)
                level_height = max(1, level_height // 2)
        temporary.replace(output_path)
    finally:
        if current_pixels is not None:
            del current_pixels
        if level_pixels is not None:
            del level_pixels
        if temporary.exists():
            temporary.unlink()
        for path in mip_paths:
            if path.exists():
                path.unlink()
    return fourcc.decode("ascii"), width, height, mip_count, total_blocks


def _dds_export_work_units(image, source_path, output_format):
    info = _dds_export_info(image, source_path)
    if output_format == "RAW32":
        return _raw_bgra8_work_units(info[2], info[3], info[4])
    return info[5]


def _find_nvtt_export(configured_path="", refresh=False):
    cache_key = bpy.path.abspath(configured_path) if configured_path else "<auto>"
    if not refresh and cache_key in _NVTT_CACHE:
        return _NVTT_CACHE[cache_key]
    candidates = []
    if configured_path:
        candidates.append(Path(bpy.path.abspath(configured_path)))
    found = (shutil.which("nvcompress.exe") or shutil.which("nvcompress") or
             shutil.which("nvtt_export.exe") or shutil.which("nvtt_export"))
    if found:
        candidates.append(Path(found))
    candidates.extend((
        Path(r"C:\Program Files\NVIDIA Corporation\NVIDIA Texture Tools\nvcompress.exe"),
        Path(r"C:\Program Files\NVIDIA Corporation\NVIDIA Texture Tools\nvtt_export.exe"),
        Path(r"C:\Program Files\NVIDIA Corporation\NVIDIA Texture Tools Exporter\nvtt_export.exe"),
    ))
    result = next((path.resolve() for path in candidates if path.is_file()), None)
    _NVTT_CACHE[cache_key] = result
    return result


def _write_nvtt_input_tga_steps(image, path, scale, alpha_image):
    """Stream a top-down TGA for NVTT and yield after each bounded row chunk."""
    import numpy as np

    width, height = tuple(image.size)
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0,
                         width, height, 32, 0x28)
    channel_sums = np.zeros(4, dtype=np.uint64)
    with path.open("wb") as stream:
        stream.write(header)
        rows_per_chunk = _raw_rows_per_chunk(width)
        for row in range(0, height, rows_per_chunk):
            row_end = min(height, row + rows_per_chunk)
            output = _read_bake_rgba8_rows(
                image, width, height, row, row_end, scale, alpha_image)
            channel_sums += output.sum(axis=(0, 1), dtype=np.uint64)
            # TGA descriptor 0x28 declares top-left origin and eight alpha bits.
            stream.write(output[:, :, [2, 1, 0, 3]].tobytes())
            yield row_end, height
    pixel_count = width * height
    return tuple(int(round(float(value) / pixel_count)) for value in channel_sums)


def _write_bake_dds_nvtt_steps(image, source_path, output_path, scale, executable):
    _header, source_fourcc, width, height, mip_count, total_blocks = \
        _dds_export_info(image, source_path)
    formats = {b"DXT1": "bc1", b"DXT3": "bc2"}
    input_path = output_path.parent / f".{output_path.stem}.rws-nvtt-input.tga"
    temporary = output_path.parent / f".{output_path.stem}.rws-nvtt-output.dds"
    log_path = output_path.parent / f".{output_path.stem}.rws-nvtt.log"
    process = None
    log_stream = None
    tga_writer = None
    try:
        output_path.parent.mkdir(parents=True, exist_ok=True)
        yield 0, total_blocks, "Preparing NVIDIA input"
        alpha_image = bpy.data.images.get(image.get("rws_source_image", ""))
        tga_writer = _write_nvtt_input_tga_steps(
            image, input_path, scale, alpha_image)
        while True:
            try:
                rows_done, row_count = next(tga_writer)
            except StopIteration as finished:
                terminal_pixel = finished.value
                break
            yield (0, total_blocks,
                   f"Preparing NVIDIA input rows {rows_done}/{row_count}")
        tga_writer = None
        if executable.stem.casefold() == "nvcompress":
            alpha_mode = "-alpha" if source_fourcc == b"DXT3" else "-noalpha"
            command = [str(executable), f"-{formats[source_fourcc]}", alpha_mode,
                       "-max-mip-count", str(mip_count), "-no-mip-gamma-correct", "-silent",
                       str(input_path), str(temporary)]
        else:
            command = [str(executable), str(input_path), "--format", formats[source_fourcc],
                       "--quality", "fastest", "--max-mip-count", str(mip_count),
                       "--output", str(temporary)]
        log_stream = log_path.open("w", encoding="utf-8")
        process = subprocess.Popen(
            command, stdout=log_stream, stderr=subprocess.STDOUT,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        while process.poll() is None:
            time.sleep(0.02)
            yield 0, total_blocks, f"NVIDIA CUDA {formats[source_fourcc].upper()} compression"
        log_stream.close()
        log_stream = None
        if process.returncode != 0 or not temporary.is_file():
            details = log_path.read_text(encoding="utf-8", errors="replace")[-2000:] \
                if log_path.is_file() else "no log output"
            raise RuntimeError(f"NVIDIA Texture Tools failed ({process.returncode}): {details}")
        encoded = temporary.read_bytes()[:128]
        actual_fourcc = encoded[84:88] if len(encoded) >= 88 else b""
        actual_size = struct.unpack_from("<II", encoded, 12) if len(encoded) >= 20 else (0, 0)
        actual_mips = max(1, struct.unpack_from("<I", encoded, 28)[0]) \
            if len(encoded) >= 32 else 0
        # NVTT 3.2.4 stops BC chains at 2x2 even with -min-mip-size 1. CSF's
        # source DDS files include 1x1, so append that one tiny terminal block.
        next_width = max(1, width // (2 ** actual_mips))
        next_height = max(1, height // (2 ** actual_mips))
        if (actual_mips + 1 == mip_count and next_width == 1 and next_height == 1 and
                actual_fourcc == source_fourcc and actual_size == (height, width)):
            terminal_block = _encode_dds_level([terminal_pixel], 1, 1, source_fourcc)
            with temporary.open("r+b") as stream:
                stream.seek(28)
                stream.write(struct.pack("<I", mip_count))
                stream.seek(0, 2)
                stream.write(terminal_block)
            actual_mips = mip_count
        if (len(encoded) < 128 or encoded[:4] != b"DDS " or
                actual_fourcc != source_fourcc or actual_size != (height, width) or
                actual_mips != mip_count):
            raise RuntimeError(
                "NVIDIA Texture Tools produced an incompatible DDS header: "
                f"got FourCC={actual_fourcc!r}, size={actual_size[1]}x{actual_size[0]}, "
                f"mips={actual_mips}; expected FourCC={source_fourcc!r}, "
                f"size={width}x{height}, mips={mip_count}")
        temporary.replace(output_path)
        yield total_blocks, total_blocks, "NVIDIA compression complete"
    finally:
        if tga_writer is not None:
            tga_writer.close()
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        if log_stream:
            log_stream.close()
        for path in (input_path, temporary, log_path):
            if path.exists():
                path.unlink()
    return source_fourcc.decode("ascii"), width, height, mip_count, total_blocks


def _resource_texture_relative_path(texture_directory):
    parts = texture_directory.resolve().parts
    for index in range(len(parts) - 1, 0, -1):
        if (parts[index].casefold() == "maps" and index + 2 < len(parts) and
                parts[-1].casefold() == "textures"):
            return Path(*parts[index - 1:])
    raise ValueError(
        "Texture directory must follow <archive>/Maps/<map>/Textures so the output hierarchy is unambiguous")


def _prepare_material_for_bake(material, resolution_scale):
    tree = material.node_tree
    nodes = tree.nodes
    links = tree.links
    alpha_mix = nodes.get("RWS_ALPHA_MIX")
    if not alpha_mix:
        return False
    image = _bake_target_for_material(material, resolution_scale)
    if not image:
        return False

    target = nodes.get("RWS_BAKE_TARGET")
    if not target:
        target = _new_node(nodes, "ShaderNodeTexImage", "RWS_BAKE_TARGET", -260, -420)
    target.image = image
    target.interpolation = "Linear"

    diffuse = nodes.get("RWS_BAKE_DIFFUSE")
    if not diffuse:
        diffuse = _new_node(nodes, "ShaderNodeBsdfPrincipled", "RWS_BAKE_DIFFUSE", 260, 260)
    diffuse.inputs["Base Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    diffuse.inputs["Roughness"].default_value = 1.0
    if diffuse.inputs.get("Metallic"):
        diffuse.inputs["Metallic"].default_value = 0.0
    base = nodes.get("RWS_BASE")
    base_color = diffuse.inputs["Base Color"]
    for link in list(base_color.links):
        links.remove(link)
    if base and base.outputs.get("Color"):
        # Keep the actual base texture visible while arranging lights. The bake
        # operator disables the Diffuse Color pass, so only illumination is still
        # written to the lightmap target.
        links.new(base.outputs["Color"], base_color)

    shader_input = alpha_mix.inputs[2]
    for link in list(shader_input.links):
        links.remove(link)
    links.new(diffuse.outputs["BSDF"], shader_input)
    for node in nodes:
        node.select = False
    target.select = True
    nodes.active = target
    material["rws_bake_prepared"] = True
    material["rws_bake_passive"] = False
    return True


def _prepare_passive_material_for_bake(material):
    """Make an unprepared preview material participate in Cycles without emitting."""
    if (not material.node_tree or material.get("rws_bake_prepared") or
            not material.get("rws_lightmaps_configured")):
        return False
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    alpha_mix = nodes.get("RWS_ALPHA_MIX")
    base = nodes.get("RWS_BASE")
    if not alpha_mix or not base:
        return False

    diffuse = nodes.get("RWS_BAKE_PASSIVE")
    if not diffuse:
        diffuse = _new_node(
            nodes, "ShaderNodeBsdfPrincipled", "RWS_BAKE_PASSIVE", 260, -280)
    diffuse.inputs["Base Color"].default_value = tuple(material.diffuse_color)
    diffuse.inputs["Roughness"].default_value = 1.0
    if diffuse.inputs.get("Metallic"):
        diffuse.inputs["Metallic"].default_value = 0.0
    base_color = diffuse.inputs["Base Color"]
    for link in list(base_color.links):
        links.remove(link)
    if base.outputs.get("Color"):
        links.new(base.outputs["Color"], base_color)

    # Keep RWS_ALPHA_MIX in place so foliage continues using its base alpha.
    shader_input = alpha_mix.inputs[2]
    for link in list(shader_input.links):
        links.remove(link)
    links.new(diffuse.outputs["BSDF"], shader_input)
    material["rws_bake_passive"] = True
    return True


def _restore_preview_material(material):
    if not material.node_tree:
        return False
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    alpha_mix = nodes.get("RWS_ALPHA_MIX")
    emission = nodes.get("RWS_EMISSION")
    if not alpha_mix or not emission:
        return False
    shader_input = alpha_mix.inputs[2]
    for link in list(shader_input.links):
        links.remove(link)
    links.new(emission.outputs["Emission"], shader_input)
    material["rws_bake_prepared"] = False
    material["rws_bake_passive"] = False
    return True


def _view_mode_updated(self, context):
    if context and context.scene:
        _apply_view_mode(context.scene)


def _manifest_updated(self, context):
    if not context or not context.scene or not self.rws_lightmap_manifest:
        return
    try:
        manifest = _resolve_path(self.rws_lightmap_manifest)
    except (OSError, RuntimeError):
        return
    if not manifest.is_file():
        return
    for candidate in (manifest.parent / "Textures", manifest.parent.parent / "Textures"):
        if candidate.is_dir():
            context.scene.rws_lightmap_texture_directory = str(candidate)
            return


class RWS_OT_configure(bpy.types.Operator):
    bl_idname = "rws_lightmaps.configure"
    bl_label = "Configure Imported Materials"
    bl_description = "Load textures and create Base, Lightmap, and combined preview nodes"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        scene = context.scene
        try:
            manifest_path = _resolve_path(scene.rws_lightmap_manifest)
            if scene.rws_lightmap_texture_directory:
                texture_directory = _resolve_path(scene.rws_lightmap_texture_directory)
            else:
                candidates = (manifest_path.parent / "Textures", manifest_path.parent.parent / "Textures")
                texture_directory = next((path for path in candidates if path.is_dir()), None)
                if not texture_directory:
                    raise ValueError("Choose the map's Textures directory")
                scene.rws_lightmap_texture_directory = str(texture_directory)
            with manifest_path.open("r", encoding="utf-8") as stream:
                manifest = json.load(stream)
            if manifest.get("format") != "rws-man-scene-manifest-v1":
                raise ValueError("This is not a CSF Mission Editor scene manifest")
            entries = manifest.get("materials", [])
            lookup = _texture_index(texture_directory)
            tagged_materials, material_uv_names = _scene_material_indexes()
            matched = base_loaded = lightmaps_loaded = alpha_materials = missing_uv2 = 0
            unmatched = []
            for entry in entries:
                name = entry.get("name", "")
                material = _material_by_manifest_name(name, tagged_materials)
                if not material:
                    unmatched.append(name)
                    continue
                base_ok, lightmap_ok, uv2_ok, uses_alpha = _configure_material(
                    material, entry, lookup, material_uv_names.get(material, []),
                    scene.rws_base_non_color,
                    scene.rws_lightmap_non_color)
                matched += 1
                base_loaded += int(base_ok)
                lightmaps_loaded += int(lightmap_ok)
                alpha_materials += int(uses_alpha)
                if entry.get("lightmap_texture") and not uv2_ok:
                    missing_uv2 += 1
            scene.rws_lightmap_material_count = matched
            scene.rws_lightmap_base_count = base_loaded
            scene.rws_lightmap_lightmap_count = lightmaps_loaded
            scene.rws_lightmap_alpha_count = alpha_materials
            scene.rws_lightmap_texture_count = len({
                entry.get("lightmap_texture") for entry in entries
                if entry.get("lightmap_texture")})
            scene.rws_lightmap_missing_material_count = len(unmatched)
            scene.rws_lightmap_missing_uv_count = missing_uv2
            _apply_view_mode(scene)
            summary = (f"Configured {matched}/{len(entries)} materials; "
                       f"base {base_loaded}, lightmaps {lightmaps_loaded}, "
                       f"alpha {alpha_materials}")
            scene.rws_lightmap_status = summary
            if unmatched:
                self.report({"WARNING"}, summary + f"; {len(unmatched)} materials not found")
            else:
                self.report({"INFO"}, summary)
            return {"FINISHED"}
        except Exception as error:
            scene.rws_lightmap_status = str(error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class RWS_OT_reload_images(bpy.types.Operator):
    bl_idname = "rws_lightmaps.reload_images"
    bl_label = "Reload RWS Images"
    bl_description = "Reload all base and lightmap files after an external rebake or edit"

    def execute(self, context):
        images = set()
        for material in bpy.data.materials:
            if not material.get("rws_lightmaps_configured") or not material.node_tree:
                continue
            for name in ("RWS_BASE", "RWS_LIGHTMAP"):
                node = material.node_tree.nodes.get(name)
                if node and node.image:
                    images.add(node.image)
        failures = 0
        for image in images:
            try:
                image.reload()
            except RuntimeError:
                failures += 1
        context.scene.rws_lightmap_status = f"Reloaded {len(images) - failures}/{len(images)} images"
        self.report({"INFO" if not failures else "WARNING"}, context.scene.rws_lightmap_status)
        return {"FINISHED"}


class RWS_OT_select_world_sectors(bpy.types.Operator):
    bl_idname = "rws_lightmaps.select_world_sectors"
    bl_label = "Select World Sectors"
    bl_description = "Select exported World Sector meshes, including terrain and static map geometry"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="DESELECT")
        selected = []
        for obj in context.scene.objects:
            if obj.type == "MESH" and obj.name.startswith("world_"):
                obj.select_set(True)
                selected.append(obj)
        hidden_shells = _hide_duplicate_terrain_shells(context, selected)
        if selected:
            context.view_layer.objects.active = selected[0]
        context.scene.rws_lightmap_status = (
            f"Selected {len(selected)} World Sector objects; "
            f"hid {len(hidden_shells)} coincident terrain shells")
        return {"FINISHED"}


class RWS_OT_prepare_bake(bpy.types.Operator):
    bl_idname = "rws_lightmaps.prepare_bake"
    bl_label = "Prepare Selected for Bake"
    bl_description = "Prepare selected lightmap targets and make other RWS materials passive during the bake"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        objects = _selected_mesh_objects(context)
        materials = _selected_configured_materials(context)
        if not materials:
            self.report({"ERROR"}, "Select configured mesh objects first")
            return {"CANCELLED"}
        missing_uv2 = [obj.name for obj in objects if len(obj.data.uv_layers) < 2]
        if missing_uv2:
            self.report({"ERROR"}, f"{len(missing_uv2)} selected meshes have no lightmap UV layer")
            return {"CANCELLED"}
        bake_materials = []
        unavailable = []
        for material in materials:
            lightmap_name = material.get("rws_lightmap_texture", "")
            lightmap = material.node_tree.nodes.get("RWS_LIGHTMAP")
            if not lightmap_name:
                continue
            if not lightmap or not lightmap.image or min(lightmap.image.size) <= 0:
                unavailable.append(material.name)
            else:
                bake_materials.append(material)
        if unavailable:
            self.report({"ERROR"}, f"{len(unavailable)} lightmap textures are not loaded")
            return {"CANCELLED"}
        if not bake_materials:
            self.report({"ERROR"}, "Selected materials have no loaded lightmap images")
            return {"CANCELLED"}
        for obj in objects:
            layers = obj.data.uv_layers
            obj["rws_bake_previous_uv"] = layers.active_index
            layers.active_index = 1
            layers[1].active_render = True
            obj["rws_bake_uv_prepared"] = True
        resolution_scale = int(context.scene.rws_bake_resolution_scale)
        prepared = sum(_prepare_material_for_bake(material, resolution_scale)
                       for material in bake_materials)
        visible_scene_objects = [
            obj for obj in context.scene.objects
            if obj.type == "MESH" and obj.data and not obj.hide_render]
        scene_materials = _configured_materials_on_objects(visible_scene_objects)
        passive = sum(_prepare_passive_material_for_bake(material)
                      for material in scene_materials)
        try:
            context.scene.render.engine = "CYCLES"
        except TypeError:
            pass
        _refresh_prepared_scene(context, objects, scene_materials)
        _show_scene_lighting(context)
        targets = {
            material.node_tree.nodes["RWS_BAKE_TARGET"].image.name
            for material in materials if material.get("rws_bake_prepared")
        }
        context.scene.rws_lightmap_status = (
            f"v0.15.0: prepared {prepared} materials using {len(targets)} targets at "
            f"{resolution_scale}x; made {passive} other materials passive; "
            "Cycles scene lighting active")
        self.report({"INFO"}, context.scene.rws_lightmap_status)
        return {"FINISHED"}


class RWS_OT_bake_selected(bpy.types.Operator):
    bl_idname = "rws_lightmaps.bake_selected"
    bl_label = "Bake Selected Lighting"
    bl_description = "Bake direct and indirect diffuse lighting into prepared targets using existing lightmap UVs"

    def _prepare(self, context):
        # Adding or editing a light changes Blender's selection. Use the durable
        # set recorded by Prepare instead of silently dropping the world sectors.
        objects = [obj for obj in context.scene.objects
                   if obj.type == "MESH" and obj.data and obj.get("rws_bake_uv_prepared")]
        materials = _configured_materials_on_objects(objects)
        prepared = [material for material in materials if material.get("rws_bake_prepared")]
        if not objects or not prepared:
            raise ValueError("Run Select World Sectors, then Prepare Selected for Bake first")
        missing = [material.name for material in materials
                   if material.get("rws_lightmap_texture") and not material.get("rws_bake_prepared")]
        if missing:
            raise ValueError(f"{len(missing)} selected lightmapped materials are not prepared")
        if context.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="DESELECT")
        for obj in objects:
            obj.hide_set(False)
            obj.select_set(True)
        context.view_layer.objects.active = objects[0]
        bake = context.scene.render.bake
        bake.use_clear = True
        bake.margin = context.scene.rws_bake_margin
        bake.use_selected_to_active = False
        bake.use_pass_direct = True
        bake.use_pass_indirect = True
        bake.use_pass_color = False
        target_names = sorted({
            material.node_tree.nodes["RWS_BAKE_TARGET"].image.name for material in prepared},
            key=str.casefold)
        for name in target_names:
            image = bpy.data.images.get(name)
            if image:
                image["rws_bake_completed"] = False
                if "rws_bake_completed_by" in image:
                    del image["rws_bake_completed_by"]
        return objects, target_names

    @staticmethod
    def _mark_completed(target_names):
        for name in target_names:
            image = bpy.data.images.get(name)
            if image:
                image["rws_bake_completed"] = True
                image["rws_bake_completed_by"] = "rws-lightmaps-batch-v1"

    def _complete(self, context):
        self._mark_completed(self._target_names)
        elapsed = _format_elapsed(time.monotonic() - self._started_at)
        context.scene.rws_bake_running = False
        context.scene.rws_bake_elapsed = elapsed
        context.scene.rws_lightmap_status = (
            f"Bake completed in {elapsed}: {len(self._objects)} objects, "
            f"{len(self._target_names)} target images; save before closing")
        self.report({"INFO"}, context.scene.rws_lightmap_status)

    def _remove_handlers(self):
        handlers = (
            (bpy.app.handlers.object_bake_complete,
             getattr(self, "_complete_handler", None)),
            (bpy.app.handlers.object_bake_cancel,
             getattr(self, "_cancel_handler", None)),
        )
        for collection, handler in handlers:
            if handler is not None and handler in collection:
                collection.remove(handler)

    def _stop_modal(self, context):
        timer = getattr(self, "_timer", None)
        if timer is not None:
            context.window_manager.event_timer_remove(timer)
            self._timer = None
        self._remove_handlers()

    def execute(self, context):
        """Synchronous path retained for scripts and headless Blender tests."""
        try:
            self._objects, self._target_names = self._prepare(context)
            self._started_at = time.monotonic()
            context.scene.rws_bake_running = True
            context.scene.rws_bake_elapsed = "00:00:00"
            context.scene.rws_lightmap_status = "Baking selected lighting with Cycles..."
            result = bpy.ops.object.bake(
                "EXEC_DEFAULT", type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"})
        except (RuntimeError, ValueError) as error:
            context.scene.rws_bake_running = False
            context.scene.rws_lightmap_status = "Bake failed"
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        if result != {"FINISHED"}:
            context.scene.rws_bake_running = False
            context.scene.rws_lightmap_status = "Bake cancelled"
            return result
        self._complete(context)
        return {"FINISHED"}

    def invoke(self, context, _event):
        if context.scene.rws_bake_running or bpy.app.is_job_running("OBJECT_BAKE"):
            self.report({"WARNING"}, "A Cycles bake is already running")
            return {"CANCELLED"}
        try:
            self._objects, self._target_names = self._prepare(context)
            self._started_at = time.monotonic()
            self._bake_result = None
            self._cancel_requested = False
            self._complete_handler = lambda _obj: setattr(self, "_bake_result", "FINISHED")
            self._cancel_handler = lambda _obj: setattr(self, "_bake_result", "CANCELLED")
            bpy.app.handlers.object_bake_complete.append(self._complete_handler)
            bpy.app.handlers.object_bake_cancel.append(self._cancel_handler)
            context.scene.rws_bake_running = True
            context.scene.rws_bake_elapsed = "00:00:00"
            context.scene.rws_lightmap_status = (
                "Cycles bake running in background; press Esc to cancel")
            result = bpy.ops.object.bake(
                "INVOKE_DEFAULT", type="DIFFUSE", pass_filter={"DIRECT", "INDIRECT"})
            if result == {"FINISHED"}:
                self._remove_handlers()
                self._complete(context)
                return {"FINISHED"}
            if result != {"RUNNING_MODAL"}:
                self._remove_handlers()
                context.scene.rws_bake_running = False
                context.scene.rws_lightmap_status = "Bake cancelled"
                return result
            self._timer = context.window_manager.event_timer_add(0.25, window=context.window)
            context.window_manager.modal_handler_add(self)
            return {"RUNNING_MODAL"}
        except (RuntimeError, ValueError) as error:
            self._remove_handlers()
            context.scene.rws_bake_running = False
            context.scene.rws_lightmap_status = "Bake failed"
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}

    def modal(self, context, event):
        # Passing Esc through lets Blender's native OBJECT_BAKE modal operator
        # request cancellation. Its handler tells us when cancellation finishes.
        if event.type == "ESC":
            self._cancel_requested = True
            context.scene.rws_lightmap_status = "Cancelling Cycles bake..."
            return {"PASS_THROUGH"}
        if event.type != "TIMER":
            return {"PASS_THROUGH"}

        elapsed = _format_elapsed(time.monotonic() - self._started_at)
        context.scene.rws_bake_elapsed = elapsed
        if self._bake_result == "FINISHED":
            self._stop_modal(context)
            self._complete(context)
            return {"FINISHED"}
        if self._bake_result == "CANCELLED":
            self._stop_modal(context)
            context.scene.rws_bake_running = False
            context.scene.rws_lightmap_status = f"Bake cancelled after {elapsed}"
            self.report({"WARNING"}, context.scene.rws_lightmap_status)
            return {"CANCELLED"}

        context.scene.rws_lightmap_status = (
            f"{'Cancelling' if self._cancel_requested else 'Baking'} selected lighting "
            f"with Cycles... {elapsed}")
        if context.area:
            context.area.tag_redraw()
        return {"RUNNING_MODAL"}


class RWS_OT_add_preview_sun(bpy.types.Operator):
    bl_idname = "rws_lightmaps.add_preview_sun"
    bl_label = "Add Strong Preview Sun"
    bl_description = "Add a known-strong Sun without losing the prepared bake set; rotate the Sun to change lighting"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        name = "RWS Preview Sun"
        sun = bpy.data.objects.get(name)
        if not sun or sun.type != "LIGHT" or sun.data.type != "SUN":
            data = bpy.data.lights.new(name, "SUN")
            sun = bpy.data.objects.new(name, data)
            context.scene.collection.objects.link(sun)
        sun.data.energy = 4.0
        sun.data.angle = math.radians(3.0)
        sun.rotation_euler = (math.radians(30.0), 0.0, math.radians(-35.0))
        sun.hide_render = False
        sun.hide_set(False)
        _show_scene_lighting(context)
        context.view_layer.update()
        context.scene.rws_lightmap_status = (
            "Added RWS Preview Sun (strength 4). Rotate it to change lighting; its position has no effect")
        self.report({"INFO"}, context.scene.rws_lightmap_status)
        return {"FINISHED"}


class RWS_OT_optimize_cycles(bpy.types.Operator):
    bl_idname = "rws_lightmaps.optimize_cycles"
    bl_label = "Apply RTX Fast Bake Preset"
    bl_description = "Enable OptiX/CUDA GPU baking and reduce irrelevant ray types for diffuse lightmaps"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        scene = context.scene
        scene.render.engine = "CYCLES"
        addon = context.preferences.addons.get("cycles")
        backend = None
        device_names = []
        if addon:
            preferences = addon.preferences
            for candidate in ("OPTIX", "CUDA"):
                try:
                    preferences.compute_device_type = candidate
                    preferences.get_devices()
                    devices = [device for device in preferences.devices
                               if device.type == candidate]
                    if not devices:
                        continue
                    for device in preferences.devices:
                        device.use = device in devices
                    backend = candidate
                    device_names = [device.name for device in devices]
                    break
                except (TypeError, RuntimeError):
                    continue
        if not backend:
            self.report({"ERROR"}, "No CUDA/OptiX Cycles device was found")
            return {"CANCELLED"}
        cycles = scene.cycles
        cycles.device = "GPU"
        cycles.samples = 32
        cycles.use_adaptive_sampling = True
        cycles.max_bounces = 3
        cycles.diffuse_bounces = 2
        cycles.glossy_bounces = 0
        cycles.transmission_bounces = 0
        cycles.volume_bounces = 0
        cycles.transparent_max_bounces = 8
        scene.rws_lightmap_status = (
            f"{backend} fast bake: {', '.join(device_names)}; 32 samples, "
            "2 diffuse bounces, non-diffuse bounces disabled")
        self.report({"INFO"}, scene.rws_lightmap_status)
        return {"FINISHED"}


class RWS_OT_diagnose_lighting(bpy.types.Operator):
    bl_idname = "rws_lightmaps.diagnose_lighting"
    bl_label = "Diagnose Lighting Preview"
    bl_description = "Report render engine, prepared shader links, visible lights, and viewport shading"

    def execute(self, context):
        prepared = [material for material in bpy.data.materials
                    if material.get("rws_bake_prepared") and material.node_tree]
        valid = 0
        for material in prepared:
            nodes = material.node_tree.nodes
            alpha_mix = nodes.get("RWS_ALPHA_MIX")
            diffuse = nodes.get("RWS_BAKE_DIFFUSE")
            base = nodes.get("RWS_BASE")
            outputs = [node for node in nodes
                       if node.type == "OUTPUT_MATERIAL" and node.is_active_output]
            output_ok = any(
                output.inputs["Surface"].links and
                output.inputs["Surface"].links[0].from_node == alpha_mix for output in outputs)
            shader_ok = bool(
                alpha_mix and diffuse and alpha_mix.inputs[2].links and
                alpha_mix.inputs[2].links[0].from_node == diffuse)
            base_ok = bool(
                base and diffuse and diffuse.inputs["Base Color"].links and
                diffuse.inputs["Base Color"].links[0].from_node == base)
            valid += int(output_ok and shader_ok and base_ok)
        lights = [obj for obj in context.scene.objects
                  if obj.type == "LIGHT" and not obj.hide_render and obj.visible_get()]
        view_modes = sorted({space.shading.type for window in context.window_manager.windows
                             for area in window.screen.areas if area.type == "VIEW_3D"
                             for space in area.spaces if space.type == "VIEW_3D"})
        prepared_objects = [obj for obj in context.scene.objects
                            if obj.type == "MESH" and obj.get("rws_bake_uv_prepared")]
        selected_prepared = sum(obj.select_get() for obj in prepared_objects)
        hidden_shells = sum(bool(obj.get("rws_duplicate_terrain_shell")) and obj.hide_render
                            for obj in context.scene.objects)
        passive = sum(bool(material.get("rws_bake_passive"))
                      for material in bpy.data.materials)
        status = (f"v0.15.0 diag: {context.scene.render.engine}; shaders {valid}/{len(prepared)}; "
                   f"bake objects {len(prepared_objects)} ({selected_prepared} selected); "
                  f"passive materials {passive}; "
                  f"hidden terrain shells {hidden_shells}; lights {len(lights)}; "
                  f"views {','.join(view_modes) or 'none'}")
        context.scene.rws_lightmap_status = status
        print(status)
        for light in lights:
            print(f"  light {light.name}: {light.data.type}, energy={light.data.energy}")
        level = {"INFO"} if prepared and valid == len(prepared) and lights else {"WARNING"}
        self.report(level, status)
        return {"FINISHED"}


class RWS_OT_restore_preview(bpy.types.Operator):
    bl_idname = "rws_lightmaps.restore_preview"
    bl_label = "Restore Preview Shaders"
    bl_description = "Restore preview emission shaders for prepared and passive RWS materials"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        if context.scene.rws_bake_running or bpy.app.is_job_running("OBJECT_BAKE"):
            self.report({"ERROR"}, "Wait for the active Cycles bake to finish or cancel it first")
            return {"CANCELLED"}
        restored = sum(
            _restore_preview_material(material) for material in bpy.data.materials
            if material.get("rws_bake_prepared") or material.get("rws_bake_passive"))
        restored_uvs = 0
        for obj in bpy.data.objects:
            if obj.type != "MESH" or not obj.data or not obj.get("rws_bake_uv_prepared"):
                continue
            layers = obj.data.uv_layers
            if layers:
                previous = min(int(obj.get("rws_bake_previous_uv", 0)), len(layers) - 1)
                layers.active_index = previous
                layers[previous].active_render = True
            obj["rws_bake_uv_prepared"] = False
            restored_uvs += 1
        _apply_view_mode(context.scene)
        context.scene.rws_lightmap_status = (
            f"Restored {restored} preview materials and {restored_uvs} mesh UV selections")
        self.report({"INFO"}, context.scene.rws_lightmap_status)
        return {"FINISHED"}


class RWS_OT_export_bakes(bpy.types.Operator):
    bl_idname = "rws_lightmaps.export_bakes"
    bl_label = "Export Game-Ready DDS"
    bl_description = "Scale completed bakes, write the selected DDS format and recreate the resource hierarchy"

    def _plan(self, context):
        scene = context.scene
        texture_directory = _resolve_path(scene.rws_lightmap_texture_directory)
        if not texture_directory.is_dir():
            raise ValueError("Configure a valid source Textures directory first")
        if not scene.rws_bake_export_directory:
            raise ValueError("Choose a Bake Export Root first")
        export_root = _resolve_path(scene.rws_bake_export_directory)
        relative_directory = _resource_texture_relative_path(texture_directory)
        destination_directory = export_root / relative_directory
        if destination_directory.resolve() == texture_directory.resolve():
            raise ValueError("Export root resolves to the unpacked source directory; choose a separate folder")
        resolution_scale = int(scene.rws_bake_resolution_scale)
        targets = sorted(
            (image for image in bpy.data.images
             if image.get("rws_bake_completed") and image.get("rws_source_lightmap_path") and
             int(image.get("rws_resolution_scale", 1)) == resolution_scale),
            key=lambda image: image.get("rws_lightmap_texture", image.name).casefold())
        if not targets:
            raise ValueError("No completed RWS bake targets at the selected resolution")
        unverified = [image.name for image in targets
                      if image.get("rws_bake_completed_by") not in {
                          "rws-lightmaps-batch-v1", "rws-lightmaps-per-atlas-v1"}]
        if unverified:
            raise ValueError(
                f"{len(unverified)} targets were not completed by a verified RWS bake; "
                "rebake them before DDS export")
        empty = []
        for image in targets:
            low, high = _bake_image_sample_range(image)
            if high - low < 1.0e-5 and (high < 1.0e-5 or 0.49 < high < 0.51):
                empty.append(image.name)
        if empty:
            examples = ", ".join(empty[:3])
            suffix = "..." if len(empty) > 3 else ""
            self.report({"WARNING"},
                f"{len(empty)} bake targets are empty or untouched ({examples}{suffix}); "
                "exporting them because sparse lightmaps can evade sampling")
        planned = []
        for image in targets:
            source_path = Path(image["rws_source_lightmap_path"]).resolve()
            if source_path.parent != texture_directory.resolve():
                raise ValueError(f"Bake source is outside the configured Textures directory: {source_path}")
            output_path = destination_directory / source_path.name
            if output_path.exists() and not scene.rws_bake_overwrite:
                raise ValueError(
                    f"Output already exists: {output_path}; enable Overwrite Existing Exports to replace it")
            planned.append((image, source_path, output_path,
                            _dds_export_work_units(
                                image, source_path, scene.rws_dds_output_format)))
        return (planned, texture_directory, export_root, relative_directory,
                destination_directory)

    def _steps(self, context, plan):
        planned, texture_directory, export_root, relative_directory, destination_directory = plan
        records = []
        file_count = len(planned)
        total_export_units = sum(item[3] for item in planned)
        prior_units = 0
        for index, (image, source_path, output_path, file_units) in enumerate(planned):
            if context.scene.rws_dds_output_format == "RAW32":
                encoder = _write_bake_dds_raw32_steps(
                    image, source_path, output_path, context.scene.rws_bake_export_scale)
            elif self._nvtt_path:
                encoder = _write_bake_dds_nvtt_steps(
                    image, source_path, output_path,
                    context.scene.rws_bake_export_scale, self._nvtt_path)
            else:
                encoder = _write_bake_dds_steps(
                    image, source_path, output_path, context.scene.rws_bake_export_scale)
            try:
                while True:
                    try:
                        completed, total, phase = next(encoder)
                    except StopIteration as finished:
                        fourcc, width, height, mip_count, _blocks = finished.value
                        break
                    percent = 100.0 * (prior_units + completed) / total_export_units
                    yield percent, index + 1, file_count, source_path.name, phase
            finally:
                encoder.close()
            records.append({
                "lightmap": source_path.name,
                "output": str(output_path.relative_to(export_root)),
                "format": fourcc,
                "width": width,
                "height": height,
                "mip_count": mip_count,
                "rgb_scale": context.scene.rws_bake_export_scale,
            })
            prior_units += file_units
            context.scene.rws_export_completed_files = len(records)
        manifest_path = export_root / "rws_bake_export.json"
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        with manifest_path.open("w", encoding="utf-8") as stream:
            json.dump({
                "format": "rws-lightmap-bake-export-v1",
                "source_textures": str(texture_directory),
                "resource_directory": str(relative_directory),
                "files": records,
            }, stream, indent=2)
            stream.write("\n")
        return records, destination_directory

    def _start(self, context, asynchronous):
        scene = context.scene
        scene.rws_export_total_files = 0
        scene.rws_export_completed_files = 0
        try:
            if scene.rws_bake_running or bpy.app.is_job_running("OBJECT_BAKE"):
                raise ValueError("Wait for the active Cycles bake to finish before exporting DDS files")
            mode = scene.rws_dds_encoder
            raw32 = scene.rws_dds_output_format == "RAW32"
            self._nvtt_path = _find_nvtt_export(scene.rws_nvtt_executable, refresh=True) \
                if not raw32 and mode in {"AUTO", "NVIDIA"} else None
            if not raw32 and mode == "NVIDIA" and not self._nvtt_path:
                raise ValueError(
                    "NVIDIA nvcompress.exe/nvtt_export.exe was not found; choose its path or use Auto/Python")
            scene.rws_export_encoder_active = (
                "Built-in raw A8R8G8B8" if raw32 else
                (f"NVIDIA CUDA ({self._nvtt_path.name})" if self._nvtt_path else
                 "Built-in Python"))
            self._plan_data = self._plan(context)
            self._generator = self._steps(context, self._plan_data)
            self._started_at = time.monotonic()
            self._timer = None
            scene.rws_export_running = True
            scene.rws_export_cancel_requested = False
            scene.rws_export_progress = 0.0
            scene.rws_export_elapsed = "00:00:00"
            scene.rws_export_completed_files = 0
            scene.rws_export_total_files = len(self._plan_data[0])
            if asynchronous:
                self._timer = context.window_manager.event_timer_add(0.1, window=context.window)
                context.window_manager.modal_handler_add(self)
                return {"RUNNING_MODAL"}
            while True:
                next(self._generator)
        except StopIteration as finished:
            self._complete(context, finished.value)
            return {"FINISHED"}
        except Exception as error:
            scene.rws_export_running = False
            scene.rws_lightmap_status = str(error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}

    def invoke(self, context, _event):
        return self._start(context, True)

    def execute(self, context):
        return self._start(context, False)

    def _complete(self, context, result):
        records, destination_directory = result
        elapsed = _format_elapsed(time.monotonic() - self._started_at)
        context.scene.rws_export_running = False
        context.scene.rws_export_progress = 100.0
        context.scene.rws_export_elapsed = elapsed
        context.scene.rws_lightmap_status = (
            f"Exported {len(records)} DDS files in {elapsed} under {destination_directory}")
        self.report({"INFO"}, context.scene.rws_lightmap_status)

    def _cancel(self, context):
        if getattr(self, "_generator", None):
            self._generator.close()
        context.scene.rws_export_running = False
        elapsed = _format_elapsed(time.monotonic() - self._started_at)
        context.scene.rws_export_elapsed = elapsed
        context.scene.rws_lightmap_status = (
            f"DDS export cancelled after {elapsed}; completed files were kept")
        self.report({"WARNING"}, context.scene.rws_lightmap_status)

    def modal(self, context, event):
        if event.type == "ESC":
            context.scene.rws_export_cancel_requested = True
        if event.type != "TIMER":
            return {"PASS_THROUGH"}
        if context.scene.rws_export_cancel_requested:
            context.window_manager.event_timer_remove(self._timer)
            self._timer = None
            self._cancel(context)
            return {"CANCELLED"}
        try:
            percent, file_index, file_count, filename, phase = next(self._generator)
            elapsed = _format_elapsed(time.monotonic() - self._started_at)
            context.scene.rws_export_progress = percent
            context.scene.rws_export_elapsed = elapsed
            context.scene.rws_lightmap_status = (
                f"DDS {file_index}/{file_count}: {filename} - {phase} - {percent:.1f}%")
            if context.area:
                context.area.tag_redraw()
            return {"RUNNING_MODAL"}
        except StopIteration as finished:
            context.window_manager.event_timer_remove(self._timer)
            self._timer = None
            self._complete(context, finished.value)
            return {"FINISHED"}
        except Exception as error:
            context.window_manager.event_timer_remove(self._timer)
            self._timer = None
            self._generator.close()
            context.scene.rws_export_running = False
            context.scene.rws_lightmap_status = str(error)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}


class RWS_OT_cancel_export(bpy.types.Operator):
    bl_idname = "rws_lightmaps.cancel_export"
    bl_label = "Cancel DDS Export"
    bl_description = "Cancel after the current small compression chunk and remove its temporary file"

    def execute(self, context):
        context.scene.rws_export_cancel_requested = True
        context.scene.rws_lightmap_status = "Cancelling DDS export..."
        return {"FINISHED"}


class RWS_PT_lightmaps(bpy.types.Panel):
    bl_label = "RWS Lightmaps"
    bl_idname = "RWS_PT_lightmaps"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "RWS Lightmaps"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        layout.label(text="RWS Lightmaps 0.15.0")

        column = layout.column(align=True)
        column.label(text="CSF Mission Editor Scene")
        column.prop(scene, "rws_lightmap_manifest", text="Manifest")
        column.prop(scene, "rws_lightmap_texture_directory", text="Textures")
        column.operator("rws_lightmaps.configure", icon="NODE_MATERIAL")
        column.prop(scene, "rws_show_advanced", toggle=True)
        if scene.rws_show_advanced:
            column.prop(scene, "rws_base_non_color")
            column.prop(scene, "rws_lightmap_non_color")
            column.prop(scene, "rws_lightmap_intensity")

        layout.separator()
        layout.label(text="Viewport Mode")
        layout.prop(scene, "rws_lightmap_view", expand=True)
        layout.operator("rws_lightmaps.reload_images", icon="FILE_REFRESH")

        layout.separator()
        layout.label(text="Cycles Lightmap Bake")
        bake_box = layout.box()
        bake_box.label(text="Save the .blend before baking", icon="ERROR")
        bake_box.prop(scene, "rws_bake_resolution_scale")
        if scene.rws_bake_resolution_scale == "4":
            bake_box.label(text="4x width/height = 16x pixels and bake cost", icon="ERROR")
        bake_box.operator("rws_lightmaps.select_world_sectors", icon="RESTRICT_SELECT_OFF")
        bake_box.operator("rws_lightmaps.prepare_bake", icon="SHADING_RENDERED")
        if scene.rws_show_advanced:
            bake_box.operator("rws_lightmaps.add_preview_sun", icon="LIGHT_SUN")
            bake_box.operator("rws_lightmaps.optimize_cycles", icon="PREFERENCES")
            bake_box.label(
                text=(f"Cycles: {scene.cycles.device}, {scene.cycles.samples} samples, "
                      f"{scene.cycles.diffuse_bounces} diffuse bounces"))
            bake_box.operator("rws_lightmaps.diagnose_lighting", icon="INFO")
        bake_box.prop(scene, "rws_bake_margin")
        bake_button = bake_box.row()
        bake_button.enabled = not scene.rws_bake_running
        bake_button.operator("rws_lightmaps.bake_selected", icon="RENDER_STILL")
        if scene.rws_bake_running:
            bake_box.label(text=f"Bake elapsed {scene.rws_bake_elapsed}", icon="TIME")
            bake_box.label(text="Press Esc to cancel the Cycles bake", icon="INFO")
        bake_box.operator("rws_lightmaps.restore_preview", icon="NODE_MATERIAL")
        bake_box.label(text="Other RWS materials become passive shadow casters")
        bake_box.label(text="Shared UVs may overlap; test one group first")

        layout.separator()
        layout.label(text="Game DDS Export")
        export_box = layout.box()
        export_box.prop(scene, "rws_bake_export_directory", text="Export Root")
        export_box.prop(scene, "rws_dds_output_format")
        if scene.rws_show_advanced:
            export_box.prop(scene, "rws_bake_export_scale")
            if scene.rws_dds_output_format == "ORIGINAL":
                export_box.prop(scene, "rws_dds_encoder")
            if (scene.rws_dds_output_format == "ORIGINAL" and
                    scene.rws_dds_encoder in {"AUTO", "NVIDIA"}):
                export_box.prop(scene, "rws_nvtt_executable", text="NVTT Executable")
                detected_nvtt = _find_nvtt_export(scene.rws_nvtt_executable)
                export_box.label(
                    text=(f"NVIDIA: {detected_nvtt.name}" if detected_nvtt else
                          "NVIDIA tool not found; Auto uses Python"),
                    icon="CHECKMARK" if detected_nvtt else "INFO")
        export_box.prop(scene, "rws_bake_overwrite")
        export_button = export_box.row()
        export_button.enabled = not scene.rws_export_running and not scene.rws_bake_running
        export_button.operator("rws_lightmaps.export_bakes", icon="EXPORT")
        if scene.rws_export_running:
            export_box.prop(scene, "rws_export_progress", text="Progress", slider=True)
            export_box.label(text=f"Elapsed {scene.rws_export_elapsed}", icon="TIME")
            export_box.operator("rws_lightmaps.cancel_export", icon="CANCEL")
            export_box.label(text=f"Encoder: {scene.rws_export_encoder_active}")
        if scene.rws_export_total_files:
            export_box.label(
                text=(f"DDS files exported: {scene.rws_export_completed_files} / "
                      f"{scene.rws_export_total_files}"), icon="FILE_TICK")
        export_box.label(text="Creates <archive>/Maps/<map>/Textures")

        if scene.rws_lightmap_material_count:
            box = layout.box()
            box.label(text=f"Materials: {scene.rws_lightmap_material_count}")
            box.label(text=f"Base textures: {scene.rws_lightmap_base_count}")
            box.label(text=f"Lightmaps: {scene.rws_lightmap_lightmap_count}")
            box.label(text=f"Unique lightmap textures: {scene.rws_lightmap_texture_count}")
            box.label(text=f"Alpha materials: {scene.rws_lightmap_alpha_count}")
            if scene.rws_lightmap_missing_material_count:
                box.label(text=f"Missing materials: {scene.rws_lightmap_missing_material_count}", icon="ERROR")
            if scene.rws_lightmap_missing_uv_count:
                box.label(text=f"Lightmapped materials without UV2: {scene.rws_lightmap_missing_uv_count}", icon="ERROR")
        if scene.rws_lightmap_status:
            layout.label(text=scene.rws_lightmap_status, icon="INFO")


CLASSES = (
    RWS_OT_configure,
    RWS_OT_reload_images,
    RWS_OT_select_world_sectors,
    RWS_OT_prepare_bake,
    RWS_OT_add_preview_sun,
    RWS_OT_optimize_cycles,
    RWS_OT_bake_selected,
    RWS_OT_diagnose_lighting,
    RWS_OT_restore_preview,
    RWS_OT_export_bakes,
    RWS_OT_cancel_export,
    RWS_PT_lightmaps,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.rws_lightmap_manifest = StringProperty(
        name="Manifest", description="Path to *.manifest.json", subtype="FILE_PATH",
        update=_manifest_updated)
    bpy.types.Scene.rws_lightmap_texture_directory = StringProperty(
        name="Textures", description="Directory containing DDS or converted texture images", subtype="DIR_PATH")
    bpy.types.Scene.rws_lightmap_view = EnumProperty(
        name="View",
        items=(
            ("BASE", "Base", "Show only base textures"),
            ("LIGHTMAP", "Lightmap", "Show only lightmap textures"),
            ("COMBINED", "Base × Lightmap", "Multiply base textures by lightmaps"),
        ),
        default="COMBINED",
        update=_view_mode_updated,
    )
    bpy.types.Scene.rws_lightmap_non_color = BoolProperty(
        name="Lightmaps as Non-Color", default=True,
        description="Treat lightmap values as linear data; disable to decode them as sRGB")
    bpy.types.Scene.rws_base_non_color = BoolProperty(
        name="Legacy Base DDS Color", default=True,
        description="Use stored DDS color values like CSF's legacy renderer instead of sRGB-decoding them")
    bpy.types.Scene.rws_lightmap_intensity = FloatProperty(
        name="Lightmap Intensity", default=2.0, min=0.0, max=8.0,
        description="Scale Lightmap and Base x Lightmap modes; CSF's default modulation is 2x",
        update=_view_mode_updated)
    bpy.types.Scene.rws_show_advanced = BoolProperty(
        name="Show Advanced Options", default=False,
        description="Show color handling, Cycles tuning, diagnostics and encoder settings")
    bpy.types.Scene.rws_bake_margin = bpy.props.IntProperty(
        name="Bake Margin", default=4, min=0, max=64,
        description="Pixels extended beyond lightmap UV islands to prevent mipmap seams")
    bpy.types.Scene.rws_bake_running = BoolProperty(default=False, options={"HIDDEN"})
    bpy.types.Scene.rws_bake_elapsed = StringProperty(default="00:00:00", options={"HIDDEN"})
    bpy.types.Scene.rws_bake_resolution_scale = EnumProperty(
        name="Bake Resolution",
        items=(
            ("1", "1x", "Use the original game lightmap width and height"),
            ("2", "2x", "Double width and height (4x as many pixels)"),
            ("4", "4x", "Quadruple width and height (16x as many pixels)"),
        ),
        default="1",
        description="Bake and export lightmaps at a multiple of the original DDS dimensions")
    bpy.types.Scene.rws_bake_export_directory = StringProperty(
        name="Bake Export Root", subtype="DIR_PATH",
        description="Separate output root where the archive/map/Textures hierarchy is created")
    bpy.types.Scene.rws_bake_export_scale = FloatProperty(
        name="CSF RGB Scale", default=0.5, min=0.0, max=2.0,
        description="Scale baked RGB before DDS compression; 0.5 compensates for CSF's 2x modulation")
    bpy.types.Scene.rws_bake_overwrite = BoolProperty(
        name="Overwrite Existing Exports", default=False,
        description="Allow game-ready DDS files from a previous export to be replaced")
    bpy.types.Scene.rws_dds_output_format = EnumProperty(
        name="DDS Format",
        items=(
            ("ORIGINAL", "Original BC1/BC2",
             "Preserve each source lightmap's DXT1/BC1 or DXT3/BC2 format"),
            ("RAW32", "32-bit Raw (A8R8G8B8)",
             "Lossless 8-bit RGB in a legacy uncompressed DDS; uses four bytes per pixel"),
        ), default="ORIGINAL",
        description="Storage format for exported game lightmaps")
    bpy.types.Scene.rws_dds_encoder = EnumProperty(
        name="DDS Encoder",
        items=(
            ("AUTO", "Auto", "Use NVIDIA Texture Tools when found, otherwise built-in Python"),
            ("NVIDIA", "NVIDIA NVTT", "Require nvtt_export.exe and CUDA compression"),
            ("PYTHON", "Built-in Python", "Portable but much slower BC1/BC2 encoder"),
        ), default="AUTO")
    bpy.types.Scene.rws_nvtt_executable = StringProperty(
        name="NVIDIA Texture Tools", subtype="FILE_PATH",
        description="Optional path to nvcompress.exe or nvtt_export.exe; common locations and PATH are detected automatically")
    bpy.types.Scene.rws_export_encoder_active = StringProperty(default="", options={"HIDDEN"})
    bpy.types.Scene.rws_export_running = BoolProperty(default=False, options={"HIDDEN"})
    bpy.types.Scene.rws_export_cancel_requested = BoolProperty(default=False, options={"HIDDEN"})
    bpy.types.Scene.rws_export_progress = FloatProperty(
        name="DDS Export", default=0.0, min=0.0, max=100.0, subtype="PERCENTAGE")
    bpy.types.Scene.rws_export_elapsed = StringProperty(default="00:00:00", options={"HIDDEN"})
    bpy.types.Scene.rws_export_total_files = bpy.props.IntProperty(default=0, options={"HIDDEN"})
    bpy.types.Scene.rws_export_completed_files = bpy.props.IntProperty(default=0, options={"HIDDEN"})
    bpy.types.Scene.rws_lightmap_status = StringProperty(name="Status", default="")
    bpy.types.Scene.rws_lightmap_material_count = bpy.props.IntProperty(default=0)
    bpy.types.Scene.rws_lightmap_base_count = bpy.props.IntProperty(default=0)
    bpy.types.Scene.rws_lightmap_lightmap_count = bpy.props.IntProperty(default=0)
    bpy.types.Scene.rws_lightmap_texture_count = bpy.props.IntProperty(default=0)
    bpy.types.Scene.rws_lightmap_alpha_count = bpy.props.IntProperty(default=0)
    bpy.types.Scene.rws_lightmap_missing_material_count = bpy.props.IntProperty(default=0)
    bpy.types.Scene.rws_lightmap_missing_uv_count = bpy.props.IntProperty(default=0)


def unregister():
    properties = (
        "rws_lightmap_manifest", "rws_lightmap_texture_directory", "rws_lightmap_view",
        "rws_base_non_color", "rws_lightmap_non_color", "rws_lightmap_intensity",
        "rws_show_advanced",
        "rws_bake_margin", "rws_bake_running", "rws_bake_elapsed",
        "rws_bake_resolution_scale",
        "rws_bake_export_directory", "rws_bake_export_scale", "rws_bake_overwrite",
        "rws_export_running", "rws_export_cancel_requested", "rws_export_progress",
        "rws_export_elapsed",
        "rws_dds_output_format", "rws_dds_encoder", "rws_nvtt_executable",
        "rws_export_encoder_active",
        "rws_export_total_files", "rws_export_completed_files",
        "rws_lightmap_status", "rws_lightmap_material_count",
        "rws_lightmap_base_count", "rws_lightmap_lightmap_count", "rws_lightmap_texture_count",
        "rws_lightmap_alpha_count",
        "rws_lightmap_missing_material_count", "rws_lightmap_missing_uv_count",
    )
    for name in properties:
        if hasattr(bpy.types.Scene, name):
            delattr(bpy.types.Scene, name)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
