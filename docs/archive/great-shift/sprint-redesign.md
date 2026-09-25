> **Archived plan — implemented.** The workbench as built is documented in
> [GUI usage](../../guides/gui.md); this document is kept for its design
> direction, theme tokens, and per-item implementation notes.

# UI sprint: workbench redesign and quality of life

## Outcome

Turn `rws-man` from a stack of working panels into a coherent workbench. It
should look like a serious reverse-engineering tool (RenderDoc, Ghidra, IDA,
Cutter) rather than a stock Dear ImGui demo. It should also keep a modding tool's
hacker character: dense, monospace-first data, hex offsets everywhere, and
nothing that hides the raw bytes.

The sprint changes presentation, navigation, and ergonomics only. Parsers,
resolvers, and export behavior stay as they are. Every Phase 01–05 workflow must
still work at the end, and each should take fewer clicks.

At the end of this sprint, a user should be able to:

- start the tool and open a mission from a start page, recent list, or resource
  browser, on Windows and Linux;
- resize, collapse, rearrange, and reset the Explorer / Viewport / Inspector
  layout, and find it the same way on the next launch;
- tell at a glance whether a value is proven, inferred, unknown/raw, dirty, or
  diagnosed;
- jump to any actor, script, chunk, class, or command with one keyboard shortcut,
  then go back and forward through the selection history;
- copy any ID, offset, path, or value with one click;
- read diagnostics and operation results in a log instead of a single status
  line that gets overwritten.

## Current state (audit, 2026-09-21)

Observations from `app/main.cpp`, `app/geometry_preview.cpp`, and the scene
preview screenshot:

**Layout**

- The app is one fixed full-screen `ImGui::Begin` with hand-sized child regions.
  The left and right panels get equal widths clamped to 180–390 px. There are no
  splitters, so a panel cannot be resized, and a wide script table or inspector
  gets squeezed.
- Both the Explorer (`show_scene_tree`) and the Inspector (`show_inspector`)
  start hidden, so a first launch shows only the viewport. The toolbar buttons
  change their labels (`Scene tree` / `Hide tree`, `Inspector` /
  `Hide inspector`) instead of showing a toggle state.
- The right panel switches between "Selection" and "Viewport tools" with two
  buttons plus a "Close" button, so selection context and render settings fight
  for one slot.
- `imgui.ini` is written to the current working directory. There is a copy in
  the repository root, and it only holds the default debug window.

**Navigation**

- There are six workspaces (Mission, Script, Animation, Scene, Geometry,
  Inspector), chosen from a combo box. Only four have hotkeys (`1`–`4`), and
  those are bare digit keys. Mission and Script have none.
- Search is per workspace and substring-only. The script search rebuilds a
  lowercase haystack for every script on every frame.
- There is no back/forward. Following a cross-reference (script → actor →
  class → model) loses the previous context.
- File > Open and Open mission are `#ifdef _WIN32` only. On Linux the only way
  to open a file is the command line or drag and drop.

**Visual language**

- The app uses stock `StyleColorsDark()` with the default ProggyClean bitmap
  font and no DPI scaling. It looks blurry and small on high-DPI Linux and
  Windows displays.
- Color meaning is ad hoc: orange for warnings in the chunk tree, green for
  script comments, a green/yellow size gradient for clumps. Nothing marks
  proven versus inferred data in the UI, even though project invariant 7
  requires it.
- The inspector content is mostly `TextWrapped` / `BulletText`. Values can't be
  selected, copied, or clicked through to their source bytes.

**Feedback**

- `status` is one overwritten string at the bottom. Errors, exports, and
  "Loaded ..." messages all look the same and disappear on the next action.
  Load failures point to `rws-man-debug.log` on disk.
- Mission loading is synchronous. The window freezes for large maps, with no
  progress or cancel.

**Code structure**

