"""Corrige províncias francesas que invadem o sudoeste da Inglaterra.

No mapa demo do opengs, PRV006126 e PRV006127 têm duas partes disjuntas: a
Bretanha (correta) e um pedaço na Cornualha/Devon, separados pelo Canal da
Mancha (que ficou sem pixels entre eles). Este script reatribui apenas os
pixels ao norte do Canal para a província inglesa mais próxima.
"""

import json
import os

import numpy as np
from PIL import Image
from scipy.ndimage import distance_transform_edt

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROV_TXT = os.path.join(ROOT, "data", "definitions", "provinces.txt")
PROV_PNG = os.path.join(ROOT, "map", "map_data", "provinces.png")
PROV_JSON = os.path.join(ROOT, "data", "history", "provinces", "provinces.json")

# Províncias com o pedaço indevido.
TARGETS = {"PRV006126", "PRV006127"}
# y <= CUTOFF_Y corresponde a lat >= ~49.9 (ao norte do Canal da Mancha).
CUTOFF_Y = 557


def main() -> int:
    colors = {}
    for line in open(PROV_TXT, encoding="utf-8"):
        parts = line.strip().split(";")
        if len(parts) < 7 or parts[1] not in ("land", "lake"):
            continue
        colors[parts[0]] = (int(parts[2]), int(parts[3]), int(parts[4]))

    owners = {k: v["province_owner"] for k, v in json.load(open(PROV_JSON, encoding="utf-8")).items()}

    image = np.asarray(Image.open(PROV_PNG).convert("RGB")).copy()

    gbr_mask = np.zeros(image.shape[:2], dtype=bool)
    for pid, owner in owners.items():
        if owner == "GBR" and pid in colors:
            gbr_mask |= np.all(image == np.array(colors[pid]), axis=2)

    # Para cada pixel, a coordenada do pixel inglês mais próximo.
    _distances, nearest = distance_transform_edt(~gbr_mask, return_indices=True)

    moved = 0
    for pid in TARGETS:
        color = np.array(colors[pid])
        mask = np.all(image == color, axis=2)
        mask[CUTOFF_Y + 1:, :] = False  # preserva a parte correta (Bretanha)
        count = int(mask.sum())
        if count == 0:
            print(f"{pid}: nenhum pixel acima de y={CUTOFF_Y}")
            continue
        image[mask] = image[nearest[0][mask], nearest[1][mask]]
        moved += count
        print(f"{pid}: {count} pixels reatribuídos para províncias inglesas")

    Image.fromarray(image, "RGB").save(PROV_PNG)
    print(f"Total movido: {moved} pixels | provinces.png salvo")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
