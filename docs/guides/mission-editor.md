# Mission editor

`csf-editor` (the CSF Mission Editor in Mission mode) and `csf-mod` make and edit
*Commandos: Strike Force* missions without touching the unpacked resources or
the game installation. Edits are applied to canonical CSFFBS trees in memory,
saved into a project, and built into a complete replacement mission archive
(`maps/<Mission>.pak`) plus `GlobalEK.pak` for the mission's texts.

Every format rule below is proven against the shipped files (byte-for-byte
round trips, corpus-wide invariants). Hello world, built by the same recipes
(`examples/hello-world/`), plays in the game; a mission made entirely through the
GUI has not been playtested yet, and runtime behaviour that nobody has seen in
the game is marked as untested where it matters.

## A mission from start to finish

This is the journey the editor is built around
([the UX plan](../archive/editor/editor-ux-redesign.md), section 2). Each step names
where it happens; [GUI usage](gui.md) describes the panels themselves.

1. **Create.** **New project...** (Home, `File`, `Ctrl+N`) asks for a name and a
   slot (a shipped mission the new one replaces). It creates the project in the
   projects folder with a flat starter terrain of the slot's ground, the slot's
   mission emptied, and a free range of text IDs, then opens it. The project's
   format is [editor-project-format.md](../reference/project-format.md).
