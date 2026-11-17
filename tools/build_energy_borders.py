"""Fronteiras TERRESTRES entre nações, derivadas do raster de donos (E5).

Método (determinístico, sem dado externo):
  * mapa/map_data/atlas/world_province.png — id da província por pixel
    (id = R + G*256), 8192x4096 sobre o mundo equirretangular;
  * mapa/map_data/atlas/world_sdf.png — canal R = 128 + d*2 texels da costa:
    R > 128 é TERRA. É o que separa fronteira terrestre de fronteira marítima
    (o raster de províncias preenche o mar com a província de terra mais
    próxima, então sem a máscara de terra um estreito viraria "fronteira");
  * dois pixels de TERRA vizinhos (4-vizinhança) com províncias diferentes
    contam um trecho de fronteira. Cada contato é convertido em km:
    passo em x = 360/8192 grau x 111,32 km x cos(latitude do pixel);
    passo em y = 180/4096 grau x 110,57 km;
  * pares com menos de MIN_BORDER_KM de fronteira somada são descartados
    (contato de um pixel em ilha/istmo é ruído de raster, não frontier).

Só entram pares de NAÇÕES JOGÁVEIS (tag de nations.json, via
province_tag: XKX/KOS e SSD/SDS). Pares com nação não jogável são contados
em meta.excluded_non_playable.

Uso: python tools/build_energy_borders.py
Saída: data/definitions/energy/land_borders.json
"""

from __future__ import annotations

import math
import os
import sys

import numpy as np

import build_energy_common as ec

ROOT = ec.ROOT
OUT = os.path.join(ROOT, "data", "definitions", "energy", "land_borders.json")

MIN_BORDER_KM = 30.0
CHUNK_ROWS = 256
KM_PER_DEG_LAT = 110.574
KM_PER_DEG_LON = 111.320