- `run_app` is a single ~2,000-line function in a 3,900-line `main.cpp`. State
  lives in locals and lambdas, so any UI change touches the same function. This
  is the main obstacle to everything else in this sprint.

## Design direction: "professional, but still hacky"

### Principles

1. **Data first, chrome last.** Borders are 1 px, corners are square or nearly
   square (rounding 0–2), and padding is tight. Screen space goes to values and
   the viewport, not to decoration.
2. **Monospace for data, sans for chrome.** Menus, buttons, and headers use a
   clean sans font. IDs, offsets, hashes, opcodes, paths, and values use a
   monospace font. Everything readable as bytes should look like bytes.
3. **One accent color, strict semantic colors.** Amber accent (it fits the
   military setting and the existing warning orange). Every other color has a
   fixed meaning (see tokens below) and is never decorative.
4. **Raw is always one click away.** Every typed value can show its source
   file, entry index, offset, and raw node. This is the "hacky" part, and it is
   also the project's provenance invariant made visible.
5. **Keyboard reachable, mouse friendly.** Every command is in a registry that
   feeds the menus, shortcuts, the command palette, and the Help cheat sheet.
6. **No gimmicks.** No CRT scanlines, glow, or animated terminal effects. The
   hacker feel comes from density, monospace, hex, and terse labels such as
   `// SELECTION`, `> search`, and `0x2C79D6`.

### Theme tokens

Define the tokens once in `app/ui/theme.cpp` and never write ImGui colors
inline outside that file.

| Token | Use | Suggested value |
|---|---|---|
| `bg.0` | Window background, viewport clear | `#0F1114` |
| `bg.1` | Panels, child regions | `#15181D` |
| `bg.2` | Headers, hovered rows, inputs | `#1C2027` |
| `line` | 1 px borders, separators | `#2A2F38` |
| `text` | Primary text | `#D4D7DD` |
| `text.dim` | Labels, secondary info | `#7D8490` |
| `accent` | Selection, active tab, focus ring | `#E8A33D` (amber) |
| `accent.dim` | Selected row background | amber at ~22% alpha |
| `ok` / `proven` | Resolved, validated, exact | `#6CC56C` |
| `inferred` | Candidate/untyped relationship | `#5FB3D9` (cyan), dashed/italic label |
| `raw` | Unknown field, preserved bytes | `#A58BD8` (violet) |
| `warn` | Warnings, ambiguous resolution | `#E0B341` |
| `error` | Errors, missing, truncated | `#E5534B` |
| `dirty` | Modified in session / staged | `#E8A33D` + `●` marker |

A second "high-contrast" theme and a light theme are stretch goals. Build
everything from the tokens so they can be added cheaply.

### Typography

- Embed two fonts through the build (FetchContent or a vendored compressed TTF,
  with licenses recorded). The sans font is Inter or IBM Plex Sans. The mono
  font is JetBrains Mono or IBM Plex Mono. Both are OFL.
- Merge an icon font (Lucide or Font Awesome Free via IconFontCppHeaders) into
  both, so toolbar and tree glyphs work without images.
- Scale with `glfwGetWindowContentScale` at startup and on
  `glfwSetWindowContentScaleCallback`. Rebuild the font atlas and call
  `ScaleAllSizes`. Add a user UI scale override (80–200%) in Preferences.

### Section and label style

- Section headers are small caps or uppercase `text.dim` with a hairline rule,
  for example `SELECTION ───────`.
- Search fields use a `>` prompt glyph and a hint such as `name, class, 0xID…`.
- Numbers are right-aligned in tables. Offsets always print as
  `0x%06X`-style hex and render as links.

## Target layout

