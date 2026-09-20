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
- Tests belong in `tests/document_tests.cpp`. Run `./test.ps1` (Windows) or
  `./test.sh` (Linux) after changes to parser, decoder, or export behavior.
- Locally available unpacked game resources live in `../CSF_unpacks`; treat them
  as read-only reference data and do not copy them into the repository.
- Preserve unknown/truncated RWS data and write modified assets to new files
  unless the user explicitly requests an overwrite.
- The worktree may contain ongoing user changes; preserve unrelated modifications.
