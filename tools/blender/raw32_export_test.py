"""Hermetic Blender regression test for legacy uncompressed lightmap export.

Usage:
  blender --background --factory-startup --python raw32_export_test.py
"""

import json
import struct
import sys
import tempfile
from pathlib import Path

import bpy


sys.path.insert(0, str(Path(__file__).resolve().parent))
import rws_lightmaps


def source_dxt1_header(width, height, mip_count):
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<I", header, 4, 124)
    struct.pack_into("<I", header, 8, 0x000A1007)
    struct.pack_into("<I", header, 12, height)
    struct.pack_into("<I", header, 16, width)
    struct.pack_into("<I", header, 20, max(8, ((width + 3) // 4) * ((height + 3) // 4) * 8))
    struct.pack_into("<I", header, 28, mip_count)
    struct.pack_into("<I", header, 76, 32)
    struct.pack_into("<I", header, 80, 0x04)
    header[84:88] = b"DXT1"
    struct.pack_into("<I", header, 108, 0x00401008)
    return header


rws_lightmaps.register()
scene = bpy.context.scene

with tempfile.TemporaryDirectory(prefix="rws-raw32-test-") as temporary:
    root = Path(temporary)
    textures = root / "Ransom" / "Maps" / "TEST" / "Textures"
    textures.mkdir(parents=True)
    source = textures / "TEST_Lm.dds"
    source.write_bytes(source_dxt1_header(4, 4, 3))

    image = bpy.data.images.new(
        "RWS_BAKE_TEST_Lm_2x", width=8, height=8, alpha=False, float_buffer=True)
    image.pixels.foreach_set([0.25, 0.5, 0.75, 1.0] * (8 * 8))
    image["rws_bake_completed"] = True
    image["rws_bake_completed_by"] = "rws-lightmaps-batch-v1"
    image["rws_source_lightmap_path"] = str(source)
    image["rws_lightmap_texture"] = "TEST_Lm"
    image["rws_resolution_scale"] = 2

    export_root = root / "stage"
    scene.rws_lightmap_texture_directory = str(textures)
    scene.rws_bake_export_directory = str(export_root)
    scene.rws_bake_resolution_scale = "2"
    scene.rws_bake_export_scale = 1.0
    scene.rws_dds_output_format = "RAW32"

    # A modal export must yield after a bounded row chunk, and closing it at that
    # point must remove both the incomplete DDS and its disk-backed mip buffer.
    assert rws_lightmaps._raw_rows_per_chunk(8192) == 128
    cancelled = export_root / "cancelled.dds"
    encoder = rws_lightmaps._write_bake_dds_raw32_steps(
        image, source, cancelled, scene.rws_bake_export_scale)
    completed, total, phase = next(encoder)
    assert 0 < completed < total
    assert phase == "Streaming raw mip 1/4"
    encoder.close()
    assert not cancelled.exists()
    assert not cancelled.with_suffix(".dds.tmp").exists()
    assert not list(export_root.glob(".cancelled.dds.mip-*.tmp"))

    assert bpy.ops.rws_lightmaps.export_bakes() == {"FINISHED"}

    output = export_root / "Ransom" / "Maps" / "TEST" / "Textures" / source.name
    encoded = output.read_bytes()
    assert encoded[:4] == b"DDS "
    assert struct.unpack_from("<II", encoded, 12) == (8, 8)
    assert struct.unpack_from("<I", encoded, 20)[0] == 32
    assert struct.unpack_from("<I", encoded, 28)[0] == 4
    assert struct.unpack_from("<I", encoded, 80)[0] == 0x41
    assert struct.unpack_from("<IIIII", encoded, 88) == (
        32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    assert len(encoded) == 128 + (8 * 8 + 4 * 4 + 2 * 2 + 1) * 4
    assert encoded[128:132] == bytes((191, 128, 64, 255))

    receipt = json.loads((export_root / "rws_bake_export.json").read_text())
    assert receipt["files"][0]["format"] == "A8R8G8B8"
    assert scene.rws_export_encoder_active == "Built-in raw A8R8G8B8"

    # The portable BC writer uses the same bounded pixel and mip streaming path.
    bc_root = root / "stage-bc"
    scene.rws_bake_export_directory = str(bc_root)
    scene.rws_dds_output_format = "ORIGINAL"
    scene.rws_dds_encoder = "PYTHON"
    assert bpy.ops.rws_lightmaps.export_bakes() == {"FINISHED"}
    bc_output = bc_root / "Ransom" / "Maps" / "TEST" / "Textures" / source.name
    bc_encoded = bc_output.read_bytes()
    assert bc_encoded[:4] == b"DDS "
    assert bc_encoded[84:88] == b"DXT1"
    assert struct.unpack_from("<II", bc_encoded, 12) == (8, 8)
    assert struct.unpack_from("<I", bc_encoded, 28)[0] == 4
    assert len(bc_encoded) == 128 + 7 * 8
    assert not list(bc_output.parent.glob(f".{source.name}.bc-mip-*.tmp"))

    # NVTT's lossless input staging must also yield by row chunk and retain BGRA
    # byte order without constructing a full float atlas.
    tga = root / "nvtt-input.tga"
    tga_writer = rws_lightmaps._write_nvtt_input_tga_steps(image, tga, 1.0, None)
    while True:
        try:
            rows_done, row_count = next(tga_writer)
            assert 0 < rows_done <= row_count
        except StopIteration as finished:
            terminal_pixel = finished.value
            break
    tga_bytes = tga.read_bytes()
    assert len(tga_bytes) == 18 + 8 * 8 * 4
    assert tga_bytes[18:22] == bytes((191, 128, 64, 255))
    assert terminal_pixel == (64, 128, 191, 255)

print("RWS_RAW32_EXPORT_OK")