```text
┌─────────────────────────────────────────────────────────────────────────────────────┐
│ File Edit View Mission Tools Export Help     [MISSION] SCRIPT ANIM SCENE GEOM HEX   │ ← menu + workspace tabs
├──────────────────┬───────────────────────────────────────────────┬──────────────────┤
│ EXPLORER    ◂ ▸  │ ◉ Lit ▾ │ Persp ▾ │ ⌖ Frame │ ◧ Layers ▾ │ 📏  │ INSPECTOR   📌 ⧉ │
│ > search…        │───────────────────────────────────────────────│ Actor  #17  ●    │
│ ▾ FR01.scn       │                                               │ FR01.scn:e1234   │
│   ▾ Actors (312) │                                               │ ── IDENTITY ──── │
│     ◆ Guard_01   │                 3D VIEWPORT                   │ name   Guard_01  │
│     ◆ Guard_02   │                                               │ class  0x4A1 ✓   │
│   ▸ Nav (1,204)  │                          ┌──┐                 │ ── TRANSFORM ─── │
│   ▸ Areas (41)   │                          │xyz│ axis gizmo     │ pos  12.0 0.4 …  │
│   ▸ Dummies      │                          └──┘                 │ yaw  90.0°  ⧉    │
│ ▸ Resources      │  fps 60 · 290k tris · cam 14867               │ ── REFERENCES ── │
│ ▸ Diagnostics ⚠3 │                                               │ ▸ used by 4 scr. │
├──────────────────┴───────────────────────────────────────────────┴──────────────────┤
│ CONSOLE │ DIAGNOSTICS ⚠3 │ REFERENCES │ CHANGES                                     │ ← bottom dock (collapsible)
│ 12:04:11 ok   loaded mission FR01.scn (0.84 s, 369 clumps, 1171 placements)         │
│ 12:04:11 warn texture FWINA05A.dds: unsupported DDS format                          │
├─────────────────────────────────────────────────────────────────────────────────────┤
│ ● FR01.scn │ Mission │ sel Actor#17 @0x2C79D6 │ 0 dirty │ root ../CSF_unpacks │ 60fps │ ← status bar
└─────────────────────────────────────────────────────────────────────────────────────┘
```

- **Explorer (left):** One tree per workspace, but always the same frame:
  a search prompt, an optional filter chip row (kind, has-diagnostics, dirty),
  then the tree. Counts sit in dim text beside each group. Workspaces that
  have no tree today (Geometry, Hex) show the RWS chunk tree.
- **Viewport (center):** A floating icon toolbar drawn over the top edge of the
  viewport, replacing the current wrapping checkbox row. It holds a view-style
  menu, projection, a Frame menu, a Layers popup with legend swatches (as
  today), Measure, and Clip. A small HUD in the corner shows fps, triangle
  count, and camera info, and can be turned off. An axis gizmo sits in the
  bottom-right corner, and clicking it snaps to orthographic views.
- **Inspector (right):** Selection only. The current "Viewport tools" content
  moves to a **View** popover on the viewport toolbar and a dockable
  **Render settings** panel, so the inspector no longer switches modes.
- **Bottom dock:** Console (operation log), Diagnostics (filterable table, and
  clicking a row selects the source), References (reverse uses of the current
  selection, currently inside the inspector), and Changes (the future authoring
  change list, see Phase 06).
- **Status bar:** Segmented monospace line: document and dirty marker ·
  workspace · selection with offset · dirty count · resource root · fps.
  Clicking a segment opens the related panel.

The **Script** workspace uses the same frame. The center shows the program
listing instead of the viewport, the Explorer shows the folder/script tree, and
the Inspector shows the selected instruction or operand with its resolved
target.

## Work breakdown

Each item below is sized to land as its own commit or PR, in the order listed.
R-00 is a hard prerequisite. Items after R-02 can be reordered.

### R-00: split `main.cpp` (no visible change)

- Introduce an `AppState` struct that owns the documents, mission models,
  selection, workspace, and UI flags that are currently `run_app` locals.
- Move the loading lambdas (`load`, `load_mission`, `pair_collision`,
  `save_copy`) into an `app/app_actions.cpp` unit that works on `AppState`.
