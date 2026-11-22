#!/usr/bin/env python3
"""Calcula a área (km²) de cada província admin-1 (atlas) e grava em
data/definitions/natural/province_areas.json.

MÉTODO (2026-11-17, corrigido): conta os pixels da PRÓPRIA província no raster
do atlas, em vez de estimar a área por uma célula de Voronoi dos centróides
sobre o nav_mask.

- `map/map_data/atlas/world_province.png` (8192×4096, ~4,9 km/texel) guarda o id
  global da província por pixel (id = R + G*256);
- só contam pixels de TERRA: `world_sdf.png` canal R > 128 (mesma definição do
  shader) E `nav_mask.png` >= 128 (a terra navegável do jogo, sem lagos e sem as
  ilhas/nacões removidas pela curadoria);
- cada pixel vale R²·cos(lat)·dlon·dlat (equirretangular, R = 6371 km, a mesma
  projeção da camada mundial).

Por que o método mudou: o Voronoi dos centróides NÃO é restrito ao território da
província — cada pixel de terra vai para o centróide mais próximo em GRAUS.
Como 1° de longitude encolhe com cos(lat), no sul do planeta uma província a
-40° "vence" a Antártica inteira: a Tasmânia 1 recebia 1,78 milhão de km²
(217× a área real), a Antártida (id 1724) só ficava com 4,2 milhões de km² dos
12,2 milhões do continente, e Austrália, Chile, Nova Zelândia e Madagascar
herdavam o resto. Com a contagem no raster o erro desaparece por construção: a
área é a da própria província.

FALLBACK (2026-11-13, mantido): uma província pode não ter nenhum pixel no raster
— ilha menor que um texel ou província cuja subdivisão pegou uma faixa fina de
costa. Área 0 zera a aptidão agrícola da cidade (`city_crop_suitability` pondera
por área × cultivável) e a cobertura florestal. Para essas províncias a área vem
do dado real do Natural Earth admin-1 já gravado em `provinces.json`
(`area_global`, contagem de pixels da camada mundial 5000×2500), convertida para
km² na MESMA projeção equirretangular (R=6371 km, cos(lat)). Só entra quando a
contagem do raster é 0, então a área do raster nunca é substituída; a contagem e
a soma do fallback vão declaradas em `_meta`.

Uso: python tools/compute_province_areas.py
"""
import json
import math
import os

import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROVINCES = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
PROVINCE_RASTER = os.path.join(ROOT, "map", "map_data", "atlas", "world_province.png")
WORLD_SDF = os.path.join(ROOT, "map", "map_data", "atlas", "world_sdf.png")
NAV = os.path.join(ROOT, "map", "map_data", "nav_mask.png")
OUT = os.path.join(ROOT, "data", "definitions", "natural", "province_areas.json")

RADIUS_KM = 6371.0
## Passo de um pixel do plano global do jogo (5000×2500 px sobre 360°×180°).
GLOBAL_PX_DEG = 360.0 / 5000.0
## Id global da província cabe em R + G*256 (0..65535): acumula nesse espaço.
ID_SPACE = 256 * 256


def natural_earth_area_km2(province: dict) -> float:
    """Área real (km²) da província a partir de `provinces.json.area_global`.

    `area_global` é a contagem de pixels da província na camada mundial do
    atlas (Natural Earth admin-1 rasterizado, 5000×2500 px). A conversão usa a
    mesma projeção equirretangular da contagem (R = 6371 km, cos(lat) no
    centróide), então o fallback fala a mesma língua do raster."""
    area_px = float(province.get("area_global", 0.0))
    if area_px <= 0.0:
        return 0.0
    lat_deg = 90.0 - float(province.get("y", 0.0)) / 2500.0 * 180.0
    dlon = math.radians(GLOBAL_PX_DEG)
    dlat = math.radians(GLOBAL_PX_DEG)
    return area_px * (RADIUS_KM ** 2) * math.cos(math.radians(lat_deg)) * dlon * dlat


