# Country: a mission built through the CLI, and what an asset registry needs

2026-09-27. "Country" takes Convoy's slot: a small French farming village at
night with a Gestapo officer to kill, a radio and a telephone to sabotage,
35 soldiers, a tank workshop and a farm. It was built only through the
command-line tools (`csf-mod project-*`, `mission-ops`, Blender headless),
pulling buildings, props, actors, animations, scripts and textures from
other missions and importing two free models, to find out what it takes to
assemble a mission "like LEGO" and what the editor should offer.

Project: `~/.config/csf-rws-tools/projects/Country` (so rws-man lists it);
the first version is kept beside it as `Country-v1` (its deployment and
rollback records stay with it). Recipe: `tools/country/build.sh PROJECT
ORIGINAL_CONVOY_PAK ORIGINAL_GLOBALEK_PAK [CORPUS] [DOWNLOADS]` creates it
from nothing in about 20 seconds: `layout.py` (the map), `terrain.py` and
`assets.py` (Blender, headless), `lightmaps.py`, `textures.py`, `project.py`
(project.csfproj), `ops.py` (the mission as mission operations, kept as
components). Text IDs are offsets into whatever range `project-new`
reserves. **State:** v2 built (`dist/20260927-145525`), validated, 0 problems
in rws-man; not yet played. v1 was played once (below).

## The mission (v2)

A 150 m square (v1: 200 m), wooded hills round it. The players start on the
south road, pass a checkpoint and reach the village square. Objectives: kill
the Gestapo officer (primary), sabotage the radio on the table in the
half-timbered house (primary), cut the telephone line on the table in the
ruined farm by the silo (secondary); the mission succeeds when both
primaries are done.

About 30 s after the intro, a Kubelwagen (the officer, his driver and an
escort) and a lorry with four soldiers, headlights on, come up the east road,
shown in a picture-in-picture view (Convoy's `VIEWPORT_*` pattern), first on
the road and then in the square. They get out and take their posts; once the
officer stands in the square, reaching the village plays his cutscene, once.
The alarm sounds (60 s, once, with a message) when a living German sees a
dead one.

| Part | From |
|---|---|
| Terrain: rolling ground, knolls with rock outcrops, a dry ditch; grass, dirt roads, rock on slopes (Convoy textures); a ploughed and a stubble field | Blender (`terrain.py`); the field textures are FR01's (`texture` records) |
| Village in FR01's own arrangement: farmhouse with cow stable, ruin with silo, house, half-timbered house (radio), yard walls, well; a pump and six street lamps | Ransom's map FR01, pieces by lightmap group (buildings, `ATREZZO_EXTERIOR_1`, `FAROLAS`) with their baked lightmaps |
| Farm: gambrel barn, plank cow shed, water tower, hand cart, round hay bales; picket fence round the paddock | FR01 pieces; the fence is a CC0 download ("Basic Wooden Fence", WeaponGuy) |
| Tank workshop: FR01's open shed, a Panzer III, crates, fuel drums, a field kitchen, a fire barrel, a sandbag nest, Convoy's log pile | the tank is a CC0 download ("panzerkampfwagen III", konserwa, 7k triangles) textured with Panzers' track texture and a generated hull plate; props are shipped classes |
| Checkpoint: barrier, curved and straight sandbag walls, MG mount, barbed wire, six Czech hedgehogs | shipped classes 201, 199, 337, 200, 278 (Gestapo, Ransom); hedgehogs are FR01 World objects |
| 101 trees (five species) and 103 bushes; four fallen logs | Convoy's own prop instances; Convoy's World logs (`piece` without `donor=`) |
| Officer's party: officer (Escape 20), two Gestapo (19), lorry driver and three soldiers; Kubel 95, lorry 42 | hand-written scripts (`ops.py`): `LLEGADA_INI`, `LLEGADA_OFICIAL`, one per passenger |
| Inside: radio operator at the radio (`SPRadio01/02`), a man reading the map at the table's other end (`SPMapaMesa`), a torch upstairs (`SPLinternaIdle`); a telephonist (`SPTfnoHablar`); a crate searcher and a man with a clipboard in the farmhouse store; a dozing and a sweeping man in the house | free floor spots from an occupancy grid of each floor; table tops from up-facing furniture surfaces |
| Outside: well talkers, a man warming at the fire barrel, a smoker at the radio house door, lookout with binoculars, MG nest guard, barrier smoker, two mechanics at the Panzer (`SPMecanico`, `SPClavaClavoAg`), a foreman, a pissing guard, a dozing sentry, a drunk; seven patrols and a doberman | guard-idle and guard-patrol recipes; clips from Gestapo, Assault, Ransom |
| Six cows (four in the paddock, two in the farmhouse stable), a horse, a tractor | Ransom 239 and its pulled `INIT_VACAS`, Bridge 30, Ransom 290 |
| Alarm when a body is seen | `CADAVER_ENCONTRADO` (`ops.py`, below) |
| Intro (checkpoint, workshop, radio house) and the officer's zone cutscene | intro recipe; `zone=` with the new `arm=` |

254 project placements (35 pieces, 15 buildings, 204 trees and bushes), 89
actors, 36 components, 108k visual / 79k collision triangles.

### To play it

```bash
# First take v1 out of the test install (it is deployed there), then deploy v2:
./build/Release/csf-mod project-rollback ~/.config/csf-rws-tools/projects/Country-v1 20260927-122744
./build/Release/csf-mod project-deploy ~/.config/csf-rws-tools/projects/Country 20260927-145525 ~/dev/csf_game
# and afterwards
./build/Release/csf-mod project-rollback ~/.config/csf-rws-tools/projects/Country 20260927-145525
```

What to check in game, in order of risk:

1. The officer's arrival: the vehicles appear and drive (they sit on the
   route's nav group, driven with `IR_A_PATHPOINT` like Convoy's lorry), the
   passengers ride hidden-then-visible and get out (`BAJAR_DE_HABITACULO`),
   the picture-in-picture shows the road, then the square.
