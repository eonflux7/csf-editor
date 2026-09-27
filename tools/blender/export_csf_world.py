"""Export the visible mesh objects of a Blender scene to a `.csfworld` file.

    blender --background scene.blend --python tools/blender/export_csf_world.py -- out.csfworld [--precise]

The exporter lives in the csf_authoring add-on (csf_authoring/csfworld.py,
conventions in its docstring); this script runs it headless. Objects the
add-on loaded as reference are skipped. `--precise` writes nine significant
digits instead of six (use it for an edited shipped map).
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from csf_authoring.csfworld import export  # noqa: E402,F401  (terrain.py calls export())


def main(argv: list[str]) -> int:
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    precise = "--precise" in args
    args = [a for a in args if a != "--precise"]
    if len(args) != 1:
        print("usage: blender --background scene.blend --python export_csf_world.py -- out.csfworld [--precise]",
              file=sys.stderr)
        return 2
    stats = export(args[0], precise=precise)
    print(f"csfworld\t{args[0]}\t{stats['faces']} faces\t{stats['materials']} materials")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