def count_raster_areas() -> tuple:
    """Área (km²) contada no raster do atlas -> ({id: km²}, largura, altura)."""
    raster = np.asarray(Image.open(PROVINCE_RASTER).convert("RGB"), dtype=np.int32)
    height, width = raster.shape[:2]
    ids = raster[:, :, 0] + raster[:, :, 1] * 256

    land = np.asarray(Image.open(WORLD_SDF).convert("RGBA"))[:, :, 0] > 128
    nav = Image.open(NAV).convert("L")
    # nav_mask está na resolução do plano (5000×2500); o raster do mundo é
    # 8192×4096, a mesma proporção. Vizinho-mais-próximo preserva a borda.
    nav_land = np.asarray(nav.resize((width, height), Image.NEAREST)) >= 128

    lat_deg = 90.0 - (np.arange(height) + 0.5) / height * 180.0
    row_km2 = (RADIUS_KM ** 2) * np.cos(np.deg2rad(lat_deg)) \
        * math.radians(360.0 / width) * math.radians(180.0 / height)

    totals = np.zeros(ID_SPACE, dtype=np.float64)
    block = 512
    for y0 in range(0, height, block):
        y1 = min(y0 + block, height)
        keep = land[y0:y1] & nav_land[y0:y1]
        if not keep.any():
            continue
        block_ids = ids[y0:y1][keep]
        weights = np.repeat(row_km2[y0:y1, None], width, axis=1)[keep]
        totals += np.bincount(block_ids, weights=weights, minlength=ID_SPACE)
    return {pid: float(totals[pid]) for pid in np.nonzero(totals)[0]}, width, height


def main() -> None:
    provinces = json.load(open(PROVINCES, encoding="utf-8"))["provinces"]
    by_id = {}
    order = []
    for entry in provinces:
        pid = int(entry["id"])
        if pid <= 0:
            continue
        by_id[pid] = entry
        order.append(pid)

    counted, width, height = count_raster_areas()
    areas = {pid: counted.get(pid, 0.0) for pid in order}

    # Fallback declarado: província sem pixel no raster recebe a área real do
    # Natural Earth admin-1 (cos(lat) aplicado). Nunca sobrescreve área positiva.
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
            "method": (
                "contagem dos pixels da própria província em "
                "atlas/world_province.png (id = R+G*256) mascarada por "
                "atlas/world_sdf.png (R>128) e nav_mask.png (>=128); "
                "equirretangular, R=6371 km, cos(lat) por linha"
            ),
            "data_quality": "estimate_display",
            "units": "km2",
            "raster": os.path.relpath(PROVINCE_RASTER, ROOT),
            "land_mask": os.path.relpath(WORLD_SDF, ROOT),
            "nav_mask": os.path.relpath(NAV, ROOT),
            "raster_size": [width, height],
            "provinces": os.path.relpath(PROVINCES, ROOT),
            "province_count": len(order),
            "provinces_from_natural_earth": len(fallback),
            "natural_earth_area_km2": round(sum(fallback.values()), 1),
            "fallback_note": (
                "província sem nenhum pixel no raster (ilha menor que um texel "
                "ou faixa fina de costa) usa a área real do Natural Earth "
                "admin-1 de provinces.json (area_global) na mesma projeção "
                "equirretangular; área 0 zera a aptidão agrícola da cidade"
            ),
        },
        "areas": {str(pid): round(v, 1) for pid, v in areas.items()},
    }
    with open(OUT, "w", encoding="utf-8") as handle:
        json.dump(out, handle, ensure_ascii=False, indent=1)
    total = sum(areas.values())
    print("províncias=%d  área_terra_total=%.0f km²  natural_earth=%d (%.0f km²)  -> %s"
          % (len(order), total, len(fallback), sum(fallback.values()), OUT))


if __name__ == "__main__":
    main()