2. The alarm: kill a guard unseen and hide him, no alarm; leave one where a
   guard walks past, the alarm. `VEO_BICHO` on a dead actor is untested.
3. The zone cutscene plays once, only after the officer has arrived, and
   not again when leaving and re-entering the zone.
4. Night: ground and buildings as dark as the actors (v1's ground was 2.5x
   too bright).
5. No ground through the building floors.
6. The Panzer III's textures (`PZ3_HULL`, `PZ3_TRACK`) and collision; the
   imported props (nests, barrier, field kitchen, fire barrels, MG mount).
7. The radio and telephone use points on their tables; the upstairs torch
   guard stands on the first floor.

## v1 playtest and what changed

| v1 in game | Cause | v2 |
|---|---|---|
| Few features: flat, 20 trees | | knolls, rock outcrops, a ditch, 101 trees in woods and groves, 103 bushes, logs, lamps, pump, hedgehogs |
| Few props, nobody inside | | 27 shipped props, 9 soldiers inside four buildings |
| Radio and telephone on crates | | on tables of the World furniture (the radio house's long table, the ruin's desk); a map on the radio table |
| Map too spread, too few soldiers | | 150 m instead of 200 m; 35 soldiers instead of 17 |
| The imported sandbag was one oversized sandbag | | dropped; shipped sandbag nests 199/337; a downloaded Panzer III with two mechanics |
| (wish) the officer arrives by car with a PiP camera | | done, 45 s after INIT |
| The zone cutscene played again on every re-entry | `ACT_BICHO_EVENT_ZONA (PLAYER) zone (BOOL FALSE)` arms a *leave* event instead of disarming the entry (KB-scn-10); the recipe used it for zone cutscenes, zone objectives and zone triggers | the recipes use `DEACT_BICHO_EVENT_ZONA ... (BOOL TRUE)` (Ransom's `CUT_ENTRADA`) and turn the script off; hello world's zone objective had the same bug (fixed there too, parity kept) |
| The alarm rang when a guard died | Convoy's `SET_ALARMA` fires on any German turning ALERTA or COMBATIENDO, which a guard under fire does | a watcher: each second, if a German is dead and a living one sees him (`VEO_BICHO`), the alarm; there is no body-found event in the script language |
| Not night, but dark actors | the placeholder terrain lightmap averaged RGB 57/70/87; Convoy's own ground averages 25/25/24 and Ransom's 18/23/26 (the game doubles them), while actors take the scene's night ambient | lightmaps at the shipped maps' level, with tree shadows |
| Ground on some floors | pieces were sunk by the donor's ground height, but Ransom's floors stand level with (the ruin 4 cm below) its ground: our flat pad met or crossed them | a building's lowest floor now stands 4 cm above our ground (`building(..., floor, ground)` in layout.py); the ruin, the open shed and the cow shed had the worst of it |

## What was added to the tools for it

v2: the intro recipe's zone cutscenes take `arm=<event>` (the setup script
waits for that mission event instead of START_GAME), and zone cutscenes,
zone objectives and zone triggers disarm their zone with
`DEACT_BICHO_EVENT_ZONA` and turn themselves off. `tools/country/catalog.py
objects` splits a map's lightmap groups into single objects (a lamp, a rock,
a hedgehog, a table) with their boxes.

