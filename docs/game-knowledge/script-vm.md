# GSC/CSC script VM

Runtime model of the `CommXPC.exe` script interpreter that executes the CSFFBS
`.gsc`/`.csc` programs. Recovered by static analysis of `CommXPC.exe`; the
evidence and confidence for each claim live in
[`../format-reversal/scripting/knowledge-base.md`](../format-reversal/scripting/knowledge-base.md)
(KB-scripting-1 … KB-scripting-31). This note describes the engine's *acceptance*
model, which is broader than the 557-signature table generated from shipped
scripts.

## Opcode/tag registry

At startup the engine builds one registry (`g_pScriptRegistry` `0x008c4360`,
builder `Script_Registry_Init` `0x00771350`) of **422 entries**: 47 type/tag
definitions, 139 operators/conditions, 220 actions, 14 control-flow and 2
comments. Each entry is 0x20 bytes:

| Entry field | Meaning |
| --- | --- |
| `category` | 0 type/tag, 1 operator/condition, 2 action, 3 control, 4 comment |
| `id` | numeric opcode/tag id |
| `value_type` | result/value-kind id (`0xf` = no value, e.g. actions) |
| `name` | the CSFFBS head identifier |
| operands | vector of 12-byte `{value-kind id, label, bWrite_target}` descriptors (authoring metadata: the runtime never reads them) |

CSFFBS stores opcodes and operand tags as strings. At load, a head string is
resolved with a case-insensitive linear scan (`Registry_FindEntryByName`
`0x00770100`); at run time the numeric id is resolved with a binary search over
the id-sorted registry (`Registry_FindEntryById` `0x00770170`). The full table is
[`../format-reversal/scripting/opcode-registry.tsv`](../format-reversal/scripting/opcode-registry.tsv).

Two id namespaces must not be conflated: **type ids** (the `id` of a category-0
entry, e.g. `0xf` = `BICHO`, `0x24` = `ANM_BDD`) and **value-kind ids** (an
entry's `value_type`, reused as an operand descriptor's `type`, e.g. `4` = Bicho,
`0x24` = AnimacionBDD). `value_type == 0xf` is the reserved **statement/void**
result kind: no category-0 tag declares it, it is the `value_type` of every
action/control/comment, and `ScriptValue_Assign`/`ScriptValue_Equals` have no case
for it. The operand descriptor's `bWrite_target` is `0` only on the assignment
destination `Variable` of `SET`/`ADD`/`ARITHMETIC` and `1` everywhere else; the
runtime never reads the descriptor vector, so both are authoring metadata
(KB-scripting-29/30).

## Node and value objects

A decoded instruction is a `ScriptNode` (8 bytes):

| Offset | Field |
| --- | --- |
| `+0x00` | `id` (`short`) — registry opcode id |
| `+0x02` | `operand_base` (`short`) — first operand's index in the node pool; `0xffff` = leaf |
| `+0x04` | `value` (`ScriptValue *`) |

Each node's value is a `ScriptValue` (12 bytes): `vtable`, a `body` pointer to a
kind-specific payload whose byte `0` is the value-kind id, and a reference word at
`+0x8`. Bodies carry the payload: Bool (`body+1`), Numero (`body+4` float), Bicho
(handles at `body+8`/`body+0xc`), AnimacionBDD / SonidoID / idArma (id at
`body+4`). `ScriptValue_Assign` (`0x00475980`) copies a value, freeing and
re-allocating the body when the kind changes. The `+0x8` word is `-1` for a
constant/literal leaf, otherwise a slot id into the VM value table at `[vm+0x20]`
(bit `0x800000` selects a global table) — it is how named variables are
represented at run time.

Operands live in a per-instance node pool (`[[vm+0x10]+0x6c]`: a
`vector<ScriptNode*>` at `+4`/`+8`, plus a bitvector at `+0x10` whose storage is at
`+0x18`); `node->operand_base` indexes it. On decode, `Script_DecodeNodeByName`
(`0x00471640`) reads the head, resolves it by name, fills the value, then for
each declared operand reserves a pool slot and recurses. After each operand it
runs a **node-interning** step (`ScriptNodePool_InternOperand` `0x00472750` →
`ScriptNodePool_InternAt` `0x004724a0`): if an equivalent node already exists in
the pool the duplicate is freed and the slot re-pointed at the existing node.
Equality is by value (`0x00471ff0`) for constant leaves, by reference slot
(`0x00471f80`) for variable leaves, and by opcode id (`0x00471f10`) for
zero-operand operations; operations with operands are never merged.

