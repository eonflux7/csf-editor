#!/usr/bin/env python3
"""Placeholder lightmaps for Country: no bake, but night lighting that matches
the shipped maps. Convoy's (FR03) ground lightmaps average about RGB 25/25/24
and Ransom's (FR01) 18/23/26 (the game doubles them); brighter ground reads as
day in game while the actors stay lit by the scene's night ambient.

    tools/country/lightmaps.py OUT_DIR

Writes OUT_DIR/COUNTRY_Lm.png (512x512 over the whole terrain, see terrain.py's
second UV set: moonlight by slope, soft shadows of the buildings and trees) and
OUT_DIR/COUNTRY_PROPS_Lm.png (a flat moonlit tone for the imported models,
whose vertices sample one texel). The FR01 buildings keep their own baked
lightmaps.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from layout import BUILDINGS, SIZE, TREE_SPECIES, height, trees  # noqa: E402

MOON = np.array([33.0, 36.0, 41.0])  # lit ground, before slope shading
SHADOW = 0.55                        # what a building's shadow leaves of it
MOON_DIRECTION = (0.4, -0.3)         # where shadows fall (x, z per cm of building height)
PROPS = (26, 28, 31)
TREE_SHADE = 0.72                    # what a canopy leaves of the moonlight


def terrain_lightmap(size: int = 512) -> Image.Image:
    cell = SIZE / size
    xs = -SIZE / 2 + (np.arange(size) + 0.5) * cell
    zs = SIZE / 2 - (np.arange(size) + 0.5) * cell  # row 0 is the north edge
    heights = np.array([[height(x, z) for x in xs] for z in zs])
    # Slope shading towards the moon (up and to the north-west).
    dhdx = np.gradient(heights, axis=1) / cell
    dhdz = -np.gradient(heights, axis=0) / cell
    normal = np.stack([-dhdx, np.ones_like(heights), -dhdz], axis=-1)
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    to_moon = np.array([-MOON_DIRECTION[0], 1.0, -MOON_DIRECTION[1]])
    to_moon /= np.linalg.norm(to_moon)
    shade = 0.55 + 0.45 * np.clip(normal @ to_moon, 0.0, 1.0) / to_moon[1]
    # Shadows: each footprint swept along the moon direction over its height.
    light = np.ones_like(heights)
    gx, gz = np.meshgrid(xs, zs)
    for piece in BUILDINGS:
        x0, z0, x1, z1 = piece.footprint()
        tall = piece.box[4] - piece.box[1]
        for t in np.linspace(0.0, 1.0, 12):
            dx, dz = MOON_DIRECTION[0] * tall * t, MOON_DIRECTION[1] * tall * t
            inside = (gx >= x0 + dx) & (gx <= x1 + dx) & (gz >= z0 + dz) & (gz <= z1 + dz)
            light[inside] = SHADOW
    radius_of = {pair: radius for pairs, radius in TREE_SPECIES.values() for pair in pairs}
    for ids, x, z, _ in trees():
        r = radius_of[ids] * 0.8
        light[(gx - x) ** 2 + (gz - z) ** 2 < r * r] *= TREE_SHADE
    # Soften the shadow edges (a small box blur).
    padded = np.pad(light, 2, mode="edge")
    light = sum(padded[i:i + size, j:j + size] for i in range(5) for j in range(5)) / 25.0
    rgb = MOON[None, None, :] * (shade * light)[..., None]
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip().splitlines()[5].strip(), file=sys.stderr)
        return 2
    out = Path(argv[1])
    out.mkdir(parents=True, exist_ok=True)
    terrain_lightmap().save(out / "COUNTRY_Lm.png")
    Image.new("RGB", (16, 16), PROPS).save(out / "COUNTRY_PROPS_Lm.png")
    print(f"lightmaps\t{out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
