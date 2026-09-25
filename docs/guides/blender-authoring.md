# Blender authoring

Blender makes the meshes of a new map: the terrain and custom buildings.
rws-man places everything and owns the mission (actors, routes, areas,
scripts); see [the editor plan](../plans/editor-blender-authoring.md). The two
meet in an [authoring project](../plans/editor-project-format.md), such as the
one `tools/hello_world/build.sh` creates.

## Install

```bash
python3 tools/blender/package_addons.py   # writes tools/blender/csf_authoring.zip
```

In Blender: **Edit > Preferences > Add-ons > Install from Disk**, choose
`csf_authoring.zip`, enable **CSF authoring**, and set **csf-mod** in its
preferences to rws-man's `build/Release/csf-mod` (or put `csf-mod` on `PATH`).

## Work on the map

1. Open the project in rws-man (`rws-man --project <project>`) and its terrain
   in Blender (`<project>/sources/world/terrain.blend` for hello world).
2. In the 3D view sidebar (**N**), tab **CSF**, set **Project** to the project
   folder.
3. **Load reference** shows the built map (wireframe), its props, the actor
   models and markers for placements, navigation routes (wire), areas (wire
   boxes) and dummies, in the locked collection **CSF reference**. It is never
   exported; load it again after changing the mission in rws-man.
4. Edit the terrain. **Send to rws-man** saves the `.blend` and writes the
   exports into the project. rws-man notices within a second, rebuilds the map,
   reloads the mission and opens the **Height report** if anything no longer
   stands on the ground; **Resnap all** there moves it.

## Assets

Select meshes and press **Terrain** or **Building** under **Asset**:

- **Terrain** is built into the World where it is modelled.
- **Building** is exported about the object's origin (scale applies; its
  position and rotation do not). rws-man places it; each placement merges its
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

Then **Send to rws-man**: the project build turns the PNGs into DXT1 DDS files,
and the mission lists and packages them (rws-man does it as an undoable edit;
save to keep it). Model a building where it will stand when you bake, so its
shadow falls on the terrain in the right place; its export ignores the object's
position. From the command line: `csf-mod project-lightmaps <project>` after
`project-build`.

**Import donor model** loads a vanilla `.rpc` (through `rws-info`) as an
editable mesh, as a starting point for a new asset.

## Headless

The exporter still runs without the add-on installed:

```bash
blender scene.blend --background --python tools/blender/export_csf_world.py -- out.csfworld
```

`tools/blender/csf_authoring_test.py` tests the add-on against a copy of an
authoring project (usage in its docstring).