- Split the drawing into `app/ui/` files: `explorer.cpp`, `inspector.cpp`,
  `script_view.cpp`, `hex_view.cpp`, `menus.cpp`, `status_bar.cpp`. Each file
  gets a `draw_*(AppState&)` entry point.
- Keep `GeometryPreview` as is, apart from adding the hooks later items need.
- Done when behavior, tests, and screenshots match `main` and `main.cpp` is a
  thin startup/frame loop.

### R-01: docking shell and persistent layout

- Switch the ImGui FetchContent tag from `v1.91.9b` to `v1.91.9b-docking`.
  Enable `ImGuiConfigFlags_DockingEnable` only. Leave multi-viewports disabled,
  because Wayland/X11 support for them is still unreliable.
- Build the default layout with `DockBuilder`: Explorer 18%, Inspector 22%,
  bottom dock 22% (collapsed by default), and the viewport in the central node.
  Add View > Reset layout.
- Store per-workspace layouts. Script mode can then give the listing more width
  and hide the viewport without affecting Mission mode.
- Move `imgui.ini` and the app settings to a per-user config directory:
  `%LOCALAPPDATA%\CSF RWS Tools\` on Windows (it already holds the recent
  pairings) and `$XDG_CONFIG_HOME/csf-rws-tools/` on Linux. Delete the stray
  `imgui.ini` from the repository root and add it to `.gitignore`.
- Show the Explorer and Inspector by default once a document is loaded.
- The viewport keeps its `AddCallback` rendering at first. If docking or
  overlays expose ordering problems, move it to an FBO + `ImGui::Image`, which
  also enables R-08 screenshots.

### R-02: theme, fonts, DPI

- Add `app/ui/theme.{hpp,cpp}` with the token table above, `apply_theme()`, and
  semantic helpers such as `ui::badge(Provenance)`, `ui::hex_link(offset)`,
  `ui::section(label)`, and `ui::dim(...)`.
- Embed the sans, mono, and icon fonts. Use mono for tables, trees of data,
  hex, and the script listing.
- Add DPI detection, the scale override, and a font rebuild on scale change.
- Replace existing inline colors (the chunk-tree warning orange, the script
  comment green, the clump-size gradient) with tokens.

### R-03: command registry, shortcuts, palette

- Add a `Command { id, label, category, shortcut, enabled(), run() }` registry.
  Menus, toolbar buttons, keyboard handling, and Help all read from it, so the
  Help menu can never drift from the real bindings again.
- Rationalize the shortcuts:

  | Action | Binding |
  |---|---|
  | Command palette | `Ctrl+Shift+P` |
  | Go to anything (actor, script, chunk, class, ID) | `Ctrl+P` |
  | Workspaces Mission / Script / Anim / Scene / Geometry / Hex | `Ctrl+1` … `Ctrl+6` |
  | Back / forward | `Alt+←` / `Alt+→` (and mouse buttons 4/5) |
  | Frame selection / frame all | `F` / `Home` |
  | Toggle Explorer / Inspector / bottom dock | `Ctrl+B` / `Ctrl+I` / `Ctrl+J` |
  | Maximize viewport | `Ctrl+Space` (keep) |
  | Copy selection identity | `Ctrl+Shift+C` |
  | Open / open mission / save copy | `Ctrl+O` / `Ctrl+Shift+O` / `Ctrl+S` (keep) |

  Bare `1`–`4` remain available as viewport-only aliases while the viewport is
  hovered, for existing muscle memory.
- The palette is a centered popup with a `>` prompt. It uses a fuzzy matcher
  (subsequence scoring with bonuses for word starts and exact ID or hex
  matches). It searches commands and every indexed symbol: actors, nav groups,
  scripts, variables, BDD classes, chunks by offset, and resources. Typing
  `0x…` jumps to an offset, and `#17` jumps to an entry.
