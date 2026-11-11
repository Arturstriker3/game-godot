#!/usr/bin/env python3
"""Calcula a área (km²) de cada território do nature.json e grava em
data/definitions/natural/territory_areas.json.

Método idêntico a tools/build_resource_map.py::compute_territory_areas (não
duplicado ali, apenas reproduzido aqui para que o runtime leia a área sem
recomputar o Voronoi dos 3,1 M pixels do nav_mask a cada execução):

- cada pixel de TERRA do nav_mask (>=128) é atribuído ao território de
  centróide mais próximo (célula de Voronoi do sítio — coerente com a geração
  Voronoi legada dos territórios);
- a área de cada célula usa a projeção equirretangular (cos(lat)), com
  R = 6371 km.

É uma estimativa honesta (ordem de grandeza), coerente com o build_resource_map.
Determinística. Requer numpy/scipy/PIL.

Uso: python tools/compute_territory_areas.py
"""
import json
import math
import os

import numpy as np
from PIL import Image
from scipy.spatial import cKDTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATURE = os.path.join(ROOT, "data", "definitions", "natural", "nature.json")
NAV = os.path.join(ROOT, "map", "map_data", "nav_mask.png")
OUT = os.path.join(ROOT, "data", "definitions", "natural", "territory_areas.json")

RADIUS_KM = 6371.0


def main() -> None:
    nature = json.load(open(NATURE, encoding="utf-8"))["territories"]
    nav = Image.open(NAV).convert("L")

    tids = []
    centroids = []
    for tid, entry in nature.items():
        c = entry.get("centroid_px", [])
        if len(c) < 2:
            continue
        tids.append(tid)
        centroids.append((float(c[0]), float(c[1])))

    nav_arr = np.asarray(nav)
    ys, xs = np.where(nav_arr >= 128)
    if ys.size == 0:
        areas = {tid: 0.0 for tid in tids}
    else:
        # Centróides em coordenadas do nav_mask (metade da resolução do mapa).
        nav_centroids = np.array([(px / 2.0, py / 2.0) for px, py in centroids])
        tree = cKDTree(nav_centroids)
        points = np.column_stack((xs, ys)).astype(float)
        _, idx = tree.query(points, k=1)

        lat_deg = 90.0 - (ys / nav.height) * 180.0
        dlon = math.radians(360.0 / nav.width)
        dlat = math.radians(180.0 / nav.height)
        cell_area = (RADIUS_KM ** 2) * np.cos(np.deg2rad(lat_deg)) * dlon * dlat
        areas = np.bincount(idx, weights=cell_area, minlength=len(tids))
        areas = {tid: float(areas[i]) for i, tid in enumerate(tids)}

    out = {
        "_meta": {
            "source": "tools/compute_territory_areas.py",
            "method": "Voronoi dos centróides sobre o nav_mask (equirretangular, R=6371 km)",
            "data_quality": "estimate_display",
            "units": "km2",
            "nav_mask": os.path.relpath(NAV, ROOT),
            "nature": os.path.relpath(NATURE, ROOT),
            "territories": len(tids),
        },
        "areas": {tid: round(v, 1) for tid, v in areas.items()},
    }
    with open(OUT, "w", encoding="utf-8") as handle:
        json.dump(out, handle, ensure_ascii=False, indent=1)
    total = sum(areas.values())
    print("territórios=%d  área_terra_total=%.0f km²  -> %s" % (len(tids), total, OUT))


if __name__ == "__main__":
    main()