## Script arrays

An array is a value body (`ScriptArray`, 0x1c bytes). The `ARRAY_*` operands use
descriptor type `0x2` (`ARRAYID`), a *variable reference*: the handler resolves
`operandValues[0]`'s `+0x8` slot id and takes the slot value's body as the array
object. The object is:

| Offset | Field |
| --- | --- |
| `+0x00` | kind byte `1` (`ARRAY`) |
| `+0x04` | element value-kind id |
| `+0x08` | cursor (current index) |
| `+0x0c` | reserved (never read) |
| `+0x10`/`+0x14`/`+0x18` | element `ScriptValue*` vector (begin/last/capacity) |

The constructor `Script_Array_Construct` (`0x00476cd0`) is called from
`Script_InstantiateVariables` (`0x00473c90`), which walks the program's `0x44`-byte
variable descriptor records: a scalar descriptor gets
`ScriptValue_SetKind(descriptor+4)`, an array descriptor (flag at `descriptor+8`)
gets `Script_Array_Construct(descriptor+4)`. So the element kind is the declared
`.TYPE`, fixed at instantiation rather than decoded from CSFFBS.

The handlers map onto a helper set: `ARRAY_RESIZE` (`0x708`),
`ARRAY_SET` (`0x709`), `ARRAY_GET` (`0x70a`), `ARRAY_FIND` (`0x70b`),
`ARRAY_SIZE` (`0x70c`), `ARRAY_ADD` (`0x70d`), `ARRAY_SET_CURRENT` (`0x70e`),
`ARRAY_CURRENT_ITEM` (`0x70f`), `ARRAY_CURRENT_INDEX` (`0x710`),
`ARRAY_DELETE_ELEMENT` (`0x711`) and `ARRAY_DELETE_INDEX` (`0x712`; `-1.0f` is a
no-op). `ARRAY_ADD` appends a copy of the element kind and leaves the cursor on
the new element; `ARRAY_GET`/`ARRAY_CURRENT_ITEM` also move the cursor. `FOREACH`
(`0xbc5`) drives the cursor: it sets it to `0` on first entry and advances it
until exhausted, then jumps past the loop via the `program+0x60` statement-loop
vector. Full field-level semantics and evidence: KB-scripting-20.

## Evaluation and dispatch

`Script_EvaluateNode` (`0x0046a9d0`) takes the VM context, a `ScriptNode` and an
output `ScriptValue`. A leaf copies its value. Otherwise it resolves the node id
and recursively evaluates each operand, collecting the operand `ScriptValue*`
into an array, then calls `Script_Opcode_Dispatch` (`0x00468120`).

The dispatcher keys on the 16-bit `node->id` and selects a handler through a
comparison tree plus jump tables. Every handler has the same signature —
`(vm, valueOut, node, operandValues)` — and returns via `RET 0xc`. Unmatched ids
fall through to the default at `0x0046a0eb`. The full `opcode → dispatch case →
handler` map is regenerated mechanically by
`format-reversal/scripting/tools/gen_opcode_handlers.py`: it symbolically executes
the decision tree for every 16-bit id and emits
[`../format-reversal/scripting/opcode-handlers.tsv`](../format-reversal/scripting/opcode-handlers.tsv)
(**374 registry rows**) and the complete slot inventory
[`../format-reversal/scripting/opcode-dispatch-table.tsv`](../format-reversal/scripting/opcode-dispatch-table.tsv)
(**835 rows**, including the 461 ids the registry does not name). Most rows call a
handler; some are inline.
The inline ones are the
context accessors `THIS`, `EVT_BICHO1/2`, `EVT_ACTITUD`, `EVT_ZONA`, `ALWAYS`
(constant true), `NOT`, `EVT_BOOL`, the null constants `BICHO_NULL`, `ARMA_NULL`,
`GRUPOBICHOS_NULL`, `GRUPOMALLA_NULL`, the action `ELIMINAR_MUERTOS`, and the
accepted-but-unimplemented stubs. They pin the VM context slots `vm+8`/`+0xc`
(current Bicho), `vm+0x30`/`+0x34` and `vm+0x3c`/`+0x40` (event Bicho),
`vm+0x48` (event attitude/zone) and `vm+0x54` (event `+0x30` byte).