- Build the search index once per load, not per frame. The same index replaces
  the per-frame script haystack in the Explorer.

### R-04: selection model, history, cross-reference navigation

- Unify selection as a tagged `SelectionRef` (chunk offset, scene instance,
  mission entry, program script/instruction, database record, resource path).
  The Explorer, Viewport, Inspector, and status bar all read it.
- Add a bounded back/forward stack of `{workspace, SelectionRef, camera}`.
  Every "go to" action pushes to it.
- Add a breadcrumb at the top of the inspector, for example
  `FR01.scn › Actors › Guard_01 › class 0x4A1 › Objetos.bdd#88`, with each
  segment clickable.
- Follow links across workspaces: clicking an actor operand in a script selects
  and frames the actor in Mission. "Used by" rows open the script at the
  instruction. The hex view scrolls to and highlights the byte range.

### R-05: inspector as property grids

- Replace `TextWrapped` / `BulletText` blocks with a two-column
  `ui::PropertyGrid` table: dim key, mono value, and an optional trailing badge
  or action.
- Each value row has:
  - a provenance badge: ✓ proven, ~ inferred, ? unknown/raw, ⚠ diagnostic.
    Hovering explains the evidence (source entry, candidate count).
  - copy-on-click (the whole value, or `Ctrl+Shift+C` for a full identity string
    like `FR01.scn:entry 1234 @0x2C79D6`);
  - a hex/dec toggle for integers, degrees/radians for angles, and a vector
    display with per-component copy;
  - a "raw" disclosure that shows the underlying CSFFBS node or chunk bytes in
    place.
- Collapsing sections (Identity, Transform, References, Resources, Physics,
  Animation, Raw) remember their open state per selection kind.
- Add a pin button (📌) that locks the inspector to the current selection, and
  a duplicate button (⧉) that opens a second inspector window for comparing two
  records side by side.
- Leave a stub for the editable-row variant needed by Phase 06 (a lock icon on
  read-only fields and an inline editor on schema-registry fields), but don't
  enable any editing in this sprint.

### R-06: explorer consistency

- Use the same search prompt, filter chips, and count-in-dim-text style in
  every workspace.
- Mark tree rows with icons by kind, a diagnostic dot, and a dirty dot, and
  color them only through tokens.
- Add right-click context menus: Frame, Isolate, Copy ID, Copy path, Show in
  hex, Show references, Export (where applicable). Every entry comes from the
  command registry.
- Add a **Resources** section that lists the mission graph (resolved, missing,
  ambiguous), so Phase 02's dependency data is visible in the GUI and not only
  in `csf-info`.
- Keep the chunk tree's clump-size coloring, but add a legend, and add an
  option to turn it off.

### R-07: start page, file access, settings

- Add a cross-platform native file dialog through `portable-file-dialogs`
  (single header, zenity/kdialog on Linux, Win32 on Windows), so Open works on
  Linux. Keep the existing Win32 path until parity is confirmed.
- Add a **resource root** setting. Once it is set, the start page and a Resource
  browser panel list the discovered missions (`Maps/*/…*.scn`) and let you
  filter and open them without a dialog.
- Replace the centered "Drop a file here" empty state with a start page that
  shows recent missions and files (generalizing `recent-pairings.txt` into a
  settings file), the discovered missions under the root, a keybinding cheat
  sheet generated from the registry, and the build/version stamp.
- Add a Preferences window: resource root, UI scale, theme, viewport defaults
  (move speed, invert Y, default view style), and a confirm-before-overwrite
  export policy. The policy must remain "new files only" by default.

### R-08: viewport quality of life

- Add the floating icon toolbar and the View popover (see layout). Keep the
  compact mode for narrow docks.
- Add the axis gizmo with click-to-orthographic view, plus numpad `1`/`3`/`7`
  views and `5` to toggle perspective.
- Add a stats HUD with fps, drawn triangles, selection, and camera position. It
  can be turned off.
