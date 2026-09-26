# Repository guide

- This is a C++20/CMake project. Windows builds target Visual Studio Build Tools
  2026 (18.9.2), x64; Linux builds target GCC 13+ or Clang 16+ with Ninja.
- From the repository root, use `./build.ps1` and `./test.ps1` on Windows, or
  `./build.sh` and `./test.sh` on Linux; both default to Release. Use
  `-Config Debug` (PowerShell) or `--config Debug` (bash) when needed.
- `./build.ps1 -Target rws-man` / `./build.sh --target rws-man` builds only the
  GUI and its dependencies. `-CoreOnly` / `--core-only` uses `build-core` without
  GLFW, ImGui, or OpenGL.
- The portable `rws_image` library decodes DDS and PNG textures with stb_image and
  is shared by the GUI and tests; keep texture decoding out of the GUI target.
- CMake presets own generator, build-directory, and test configuration. Keep
  scripts, presets, and README commands consistent when changing the build.
- Do not edit or commit generated content under `build*`, `_deps`, or Python
  `__pycache__` directories.
- `rws_core` contains parsing/export logic; `rws-man` is the GUI; `rws-info` and
  `rws-corpus` are console tools.
- GUI layout: `app/main.cpp` is only startup and the frame loop. `AppState`
  (`app/app_state.hpp`) owns documents, selection, and UI state; `app/app_actions.cpp`
  and `app/mission_loader.cpp` (worker-thread mission load) change it; each panel is a
  `draw_*(AppState&)` function in `app/ui/`. The modes (Mission, Script, Inspect) and
  which panels each workspace docks where are in `app/ui/layout.cpp`; a new panel
  needs a `Panel` value, a slot there and a case in `app/ui/shell.cpp`, and
  `show_panel` (`app/commands.hpp`) is how anything brings one to the front. Menus,
  shortcuts, the command palette, and the Help cheat sheet all read the one registry
  in `app/commands.cpp`; add commands there. Write ImGui colors only in
  `app/ui/theme.cpp` (tokens, the per-kind palette and the viewport palette). Build
  authoring panels from the `app/ui/widgets.hpp` helpers (cards, buttons by role,
  empty states, help markers) so they stay consistent; the design is
  `docs/plans/editor-ux-redesign.md`.
- Mission editing: `csf::MissionEditor` (`src/csf_mission_edit.cpp`) owns the
  edited files as canonical `csf::Tree`s and is the only writer; the GUI calls it
  through `app/mission_editing.cpp`, which rebuilds the mission views at the start
  of the next frame (never mid-frame). Edit widgets live in
  `app/ui/mission_editor.cpp`, the viewport gizmo in `app/geometry_preview_editing.cpp`,
  and the viewport's authoring tools (Place, Route, Zone, Cover), multi-selection,
  project placements as records and eyedropper picks (`pick_button`) in
  `app/viewport_tools.cpp`. Project edits go through `edit_authoring_project`
  (`app/authoring.hpp`), which shares one undo history with the mission editor;
  project edits stay in memory until Save, which saves both. Recipes the GUI
  makes are components (`csf/mission_components`: their operation lines in the
  workspace's `components.csfops`, regenerated in place on edit); their card
  is `app/ui/component_card.cpp`, and moves or deletes of their records go
  through `move_component_record`/`delete_component_record`
  (`app/mission_authoring.hpp`).
  `csf-mod audit` must keep every corpus file byte-identical through the tree
  writer and the source text; regenerate `src/csf_script_signatures.cpp` with
  `tools/generate_script_signatures.py` instead of editing it.
- Map Worlds: `rws::WorldModel` (`src/world_model.cpp`) parses and writes `0x0B`
  Worlds, `src/world_source.cpp` compiles `.csfworld` (written by
  `tools/blender/export_csf_world.py`) and `src/map_assembly.cpp` copies donor
  props. `csf-mod world-audit ../CSF_unpacks --rebuild` must keep every shipped
  World and scene-instance record byte-identical.
- Mission authoring on top of `MissionEditor` stays GUI-free in `csf_core`:
  presets in `csf/mission_recipes`, the text operations API in `csf/mission_ops`
  (`csf-mod mission-ops`), components in `csf/mission_components`
  (`csf-mod mission-components`) and the read-only flow in `csf/mission_flow`.
  A project's lifecycle (new project in a slot, both archives into `dist/`,
  deploy and roll back, the playtest log) is `csf/project_pipeline`
  (`csf-mod project-new|project-archives|project-deploy|project-rollback|
  project-playtest`); the GUI calls it through `app/project_actions.cpp`
  (the wizard and Build panel cards are `app/ui/project_panels.cpp`, Edit in
  Blender runs `tools/blender/open_project.py`).
  `tools/hello_world/parity.sh [--components] <project>` must keep hello world
  byte-identical when built through them, and with `--components` every
  component must regenerate identically.
