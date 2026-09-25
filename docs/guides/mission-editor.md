# Mission editor

`rws-man` and `csf-mod` edit *Commandos: Strike Force* missions without touching
the unpacked resources or the game installation. Edits are applied to canonical
CSFFBS trees in memory, saved into a mod project, and exported as a complete
replacement mission archive (`maps/<Mission>.pak`).

Nothing here has been tested in the game yet. Every format rule below is proven
against the shipped files (byte-for-byte round trips, corpus-wide invariants);
the runtime behaviour of edited data is marked as untested where it matters.

## Workflow

1. Open a mission (`File > Open mission...`, a `.scn` file in an unpacked mission
   package such as `CSF_unpacks/Ransom/Maps/FR01/Ransom.scn`).
2. Select a record in the viewport or Explorer. The Inspector's **Edit** section
   edits it; the move (**G**) and rotate (**R**) tools drag it in the viewport.
3. **Ctrl+S** saves the edits into a mission project (by default
   `<config dir>/projects/<Mission>`). Only files that differ from the package are
   written, each with a `.changes.json` manifest of the operations applied.
4. **File > Export mission archive (.pak)...** (**Ctrl+E**) rebuilds the complete
   mission archive from the shipped one and optionally installs it into the game,
   backing up the original.
5. **File > Open mission project...** (or `rws-man --project <folder>`) reopens a
   project later; its authored files replace the package copies.

Every operation is one undo step (**Ctrl+Z**, **Ctrl+Y** / **Ctrl+Shift+Z**). The
**Changes** panel lists the history (click an entry to go back to it), the files
that differ from the package, mission properties, adding actors, and importing
from other missions. Opening another mission or quitting with unsaved edits asks
to save or discard them.

## What can be edited

| Target | Operations |
| --- | --- |
| Actors (`.BICHOS`) | Position, heading, pitch, name, class, model (keeping behaviour), faction, collision/flags/secondary explosion, per-actor scripts, animation overrides; duplicate, add, delete |
| Dummies | Position, rotation, pitch; add, duplicate, delete |
| Lights | Position, color, radius, modulation; duplicate, delete |
| Navigation | Move points (with the actor standing on them), add points, link and unlink points within or across groups, delete points; add and delete groups |
| Areas | Move, insert and remove vertices; height; add and delete zones |
| Assets | Place any class of any discovered mission (**Changes > Assets**): imported when missing, then placed at the view centre |
| Presets | Guard on patrol, guard idling, animal on patrol, cover group, walk grid (**Changes > Presets**): each writes the actor, its groups and its script as one undo step |
| New mission | **Mission > Start a new mission in this slot** empties actors, navigation, zones, dummies, lights, effects and scripts, keeping the environment |
| Objectives | **Changes > Objectives**: reach a zone, kill an actor, use an object (primary or secondary), the success check, starting equipment and mission tips |
| Flow | **Changes > Flow**: events and the scripts they start, objectives, and findings such as a mission event nothing raises |
| Cutscene | **Changes > Cutscene**: capture travelling shots from the viewport, preview them, and create an intro (camera helpers, paths, dummies and both programs' scripts) |
| Text | **Changes > Texts**: the authoring project's mission strings (GlobalEK) |
| Mission | Starting player, available commandos, maximum/minimum score, every scalar `.MUNDOVIS` environment field |
| Scripts (`.gsc`, `.csc`) | Edit any script as text, toggle trigger/enabled, add and delete scripts |
| Map props | Move and turn static props (CSF scene instances in the map `.rws`) |
| Imports | Copy a class or an animation from another unpacked mission |
| Any record | **All fields** in the Edit section edits every stored scalar, keeping its kind |

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

Classes from other missions can be imported (**Changes > Import**, or
`--import-class`). Each mission's databases are a filtered subset of one
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
another mission when the mission lacks it (**Changes > Import**,
`--import-anim`). Overrides are written sorted by animation ID and their IDs are
added to the mission program's `.RECURSOS/.ANIMACIONES` preload list, as in the
shipped missions. The Animation workspace can play the clip on the actor. Scripts
may still play other animations on top.

### Scripts

The Script workspace's **Edit script source** section shows a script in a
recompilable text form (below). **Apply** (**Ctrl+Enter**) replaces the script and
reports, without refusing:

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
`objective`/`objectives`, `kit`/`equipment`, `tips`, `shot`/`intro`; the
full syntax is in `include/csf/mission_ops.hpp`). `--ground <source.csfworld>`
gives walk grids their heights. Hello world is built this way by
`tools/hello_world/ops.py`, and `tools/hello_world/parity.sh <project>` checks
that every mission file matches the `scene.py` build byte for byte.

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

`rws-man` links pak-man's `pakman_core` when the sibling `../pak-man` checkout is
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

An [authoring project](../plans/editor-project-format.md) keeps its mission
workspace in `mission/`; open the project folder itself (**File > Open mission
project**, or `rws-man --project <folder>`). While it is open, rws-man checks
the Blender exports every second: when one changes, it rebuilds the map in the
background and reloads the mission (after you save, if the mission has unsaved
edits). The **Height report** (**Mission > Authoring project**) opens by itself
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
  [authoring project](../plans/editor-project-format.md) (see
  [blender-authoring.md](blender-authoring.md)), not in this editor.
- Updating `.phd` physics descriptors for imported classes, and editing particles (`.sp`), UI
  (`.fbs`), audio (`.wad`) and localization.
- Human animation sets beyond per-actor slot overrides (their naming convention
  is not recovered).
- Semantics of individual script opcodes: the signature table says what the
  shipped scripts do, not what the engine accepts.
