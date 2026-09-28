# Blender authoring

Blender makes the meshes of a new map: the terrain and custom buildings.
csf-editor places everything and owns the mission (actors, routes, areas,
scripts); see [the editor plan](../archive/editor/editor-blender-authoring.md). The two
meet in an [authoring project](../reference/project-format.md), such as the
one `examples/hello-world/build.sh` creates.

## Install

```bash
python3 tools/blender/package_addons.py   # writes tools/blender/csf_authoring.zip
```

In Blender: **Edit > Preferences > Add-ons > Install from Disk**, choose
`csf_authoring.zip`, enable **CSF authoring**, and set **csf-mod** in its
preferences to csf-editor's `build/Release/csf-mod` (or put `csf-mod` on `PATH`).

## Work on the map

1. Open the project in csf-editor (`csf-editor --project <project>`) and its terrain
   in Blender (`<project>/sources/world/terrain.blend` for hello world).
2. In the 3D view sidebar (**N**), tab **CSF**, set **Project** to the project
   folder.
3. **Load reference** shows the built map (wireframe), its props, the actor
   models and markers for placements, navigation routes (wire), areas (wire
   boxes) and dummies, in the locked collection **CSF reference**. It is never
   exported; load it again after changing the mission in csf-editor.
4. Edit the terrain. **Send to csf-editor** saves the `.blend` and writes the
   exports into the project. csf-editor notices within a second, rebuilds the map,
   reloads the mission and opens the **Height report** if anything no longer
   stands on the ground; **Resnap all** there moves it.

## Assets

Select meshes and press **Terrain** or **Building** under **Asset**:

- **Terrain** is built into the World where it is modelled.
- **Building** is exported about the object's origin (scale applies; its
  position and rotation do not). csf-editor places it; each placement merges its
  triangles into the World, so it gets collision and lightmaps like the
  shipped buildings. Model it at the origin.

The asset ID (editable in the panel) is the object's identity for the project:
renaming the object does not change it. Objects that share an ID are exported
together. Materials pick donor textures and surfaces through their
`csf_texture`, `csf_surface` and `csf_shade` properties (see
`tools/blender/csf_authoring/csfworld.py`).

## Baked lighting

Add lights (a Sun: its rotation is the light direction), then **Bake lighting**
(size and samples). Each tagged asset gets its own lightmap, `<ASSET>_Lm`:

- A second UV layer (`CSF_Lightmap`) is unwrapped when the mesh has only one;
  unwrap it yourself first for a cleaner result.
- Only light is baked (direct and indirect, no colour), at half strength: the
  game doubles lightmaps.
- The asset's materials get `csf_lightmap`; a material another asset uses is
  copied first, so each asset names its own lightmap.
- The PNG goes to `sources/lightmaps/` and the project records it.

Then **Send to csf-editor**: the project build turns the PNGs into DXT1 DDS files,
and the mission lists and packages them (csf-editor does it as an undoable edit;
save to keep it). Model a building where it will stand when you bake, so its
shadow falls on the terrain in the right place; its export ignores the object's
position. From the command line: `csf-mod project-lightmaps <project>` after
`project-build`.

**Import donor model** loads a vanilla `.rpc` (through `rws-info`) as an
editable mesh, as a starting point for a new asset.

## A map without a project

To edit a shipped map (or build one of your own on its materials) without an
authoring project, the map goes through `.csfworld` and back:

```bash
# 1. The map's source: its visual World and _col.rws collision, every triangle
#    with its material, lightmap, surface and collision shade. --check builds
#    it again and compares (every shipped map comes back unchanged).
csf-mod world-source ../CSF_unpacks/Convoy/Maps/FR03/FR03.rws work/FR03.csfworld --check
# 2. In Blender: File > Import > CSF map (.rws, .csfworld); edit; File > Export >
#    CSF World (.csfworld).
# 3. Build it with the original map as donor; --keep-props keeps its props.
csf-mod world-build work/FR03-edited.csfworld ../CSF_unpacks/Convoy/Maps/FR03/FR03.rws \
    patch/Maps/FR03/FR03.rws --keep-props
```

**Import map** (sidebar box *Map without a project*, or File > Import) takes the
`.rws` itself (it runs `csf-mod world-source` and keeps the `.csfworld` next to
the saved `.blend`) or a `.csfworld`. It makes one object per role:
*visual* (textures from the map's `Textures` folder, base and `Lightmap` UVs,
the shipped normals) and *collision* (drawn as wire, each surface in the
colour the game gives it in `Materiales.bdd`: Intangible, the invisible walls,
is red, Madera brown, Tierra lavender; switch Viewport Shading's colour to
*Material* to see them). Materials carry `csf_texture`, `csf_surface`,
`csf_lightmap` and, on collision, `csf_surface_color`. Keep the collision
object visible: hidden objects are not exported.