- Add camera bookmarks per mission (`Ctrl+Shift+1…9` to store, `Shift+1…9` to
  recall), saved in settings keyed by the mission's content signature.
- Add hover tooltips over overlay points that show kind, name, and ID before
  clicking.
- Add screenshots to PNG (`F12`). This needs FBO readback plus
  `stb_image_write`. Put the writer in `rws_image`, not in the GUI target, to
  respect the rule that the GUI does not decode or encode images.
- Add a Measure mode readout panel and clip-plane gizmos. Both move from the
  tool panel into the viewport.

### R-09: console, diagnostics, progress

- Add a **Console** panel: a timestamped, severity-colored ring buffer of
  operations such as load, pair, export, and save. Every current `status = ...`
  assignment becomes `log(level, message)`. It can copy all lines or save them
  to a new file. The status bar shows the latest line.
- Show short toasts for success and failure of user-initiated actions (export,
  save copy), with an "open folder" action.
- Add a **Diagnostics** panel: a sortable, filterable table (severity, file,
  entry, offset, message) that merges RWS, CSFFBS, mission, and navigation
  diagnostics. Clicking a row navigates through R-04.
- Load missions in the background. CPU-side parsing and resolution run on a
  worker thread, GPU uploads happen on the main thread, and a progress overlay
  shows the stage (resolve, parse SCN, parse map, textures, programs) with a
  Cancel button. The existing transactional load (the old state is kept on
  failure) must be preserved.

## Out of scope

- Any new parser, resolver, or format semantics.
- Enabling edits in the GUI. That remains gated by Phase 06; this sprint only
  prepares the slots (Changes panel, lock/edit row variant).
- ImGui multi-viewport (panels as separate OS windows).
- Rewriting the renderer, adding PBR, or other visual-fidelity work in the
  viewport.
- A node-graph or visual script editor.
- Renaming the product or executable.

## Testing and verification

- GUI-free helpers go into a small static library (for example
  `rwsman_ui_model`) that has no ImGui, GLFW, or GL dependency, so the core-only
  build and tests can cover them: the fuzzy matcher and ranking, the command
  registry (conflict detection for duplicate shortcuts), the back/forward
  history, settings serialization and round trip, and search index
  construction. Add the tests to `tests/document_tests.cpp` according to the
  repository guide.
- R-00 is checked by manual parity against the pre-split build on both ST05 and
  one mission from `../CSF_unpacks`, plus the existing test suite.
- Keep a manual checklist in the PR for each item: 1080p, 1440p at 150%, and
  4K at 200%; Windows and Linux (X11 and Wayland); a first launch with no
  settings file; a corrupted settings file (fall back to defaults and warn in
  the Console).
- Update the scene preview screenshot in `docs/images/` and the README GUI
  section when the shell lands.
- Keep `build.sh`/`build.ps1`, CMake presets, and README commands in sync if the
  new font or dialog dependencies change the fetch steps.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| The R-00 refactor regresses subtle load/selection behavior | Do it first, with no visual change, as its own PR, verified side by side |
| The docking branch diverges from tagged releases | Pin the exact `-docking` tag, the same way the current pin works |
| The `AddCallback` viewport misrenders under docking or overlays | Fall back to FBO rendering (also needed for screenshots) |
| The threaded load races with GL resource ownership | Worker produces CPU-only data. GL uploads stay on the main thread. Swap state atomically at frame start |
| Font/icon licensing | Use only OFL/MIT fonts and record their licenses beside the vendored files |
| Theme colors imply certainty that doesn't exist | Provenance badges come only from resolver/scene status fields, never from UI heuristics |
| Scope creep into authoring | The Changes panel and edit rows stay stubs until the Phase 06 GUI work picks them up |

## Suggested sequencing

