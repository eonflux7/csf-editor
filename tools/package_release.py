#!/usr/bin/env python3
"""Package a Release build of CSF Mission Editor for download.

    python3 tools/package_release.py BUILD_DIR CONFIG_DIR NAME OUT_DIR

BUILD_DIR is the CMake build tree (its _deps/ holds the fetched dependencies),
CONFIG_DIR the folder with the executables (build/Release), NAME the package
name (csf-editor-<version>-linux-x64). Writes OUT_DIR/NAME.tar.gz on Linux and
OUT_DIR/NAME.zip on Windows: the editor and the command-line tools, the Blender
add-ons and scripts the editor runs from beside itself (tools/blender), the
README, the licence and every bundled dependency's licence.
"""

from __future__ import annotations

import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROGRAMS = ("csf-editor", "csf-mod", "csf-info", "rws-info", "rws-corpus")
# Fetched dependency (its _deps/<name>-src folder) and the licence file to ship.
LICENSES = {
    "glfw": "LICENSE.md",
    "imgui": "LICENSE.txt",
    "stb": "LICENSE",
    "pfd": "COPYING",
    "zlib": "LICENSE",
}


def main() -> int:
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        return 2
    build, config, name, out = (Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4]))
    windows = sys.platform == "win32"
    stage = out / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    for program in PROGRAMS:
        binary = config / (program + (".exe" if windows else ""))
        if not binary.is_file():
            print(f"missing {binary}", file=sys.stderr)
            return 1
        shutil.copy2(binary, stage / binary.name)

    # The editor runs tools/blender/open_project.py (Edit in Blender) from beside itself.
    shutil.copytree(ROOT / "tools" / "blender", stage / "tools" / "blender",
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*_test.py", "smoke_test.py"))
    for document in ("README.md", "LICENSE"):
        shutil.copy2(ROOT / document, stage / document)

    licenses = stage / "licenses"
    licenses.mkdir()
    shutil.copy2(ROOT / "app" / "ui" / "fonts" / "LICENSES.md", licenses / "fonts.md")
    for dependency, file in LICENSES.items():
        source = build / "_deps" / f"{dependency}-src" / file
        if not source.is_file():
            print(f"missing {source}", file=sys.stderr)
            return 1
        shutil.copy2(source, licenses / f"{dependency}-{file}")

    if windows:
        archive = out / f"{name}.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as package:
            for path in sorted(stage.rglob("*")):
                package.write(path, Path(name) / path.relative_to(stage))
    else:
        archive = out / f"{name}.tar.gz"
        with tarfile.open(archive, "w:gz") as package:
            package.add(stage, arcname=name)
    print(archive)
    return 0


if __name__ == "__main__":
    sys.exit(main())
