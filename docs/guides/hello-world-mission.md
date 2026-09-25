# Build and test hello world

From the repository root, with Blender available on `PATH`, the unpacked
resources at `../CSF_unpacks`, and untouched original archives:

```bash
./build.sh
bash tools/hello_world/build.sh ~/dev/csf-mods/hello-world-project \
  ~/dev/csf-mods/Convoy.original.pak ~/dev/csf-mods/GlobalEK.original.pak
```

The project directory must be new. It is an
[authoring project](../plans/editor-project-format.md):

- `project.csfproj` and `local.csfproj`: the slot, the terrain asset and the
  house, tree and plant placements; the corpus path.
- `sources/world/`: the terrain as `terrain.blend` (open it in Blender) and
  its export `terrain.csfworld`.
- `build/`: the map, collision map and sector map, rebuilt by
  `csf-mod project-build <project>` when the terrain export or a placement
  changes.
- `mission/` and `texts/`: the csf-mod workspaces of the two archives.
- `dist/<build-id>/`: both archives, build logs, a copy of the recipe and
  `build.json` with revision, working-tree status, Blender version and archive
  hashes.
The corpus is read-only. Objective strings require the GlobalEK archive as
well as the mission archive.

Open the built project with:

```bash
./build/Release/rws-man --project ~/dev/csf-mods/hello-world-project
```

Deploy into the separate test install (each command prints a rollback state
path; retain **both**):

```bash
dist=~/dev/csf-mods/hello-world-project/dist/<build-id>
./build/Release/csf-mod deploy-pak ~/dev/csf-mods/hello-world-project/mission \
  $dist/hello-world.pak ~/dev/csf_game maps/Convoy.pak --apply
./build/Release/csf-mod deploy-pak ~/dev/csf-mods/hello-world-project/texts \
  $dist/hello-texts.pak ~/dev/csf_game GlobalEK.pak --apply
```

If the second deployment fails, roll back the first before playing. To undo
the pair, run `csf-mod rollback <deployment.state>` for the text deployment,
then the mission deployment. Use the paths printed for this build, not an
older version's backups.

## v12 playtest

Start Convoy afresh; a save from an older build is not a check of initialization.
Record the archive hashes from `build.json` with the result.

- Intro finishes, guards patrol and the dog walks.
- Alert the camp from the east: soldiers should move toward the cover points
  behind the truck/wood piles. Check the north guard near the scrap truck too.
- The radio rests on a wooden crate. Approach and aim at the radio: its
  highlight and sabotage prompt should appear, and E should complete the
  secondary objective once. Its prompt and highlight should then turn off.
- Complete the two primary objectives in both orders, on separate runs.
  Both must succeed before the mission ends. Radio sabotage remains optional.
- Check ground and building collision and movement around the camp props.

These checks require a person in the game. Compilation, package validation
and editor rendering do not establish that combat AI or interaction works.

## Changes from v11

Cover initialization now sets `MOVIL_A_PARAPETO` for alert and combat as well
as selecting the cover group, following the shipped Escape scripts. The radio
uses Escape class 484's telephone ghost behavior with Convoy class 211's radio
model through `MissionEditor::set_actor_look`. Interaction and illumination
are enabled explicitly and disabled after use. Escape class 383 supplies the
static crate; its bounding-box top and the radio's bottom determine placement.
This replaces v11's decorative radio and separate invisible class 241 target.
The user confirmed v12 works in-game. The v13 camera update adds slow travelling
movement within the five shots; the user confirmed v13 works in-game as well
(camera movement, aim and return to player control). The planned editor
workflow is in [the editor/Blender plan](../plans/editor-blender-authoring.md).
