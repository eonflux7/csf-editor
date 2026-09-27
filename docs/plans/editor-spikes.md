# Plan: research spikes for custom props and characters

Stage 8 of the [editor/Blender plan](../archive/editor/editor-blender-authoring.md). Each spike
is a small in-game experiment with a go/no-go gate; editor work for its area
starts only after a "go". Started 2026-09-25.

## Spike A: custom props (a new model file and class)

| Step | Question | State |
|---|---|---|
| A1 | Does the game load a class whose model is a **new file** we wrote? | built, in-game test pending |
| A2 | Does it load a **new mesh** (other vertex and triangle counts) in a donor Clump's structure? | not started |
| A3 | Does collision follow: a basic box (class record `MODELO_COLISION`), then a `.cmo`? | box covered by A1; `.cmo` not started |

**A1.** `MissionEditor::add_scaled_class` (`csf-mod mission-ops` line
`scale-class class= scale=`) scales a class's body model with
`rws::scale_clump` (frame translations, morph-target positions and bounding
spheres; everything else byte for byte), writes it as `<stem>_x<percent>.rpc`
beside the original, lists it in the model index (`.m3d`), and adds a class
copy (IDs from 500) naming it, with the box, the basic collision box and the
physics entry scaled. Classes with LOD models or a `.cmo` are refused.

Test build: `~/dev/csf-mods/hello-world-spike-scaled-prop` (hello world plus
Escape's crate at 1.5x as class 501, `BIG_CRATE` near the MP40 pickup;
archives in `dist/spike/`). csf-editor shows the 135 cm crate textured.

**Gate A1:** the mission loads; the big crate is drawn at its size where
placed; the player collides with its box (walk into it). Go: A2. No-go: find
which of the model file, the class record, the model index or the physics
descriptor the game rejects, one change at a time.

**A2 (next).** Keep a donor Clump's frames, materials and plug-ins; replace one
Geometry's vertex and triangle arrays with a Blender mesh; rebuild the Bin Mesh
plug-in and the bounding sphere; leave the Atomic's Pyro metadata as the donor
has it (its fields are not decoded; it is the main risk). Gate: a custom box of
different size and shape renders and is lit like the donor.

## Spike B: characters and animation

The skeleton, the animation catalog and the `.anm` write contract are
documented (`docs/format-reversal/anm/knowledge-base.md`: KB-anm-8 HAnim
binding, KB-anm-24/28 catalog IDs, KB-anm-26 the `.anm` write contract). A new
rig is the largest and least useful step, so the order is:

| Step | Question | State |
|---|---|---|
| B1 | Does a **new animation clip** on a vanilla skeleton play (Blender action → `.anm`, a new `Anims.bdd` record, `PLAY_ANMBDD`)? | not started |
| B2 | Does a **re-shaped character mesh** skinned to a vanilla skeleton render and animate? | not started |
| B3 | A new skeleton | not planned |

**Gate B1:** an idle guard plays the new clip in a loop without the actor
freezing or the game crashing; root motion stays put. Go: an animation
importer in the Blender add-on and an Anims.bdd editor. No-go: compare the
written `.anm` with a shipped clip of the same skeleton, record by record.
