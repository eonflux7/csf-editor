# Blender lightmap workflow

The included add-on requires Blender 4.0 or newer. Install
[tools/blender/rws_lightmaps.zip](../../tools/blender/rws_lightmaps.zip) using
**Edit > Preferences > Add-ons > Install from Disk**.

For material and lightmap preview:

1. Import an exported `.gltf` into a clean Blender scene.
2. In the 3D View, press `N` and open the **RWS Lightmaps** tab.
3. Select the export's sibling `.manifest.json` file.
4. Confirm the detected `Textures` directory.
5. Choose **Configure Imported Materials**.
6. Switch between **Base**, **Lightmap**, and **Base x Lightmap**.

The add-on connects base textures to the first UV layer and lightmaps to the
second, preserves foliage alpha, and keeps `FFLR*` terrain materials opaque. Its
legacy DDS color handling and the game's `2.0x` lightmap modulation are enabled by
default.

It can also prepare selected meshes for Cycles light-only baking and stage baked
lightmaps as legacy DXT1/DXT3 DDS files. Staging applies CSF's default `0.5` RGB
encoding scale and recreates the original archive/map/`Textures` hierarchy in a
separate output directory. The portable encoder requires no external tools;
NVIDIA Texture Tools can optionally accelerate large exports.

See the [Blender add-on guide](../../tools/blender/rws_lightmaps/README.md) for bake
memory estimates, material behavior, encoder setup, and the complete game-ready
DDS workflow.