## Example: `PLAY_ANMBDD`

`PLAY_ANMBDD` (id `0x13af`; operands Bicho + AnimacionBDD) reaching
`Script_Op_PLAY_ANMBDD` (`0x00462520`) was traced end to end. It resolves the
Bicho operand to a live entity (global manager `0x008b7cf0`), then issues the
internal animation command `{0x19, anmBddId, extra, -1}` to the entity's
animation component (virtual slot `+0xa8`). `PLAY_ANMBDD_TIME`,
`PLAY_ANMBDD_CICLOS` and `PLAY_ANMBDD_SONIDOID` share the shape and put a timer,
a cycle-derived duration, or a sound id in the command's `extra` field. The
command tag `0x19` is an internal animation enum, not the registry id `0x19`
(`BANDO`).

## Example: `ADD_SCRIPT`

`ADD_SCRIPT` (id `0x13aa`; operands Bicho + Script) reaching
`Script_Op_ADD_SCRIPT` (`0x004623e0`) is the second handler traced end to end. It
resolves the Bicho entity, and when the entity is not already busy and has no
matching attached script, gets/creates the script instance named by the Script
operand's id and starts or replaces it. It validates the operand model for a
`Script` value-kind (`6`) as well as a Bicho (`4`).

## Example: `CREAR_BICHO`

`CREAR_BICHO` (id `0x3ec`; one `ClassID` operand) reaching `Script_Op_CREAR_BICHO`
(`0x0045eb80`) spawns a new actor and returns a live `Bicho` handle. The numeric
class id lives in the operand's value body at `+4`. The handler resolves it to an
archetype through the world singleton `g_pWorld` (`0x008b85b4`) field `+0x1fc8`
(vtable `+0x10` = archetype index by class id, `+0x8` = archetype by index); the
archetype's `+0xb4` is a small actor-type enum consumed by
`Actor_SpawnFromClass` (`0x004ec2a0`) and `Actor_Construct` (`0x004f1c60`).
Allocation (`Actor_Allocate` `0x00444cd0`) reuses a pooled entity or constructs a
new one, stamps `++g_dwNextActorSerial` (`0x008ae6fc`) into `entity+0x20`, and
`BichoManager_RegisterActor` (`0x00456200`) appends it to `g_pBichoManager`
(`0x008b7cf0`) and writes the slot index into `entity+0x24`. The result value's
body receives `{serial@+8, index@+0xc}` — exactly the handle layout that
`Script_ResolveBicho` (`0x004528a0`) and `GET_CLASSID` (`0x0045f650`) consume, so
the returned value is immediately usable by other Bicho opcodes. `CREAR_BICHO`
has no position operand: the initial transform is not script-supplied and the new
Bicho is placed by a subsequent opcode. Full evidence: KB-scripting-22.

## Example: the `GET_*` family

The twelve `GET_*` operators (`GET_VIDA` `0x58c`, `GET_VIDA_MAXIMA` `0x58d`,
`GET_MUNICION_ARMA` `0x58e`, `GET_SCORE_KILLS` `0x587`, `GET_MISSION_SCORE`
`0x588`, `GET_NUMPATHPOINTS` `0x578`, `GET_NUMPATHPOINTS_LIBRES` `0x58b`,
`GET_NUMBICHOS_GRUPO` `0x579`, `GET_NUMBICHOS_SOBRE_GRUPO` `0x57f`,
`GET_NUMBICHOS_YENDO_GRUPO` `0x580`, `GET_NUMBICHOS_VISION` `0x57a`,
`GET_NUMBICHOS_ZONA` `0x581`) all return a `Numero` (float). They establish the
per-entity layout the VM reads: `entity+0x38` is the health/parameter component
(vtable `0x84` max, `0xa0` current, gated on the humanoid actor type at
`entity+0x50`), `entity+0x44` the weapon component, `entity+0x3c` the vision
component, and `entity+0x60` a mission-score contribution. The group/pathpoint
counters share `PathGroup_CollectFiltered` (`0x00465b00`) with an occupant-state
filter (unoccupied / state 1 / state 2); the score functions read the mission
state (`g_pMissionState+0xe8` weighted player stats, `+0xec` actor-score list).
Full evidence and the field table: KB-scripting-23.