2. **Terrain and buildings.** **Edit in Blender** (the Map card in Properties,
   `Build`, or the status bar's Blender item) starts Blender with the CSF
   add-on and the project set, making `terrain.blend` from the starter terrain
   the first time. **Send** in Blender's CSF tab rebuilds the map here, even
   while the mission has unsaved edits; see
   [blender-authoring.md](blender-authoring.md). Height findings (something no
   longer standing where its height rule says) go to **Problems**, with
   **Resnap**.
3. **Dressing.** The **Assets** panel lists the classes of every discovered
   mission and the project's buildings, by category, with a search. Pick one
   and click the ground with the **Place** tool (`P`; `[`/`]` turn it, Shift
   keeps placing, random rotation is an option), drag it onto the ground, or
   double-click it to place it at the view centre. Placing (and moving with
   snapping) stands things on the surface under the pointer: the ground, a
   floor, or furniture such as a table top, also when the furniture is only
   visible geometry without collision of its own (up to 2 m above the floor
   under it). A class the mission lacks is
   imported first. A prop dropped on another record stands on it. Box select
   (Shift+drag), duplicate (`Ctrl+D`) and **Align to ground** work on the whole
   selection.
4. **Actors and behaviours.** Characters come from Assets too. Select a
   guard or an animal that has no behaviour yet and its **Behaviour** card in
   Properties offers **Guard a post** (it stands there playing idle
   animations) or **Patrol a route** (a two-point route from where it stands;
   an animal gets **Walk a route**). The recipe makes the actor again in its
   place, keeping its ID, name, class, position and heading, so objectives
   that target it still do; its other scripts and animation overrides are
   dropped. The behaviour's card then edits it: the idle animations are picked
   by name, each played once or a random number of times, and the play button
   beside each previews it on the guard in the viewport. Clips made for
   another weapon are left out of the list: `SF*` clips are rifle soldiers',
   `SM*` submachine-gun soldiers' (from the class's first weapon), `SP*` fit
   anyone. **Starts** says when the behaviour begins: when the mission starts
   (INIT), or when a mission event is raised (picked from the events the
   mission's scripts raise, or a new name), such as a vehicle's passengers
   taking their posts once it has arrived. The **Behaviours**
   tab makes new guards, animals, **cover groups** (points where guards take
   cover when alerted or in combat; a guard uses the one named on its card)
   and the experimental **walk grid** (a generated grid of linked navigation
   points; whether guards use it off their routes is not confirmed in the
   game). **Draw in viewport** takes a guard's post or a route's points from
   clicks there (the Route and Cover tools). Moving a guard or its route
   points regenerates its script.
5. **Players.** The **Mission** tab sets the starting player, the available
   commandos, a **starting kit** for each player (weapons picked by name, with
   their ammunition, the weapon in hand, a disguise), the mission **tips**,
   the score and the environment, and **Start over**.
6. **Zones and navigation.** The **Zone** tool (`B`) draws a zone's corners
   (polygon problems appear while drawing); the walk grid preset can be
   previewed in the viewport before it is made, and routes join it.
7. **Objectives and logic.** The **Objectives** tab holds the mission's one
   objectives list. **Add objective** appends one aimed at the first zone (or
   enemy); its card sets whether it is primary or secondary, the text the
   player reads, what completes it (the player reaches a zone, an actor is
   killed, the player uses an object; the target is picked from a list or
   with the eyedropper) and its message, and below them the success message.
   An objective completes itself: no trigger is needed for that. When every
   primary objective is complete the mission is won. **Triggers** below are
   everything else, without script text: *when* the player enters a zone, an
   actor dies, an object is used, the mission starts, some seconds pass, a
   guard is alerted (Convoy's camp alarm: any German turning alert or
   fighting, which a guard who is shot at does too) or a guard finds a body
   (a watched soldier sees another dead; there is no such event in the game,
   so the trigger checks the watched soldiers once a second),
   *if* an objective is (or is not) complete, *do* actions such as showing a
   message, sounding the alarm, changing a guard's alert behaviour or making
   an object usable. A trigger that completes an objective which completes
   itself is flagged: if it runs first, the objective's own script (which
   checks for the mission's success) never does. Events and actions not yet
   played in a mission made here appear only with **Offer unverified events
   and actions**. Script mode is there for everything else.
8. **Intro.** The **Intro cutscene** panel is a timeline of travelling shots.
   Frame a shot in the viewport and **Capture shot**: the first one creates the
   intro. Each shot is a strip; drag its right edge to change how long it
   lasts, drag on the ruler to move the playhead (the view follows the camera),
   and **Play** to watch the whole intro. A shot's camera, its end and its
   target are records in the viewport, drawn with the camera's path and sight
   lines while the panel is shown: **Select** one and move it with the gizmo,
   or take it **From view**. Playing approximates the game (its field of view
   differs). A mission can have more cutscenes: **Zone cutscene** makes one
   that plays once when the player enters a zone (Ransom's farm entrance),
   the list at the top of the panel switches between them (selecting a
   cutscene's record in the viewport does too), and **Plays** sets when it
   plays: at the start, or on entering a zone, armed from the start or when a
   mission event is raised (an officer reaching his post).
9. **Check.** **Problems** gathers the flow's findings (an event that starts
   scripts but that nothing raises, an objective nothing completes or more
   than one script completes, a text the project does not have), height
   findings, project checks, project lightmaps much brighter than the slot
   map's own (a night map lit like day), references to missing records and polygon
   problems. Every row links to its subject, and many have a fix. The Flow
   panel's **Graph** shows events, scripts and objectives with what connects
   them.
10. **Build and play.** The **Build** panel (`Ctrl+Shift+B` builds) saves and
    writes the mission archive and GlobalEK.pak (when the project has texts)
    into `dist/<build>/`, starting from the untouched shipped archives (found
    in the test install's first deployment backup, or chosen there).
    **Deploy** copies a build into the test install after asking, keeping what
    it replaces, and each deployment has **Roll back**. The **Playtest log**
    records whether a build worked, with a note, in the project.

Every step is undoable. Mission and project edits share one history
(**Ctrl+Z**, **Ctrl+Y** / **Ctrl+Shift+Z**; the **History** panel goes back to
any entry), destructive actions show a toast with **Undo**, and **Ctrl+S**
saves the mission and the project together. Opening another mission or
quitting with unsaved edits asks to save or discard them.

### Components

What the Behaviour card, the Behaviours, Objectives, Mission and Intro panels
create (patrols, guards at a post, animals, cover groups, walk grids,
objectives, equipment, tips, triggers, the intro cutscene) is a **component**: the operation lines that
made its records (`csf/mission_components.hpp`), kept in the workspace's
`components.csfops`. Selecting one of its records shows its card in
Properties (the Objectives, Mission and Intro panels show the ones without a
record to select). A card's **Advanced** section has its operation lines,
**Detach** and **Delete**. Changing a value there, or moving its records in the viewport,
makes its records again in place as one undo step. The Outliner nests a
patrol's route under its guard and the intro's helpers under one row. When a
component's records were edited by hand, its card says so and offers **Keep
my edits** (it becomes ordinary records) or **Regenerate**; it never
overwrites them on its own. A recipe's script is read-only in Script mode
until it is detached.

### Opening a shipped mission

`File > Open mission...` (`Ctrl+Shift+O`) opens any `.scn` of an unpacked
mission package (such as `CSF_unpacks/Ransom/Maps/FR01/Ransom.scn`). The same
tools edit it; **Ctrl+S** saves the edits into a mission project (by default
`<config dir>/projects/<Mission>`), writing only the files that differ from
the package, each with a `.changes.json` manifest of the operations applied.
**File > Export mission archive (.pak)...** (`Ctrl+E`) rebuilds the complete
archive from the shipped one and optionally installs it, backing up the
original. `csf-editor --project <folder>` reopens a project.

## What can be edited

| Target | Operations |
| --- | --- |
| Actors (`.BICHOS`) | Position, heading, pitch, name, class, model (keeping behaviour), faction, collision/flags/secondary explosion, per-actor scripts, animation overrides (slots named in English, each clip previewable on the actor); duplicate, add, delete |
| Dummies | Position, rotation, pitch; add, duplicate, delete |
| Lights | Position, color, radius, modulation; duplicate, delete |
| Navigation | Move points (with the actor standing on them), add points, link and unlink points within or across groups, delete points; add and delete groups |
| Areas | Move, insert and remove vertices; height; add and delete zones |
| Assets | Place any class of any discovered mission, or a project building: imported when missing |
| Behaviours | A behaviour for an existing guard or animal (Properties); guard on patrol, guard at a post, animal on patrol, cover group, walk grid: each a component |
| New mission | **Mission > Start a new mission in this slot** empties actors, navigation, zones, dummies, lights, effects and scripts, keeping the environment (a new project does this for you) |
| Objectives | One list: reach a zone, kill an actor, use an object (primary or secondary), and the success message |
| Triggers | When / If / Do logic as components; **Convert to script** detaches one |
| Flow | Events and the scripts they start, objectives, and findings such as a mission event nothing raises; the graph |
| Cutscene | The intro's travelling shots on a timeline, their cameras and targets in the viewport |
| Text | The authoring project's mission strings (GlobalEK) |
| Mission | Starting player, available commandos, starting kits, mission tips, maximum/minimum score, every scalar `.MUNDOVIS` environment field, start over |
| Scripts (`.gsc`, `.csc`) | Edit any script as text (highlighting, completion, go to definition), toggle trigger/enabled, add and delete scripts |
| Map props | Move and turn static props (CSF scene instances in the map `.rws`) |
| Imports | Copy a class or an animation from another unpacked mission |
| Any record | **All fields** edits every stored scalar, keeping its kind |

Deleting refuses while scripts, the starting player, effects, or links still
reference the record; **Shift+Delete** (or `--force`) deletes anyway and reports
the dangling references.

### Moving actors

In 2,859 of 2,988 shipped actors, `.CELDA` names a navigation point whose position
equals the actor's `.POS` and whose `.ROT` (radians) equals `.ANGULO` (degrees).
Moving an actor moves that placement point too, and moving the point moves the
actor. A point that does not mirror its actor is left alone (the editor warns).
Which of the two the engine reads is untested. Duplicated and added actors get
their own placement point in the same (or the nearest actor's) navigation group.

The move gizmo's center drags along the ground: with **Snap to ground** it drops
the record onto the collision surface under the pointer; **Alt** toggles snapping
during a drag. The arrows move along one axis. The rotate ring turns the heading;
**Ctrl** snaps to 15 degrees. **Esc** cancels a drag.

### Changing an actor's model

An actor's model comes from its class: `.CLASSID` names an `Objetos.bdd` record
whose `.MODELO` (with LOD, collision and physics fields) selects the files. The
class picker lists the classes of the mission's own `Objetos.bdd`; the editor
warns when the new class has a different `TIPO`, `COMPOR` or `HOMBRE` than the old
one, because scripts written for one family may not suit another.

Classes from other missions can be imported (**Assets > Import a class without
placing it**, or `--import-class`). Each mission's databases are a filtered subset of one
consistent global database, so the record is copied unchanged, together with the
class's weapons (`Armas.bdd`), its animation records, every referenced model,
collision and physics file the package lacks, the model's textures (found through
the donor's `.txl`), and new `.m3d`, `.and` and `.txl` entries. Imported records
are inserted in ID order, as shipped databases are sorted. The physics descriptor
(`.phd`) is not updated for imports.

The Inspector's **model** row gives an actor the look of another class while it
keeps its behaviour (`--actor-look`). The look is the body model (`.MODELO_TERCERA`
for player classes, whose `.MODELO` is the first-person hands, otherwise
`.MODELO`), `.LOD1_NOMBRE`, `.LOD1_DIST`, `.LOD2_DIST` and `.BBOX`. The actor
moves to a copy of its class with that look, named `<class> (look: <model>)`;
everything else (type, behaviour, weapons, collision, physics, animations) stays.
Copies take IDs from 500, above every shipped class ID (the highest is 499), so
they never collide with a class imported from another mission. An existing class
that already matches is reused, so changing the look back returns the actor to
its original class, and a copy that no actor or script uses any more is removed.
The mission's `.phd` gets an entry for the copy (below). The editor warns when the
new model comes from another folder (for example `Models\Deco` instead of
`Models\Char`): such a model probably lacks the skeleton that the actor's
animations and weapons attach to, and nobody knows yet what the game does with
it. Scripts that name the old class with `CLASSID` stop matching the actor, which
is also reported.

### Animation overrides

`.ANIMACIONES` on an actor is a list of `{.ID, .TIPO}` slot overrides, placed just
before `.CELDA`, using the same mechanism as class animation lists. `.TIPO` must
be a slot name; the editor offers the 378 names of the contiguous slot table in
`CommXPC.exe` (between `LANZAR_TERMINAR_CORTO` and `DESTRUCCION`), which contains
every slot the shipped data uses. `.ID` is an `Anims.bdd` record; import one from
another mission when the mission lacks it (**Assets > Import a class without
placing it**, `--import-anim`). Overrides are written sorted by animation ID and their IDs are
added to the mission program's `.RECURSOS/.ANIMACIONES` preload list, as in the
shipped missions. The Animation workspace can play the clip on the actor. Scripts
may still play other animations on top.

### Scripts

Script mode's editor shows a script in a recompilable text form (below): the
text is coloured, a syntax error's line is marked as you type, **Tab** completes
opcodes and operand tags (most used first), the current opcode's operands are
shown, and **Ctrl+click** on an operand goes to the actor, zone, marker, route
or script it names. **New script...** asks for the event it listens to.
**Apply** (**Ctrl+Enter**) replaces the script and reports, without refusing:

- opcodes, operand tags, argument counts and argument shapes that no shipped
  script uses (from a table of 557 corpus signatures plus the engine-known
  opcodes the corpus never uses, generated from all 30 distinct shipped programs
  by `tools/generate_script_signatures.py`);
- actors, dummies, areas, navigation points, animations and scripts named by ID
  that do not exist in the mission (`0` is the conventional "none");
- unbalanced `IF`/`ELSE`/`ENDIF`, `WHILE`/`WEND` and `FOREACH`/`ENDFOR` markers.

New `ANM_BDD` and `CLASSID` operands are added to `.RECURSOS`, where every shipped
program preloads them. An actor's `.SCRIPT` list may name only non-trigger
scripts (in the shipped data every actor script has `.TRIGGER 0`; `THIS` is the
actor). Script IDs are allocated after the highest ID of all mission programs.

### Map props

The map's `.rws` stream holds the static props as CSF scene-instance records
(`0x16FC0`) with an embedded 64-byte Matrix. Moving a prop rewrites only its 9
rotation and 3 position floats in place; the file keeps its size and every other
byte. The collision map (`*_col.rws`) is a baked BSP and does not follow, so a
moved prop keeps its old collision. Runtime effect is untested.

## Recompilable source text

`csf-mod decompile <file>` prints any CSFFBS document as text and `csf-mod compile`
turns it back into a document; an unedited round trip is byte-identical for all
1,262 shipped CSFFBS files (checked by `csf-mod audit`).

```text
[                                   # array (records)
  .ID 22                            # labelled integer
  .NOMBRE GE_DIST01                 # bare-word string
  .CARPETA ""                       # empty string (stored as zero bytes)
  .POS (15829.289 2.890015 12263.078) # group of reals: reals always have '.', 'e' or %bits
  .FLAGS [ .TRIGGER 0 .ENABLED 1 .VALIDO 1 ]
  .EVENTOS ( (START_GAME) )
  .ACCIONES {                       # one instruction per line; indentation is cosmetic
    PAUSE (RANDOM (NUMERO 0.0) (NUMERO 2.0))
    WHILE (BOOL TRUE)
      PLAY_ANMBDD (THIS) (ANM_BDD 1937)
    WEND
  }
]
```

- `( )` is a group (entry type 1) and `[ ]` an array (type 2), as in
  `csf-info --export-text`.
- A label is `.WORD` or `word:`; any other label is quoted, `"na me":`.
- Strings are bare words or quoted Windows-1252 text written as UTF-8 with
  `\\ \" \n \r \t \xHH` escapes; `"..."~` has no final NUL.
- `%7fc00001` is a real given by its raw bits (NaN and infinities).
- Inside `{ }` each line is an unlabelled group: the opcode and its operands.
  Parentheses may continue a line; `@ value` inserts a value as the instruction.
- `#` starts a comment. Errors report line and column.

## Command line

```bash
# Edit, then save into a project (created on first use; the mission root is found
# as the folder with BDD/, or given with --package).
./build-core/Release/csf-mod mission-edit /tmp/ransom-mod \
    ../CSF_unpacks/Ransom/Maps/FR01/Ransom.scn \
    --move-actor 67 9300 1078.38 11070.31 \
    --actor-class 67 142 \
    --actor-look 68 29 \
    --import-anim ../CSF_unpacks/Snipers 1797 \
    --actor-anim 67 DISTRAIDO_IDLE_ARMA1 1797

# Rebuild the complete mission archive from the shipped one.
./build-core/Release/csf-mod export-mission /tmp/ransom-mod \
    ../csf_game/maps/Ransom.pak /tmp/Ransom.pak

# Install it into a test copy of the game (dry run first; --apply writes and
# backs up the original; `csf-mod rollback` restores it).
./build-core/Release/csf-mod deploy-pak /tmp/ransom-mod /tmp/Ransom.pak \
    /games/csf-test maps/Ransom.pak
```

Other `mission-edit` operations: `--actor-look <id> <class-id>`, `--actor-name`, `--clear-actor-anims`,
`--actor-scripts <id> <a,b,...>`, `--player`, `--duplicate-actor`, `--add-actor`,
`--delete-actor`, `--move-dummy`, `--move-light`, `--move-instance <map-offset>`,
`--print-script`, `--set-script <id> <file>`, `--add-script <file>`,
`--delete-script`, `--import-class`, and `--force`. Navigation, trigger areas,
dummies and lights: `--add-nav-point <group> <x> <y> <z>`, `--move-nav-point`,
`--delete-nav-point`, `--link-nav`/`--unlink-nav <group> <point> <group> <point>`,
`--move-area-point`/`--insert-area-point <area> <index> <x> <y> <z>`,
`--remove-area-point`, `--area-height`, `--duplicate-dummy`, `--delete-dummy`
and `--delete-light`. A rejected operation saves nothing.

`csf-mod mission-flow <scene> [--workspace <dir>]` prints the flow and its
findings.

`csf-mod mission-ops <workspace> <scene> <ops-file>` runs a text file of
operations and presets, one per line (`new-mission`, `actor`, `prop`,
`nav-group`, `link`, `link-nearest`, `dummy`, `area`, `look`, `script`,
`guard-patrol`, `guard-idle`, `animal-patrol`, `cover-group`, `walk-grid`,
`objective`/`objectives`, `kit`/`equipment`, `tips`, `shot`/`intro`,
`trigger`; the full syntax is in `include/csf/mission_ops.hpp`).
`--ground <source.csfworld>` gives walk grids their heights, and
`--components` makes each recipe a component. `csf-mod mission-components
<workspace> <scene> list|check` lists a workspace's components and checks
that each regenerates identically. Hello world is built this way by
`examples/hello-world/ops.py`, and `examples/hello-world/parity.sh [--components]
<project>` checks that every mission file matches the `scene.py` build byte
for byte.

A project's lifecycle has its own commands, which the GUI's wizard and Build
panel call: `csf-mod project-new <dir> --slot <mission>` creates one,
`project-archives` builds both archives into `dist/`, `project-deploy` and
`project-rollback` install a build into a test copy of the game and undo it,
and `project-playtest` records a playtest.

## Mission archives

Each mission is one self-contained PAKC archive in the game's `maps` folder,
loaded by name (`Maps/%s.pak` in the executable). The executable names only
`Patch.pak`, `PatchMP.pak` and `Global.pak` besides mission archives, so a mod in
an arbitrary `Mods/*.pak` is not loaded; replacing the mission archive needs no
assumption about overlay precedence.

`pakman-cli create` cannot reproduce a shipped archive: it sorts entries, drops
the duplicate case-variant paths the shipped archives contain, and recompresses
everything (modern zlib output differs from the 2006 encoder). The export
instead uses pak-man's `rebuild_archive`: the original header, entry order, path
bytes, duplicates and timestamps are kept and unchanged compressed records are
copied verbatim; replaced entries are re-encoded as 4 KiB zlib blocks, matched
case-insensitively (all duplicates of a path are replaced); added files are
appended. With no replacements the output is byte-identical to the original
(checked on `C47_Cut.pak` and `Ransom.pak`). The result is fully verified before it
is published, and a `.package.json` beside it records its hash and the original's.

`csf-editor` links pak-man's `pakman_core` when the sibling `../pak-man` checkout is
present (`RWSMAN_PAKMAN_DIR` selects another); otherwise it runs `pakman-cli` from
`PATH`.

## Mod projects

A mission project is a `csf-mod` project (see
[guarded-authoring-and-mods.md](guarded-authoring-and-mods.md)) plus a local
`.csf-mission` file naming the scene and the shipped archive. Its `authored/`
folder holds each changed file at its package path with a `.changes.json`
manifest; files that are edited back to their original content leave the
project on the next save. A file added from elsewhere with `csf-mod add` (for
example an authoring project's `build/` map) stays where it is: saving copies it
into `authored/` only when an edit in the editor changed it. `csf-mod validate`,
`conflicts`, `build` and `package` work on it as on any project.

An [authoring project](../reference/project-format.md) keeps its mission
workspace in `mission/`; open the project folder itself (**File > Open mission
project**, or `csf-editor --project <folder>`). While it is open, csf-editor checks
the Blender exports every second: when one changes, it rebuilds the map in the
background and reloads the mission (after you save, if the mission has unsaved
edits). Height findings appear in **Problems** (and the **Height report**)
when something no longer stands where its height rule says; **Resnap all**
stores the resolved heights (actor moves are ordinary undoable edits; save to
keep them) and rebuilds the map. The same checks from the command line: After a terrain change, `csf-mod project-heights
<project>` lists the placements and anchored actors that no longer stand where
their height rule says, and `--resnap` moves them (placements in the project,
actors in the mission); `csf-mod project-build <project>` then rebuilds the map
and updates the workspace's records of it.

## Format findings used by the editor

- `raw_next_entry` is the index of the element's next sibling (0 for the last),
  stored on the element's first entry (the identifier entry of a labelled
  scalar). It holds for all 1,867,239 container elements of the 1,262 files.
- Identifier and string tables hold unique values in first-use order; the empty
  string is stored as zero bytes (349 occurrences, never as a lone NUL).
- The canonical writer (`csf::Tree`) reproduces every shipped file exactly, so
  structural edits (adding or removing records) need no offset patching.
- SCN record lists are sorted by `.ID`; actors, dummies, lights, areas, effects
  and navigation groups have independent ID spaces; new records take the highest
  ID of their list plus one. GSC `.SCRIPTS` are not sorted; BDD `.LISTADATOS` are
  (56 of 60 files).
- `.m3d` lists `u32 length, path, FF FF FF FF` records; `.and` lists
  `u32 length, path, two flag bytes` (meaning unknown: imports copy the donor's);
  both end with a zero length. `.txl` is CRLF text.
- `.phd` is `FD FC FC FC`, u32 version 1, u32 count, then per entry: u32 type,
  u32-length physics file (`.PHYSIC .MODEL_FILE`), u32-length model (`.MODELO`),
  the six `.BBOX` floats and `.PHYSIC .MASS`, `.BOUNCE`, `.SLIDE`. All 12 shipped
  files parse exactly. There is one entry per distinct combination among the
  classes and weapons a mission places (not every class has one), and the same
  model may appear more than once with different physics. The type code does not
  follow uniquely from `.PHYSIC .TIPO` (`FILE` is mostly 3, `BOX` 2, `NULL` 0, 2 or
  3), so a look copy's entry copies the type from the original class's entry.

## Not yet supported

- New map geometry and collision are made in Blender and built by an
  [authoring project](../reference/project-format.md) (see
  [blender-authoring.md](blender-authoring.md)), not in this editor.
- Updating `.phd` physics descriptors for imported classes, and editing particles (`.sp`), UI
  (`.fbs`), audio (`.wad`) and localization.
- Human animation sets beyond per-actor slot overrides (their naming convention
  is not recovered).
- Semantics of individual script opcodes: the signature table says what the
  shipped scripts do, not what the engine accepts.
