# UI tests

`rws-man --run-script <file>` drives the GUI from a script, one step per
frame, in a hidden window. It clicks and types through ImGui's own input
handling, and it checks the application state and the rendered frames. The
design is in [the UX plan](../plans/editor-ux-redesign.md) (§7).

```bash
./test.sh --ui                        # builds rws-man and runs every UI test
RWSMAN_HELLO_WORLD=~/dev/csf-mods/hello-world-project ./test.sh --ui   # also the local tier
rws-man --run-script tests/ui/fixture_editing.uiscript --config-dir /tmp/cfg \
        --output-dir /tmp/out --golden-dir tests/ui/golden --overwrite --size 1280x720
```

`ctest -L ui` runs them too. Without a display they run under `xvfb-run`, or
are skipped when it is missing.

## Tests

- `tests/ui/*.uiscript` open the **fixture**, a mission that
  `rwsman_ui_fixture <dir>` generates (a small World, four actors, a route,
  cover, a zone, two scripts, an empty cutscene program and the invisible
  camera class the intro needs, with no game data). Scripts find it through
  `$RWSMAN_UI_FIXTURE`. These run everywhere, CI included.
- `tests/ui/local/*.uiscript` need local game data through environment
  variables and begin with `require <VARIABLE>`: `RWSMAN_HELLO_WORLD` (hello
  world's project), `RWSMAN_CORPUS` (the unpacked game) and
  `RWSMAN_TEST_INSTALL` (a game folder with untouched `maps/Convoy.pak` and
  `GlobalEK.pak`; the pipeline script copies it and deploys into the copy).
  They are skipped when it is unset. `hello_world_e2e` builds hello world's
  mission through the GUI alone and checks its structure. `ransom_performance` fails when Ransom's
  median CPU frame time, or the Outliner's or Problems' build time, exceeds
  its budget; it is skipped in Debug builds (`require-release`).
- For a long random walk, `RWSMAN_UI_MONKEY_STEPS=5000 ./test.sh --ui` makes
  every `monkey` step that many steps long (run it on a Debug build too, for
  ImGui's assertions).
- `tests/ui/golden/*.png` are the references for `compare`. They are rendered
  from the fixture only, never from game data.

## Script steps

One step per line; `#` starts a comment and `;` separates steps. Quote an
argument with spaces. Paths expand `~/` and `$VARIABLE`.

| Step | Does |
|---|---|
| `open <path>`, `open-project <folder>` | open a file, mission or project folder |
| `copy <folder> <name>` | copy a folder into the output directory, to edit it; `$UI_OUTPUT/<name>` is the copy |
| `wait idle`, `wait frames <n>` | wait for background work, or a number of frames |
| `command <id>`, `undo`, `redo` | run a registry command (fails when it is unknown or disabled) |
| `goto [<kind>:]<text>` | select the best Go to match (fails when there is none) |
| `palette <text>`, `goto-palette <text>` | open the palette with a query |
| `click <target> [mods]`, `double-click`, `hover` | pointer input on a widget (`click Outliner::tree-2 Shift`) |
| `click-world <x y z> [mods]`, `drag-world <x0 y0 z0 x1 y1 z1> [mods]` | pointer input at game coordinates in the viewport, with `Shift`, `Ctrl` or `Shift+Ctrl` held |
| `drag <target> <dx> <dy> [mods]` | drag a widget by an offset in pixels (a timeline shot's edge) |
| `drag-to-world <target> <x y z>` | drag a widget into the viewport (an asset onto the ground) |
| `key <shortcut>`, `type <text>` | keyboard input (`key Ctrl+Z`, `key Enter`); a text starting with `$` or `~/` is a path (`type $UI_OUTPUT/name`) |
| `expect <key> <value>`, `expect-not`, `expect-contains` | check a state key (below) |
| `remember <key>`, `expect-same <key>`, `expect-changed <key>` | compare with an earlier value |
| `expect item:<target> <state>` | `missing`, `visible`, `checked` or `unchecked` |
| `screenshot <file>`, `compare <name> [<percent>]` | save the frame, or compare it with `golden/<name>.png` |
| `dump-state <file>`, `list-items [<text>]` | write the state as JSON, print visible widgets |
| `lint commands` | fail on command registry problems |
| `resize <w>x<h>`, `require <VARIABLE>`, `require-release` | resize the window, skip without the variable, skip in a Debug build |
| `expect-at-most <key> <number>` | check a numeric state key against a budget (`perf.frame_ms`) |
| `monkey <seed> <steps>`, `undo-all` | a seeded random walk of edits; undo every mission and project edit (`RWSMAN_UI_MONKEY_STEPS=<n>` makes every walk `n` steps: the long run) |
| `add-component <lines>` | add a component from mission operation lines, with `\|` between lines |
| `setting <key> <value>` | `resource_root`, `game_root`, `projects_root` or `blender` for this run (never saved) |
| `remove <path>` | delete a file or folder inside the output directory (a project a script makes again) |

A **target** is a widget label, optionally qualified by its window:
`Inspector::Duplicate`. It matches the visible label (icons dropped), the
part after `##`, or the whole label. ImGui does not label combo boxes for
the harness; call `name_last_item("##id")` (`app/ui/widgets.hpp`) after one
so scripts can click it. A qualified target that is scrolled out
of view is scrolled to. `list-items` prints what can be addressed.

Every step waits until the app is idle: no mission load, authoring job, file
dialog, pending edit or gizmo drag. A widget step waits up to 60 frames for
its target to appear.

## State keys

`dump-state` writes them all. The main ones are:

- `app.workspace`, `selection.kind`, `selection.title`, `selection.record`,
  `selection.id`, `selection.position`, `selection.heading`, `selection.count`
  (the primary selection and the Shift/Ctrl-clicked ones)
- `mission.open`, `mission.editable`, `mission.dirty`,
  `mission.history.size`, `mission.history.top`, `mission.hash` (all mission
  files)
- `count.actors`, `count.nav_groups`, `count.nav_points`, `count.areas`,
  `count.scripts`, `count.placements` (the project's), `count.components`
- `selection.component` (the component that made the selection, "Guard
  patrol GE_CAMP"), `selection.component_state` (`clean` or `modified`) and
  `selection.component_lines` (its operation lines, joined by ` | `)
- `project.open`, `project.busy`, `project.dirty` (unsaved project edits),
  `project.slot`, `project.builds` (in `dist/`), `project.deployed` (active
  deployments), `project.playtests`,
  `project.history.size`,
  `project.history.position`, `project.height_findings`, `log.errors`,
  `log.last`, `log.last_warning`, `toast.last`, `problems.errors`,
  `problems.warnings`, `problems.notes` (the Problems panel's list)
- `panel.<name>` (a panel shown this frame), `ui.palette`, `viewport.tool`
  (`select`, `move`, `rotate`, `place`, `route`, `zone`, `cover`),
  `viewport.sketch` (points of the route or zone being drawn),
  `viewport.place_asset`, `viewport.picking`, `viewport.grid_preview_points`
- `form.trigger_actions`, `form.trigger_unverified` (the New trigger form),
  `flow.unraised_events` (events that start scripts but nothing raises),
  `objectives.targets` (each objective's target ID, in order: "1,7")
- `perf.frame_ms` (median CPU time of the last 120 frames), `perf.outliner_ms`,
  `perf.problems_ms` (the latest builds of the Outliner rows and the Problems
  list); they vary from run to run, so compare them with `expect-at-most`
- `timeline.durations` (the timeline's cutscene's shots, "4,2.5"),
  `timeline.time` (the playhead, seconds), `timeline.shot` (the selected
  shot, from 1), `timeline.cutscene` (its name, `CUT_INICIO`),
  `timeline.zone` (the zone that plays it, empty at the start) and
  `count.cutscenes`

## Failures

A failing step prints `<script>:<line>: <step>: <reason>`, writes
`<script>.failure.json` (the state) to the output directory, and exits 1. A
failed `compare` also writes `<name>.actual.png` and `<name>.diff.png`.
ImGui assertions (Debug builds) and two widgets sharing an ID are failures
too.

Runs are deterministic. The time step is fixed, and frame rates, build dates
and load times are hidden. Paths under `$RWSMAN_UI_FIXTURE`,
`$RWSMAN_UI_CONFIG` and `$HOME` are shown by those names. Settings and
layouts are never saved.

After an intended visual change, run the script with `--update-goldens`,
check the new PNG and commit it.