## Pathpoints, groups and the `IR_A_*` move orders

The world keeps a pathpoint space at `(world+0x1fd8)`; inside it an **occupant
map** at `+0x34` holds `0x1c`-byte records `{state@+0, flag@+4, serial@+8,
slot@+0xc, x/+0x10, y/+0x14, z/+0x18}`. State `1` means "standing on" and `2`
"going to" a point; `PathOccupantMap_FindByPosition` (`0x004df4d0`) and
`PathOccupantMap_FindBySerial` (`0x004df490`) locate records, `PathOccupantMap_Store`
(`0x004dff10`) updates them. A `PathPoint` value is `{x@+4, y@+8, z@+0xc,
groupId@+0x10, pointId@+0x14}`; a `Grupo` value carries the group id at `+4`.

The `BICHO_SOBRE_PATHPOINT`/`BICHO_YENDO_PATHPOINT`/`BICHO_PATHPOINT` operators
(ids `0x3ed`/`0x3ee`/`0x3ef`) and the `BICHO_{SOBRE,YENDO,}_GRUPO` trio
(`0x3f1`/`0x3f2`/`0x3f3`) are **queries**: they return the Bicho handle of the
actor occupying the given point, filtered by state (`1`, `2`, or both). The
`_GRUPO` forms share `Script_PathGroup_GetBichoByState` (`0x00465cf0`), which
collects the group's points with `PathGroup_CollectFiltered` and picks one at
random.

Placement is instead an **order**: `CREAR_BICHO` leaves the new actor
unpositioned, and the `IR_A_*` family drives it. `IR_A_PATHPOINT` (`0x138b`),
`IR_A_POSICION` (`0x13ac`) and their `_ORIENT`/`_ANIM`/`_ITEM` variants resolve
the Bicho, build a `ScriptOrder_Init` (`0x0045e9c0`) descriptor `{tag@+0,
x@+4, y@+8, z@+0xc, anim@+0x10, item@+0x14, orient@+0x15, absPos@+0x16}` and
pass it to `entity[+0x34]`'s vtable `+0xa8` — the same command component and slot
`PLAY_ANMBDD` uses. `RESERVAR_PATHPOINT` (`0x13b2`) writes a state-`2` occupant
record so the map reflects the pending move. Full evidence: KB-scripting-26.

`entity+0x34` is the actor's **order/command component**, attached by
`Actor_AttachOrderComponent` (`0x00493ca0`) through `OrderComponent_Create`
(`0x004d3cc0`), which picks one of 22 per-archetype classes; the `kind` is
`archetype+0xd8` (overridden for a few actor types). Its vtable slot `+0xa8` is
`IssueCommand(descriptor)`; the actor's model is at `entity+0x16c`
(`Actor_GetModel` `0x00497780`) and `model->vtable[0xa8]` must return `0` before a
new order is accepted. The descriptor's `+0x00` dword is a **command tag**
(`0` = move to position, `0x19` = play animation, `0x26` = stop, …), interpreted
per class. The main-class sink `OrderComponent_Main_IssueCommand` (`0x004dbfe0`)
consumes `+0x10` displacement animation, `+0x14` item, `+0x15` orient and `+0x16`
absolute position through `Order_MoveToNoAnim`/`Order_MoveToWithAnim`/
`Order_MoveToWithAnimItem` (`0x004db970`/`0x004dac30`/`0x004dac80`). Full
kind→class→sink and tag tables: KB-scripting-27.

## Mission objectives and campaign values

Mission objectives are records in `g_pMissionState` (`0x008b85c8`) at `+0xec`
(`MissionState_GetObjectives` `0x00501180`). Each record is **100 bytes** with the
id at `+0x38`, the state at `+0x3c` (`0` success, `1` fail), a score at `+0x60`
and a flag at `+0x28`; the manager's `+0x1c` holds the mission result. The opcodes
`SET_OBJETIVO`/`_DUMMY_V2`/`_BICHO_V2`/`_LABEL`/`_SUCCESS`, `SET_MISSION_SUCCESS`,
`BORRA_OBJETIVO`, `EXISTE_OBJETIVO`, `OBJETIVO_COMPLETADO` and `OBJETIVO_FALLADO`
manipulate these records. `CAMPANYA_*` read and write a global string-keyed
campaign value store. Full evidence: KB-scripting-25.

