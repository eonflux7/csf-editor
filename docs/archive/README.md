# Archive

Historical plans and roadmaps whose work has shipped. They are kept for design
rationale, risk analysis, and test plans that the as-built reference and guide
docs do not repeat. Treat the pages under [`reference/`](../game-knowledge/) and
[`guides/`](../guides/) as the description of current behavior; these pages are
the record of how it was decided.

- [RWS implementation roadmap](rws-roadmap.md) — the pre-shift RenderWare
  roadmap that predates the mission workbench.
- [Sprint 01: level collision recovery](sprint-01-roadmap.md) — the shared
  recovered-World core and the collision overlay.
- [Sprint 02: collision inspection](sprint-02-roadmap.md) — picking, BSP
  topology, projections, clipping, measurement, and collision-only export.
- [The Great Shift](great-shift/README.md) — the decision to grow from an RWS
  inspector into a mission workbench, with its seven phase designs:
  - [Phase 01: generic CSFFBS core](great-shift/phase-01-generic-csffbs-core.md)
  - [Phase 02: mission package resolution](great-shift/phase-02-mission-package-resolution.md)
  - [Phase 03: visual Mission Explorer](great-shift/phase-03-mission-explorer.md)
  - [Phase 04: actor models and Physics](great-shift/phase-04-actors-and-physics.md)
  - [Phase 05: animation and cutscenes](great-shift/phase-05-animation-and-cutscenes.md)
  - [Phase 06: guarded authoring](great-shift/phase-06-guarded-authoring.md)
  - [Phase 07: mod staging and ecosystem](great-shift/phase-07-mod-staging-and-ecosystem.md)
  - [Format sprint 01: mission programs](great-shift/format-sprint-1.md)
  - [UI sprint: workbench redesign](great-shift/sprint-redesign.md)
- The mission editor (2026-09-25 to 09-27), which turned the workbench into
  CSF Mission Editor; what they left open is in the
  [roadmap](../plans/roadmap.md):
  - [Hello-world mission](editor/hello-world-mission.md): the first new-world
    mission, v1 to v13 (now an [example](../../examples/hello-world/README.md))
  - [Editor/Blender authoring](editor/editor-blender-authoring.md): authoring
    projects, mission operations, recipes, stages 1-8
  - [UI/UX redesign](editor/editor-ux-redesign.md): Mission, Script and
    Inspect modes, components, triggers, the timeline and the UI test harness
    (phases 0-7)
