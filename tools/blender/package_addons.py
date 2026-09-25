#!/usr/bin/env python3
"""Zip the Blender add-ons for Edit > Preferences > Add-ons > Install from Disk.

    python3 tools/blender/package_addons.py

Writes tools/blender/csf_authoring.zip and rws_lightmaps.zip (the portable
counterpart of build_addon.ps1). Entries are sorted and time-stamped 1980 so
an unchanged add-on packs to the same bytes.
"""

from __future__ import annotations

import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ADDONS = {"csf_authoring": ("__init__.py", "csfworld.py", "README.md"),
          "rws_lightmaps": ("__init__.py", "README.md")}


def main() -> int:
    for name, files in ADDONS.items():
        output = HERE / f"{name}.zip"
        with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
            for file in sorted(files):
                info = zipfile.ZipInfo(f"{name}/{file}", date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, (HERE / name / file).read_bytes())
        print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