## VM entry points

The program/instance object is `[vm+0x10]` and holds three inline vectors
(8-byte records):

- `+0x0c` — event-handler records `{u32 event_id, ScriptNode* handler}`, read by
  `Script_RunEventHandlers` (`0x0046b120`), which matches `event+4` (or the
  custom form) against `record[0]` and evaluates `record[1]` (mode-dependent:
  `vm+0x18 == 0` iterates the list; `vm+0x18 == 2` uses an alternate lookup). The
  records are decoded by `Script_DecodeEventRecord` (`0x00471730`), one per
  `.EVENTOS` entry.
- `+0x1c` — condition nodes, read by `Script_RunConditionsThenBody` (`0x0046afc0`),
  which evaluates each to a boolean; when all pass it calls `Script_RunStatements`.
- `+0x30` — statement (body) nodes, read by `Script_RunStatements` (`0x0046aad0`),
  which evaluates each in turn and stops when a statement clears `vm+0x82` or a
  1000-iteration guard trips.

Event handling is gated by `Script_IsActive` (`0x0045eb30`) and driven by
`Script_RunEventHandlers` (`0x0046b120`) in two modes: at `vm+0x18 == 0` it scans
the event-handler vector and runs the first node whose id matches the event; at
`vm+0x18 == 2` it resumes a suspended script when the event id matches the awaited
id at `vm+0x6c`. On a match `Event_ApplyToVm` (`0x00466d30`) copies the event
record into the VM context slots the `EVT_*` operators read (`event+0x0c`/`+0x10`
→ `vm+0x30`/`+0x34`, `event+0x18`/`+0x1c` → `vm+0x3c`/`+0x40`, `event+0x24` →
`vm+0x48`). `Event_MatchesId` (`0x0045e880`) is the id predicate, including the
`0x8000000` interned-id form (`event+0x24 == id & 0xf7ffffff` when `event+4 == 1`).
`Script_RunEventHandlers` is driven by `Event_Dispatch` (`0x0046c300`), the
script manager's virtual slot 0, which collects the active scripts registered for
the event type (`ScriptManager_CollectEventHandlers` `0x0046bb50`, two per-type
tables at `manager+0x68`/`+0x78`) and dispatches to each.

## VM driver modes and scratch slots

The VM's suspend/resume state is `vm+0x18`. It is advanced once per frame per
live instance by `Script_Instance_Tick` (`0x0046ac40`; `this` = one script
instance, `dt` float), called from `ScriptManager_UpdateInstances` (`0x0046c560`)
over the manager's live-instance vector `manager+0x28`. The tick `switch`es on
the mode and, on completion, increments the statement cursor `vm+0x1c` and
re-runs `Script_RunStatements`. The complete set:

| Mode | Meaning | Set by |
|---|---|---|
| `0` | idle / not running | `Script_RunStatements` finish |
| `1` | run; check the current actor's order component | `Script_RunStatements` entry, `CONTINUE` |
| `2` | resume on the awaited event id (`vm+0x6c`) | `WAIT_EVENT` |
| `3` | wait for a child script/trigger instance (`vm+0x70`) | `TRIGGER_EXE`, `SCRIPT_EXE` |
| `4` | wait for navigation (`WAIT_NAVEGACION_FIN`) | `WAIT_NAVEGACION_FIN` |
| `5` | wait for an attack | `WAIT_ATAQUEFIN`, `ATAQUE_STOP` |
| `6` | wait for a cutscene (`vm+0x74`) | `CUTSCENE_EXE` |
| `7` | wait for a camera animation | `CAMARA_PLAY_ANM` |
| `8` | wait for a positioned sound (`vm+0x7c`) | `PLAY_SONOID_DUMMY`, `PLAY_SONOIDOID_BICHO_CICLOS` |
| `9` | activate a vehicle (sound, then idle sound) | `ACTIVAR_VEHICULO` |
| `0xa` | wait on a condition (re-run from `vm+0x1c`) | `WAIT_CONDICION` |
| `0xb` | wait on a UI window | `VENTANA_SHOW` |
| `0xc` | timed pause (`vm+0x78 -= dt`) | `PAUSE`, `VENTANA_BANNER`, loop guard |