**Export .csfworld** writes the visible meshes with nine significant digits,
so untouched vertices come back where they were. `world-build` resolves
materials in the donor map: a texture must be one of its visual materials
(with its lightmap if it names one) and a surface one of its collision
materials. For textures and lightmaps of your own, pass them to the build:

```bash
csf-mod world-build work/FR03-edited.csfworld ../CSF_unpacks/Convoy/Maps/FR03/FR03.rws \
    patch/Maps/FR03/FR03.rws --keep-props \
    --texture MYWALL sources/mywall.png FDET_40A \
    --lightmap MYHOUSE_Lm sources/myhouse_lm.png
```

`--texture <name> <image> <like>` makes materials naming `<name>` copies of the
donor material of texture `<like>`; `--lightmap <name> <image>` is a lightmap a
material names in `csf_lightmap`. Both are written as DXT1 DDS (a PNG's alpha
is dropped; pass a `.dds` to keep it) to `Textures/` next to the new map, and
listed in a copy of the donor's texture list (`FR03.txl`) written there too.

The map folder then holds what the game loads from `Maps/<map>/`. The game
mounts `Patch.pak` from its own folder (next to `GlobalEK.pak`; shipped installs
have none) and reads it before the mission archives (package-loader KB-3), so
these files packed under their game paths (`Maps/FR03/FR03.rws`, ...;
`pakman-cli create`) should replace the map's in every mission that uses that
folder (Ransom, Resist and Parachut share `Maps/FR01`). This is established
from the executable, not yet tried in game; the other way, a whole mission
archive, is `csf-mod export-mission` ([mission-editor.md](mission-editor.md)). Keep
the mission's sector map (`Maps/Secs/<Mission>.sec`, where actors walk) unless
the walkable ground changed: `csf-mod sector-build` makes one from every
collision face, which hello world uses on its flat terrain but which is untried
on a full shipped map.

A glTF from `rws-info --export-scene-gltf` imported into Blender also exports:
its materials fall back to their `rws_base_texture`, `rws_surface_name` and
`rws_lightmap_texture` properties and its props (`rws_kind`) are skipped, but
it has no collision object or shades, so prefer Import map.

## Collision shade

Each collision triangle carries a shade byte, 0-255. The game's own exporter
baked it from the collision World's vertex colours, as the integer mean of R,
G and B over the triangle's three corners. It is meant for darkening the
entities that stand in shadow, but no runtime reader of it has been found yet
(format-reversal KB-world-geometry-4), so check in game whether it does.
Import map keeps it in the same form: the colour attribute `csf_shade_color`
of the collision object, grey per face, with 255 as white. A project's terrain
and buildings (role *both*) take it the same way.

- **Paint** it in Vertex Paint mode, or see it with Viewport Shading's colour
  set to *Attribute*. Export writes each triangle's shade as the game's
  exporter did: `(R + G + B of its three corners) / 9`, from the sRGB bytes
  you see. A colour attribute with that name per vertex works too.
- **Bake shade** (sidebar box *Collision shade*) bakes the scene's light into
  the selected collision objects (role *collision* or *both*; with none
  selected, all of them) with Cycles, direct and indirect, without colour. The
  scale is linear: a Sun of strength 1 falling straight on gives 255 and the
  world colour lights the shadows. Visual meshes cast shadows too. Use 64 or
  more samples, because fewer leave the shade noisy.
- **Shade colours** gives a new collision mesh the attribute, filled with its
  current shades, so you can start painting.

Without `csf_shade_color`, a face's shade comes from the integer face attribute
`csf_shade` (what versions before 0.4.0 imported), else from the material's
`csf_shade`, else 228, the most common shipped value.

## Headless

The exporter still runs without the add-on installed:

```bash
blender scene.blend --background --python tools/blender/export_csf_world.py -- out.csfworld
```

`tools/blender/csf_authoring_test.py` tests the add-on against a copy of an
authoring project, and `tools/blender/standalone_map_test.py` a shipped map
through Import map, Export and `world-build` (usage in their docstrings).
`export_csf_world.py -- out.csfworld --precise` writes nine significant digits,
as Export .csfworld does.