| Milestone | Items | User-visible result |
|---|---|---|
| M1: foundation | R-00, R-01, R-02 | Resizable, persistent, themed, crisp at any DPI |
| M2: navigation | R-03, R-04 | Palette, go-to-anything, back/forward, consistent shortcuts |
| M3: reading data | R-05, R-06 | Property-grid inspector with provenance, uniform explorer, context menus |
| M4: comfort | R-07, R-08, R-09 | Start page, Linux file dialogs, viewport QoL, console/diagnostics, background load |

## Definition of done

- Every existing workflow (Scene, Geometry, Hex/Inspector, Mission, Script,
  Animation, exports, collision pairing, save copy) still works and is reachable
  from menus, the palette, and a shortcut.
- Layout, theme, scale, recent files, and resource root persist in the per-user
  config directory. Nothing is written to the working directory.
- No ImGui color literals remain outside `app/ui/theme.cpp`.
- Every inspector value can be copied and shows its provenance. Every
  source-backed value can reveal its raw bytes.
- Open and Open mission work on Linux without the command line.
- Loading a full mission does not freeze the window and can be cancelled.
- `./test.sh` and `./test.ps1` pass, including the new UI-model tests.

## Implementation status (2026-09-22)

All items R-00 to R-09 are implemented in the working tree. Notes on how each was
realized, and where it deliberately falls short of the text above:

- **R-00.** `run_app` is now `app/main.cpp` (startup, frame loop, developer
  options), `AppState`, `app/app_actions.cpp`, `app/mission_loader.cpp`, and
  `draw_*` entry points in `app/ui/`. `GeometryPreview` only gained hooks (camera
  snapshot, frame selection, projection, HUD/gizmo drawing).
- **R-01.** Docking uses `v1.91.9b-docking`. Per-workspace layouts are separate
  dockspaces with workspace-scoped window IDs (`Explorer###mission.explorer`), so
  ImGui persists all of them in one `layout.ini` in the per-user config directory.
  The viewport still renders through `AddCallback`; no FBO was needed. The stray
  repository-root `imgui.ini` was deleted (it stays in `.gitignore`).
- **R-02.** Theme tokens (plus a high-contrast palette) and the viewport palette are
  in `app/ui/theme.cpp`; no ImGui color literals remain elsewhere. Fonts are IBM Plex
  Sans/Mono and a Lucide subset, embedded compressed. Text is rasterized at
  `OS scale x user scale x framebuffer scale` and rebuilt when any of them changes.
- **R-03 / R-04.** One command registry; `SelectionRef` is derived each frame from the
  viewport, Explorer, and script-outline slots (whichever changed last), and every
  change pushes a bounded history entry with workspace and camera. Mouse buttons 4/5
  and `Alt+Left/Right` navigate it.
- **R-05.** Provenance badges come from typed-field parsing, resolver statuses, and
  association evidence, never from UI heuristics. Raw-byte disclosure is per field for
  CSFFBS mission records (the field's own entry bytes) and per record for chunks,
  scene instances, scripts, and instructions. The decoded-chunk section still renders
  the existing text decoders; those lines are not individual property rows yet.
- **R-06.** Filters are substring matches over a per-load lowercase index; the
  Explorer no longer rebuilds script text per frame. The dirty marker uses the byte
  edits recorded in the Hex workspace.
- **R-07.** Linux uses portable-file-dialogs (non-blocking, polled); Windows keeps the
  Win32 open dialogs and uses portable-file-dialogs only for the resource-root folder
  picker (not compiled or run on Windows in this pass). The default export policy
  never replaces an existing file.
- **R-08.** Clip-plane "gizmos" are translucent outlines at each enabled plane, with
  the controls in the toolbar's Clip popover; they are not draggable handles.
- **R-09.** Mission loading is one worker thread producing CPU-only data; the main
  thread commits it atomically. Cancel is checked between stages and per node/actor,
  so a cancel can wait for the resource-graph build to reach its next checkpoint.