`vm+0x60`/`vm+0x64` hold the last ordered/acted-on actor handle, `vm+0x6c` the
awaited event id, `vm+0x70` the awaited child instance, `vm+0x74` the cutscene
id, `vm+0x78` the timed-wait seconds, `vm+0x7c` the sound handle, and `vm+0x82`
is the "resume on the next tick" flag the waiting handlers clear. `CONTINUE`
(`0xbb8`) cancels every pending wait (resets all of those slots), sets mode `1`
and `vm+0x82 = 1`. Full handler evidence: KB-scripting-12/28/31.

## Script lifecycle and the `.RECURSOS` preload contract

There are two `ScriptManager` objects under the game object
(`g_pBichoManager`, `0x008b7cf0`): the **mission** manager at `[game+0xbc]` and the
**campaign** manager at `[game+0xc0]`. `Mission_Load` (`0x00505cd0`) calls
`Mission_LoadScriptPrograms` (`0x00500cf0`), which loads `<mission>.gsc` into the
mission manager and, on success, `<mission>.csc` into the campaign manager via
`ScriptManager_LoadProgramFile` (`0x0046e9c0`).

A program file is CSFFBS with three sections:

| Section | Effect |
|---|---|
| `.VARIABLES` | parsed into the manager's global value template at `manager+0x3c` |
| `.SCRIPTS` | one 0x80-byte `ScriptProgram` per child, into the list at `manager+0x18` |
| `.RECURSOS` | typed resource descriptors at `manager+0xc` (preload list) |

A `ScriptProgram` (`ScriptProgram_ReadEntry` `0x00471800`) holds `.FLAGS`
(`+0x00` = `.TRIGGER == 1`, `+0x01` = `.ENABLED == 1`), `.ID` (`+0x04`), the
`.EVENTOS` vector (`+0xc`, 8-byte `{event_id, ScriptNode*}` records),
`.CONDICIONES` (`+0x1c`), its own `.VARIABLES` (`+0x28`) and `.ACCIONES` (`+0x30`).

A program is *not* running by itself. The runtime VM — a 0x84-byte `ScriptInstance`
(`ScriptInstance_Construct` `0x0046a8e0`, whose `+0x10` is the program and `+0x20`
the local variables) — is created by `ScriptManager_CreateInstance` (`0x0046bdc0`)
and pushed onto the live-instance vector `manager+0x28` that
`ScriptManager_UpdateInstances` ticks. Activation:

- **At mission start**, `ScriptManager_StartTriggeredPrograms` (`0x0046be60`)
  creates a VM for every program with `.TRIGGER == 1` (3507 of the 5762 shipped
  `.SCRIPTS` entries across the 21 `.gsc` files).
- **On demand**, `ScriptManager_StartProgramById` (`0x0046be30`, lookup
  `ScriptManager_FindProgramById` `0x0046b470` by `.ID`) starts a named program,
  used by `ADD_SCRIPT` and the trigger/`SCRIPT_EXE` drivers.

Event routing uses two 44-slot per-event-type tables built by
`ScriptManager_InitEventHandling`: `manager+0x68` holds the `.EVENTOS` handler
registrations (`ScriptManager_RegisterInstanceEvents` `0x0046b9c0`) and
`manager+0x78` the `WAIT_EVENT` waiters (`ScriptManager_RegisterEventWaiter`
`0x0046ba70`); each slot is a vector of instance pointers, and a custom event
(bit `0x8000000`) goes to slot `1`. `ScriptManager_CollectEventHandlers`
(`0x0046bb50`) scans both tables per event type, filtered by `Script_IsActive`.

`.RECURSOS` lists resources to preload, grouped by section:
`.CLASSID` (`0x20`), `.ANIMACIONES` → `ANM_BDD` (`0x24`), `.SONIDOSID` →
`SONIDO_BDD` (`0x26`), `.EFFCLASSID` (`0x29`), `.ARMACLASSID` (`0x2f`),
`.EFECTO` (`0x11`), `.SONIDOS` (`0x25`), `.FBS` (`0x12`) and `.FNC` (`0`) — the
values are registry `value_type`s. Each element becomes a 0x44-byte descriptor
(id `0xffffffff`, value-kind, scalar flag, float id or string) in `manager+0xc`.
`ScriptManager_PreloadResources` (`0x00502840`) walks the vector and loads each
entry through the world singletons (class/effect/weapon archetype registries, the
animation registry and the sound registry); `Mission_PreloadScriptResources`
(`0x00504c60`) runs it for both managers at mission load (from
`Mission_PostLoadInit` `0x00504e70`).