- Authoring projects: `csf::AuthoringProject` (`src/csf_authoring_project.cpp`)
  reads `project.csfproj`/`local.csfproj` and builds a project's map, collision
  and sector map (`rws::build_map_files`, `rws::build_sector_map`) into its
  `build/`; the format is `docs/plans/editor-project-format.md`. Hello world's
  recipe (`tools/hello_world/build.sh`) creates one; its World and sector
  files must stay identical to v13's unless a change intends otherwise.
- GUI-free helpers (settings, command registry, fuzzy matcher, navigation history,
  operation log, search index, diagnostics table, mission discovery, entity kinds,
  the problem list, UI scripts, contrast) live in the
  `rwsman_ui_model` library (`include/rwsman/`, `src/ui_model_*.cpp`). It must not
  depend on ImGui, GLFW, or OpenGL, and its tests go in `tests/document_tests.cpp`.
- The GUI writes only to the per-user config directory (`settings.ini`, `layout.ini`,
  `screenshots/`, debug log), never to the working directory. Embedded fonts are
  generated by `tools/generate_ui_fonts.py`; record licenses in `app/ui/fonts/LICENSES.md`.
- `rws-man --screenshot out.png [--frames N] [--size WxH] [--config-dir DIR]
  [--commands id,goto:kind:text,palette:text,...] [scene.scn | project folder]` renders
  hidden, runs the commands one per frame, saves a PNG, and exits. Use it to check UI
  changes (also with a Debug build, which enables ImGui's assertions); for anything
  repeatable write a UI script instead.
- Tests belong in `tests/document_tests.cpp`. Run `./test.ps1` (Windows) or
  `./test.sh` (Linux) after changes to parser, decoder, or export behavior.
- UI tests are `tests/ui/*.uiscript` scenario scripts (`rws-man --run-script`,
  guide: `docs/guides/ui-tests.md`) against the generated fixture mission
  (`tests/ui/make_fixture.cpp`); `tests/ui/local/` holds scripts that need
  local game data and skip without it. Run them with `./test.sh --ui` /
  `./test.ps1 -Ui` after GUI changes, and add or extend a script for new UI
  behaviour. Reference PNGs in `tests/ui/golden/` come from the fixture only.
- Locally available unpacked game resources live in `../CSF_unpacks`; treat them
  as read-only reference data and do not copy them into the repository.
- Executable reverse-engineering work lives in `docs/format-reversal/`, with its
  own `AGENTS.md`, `roadmap.md`, `state.md` and one folder per workstream; keep
  that workspace's evidence/confidence conventions and read-only rules. It is a
  separate git repository nested at that path (rws-man's `.gitignore` excludes
  it): commit research there, and only rws-man code/docs here.
- Research automation (queue generator, external tracer driver, `/continue-research`,
  the `tools/ghidra/gq` export-backed Ghidra CLI and `annotations/*.tsv` write path,
  and the `tools/kb.py` claims store / knowledge graph over `claims/*.jsonl`) lives in `docs/format-reversal/tools/` and `docs/format-reversal/.opencode/`;
  see `docs/format-reversal/tasks/README.md`. Generated queue/result files there
  are gitignored.
- Preserve unknown/truncated RWS data and write modified assets to new files
  unless the user explicitly requests an overwrite.
- The worktree may contain ongoing user changes; preserve unrelated modifications.
