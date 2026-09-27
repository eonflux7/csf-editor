# Roadmap: what is open

2026-09-27. The editor plans that built CSF Mission Editor have shipped and
moved to [the archive](../archive/editor/); this page collects what they,
the research spikes and the two example missions left open. When an item is
done, describe the result in [`guides/`](../guides/) and drop it here.

## Checks in the game

Built and tested here, not yet confirmed in *Commandos: Strike Force*:

- **Country v2** (the [example](../../examples/country/README.md), "What to
  check in game"): the officer's arrival by car with the picture-in-picture
  view, the body-found alarm (`VEO_BICHO` on a dead actor), the zone cutscene
  playing once, night lighting, floors, the Panzer III's textures, the radio
  and telephone on their tables.
- **Hello world**: the stage 2 acceptance playtest (change the hill in
  Blender, rebuild, play) and the stage 7 lightmap bake
  ([editor/Blender plan](../archive/editor/editor-blender-authoring.md)).
- **Spike A1**, a class with a new model file: the scaled crate test build
  ([spikes](editor-spikes.md)).
- The editor's unverified trigger events and actions: an actor script on a
  custom event from the mission program, the alarm action, a guard finding a
  body.
- Checkpoint (the first GUI-made project): whether the officer's glow is the
  Spy's uniform.

## Research spikes

The go/no-go experiments for custom props and characters are in
[editor-spikes.md](editor-spikes.md): A2 a new mesh in a donor Clump, A3 a
`.cmo` collision model, B1 a new animation clip, B2 a re-shaped character.

## Editor

From Country (the "LEGO" asset registry and its editor wishlist):

- A **building library** and an **object browser** for other maps' Worlds
  (lightmap groups and the single objects inside them), with rendered
  thumbnails, placing a `piece` sunk by its recorded ground height.
- **Texture import**: a model's PNG becomes a `texture` record, the donor
  material to imitate picked by name.
- **Pull a shipped script** by name, with its IDs remapped.
- **Door actors** (`.DOOR_BOX`, class 143), which `mission-ops` cannot write.
- **Floor-space hints**: the free cells of an interior floor drawn while
  placing, and floor-aware pieces (the height report flags a piece whose
  floor is at or below the terrain).
- A **scatter tool** for trees and bushes with keep-out zones.
- A **vehicle arrival** recipe: vehicle, route, passengers and seats, their
  posts, an optional picture-in-picture camera.
- **Animal idle presets** (grazing cows, dogs) and behaviour presets from the
  catalogue (radio operator, smoker, lookout, talking pair).
- **Recipe text IDs** as offsets into the project's reserved range.
- The Timeline panel is still titled "Intro cutscene" although it edits any
  cutscene.

Left from the UX plan's phases
([editor-ux-redesign.md](../archive/editor/editor-ux-redesign.md)):

- Assets: thumbnails and favourites, a translucent model under the Place
  cursor with a "cannot go here" state, copy/paste, distribute and group.
- Components: adopting a plain actor as a component (keeping its other
  scripts), group moves and Align to ground through the component.
- Triggers: an If on zone state; the flow graph's layout for large shipped
  missions.
- Projects: thumbnails on Home, the Blender add-on reporting its version,
  Deploy also launching the game.
- Byte identity of hello world built through the GUI: settable record IDs,
  `scene.py`'s rounding, players on a start group, typed shot positions, a
  portrait field on the guard presets, zone names in Properties.