v1:

| Change | Where | Why |
|---|---|---|
| `donor <key> <mission> <visual> <collision>` project record; `piece <id> donor=<key> lightmaps=<groups> ...` | `csf::AuthoringProject`, `.csfworld` `piece` options, `rws::WorldDonor` | Buildings are World triangles, not props, so taking Ransom's farmhouse means cutting it out of *Ransom's* map. The piece keeps its own materials; its textures and baked lightmaps are copied into the map's texture folder (renamed `<KEY>_<name>` when the slot's map has a different file of that name) and listed in the `.txl`. |
| Lightmap-group filter on pieces | `compile_world_source` | A building is its lightmap group (`EDIFICIO_5` = `EDIFICIO_5_Lm`, plus `EDIFICIO_5_INTERIOR` and `ATREZZO_EDIFICIO_5` for its inside and furniture). Filtering by group leaves the ground, fences and trees around it behind; collision is kept where it lies on the kept visual triangles (30 cm). |
| `texture <name> <source.png\|dds> <like>` project record | `AuthoringProject::build_lightmaps`, `WorldCompileOptions::new_textures` | Imported models bring their own textures. The DDS is built like a lightmap; World materials naming it copy the donor material of `like` with the texture renamed. Also pulls another map's texture (FR01's ploughed field) into the terrain. |
| `intro ... zone=<area> [setup=]` | `csf::IntroCutscene::zone` | A cutscene triggered when the player enters a zone (Ransom's `CUT_ENTRADA` pattern) instead of at the start. |
| `project-lightmaps` / the GUI's registration list copied donor textures and own textures too | `AuthoringProject::packaged_textures` | They are packaged exactly like lightmaps. |
| `mission-components ... --ground` | `csf-mod` | A walk-grid component could not be checked or regenerated from the CLI without the ground. |

Bugs found on the way and fixed:

- `import-anim` copied an animation's hand-held model (`.MODEL3D_ITEM`:
  binoculars, a jug) but did not list it in the model index or its
  textures in the `.txl`, as `import-class` does.