def accumulate(ids, land, nations, pairs):
    height, width = ids.shape
    step_lon_deg = 360.0 / width
    step_lat_deg = 180.0 / height
    for y0 in range(0, height - 1, CHUNK_ROWS):
        y1 = min(height - 1, y0 + CHUNK_ROWS)
        rows = np.arange(y0, y1)
        lat = 90.0 - (rows + 0.5) * step_lat_deg
        cos_lat = np.cos(np.deg2rad(lat))[:, None]
        km_x = step_lon_deg * KM_PER_DEG_LON * cos_lat
        km_y = step_lat_deg * KM_PER_DEG_LAT

        # Horizontal: (y, x) x (y, x+1)
        left = ids[y0:y1, :-1]
        right = ids[y0:y1, 1:]
        mask = land[y0:y1, :-1] & land[y0:y1, 1:] & (left != right) & (left > 0) & (right > 0)
        if mask.any():
            a = np.minimum(left[mask], right[mask]).astype(np.int64)
            b = np.maximum(left[mask], right[mask]).astype(np.int64)
            key = a * 65536 + b
            uniq, counts = np.unique(key, return_counts=True)
            for k, c in zip(uniq.tolist(), counts.tolist()):
                pairs[(k // 65536, k % 65536)] = pairs.get((k // 65536, k % 65536), 0.0) + c * float(km_x.mean())

        # Vertical: (y, x) x (y+1, x)
        top = ids[y0:y1, :]
        bottom = ids[y0 + 1:y1 + 1, :]
        mask = land[y0:y1, :] & land[y0 + 1:y1 + 1, :] & (top != bottom) & (top > 0) & (bottom > 0)
        if mask.any():
            a = np.minimum(top[mask], bottom[mask]).astype(np.int64)
            b = np.maximum(top[mask], bottom[mask]).astype(np.int64)
            key = a * 65536 + b
            uniq, counts = np.unique(key, return_counts=True)
            for k, c in zip(uniq.tolist(), counts.tolist()):
                pairs[(k // 65536, k % 65536)] = pairs.get((k // 65536, k % 65536), 0.0) + c * float(km_y)


def main():
    provinces = ec.load_provinces()
    province_nation = {p["id"]: p["nation"] for p in provinces}
    tags, _ = ec.load_registry()
    playable_tags = set(tags)

    ids = ec.load_world_province_ids()
    land = ec.load_world_land_mask()
    pairs = {}
    accumulate(ids, land, province_nation, pairs)
    del ids, land

    totals = {}
    excluded_non_playable = 0
    excluded_small = 0
    province_pairs = 0
    for (pid_a, pid_b), km in pairs.items():
        nat_a = province_nation.get(pid_a, "")
        nat_b = province_nation.get(pid_b, "")
        if not nat_a or not nat_b or nat_a == nat_b:
            continue
        province_pairs += 1
        tag_a = nat_a if nat_a in playable_tags else ""
        tag_b = nat_b if nat_b in playable_tags else ""
        if not tag_a or not tag_b:
            excluded_non_playable += 1
            continue
        if km < MIN_BORDER_KM:
            excluded_small += 1
            continue
        key = (tag_a, tag_b) if tag_a < tag_b else (tag_b, tag_a)
        entry = totals.setdefault(key, {"km": 0.0, "province_pairs": 0})
        entry["km"] += km
        entry["province_pairs"] += 1

    borders = []
    for (tag_a, tag_b), entry in sorted(totals.items()):
        borders.append({
            "a": tag_a,
            "b": tag_b,
            "length_km": round(entry["km"], 1),
            "province_pairs": entry["province_pairs"],
            "method": "atlas_province_owner_raster",
            "confidence": "medium",
        })

    neighbors = {}
    for border in borders:
        neighbors.setdefault(border["a"], []).append(border["b"])
        neighbors.setdefault(border["b"], []).append(border["a"])
    degrees = {tag: len(v) for tag, v in sorted(neighbors.items())}

    payload = {
        "meta": {
            "generated_by": "tools/build_energy_borders.py",
            "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E5",
            "method": ("4-vizinhança entre pixels de TERRA (world_sdf R>128) com "
                       "províncias diferentes; km por passo de pixel com cosseno da "
                       "latitude; pares abaixo de %.0f km descartados" % MIN_BORDER_KM),
            "sources": {
                "province_owner_raster": "map/map_data/atlas/world_province.png "
                                         "(id = R + G*256, 8192x4096)",
                "land_mask": "map/map_data/atlas/world_sdf.png, canal R (128 + d*2, >128 = terra)",
                "provinces": "map/map_data/atlas/provinces.json (nation/tag)",
            },
            "rules": {
                "no_maritime": "fronteira marítima NÃO entra: exige os dois lados em terra",
                "playable_only": "pares com nação fora de nations.json ficam de fora",
                "min_border_km": MIN_BORDER_KM,
            },
            "license": ("sem dado de terceiro além do Natural Earth (domínio público, "
                        "via o pipeline do atlas); world_province/world_sdf e provinces.json "
                        "são artefatos do próprio projeto"),
            "accuracy": ("comprimento contado em arestas de pixel (4-vizinhança): "
                         "fronteira diagonal é superestimada (CAN-USA 9.841 km vs ~8.900 "
                         "oficial); a LISTA de pares é o dado, o km é aproximado (±20%)"),
            "counts": {
                "borders": len(borders),
                "nations": len(degrees),
                "province_pairs": province_pairs,
                "excluded_non_playable": excluded_non_playable,
                "excluded_below_minimum": excluded_small,
            },
            "usage": "E5: venda de energia só entre redes vizinhas com fronteira terrestre; "
                     "recalcular quando o território muda (mesmo raster).",
        },
        "borders": borders,
        "neighbors": degrees,
    }
    ec.write_json(OUT, payload)

    print("fronteiras: %d pares | nações: %d | pares de província: %d"
          % (len(borders), len(degrees), province_pairs))
    print("excluídos: %d sem nação jogável | %d abaixo de %.0f km"
          % (excluded_non_playable, excluded_small, MIN_BORDER_KM))
    for tag, count in sorted(degrees.items(), key=lambda kv: -kv[1])[:8]:
        print("  %-4s %2d vizinhos: %s" % (tag, count, ", ".join(sorted(neighbors[tag]))))
    for name in ("USA", "BRA", "NOR", "CHN", "DEU"):
        mine = [b for b in borders if name in (b["a"], b["b"])]
        print("  %s: %d fronteiras, %.0f km no total"
              % (name, len(mine), sum(b["length_km"] for b in mine)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
