"""Gera map/map_data/nav_mask.png a partir das provincias do mapa.

Cada celula cobre 2x2 px do provinces.png e vale:
  255 = celula toda terra
    0 = celula toda agua
  128 = celula de costa (mistura terra/agua)

O trade_convoys.gd usa essa mascara para caminhoes andarem so em terra e
navios so em agua. Rode novamente sempre que o mapa mudar.
"""
import io
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PROVINCES_TXT = ROOT / "data" / "definitions" / "provinces.txt"
PROVINCES_PNG = ROOT / "map" / "map_data" / "provinces.png"
OUTPUT = ROOT / "map" / "map_data" / "nav_mask.png"
BLOCK = 2


def load_water_colors() -> set:
    water = set()
    with io.open(PROVINCES_TXT, encoding="utf-8") as handle:
        for line in handle:
            parts = line.strip().split(";")
            if len(parts) < 8 or parts[1] not in ("ocean", "lake"):
                continue
            water.add((int(parts[2]), int(parts[3]), int(parts[4])))
    return water


def main() -> None:
    water = load_water_colors()
    pixels = np.asarray(Image.open(PROVINCES_PNG).convert("RGB"))

    flat = pixels.reshape(-1, 3)
    is_water = np.array([tuple(int(v) for v in px) in water for px in flat])
    land = ~is_water.reshape(pixels.shape[:2])

    height, width = land.shape
    blocks = land.reshape(height // BLOCK, BLOCK, width // BLOCK, BLOCK)
    fraction = blocks.mean(axis=(1, 3))
    mask = np.where(fraction >= 0.999, 255, np.where(fraction <= 0.001, 0, 128)).astype(np.uint8)

    Image.fromarray(mask, mode="L").save(OUTPUT)
    print("gerado %s (%dx%d)" % (OUTPUT, mask.shape[1], mask.shape[0]))


if __name__ == "__main__":
    main()