- The intro recipe took its cutscene-program script IDs from a second
  counter, but script IDs are one space across both programs: the cutscene
  scripts collided with the mission script and were renumbered. The first
  intro survived because every script shifted by one; a second cutscene
  broke (caught by `mission-flow`: "runs cutscene 9525, which the cutscene
  program does not have"). IDs now come from one counter; components pin a
  zone cutscene's setup script too. Hello world stays byte-identical.
- Actor names are unique per mission: a second cutscene's camera actors are
  named after it (`CUT_OFICIAL_CAMERA_1`).

## Findings for an asset registry

### Buildings are lightmap groups of a map's World

Every shipped map bakes its buildings into the visual World. Each building,
interior and furniture set has its own lightmap, so the lightmap name is the
building's identity and a free semantic label: `EDIFICIO_n`, `EDIFICIO_n_INTERIOR`,
`ATREZZO_EDIFICIO_n` (furniture), `ATREZZO_EXTERIOR_n` (yard props: well,
carts, hay bales), `VALLAS` (fences), `MURO` (walls), `SUELO` (ground),
`ARBOL_*`/`ARBOLES_ALPHA` (trees). Per-group bounds come straight out of the
glTF export (`rws-info --export-scene-gltf`, material extras
`rws_lightmap_texture`); connected components inside a group split the yard
props into single objects. A registry entry for a building should record: map,
groups, bounds, floor height (most common low y) and the ground height around
it (to sink it correctly), interior floors (up-facing triangles), door actors,
and a rendered thumbnail. `tools/country/fr01_catalog.py` does this for FR01.

FR01 (Ransom, Resist and Parachut share it) is a farm village: `EDIFICIO_1`
farmhouse and cow stable (Ransom's cows stand inside), `EDIFICIO_2` ruin with a
silo, `EDIFICIO_3` gambrel-roofed barn, `EDIFICIO_4`/`5` houses with two-storey
interiors, `EDIFICIO_6` house with a tower (on a slope: its walls need the
slope), `EDIFICIO_7` plank shed, `EDIFICIO_8` water tower and hut,
`EDIFICIO_9` open shed; `ATREZZO_EXTERIOR_1` a well with a hand cart, a wood
shelter with firewood, a hay cart; `ATREZZO_EXTERIOR_2` ~40 round hay bales.
Fences (`VALLAS`) follow FR01's hills and do not fit another terrain.

Doors are separate actors (class 143 `POBJ_PUERTA`, no model, a `.DOOR_BOX`)
placed in the doorways. The doorways are open in the collision World, so a
copied building is enterable without them, but `mission-ops` has no way to
write `.DOOR_BOX` yet.

### Textures

Maps share most base textures by name (FR01 and FR03 have 39 identical
files), but lightmap names collide (`EDIFICIO_1_Lm` exists in both, with
different content) and so do a few base textures (`FDET_05A`, `FFLRA11B`).
A registry needs content hashes per texture, not names.

### Animations: the prefix is the weapon stance

Across all missions' actors and the animations their own scripts play:
`SF*` animations are played only by rifle classes ("... Rifle"), `SM*` by
SMG classes, `SP*` by any class (weapon away: smoking, radio, maps, drunk,
cards). A behaviour preset must pick the variant matching the class, or
filter the picker by it. Animation IDs are global (the same ID is the same
clip in every mission's `Anims.bdd`), so a registry can key them by ID and list
the missions that carry each one (`tools/country/catalog.py anims`; `classes`
and `scripts` list the other two tables: 1175 classes, 1217 clips and 5946
scripts across the corpus).

Behaviour idles found (ID, name): radio operator 1949 `SPRadio01` (loop),
1950/2254; smoking 1881/1385; pissing 1402 `SPMeando`; dozing 1425
`SPDormitando`, yawning 1421; map on a table 1429, map in hand 1941; binocular
lookout 1374 `SPPrismMirarIdle`; talking in pairs 2316/2317/2318
`S?HablaPieCiclo`, 2046/2047 `SFTalk`; drunk 1763 `SPBarBorracho`; cards 1833;
sneezing 2089; cows 2034/2030; dog 2038-2042 (FR02, Snipers) or the doberman
2383/2384 (Ransom).

### Scripts worth pulling

- Convoy `SET_ALARMA` (script 25): any German whose AI state turns ALERTA or
  COMBATIENDO raises a far acoustic threat and `ACTIVAR_ALARMA 60`, once.
  v1 pulled it; in game it rang when a guard was shot (he turns COMBATIENDO
  before dying), so v2 replaces it with a body watcher (below).
- Ransom `INIT_VACAS` (93): cows idling and grazing at random.
- Ransom `CUT_ENTRADA` (141) / Resist `SIT0n_CTRL_CUTSCENE`: cutscenes started
  by the player entering a zone (fade out, third person, reposition actors,
  `CUTSCENE_EXE`, fade in). `CUT_VACAS` (43) plays one when a ghost is used.

### Patterns written by hand in v2 (candidates for recipes)

- **A vehicle arriving with passengers** (Convoy's lorry, Assault's escape
  car): the vehicle is an actor standing on a point of a chained nav group;
  at START_GAME `LINK_A_HABITACULO <passenger> <vehicle> <seat>` (seat 0 is
  the driver, and the vehicle only drives with one) and `SET_INVISIBLE`;
  later `SET_INVISIBLE FALSE`, `CONTINUE (IR_A_PATHPOINT <vehicle> <point>)`,
  `WHILE (ESTA_YENDO_PUNTO <vehicle>)`, then each passenger's own script
  (TRIGGER 0, on a mission event) `BAJAR_DE_HABITACULO (THIS)` and walks to
  his post (`IR_A_PATHPOINT_ORIENT`). The lorry 42's headlights are effects
  on its model dummies 220-222 (`EFFECTO_DUMMY_BICHO`, Convoy); its engine
  sound is Convoy's 255.
- **Picture in picture**: `VIEWPORT_CREATE (NUMERO 1.0) (VECTOR 0.7 0.25 0.0)
  (VECTOR 0.925 0.475 0.0)` at START_GAME (hidden), then
  `VIEWPORT_CAM_SETPOS (NUMERO 1.0) (DUMMY camera) (DUMMY target)`,
  `VIEWPORT_SETICON`, `VIEWPORT_SHOW`. Convoy's dummies: the first is the
  camera, the second the point it looks at, both with the view's yaw and
  pitch. 16 missions use it.
- **A body found**: the language has no such event (scripts see
  `IA_CHANGE_STATE`, `MUERTO`, `MORIBUNDO`). `CADAVER_ENCONTRADO` loops once
  a second over the Germans: for each dead one (`NOT (ESTA_VIVO ...)`) whether
  a living one `VEO_BICHO`s him; 35 soldiers make 1190 checks and a 560 KB
  program (shipped ones reach 950 KB); only the dead cost anything at run
  time. A registry "alarm when a body is found" preset would generate it from
  the mission's Germans.
- **Zone cutscene after an event**: `intro ... zone=6 arm=OFICIAL_EN_PLAZA`,
  raised by the officer's script when he reaches his post.

### Lighting

Shipped night maps' lightmaps are dark: Convoy's ground averages RGB
25/25/24, Ransom's 18/23/26, their buildings 13-40, interiors 8-28 (the
game doubles them; `MODULATE2X 1`). Actors are lit by the scene's
`AMBIENTE`/`AMBESCENA`, not by lightmaps, so a brighter placeholder
lightmap reads as day with dark figures on it. A project's lightmaps should
be checked against the slot's own (mean and range) and the scene's ambient.

### Floors and ground

Ransom's (FR01) building floors stand level with its ground (farmhouse 1080
on 1080) or below it (the ruin's 1037-1045 on 1065-1080). Sunk by the donor
ground, a copied building's floor meets our flat pad and the terrain shows
through. A piece needs its lowest floor as well as its ground height, and
the pad under it must be lower than the floor (v2: 4 cm).

### What a registry entry needs (from building Country)

- **Identity.** Class and animation IDs are global: across the 17 missions'
  databases no ID names two different classes or clips, so they can key a
  registry directly (a mission only carries a subset). Map textures and
  lightmaps are not: the same file name has different content per map, so
  those need the map plus a content hash.
- **Placement facts.** For buildings: floor height, ground height around it,
  interior floors, free floor space (Country's radio first landed inside a
  table; an occupancy map of the furniture found a clear room), door actors.
  For actors: whether the class's stance fits an animation.
- **Dependencies.** A class brings models, textures, weapons and physics; an
  animation brings its `.anm` and hand-held model; a building brings textures
  and lightmaps; a script brings the actors, zones, animations and events it
  names. `import-class`/`import-anim` resolve the first two; pulled scripts
  still need their IDs remapped by hand (Country edits them in `ops.py`).
- **Behaviour presets = recipe + animations + props.** "Radio operator" is a
  guard-idle with 1949/1950, a radio on a crate and a use point; "grazing
  cows" is class 239 with its script; "binocular lookout" is 1374 on any
  class. These are the LEGO bricks: a registry entry per preset that names
  its parts would let the editor offer them by name.
- **Semantic names are mostly there.** Spanish lightmap and class names
  (`EDIFICIO_RADIO`, `GRANJA`, `Vaca`, `SPMeando`) are descriptive; a
  registry mainly needs English labels, tags (farm, military, interior) and
  thumbnails on top.

## Editor changes from this feedback

Built 2026-09-27 from the wishlist and the v2 notes below (all tested; not
yet used in game):

| Wish | What the editor does now | Where |
|---|---|---|
| Behaviour after an event | a behaviour's **Starts** row: the mission start (INIT) or a mission event, picked from the ones the scripts raise or typed; `start=<event>` on `guard-patrol`, `guard-idle`, `animal-patrol` | `csf::GuardIdle::start_event` etc., `app/ui/component_card.cpp` |
| "A guard is alerted" When | trigger `when=alerted`: Convoy's `SET_ALARMA` exactly (`IA_CHANGE_STATE`, German, ALERTA or COMBATIENDO), a proven pattern; the help says a guard shot at counts too | `csf::trigger_script_texts` |
| "Body found" alarm preset | trigger `when=body-found watch=<actors>`: the v2 watcher (two local variables, `VEO_BICHO` once a second, the alarm heard at the finder); every enemy is watched by default and the card flags enemies added later; unverified until played | `csf::ScriptVariable`, `script_text(..., variables)` |
| Zone cutscenes on the timeline | the timeline edits any cutscene: a list at the top, **Zone cutscene** makes one from the view, **Plays** sets start or a zone (picked or with the eyedropper) and the arming event; selecting a cutscene's record switches to it | `app/ui/timeline.cpp`, `cutscene_components`, `set_cutscene_when`, `add_zone_cutscene` |
| Place on furniture | the Place tool, drops and snapped moves stand on the nearest level visible World surface in front of the collision surface and at most 2 m above it, so furniture without collision works too (0.3 ms per query on Country) | `GeometryPreview::placement_point` |
| Go to for project placements; the piece's donor and groups | Go to finds placements (`placement:` or `building:` prefix; lightmap groups are searchable); Properties shows **Cut from** (the donor mission's map), **Groups** and **Box**; the Outliner says "piece of fr01: EDIFICIO_5 +2" | `SymbolKind::placement`, `app/ui/properties.cpp` |
| Lightmap check | Problems (source "Lighting") and `project-build` warn about a lightmap brighter than both the slot map's brightest and 1.5 times its median one: Country v1's 71 against Convoy's 30 (brightest 40) is flagged, v2 is not | `AuthoringProject::lightmap_brightness` |
| Animations by weapon stance | idle and override pickers leave out clips made for another weapon (`SF*` rifle, `SM*` submachine gun, from the class's first `.ARMAS` weapon: Mauser, Mp40, Luger); the tooltip says how many | `rwsman::weapon_stance`, `animation_combo(..., soldier_class)` |

Still open from the lists below: the building library and object browser
(with thumbnails), texture import, pulling a shipped script with its IDs
remapped, door actors (`.DOOR_BOX`), the occupancy overlay, floor-aware
pieces in the height report, the scatter tool, the vehicle arrival recipe,
animal idle presets and recipe text IDs as range offsets. The Timeline
panel is still titled "Intro cutscene".

## GUI parity

Opening the finished project in rws-man (UI scripts, screenshots): it shows
0 problems; the Outliner lists every FR01 piece as a "donor piece" and the
imported models as project buildings with their counts; actors made by
recipes are editable components (the officer's idle animations are picked by
name, his cover group by name); the three objectives appear with their text;
the cows show their pulled script. A GUI user could make the same mission
except for:

- placing a piece of *another* map (no building browser; Properties now
  shows the piece's donor map and groups);
- adding a `texture` record (import a model's texture);
- pulling a shipped script (Script mode can add scripts, not browse other
  missions' programs);
- a behaviour for animals (the cows show "Runs its own scripts").

(Zone cutscenes and Go to for placements such as `radio-house` were on this
list; see "Editor changes from this feedback".)

## Editor wishlist (show these in rws-man)

- **Building library**: browse other maps' buildings by lightmap group with
  thumbnails, drop one onto the terrain (a `piece` with `donor=` and
  `lightmaps=`), sunk by its recorded ground height; its door actors come
  with it.
- **Yard props from Worlds**: the same for `ATREZZO_EXTERIOR_*` components
  (well, carts, hay bales).
- **Texture import**: a model's PNG becomes a `texture` record, with the
  donor material to imitate picked by name.
- **Behaviour presets from the catalogue**: radio operator, smoker, pissing
  guard, dozing sentry, map reader, binocular lookout, talking pair, drunk,
  card players, grazing cows; filtered by the actor's weapon stance.
- **"A guard is alerted" When** for triggers (Convoy's `IA_CHANGE_STATE`
  pattern), so the alarm is a trigger instead of a pulled script.
- **Zone cutscenes** on the timeline: a cutscene's "When" (start or entering
  a zone); the timeline edits whichever cutscene is selected.
- **Pull script**: pick a shipped script by name, see its IDs, remap them.
- **Door actors** (`.DOOR_BOX`).
- **Free-space hints** inside interiors (the occupancy map above) when
  placing actors and props.
- **Go to** for project placements; the piece's donor map and groups in its
  Properties.
- **Animal behaviours**: cows grazing (2034/2030), dogs idling (2038-2042).

### Editor notes from v2

What v2 needed that the editor cannot do yet, in the order it would have
helped most:

- **Place on furniture.** Putting the radio on a table meant finding the
  table's top by hand (up-facing triangles 65-110 cm above the floor). The
  viewport's place tool should snap to the surface under the cursor
  (tables, benches, upper floors), not only the terrain, and remember it as
  an `on` height.
- **Occupancy overlay for interiors**: the free floor cells (floor minus
  walls and furniture, 40 cm clear) drawn in the viewport while placing, so
  a guard is not put inside a table or on top of Ransom's cows.
- **Floor-aware pieces.** A piece should record its donor floor and sit the
  floor on the ground (v2's `building(..., floor, ground)`), with the Height
  report flagging any piece whose floor is at or below the terrain.
- **Object browser for Worlds**: `catalog.py objects` found lamps, a pump,
  Czech hedgehogs, logs and tables inside lightmap groups; rendered
  thumbnails (v2 used a quick software render) would make them pickable.
- **Scatter tool**: trees and bushes by species along roads, field edges and
  hill bands, with keep-out zones (roads, buildings, fields), like
  `layout.trees()`/`bushes()`; and the FR03 species table (instance pairs by
  canopy size) in the asset browser.
- **Vehicle arrival recipe**: vehicle, route, passengers and seats, their
  posts and behaviours after getting out, an optional picture-in-picture
  camera; the pattern above as one component.
- **"Body found" alarm preset** (the watcher above) instead of "a guard is
  alerted".
- **Behaviour after an event**: guard-idle and guard-patrol scripts start on
  INIT; arrivals needed hand-written scripts to start on another event.
  A recipe option `start=<event>` would cover it.
- **Lightmap check**: compare a project's lightmaps with the slot map's and
  warn when they are much brighter (v1's "not night").
- **Recipe text IDs**: project strings as offsets into the reserved range, so
  a recipe survives a different range (v2 needed it once v1 kept 1000-1099).
