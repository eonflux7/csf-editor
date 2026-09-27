#!/usr/bin/env python3
"""Textures of Country's own that no other map has: the Panzer III's painted
hull (the downloaded model is untextured).

    examples/country/textures.py OUT_DIR

Writes OUT_DIR/PZ3_HULL.png: 256x256, tiling, a worn panzer-grey steel plate
(RAL 7021 dark grey, rust and dirt streaks, faint plate seams), dark enough for
the game's doubled lighting like the shipped vehicle textures. The tracks use
Panzers' own `pcadena.dds` (copied by build.sh as PZ3_TRACK.dds).
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

GREY = np.array([47.0, 51.0, 52.0])
RUST = np.array([58.0, 40.0, 28.0])


def tiling_noise(rng: np.random.Generator, size: int, cells: int) -> np.ndarray:
    """Smooth value noise that wraps at the edges."""
    grid = rng.random((cells, cells))
    t = np.arange(size) * cells / size
    i0 = np.floor(t).astype(int) % cells
    i1 = (i0 + 1) % cells
    f = t - np.floor(t)
    f = f * f * (3 - 2 * f)
    rows = grid[i0][:, None, :] * (1 - f)[:, None, None] + grid[i1][:, None, :] * f[:, None, None]
    rows = rows[:, 0, :]
    return rows[:, i0] * (1 - f)[None, :] + rows[:, i1] * f[None, :]


def hull(size: int = 256) -> Image.Image:
    rng = np.random.default_rng(1941)
    grime = 0.5 * tiling_noise(rng, size, 4) + 0.3 * tiling_noise(rng, size, 16) + 0.2 * tiling_noise(rng, size, 64)
    rust = np.clip((tiling_noise(rng, size, 8) - 0.62) * 4.0, 0.0, 1.0)
    streaks = tiling_noise(rng, size, 32)[:, :1].repeat(size, axis=1).T  # vertical run-off
    shade = 0.8 + 0.35 * grime - 0.12 * streaks
    rgb = GREY[None, None, :] * shade[..., None]
    rgb = rgb * (1 - rust[..., None] * 0.6) + RUST[None, None, :] * rust[..., None] * 0.6
    seams = np.zeros((size, size))
    seams[::128, :] = 1.0
    seams[:, ::128] = 1.0
    rgb *= (1.0 - 0.35 * seams)[..., None]
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[3].strip(), file=sys.stderr)
        return 2
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    hull().save(out / "PZ3_HULL.png")
    print(f"textures\t{out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
