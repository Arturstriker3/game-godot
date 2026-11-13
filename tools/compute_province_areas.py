#!/usr/bin/env python3
"""Calcula a área (km²) de cada província admin-1 (atlas) e grava em
data/definitions/natural/province_areas.json.

Método idêntico a tools/compute_territory_areas.py, aplicado às províncias do
atlas (map/map_data/atlas/provinces.json, ~4.529 admin-1) em vez dos territórios
naturais (nature.json, 739):

- cada pixel de TERRA do nav_mask (>=128) é atribuído à província de centróide
  mais próximo (célula de Voronoi do sítio);
- a área de cada célula usa a projeção equirretangular (cos(lat)), com
  R = 6371 km.

A unidade de captação agrícola por cidade passa a ser a PROVÍNCIA (mais fina),
então a área vem da própria província. É uma estimativa honesta (ordem de
grandeza), coerente com o método dos territórios. Determinística. Requer
numpy/scipy/PIL.

FALLBACK (2026-11-13): a célula de Voronoi de uma província pode ficar vazia —
ilha menor que a célula de 16 km do nav_mask (Barbados, Granada, São Cristóvão,
São Vicente) ou província minúscula cujo centróide vizinho está mais perto de
todos os pixels de terra (municípios da Eslovênia). Área 0 zera a aptidão
agrícola da cidade (`city_crop_suitability` pondera por área × cultivável) e a
cobertura florestal. Para essas províncias a área vem do dado real do Natural
Earth admin-1 já gravado em `provinces.json` (`area_global`, contagem de pixels
da camada mundial 5000×2500), convertida para km² na MESMA projeção
equirretangular (R=6371 km, cos(lat)). Só entra quando a célula de Voronoi é 0,
então a área do raster nunca é substituída; a contagem e a soma do fallback vão
declaradas em `_meta`.

Uso: python tools/compute_province_areas.py
"""
import json
import math
import os

import numpy as np
from PIL import Image
from scipy.spatial import cKDTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROVINCES = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
NAV = os.path.join(ROOT, "map", "map_data", "nav_mask.png")
OUT = os.path.join(ROOT, "data", "definitions", "natural", "province_areas.json")

RADIUS_KM = 6371.0
## Passo de um pixel do plano global do jogo (5000×2500 px sobre 360°×180°).
GLOBAL_PX_DEG = 360.0 / 5000.0


def natural_earth_area_km2(province: dict) -> float:
    """Área real (km²) da província a partir de `provinces.json.area_global`.

    `area_global` é a contagem de pixels da província na camada mundial do
    atlas (Natural Earth admin-1 rasterizado, 5000×2500 px). A conversão usa a
    mesma projeção equirretangular do Voronoi (R = 6371 km, cos(lat) no
    centróide), então o fallback fala a mesma língua do raster."""
    area_px = float(province.get("area_global", 0.0))
    if area_px <= 0.0:
        return 0.0
    lat_deg = 90.0 - float(province.get("y", 0.0)) / 2500.0 * 180.0
    dlon = math.radians(GLOBAL_PX_DEG)
    dlat = math.radians(GLOBAL_PX_DEG)
    return area_px * (RADIUS_KM ** 2) * math.cos(math.radians(lat_deg)) * dlon * dlat


def main() -> None:
    provinces = json.load(open(PROVINCES, encoding="utf-8"))["provinces"]
    nav = Image.open(NAV).convert("L")

    pids = []
    centroids = []
    by_id = {}
    for entry in provinces:
        x = entry.get("x")
        y = entry.get("y")
        if x is None or y is None:
            continue
        pids.append(int(entry["id"]))
        centroids.append((float(x), float(y)))
        by_id[int(entry["id"])] = entry

    nav_arr = np.asarray(nav)
    ys, xs = np.where(nav_arr >= 128)
    if ys.size == 0:
        areas = {pid: 0.0 for pid in pids}
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
        areas = np.bincount(idx, weights=cell_area, minlength=len(pids))
        areas = {pid: float(areas[i]) for i, pid in enumerate(pids)}

    # Fallback declarado: província cujo raster não deu célula recebe a área
    # real do Natural Earth admin-1 (cos(lat) aplicado). Nunca sobrescreve área
    # positiva do raster.
    fallback = {}
    for pid, value in areas.items():
        if value > 0.0:
            continue
        real = natural_earth_area_km2(by_id.get(pid, {}))
        if real > 0.0:
            areas[pid] = real
            fallback[pid] = real

    out = {
        "_meta": {
            "source": "tools/compute_province_areas.py",
            "method": "Voronoi dos centróides sobre o nav_mask (equirretangular, R=6371 km)",
            "data_quality": "estimate_display",
            "units": "km2",
            "nav_mask": os.path.relpath(NAV, ROOT),
            "provinces": os.path.relpath(PROVINCES, ROOT),
            "province_count": len(pids),
            "provinces_from_natural_earth": len(fallback),
            "natural_earth_area_km2": round(sum(fallback.values()), 1),
            "fallback_note": (
                "província com célula de Voronoi vazia (ilha menor que a célula "
                "de 16 km do nav_mask ou província minúscula) usa a área real do "
                "Natural Earth admin-1 de provinces.json (area_global) na mesma "
                "projeção equirretangular; área 0 zera a aptidão agrícola da cidade"
            ),
        },
        "areas": {str(pid): round(v, 1) for pid, v in areas.items()},
    }
    with open(OUT, "w", encoding="utf-8") as handle:
        json.dump(out, handle, ensure_ascii=False, indent=1)
    total = sum(areas.values())
    print("províncias=%d  área_terra_total=%.0f km²  natural_earth=%d (%.0f km²)  -> %s"
          % (len(pids), total, len(fallback), sum(fallback.values()), OUT))


if __name__ == "__main__":
    main()