Full evidence: KB-scripting-32/33 in
`format-reversal/scripting/knowledge-base.md`.

## Event names and the event bus

An event's numeric type (`event+4`) is either a **built-in event name index** or
the custom-event channel. Built-in names live in a 44-slot global table
(`g_szEventNameTable` `0x008b1750`; lookup `EventName_FindBuiltin` `0x00768db0`),
with populated entries such as `0 START_GAME`, `3 DISPARO`, `4 IMPACTO`,
`6 MORIBUNDO`, `7 CURADO`, `8 MUERTO`, `10 BICHO_ENT_ZONA`, `11 BICHO_SAL_ZONA`,
`18 CHECK_MISSION_COMPLETED`, `19 BICHO_DESTRUIDO`, `23 ALWAYS`,
`24 IA_CHANGE_STATE`, `27 OBJETO_COGIDO`, `37 PUERTA_USADA`, `38 ALARM_SET_OFF`.
Mission-defined names are resolved by `Event_ResolveName` (`0x00471210`) from a
per-owner name vector; they are delivered as type `1` with the numeric id in
`event+0x24` and the `0x8000000` bit set (`Event_MatchesId`).

Scripts raise events through `SEND_EVENT` (`0x00466690`) and `SEND_EVENT_BICHO`
(`0x00466760`); the engine raises them from input, AI, weapons and mission code.
All go through the central bus `[g_gameManager+0x184]`:
`EventManager_Raise` (`0x004786d0`) either invokes every registered listener
object's vtable slot 0 with the event record (`bus+0x28` vector plus a global
list) or enqueues it. Listeners are appended by `EventManager_AddListener`
(`0x00478620`); the script manager registers itself in
`ScriptManager_InitEventHandling` (`0x0046def0`), which is why `Event_Dispatch`
has no direct caller.

## Scope and uncertainty

`PLAY_ANMBDD`, `ADD_SCRIPT` and the control-flow/event handlers (`IF`, `ELSE`,
`WHILE`, `BREAK`, `NEXT`/`WEND`/`ENDIF`/`ENDFOR`, `SET`, `ADD`, `ARITHMETIC`,
`FOREACH`, `SEND_EVENT`, `WAIT_EVENT`, `TRIGGER_*`, …) are traced, as are the
boolean operators `AND`/`OR`/`NOT`/`ALWAYS`/`EVT_BOOL`, the `ARRAY_*` family and
the `CMP_OP_*`/`ES_*` predicates; the inline cases are annotated (the typed
accessors, the null constants, `ELIMINAR_MUERTOS` and the no-op stubs). The map is
now derived mechanically rather than patched by hand: `opcode-handlers.tsv` holds
the 374 registry opcodes and `opcode-dispatch-table.tsv` the 835 dispatcher slots
(see KB-scripting-19 in `format-reversal/scripting/knowledge-base.md`); the earlier
three hand corrections (two missed jump tables, then the byte-mapped
`0x70a`–`0x7d6` table and several dropped ids) are confirmed by the regeneration.
Most handlers' side effects remain only named by address. `SET_EVENT_FKEY` is registered but has no runtime effect in this build
(the only route is the no-op tail), and the script manager's event-bus
registration site is now known (`ScriptManager_InitEventHandling`). The 461
unnamed slots in `opcode-dispatch-table.tsv` are **unreachable**: the compiler
emits a dense slot per id in each range, and the only node-id writer
(`ScriptNode_SetOpcode`) is called solely from the value decoder and passes a
registry id or `0`. Id `0` therefore means "no opcode / uninitialised node" in
four places (`NO_FUNCTION`, the empty-node sentinel, the unknown-name decoder
fallback, and every category-0 value leaf). The operand-descriptor `flag`
(`bWrite_target`) and `value_type == 0xf` are now resolved (KB-scripting-29/30),
as is the complete `vm+0x18` driver-mode set (KB-scripting-31) and the script
lifecycle / `.RECURSOS` preload contract (KB-scripting-32/33). Still open:
whether the pool bitvector tracks live slots, and which order-component kind each
archetype carries (`archetype+0xd8`).
