"""Inspeciona províncias no raster (cor-chave, pixels, bounding box)."""
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROV_TXT = os.path.join(ROOT, "data", "definitions", "provinces.txt")
PROV_PNG = os.path.join(ROOT, "map", "map_data", "provinces.png")


def load_provinces():
    prov = {}
    with open(PROV_TXT, encoding="utf-8") as handle:
        for line in handle:
            parts = line.strip().split(";")
            if len(parts) < 7:
                continue
            if parts[1] not in ("land", "lake"):
                continue
            prov[parts[0]] = tuple(int(v) for v in parts[2:7])  # r,g,b,x,y
    return prov


def main() -> int:
    ids = sys.argv[1:]
    if not ids:
        print("Uso: python inspect_provinces.py PRV000001 PRV000002 ...")
        return 1

    prov = load_provinces()
    img = np.asarray(Image.open(PROV_PNG).convert("RGB"))

    for pid in ids:
        if pid not in prov:
            print(f"{pid}: nao encontrado em provinces.txt")
            continue
        r, g, b, x, y = prov[pid]
        mask = np.all(img == np.array((r, g, b)), axis=2)
        count = int(mask.sum())
        if count == 0:
            print(f"{pid}: cor ({r},{g},{b}) meta ({x},{y}) -> SEM PIXELS no raster")
            continue
        rows = np.where(mask.any(axis=1))[0]
        cols = np.where(mask.any(axis=0))[0]
        print(
            f"{pid}: cor ({r},{g},{b}) meta ({x},{y}) pixels={count} "
            f"bbox y[{rows.min()}-{rows.max()}] x[{cols.min()}-{cols.max()}]"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
