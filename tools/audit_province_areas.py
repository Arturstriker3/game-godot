#!/usr/bin/env python3
"""Audita data/definitions/natural/province_areas.json.

Duas checagens, ambas determinísticas e sem Godot:

1. (sempre) confronta a área de cada província com duas referências independentes:
   - a contagem no raster do atlas (`atlas/world_province.png` ∩ terra do
     `atlas/world_sdf.png` ∩ `nav_mask.png`, cos(lat), R = 6371 km) — a MESMA
     fonte que `tools/compute_province_areas.py` usa, então divergência aqui é
     sinal de arquivo desatualizado ou corrompido;
   - o admin-1 do Natural Earth já gravado em `atlas/provinces.json`
     (`area_global` × km²/px na projeção equirretangular), que é uma fonte
     externa ao raster.
   Lista as províncias com razão > 2× ou < 0,5× nas duas comparações.

2. (`--cities ARQUIVO_ANTIGO`) replica em Python a bacia de captação de
   `core/economy/geography_service.gd` (`assign_provinces`: a cidade da mesma
   nação mais próxima do centróide, restrita à mesma `landmass`; cidade sem terra
   recebe fração da província mais próxima da nação) e imprime quanto da ÁREA
   CULTIVÁVEL de cada cidade vinha do arquivo antigo e quanto vem do atual.
   Passe um JSON no formato de `province_areas.json` (ex.: extraído de
   `git show <commit>:data/definitions/natural/province_areas.json`).

Uso:
  python tools/audit_province_areas.py
  python tools/audit_province_areas.py --cities old_province_areas.json
"""
import argparse
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
AREAS = os.path.join(ROOT, "data", "definitions", "natural", "province_areas.json")
CITIES = os.path.join(ROOT, "data", "definitions", "scenario", "cities.json")
NATIONS = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
PROVINCE_NATURE = os.path.join(ROOT, "data", "definitions", "natural", "province_nature.json")

RADIUS_KM = 6371.0
GLOBAL_PX_DEG = 360.0 / 5000.0
## Tag do jogo -> tag `nation` do atlas admin-1 (ver geography_service.gd).
PROVINCE_TAG_OVERRIDES = {"XKX": "KOS", "SSD": "SDS"}
CITY_STATE_MIN_SHARE = 0.001


def load_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def natural_earth_area_km2(province):
    area_px = float(province.get("area_global", 0.0))
    if area_px <= 0.0:
        return 0.0
    lat_deg = 90.0 - float(province.get("y", 0.0)) / 2500.0 * 180.0
    step = math.radians(GLOBAL_PX_DEG)
    return area_px * (RADIUS_KM ** 2) * math.cos(math.radians(lat_deg)) * step * step


def raster_areas():
    """Contagem independente: id por pixel × km² do pixel (blocos de 512 linhas)."""
    raster = np.asarray(Image.open(PROVINCE_RASTER).convert("RGB"), dtype=np.int32)
    height, width = raster.shape[:2]
    ids = raster[:, :, 0] + raster[:, :, 1] * 256
    land = np.asarray(Image.open(WORLD_SDF).convert("RGBA"))[:, :, 0] > 128
    nav_land = np.asarray(Image.open(NAV).convert("L").resize((width, height),
                                                              Image.NEAREST)) >= 128
    lat_deg = 90.0 - (np.arange(height) + 0.5) / height * 180.0
    row_km2 = (RADIUS_KM ** 2) * np.cos(np.deg2rad(lat_deg)) \
        * math.radians(360.0 / width) * math.radians(180.0 / height)
    totals = np.zeros(256 * 256, dtype=np.float64)
    for y0 in range(0, height, 512):
        keep = land[y0:y0 + 512] & nav_land[y0:y0 + 512]
        if not keep.any():
            continue
        block_ids = ids[y0:y0 + 512][keep]
        weights = np.repeat(row_km2[y0:y0 + 512, None], width, axis=1)[keep]
        totals += np.bincount(block_ids, weights=weights, minlength=256 * 256)
    return {pid: float(totals[pid]) for pid in np.nonzero(totals)[0]}


def ratio(a, b):
    if b <= 0.0:
        return float("inf") if a > 0.0 else 1.0
    return a / b


def audit_areas(areas_path=AREAS):
    provinces = load_json(PROVINCES)["provinces"]
    areas = load_json(areas_path)
    values = {int(pid): float(v) for pid, v in areas["areas"].items()}
    print("auditando %s" % areas_path)
    raster = raster_areas()

    inflated, deflated, missing = [], [], []
    rows = []
    for entry in provinces:
        pid = int(entry["id"])
        value = values.get(pid, 0.0)
        counted = raster.get(pid, 0.0)
        real = natural_earth_area_km2(entry)
        rows.append((pid, entry.get("name") or "", entry.get("nation") or "",
                     value, counted, real))
        if counted <= 0.0:
            if value > 0.0:
                missing.append(pid)
            continue
        r = ratio(value, counted)
        if r > 2.0:
            inflated.append(rows[-1])
        elif r < 0.5:
            deflated.append(rows[-1])
    bad_ne = [row for row in rows
              if row[5] > 0.0 and (ratio(row[3], row[5]) > 2.0 or ratio(row[3], row[5]) < 0.5)]

    print("províncias=%d  soma_json=%.0f km²  soma_raster=%.0f km²"
          % (len(rows), sum(row[3] for row in rows), sum(row[4] for row in rows)))
    print("vs raster: infladas >2x=%d (+%.0f km²)  deficitárias <0.5x=%d (-%.0f km²)  sem raster=%d"
          % (len(inflated), sum(r[3] - r[4] for r in inflated),
             len(deflated), sum(r[4] - r[3] for r in deflated), len(missing)))
    print("vs Natural Earth admin-1: fora de [0.5x, 2x]=%d de %d"
          % (len(bad_ne), len(rows)))
    print("zeros/negativos no JSON: %d" % sum(1 for v in values.values() if v <= 0.0))

    print("\n-- 10 maiores excesso vs raster --")
    for row in sorted(inflated, key=lambda r: r[3] - r[4], reverse=True)[:10]:
        print("%6d %-28s %-4s json=%12.1f raster=%10.1f ne=%10.1f x%.2f"
              % (row[0], row[1][:28], row[2], row[3], row[4], row[5],
                 ratio(row[3], row[4])))
    return 0


def audit_cities(old_path):
    old = {int(pid): float(v) for pid, v in load_json(old_path)["areas"].items()}
    new = {int(pid): float(v) for pid, v in load_json(AREAS)["areas"].items()}
    cities_raw = load_json(CITIES)
    playable = {nation["id"] for nation in load_json(NATIONS)}
    provinces = load_json(PROVINCES)["provinces"]
    nature = load_json(PROVINCE_NATURE)["provinces"]

    cities = [{"id": c["id"], "tag": c.get("nation", ""), "x": float(c["x"]),
               "y": float(c["y"]), "landmass": c.get("landmass") or ""}
              for c in cities_raw]
    by_tag = {}
    for city in cities:
        by_tag.setdefault(city["tag"], []).append(city)
    tag_of = {PROVINCE_TAG_OVERRIDES.get(tag, tag): tag for tag in playable}

    def dist2(a, b):
        return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2

    def landmass_of(point):
        return min(cities, key=lambda c: dist2((c["x"], c["y"]), point))["landmass"]

    catchment = {}
    for entry in provinces:
        tag = tag_of.get(entry.get("nation") or "", "")
        if not tag:
            continue
        candidates = by_tag.get(tag, [])
        if not candidates:
            continue
        point = (float(entry["x"]), float(entry["y"]))
        landmass = landmass_of(point)
        same = [c for c in candidates if c["landmass"] == landmass] or candidates
        owner = min(same, key=lambda c: dist2((c["x"], c["y"]), point))
        catchment.setdefault(owner["id"], []).append(int(entry["id"]))

    def cultivable(pid):
        return float(nature.get(str(pid), {}).get("cultivable", 0.0))

    def city_area(city_id, areas):
        return sum(float(areas.get(pid, 0.0)) * cultivable(pid)
                   for pid in catchment.get(city_id, []))

    rows = []
    for city in cities:
        rows.append((city["id"], city["tag"], city_area(city["id"], old),
                     city_area(city["id"], new)))
    total_old = sum(r[2] for r in rows)
    total_new = sum(r[3] for r in rows)
    print("cidades com bacia=%d de %d" % (sum(1 for r in rows if r[2] > 0 or r[3] > 0), len(rows)))
    print("área cultivável somada: antigo=%.0f km²  atual=%.0f km²  (%+.1f%%)"
          % (total_old, total_new, 100.0 * (total_new - total_old) / total_old))
    print("\n-- 10 cidades que mais perdem área cultivável (km²) --")
    for row in sorted(rows, key=lambda r: r[2] - r[3], reverse=True)[:10]:
        pct = 100.0 * (row[2] - row[3]) / row[2] if row[2] > 0 else 0.0
        print("%-12s %-4s antes=%12.0f depois=%10.0f perde=%10.0f (%5.1f%%)"
              % (row[0], row[1], row[2], row[3], row[2] - row[3], pct))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cities", metavar="JSON_ANTIGO",
                        help="compara a área cultivável das cidades contra este "
                             "province_areas.json antigo")
    parser.add_argument("--areas", metavar="JSON", default=AREAS,
                        help="arquivo a auditar (padrão: province_areas.json)")
    args = parser.parse_args()
    audit_areas(args.areas)
    if args.cities:
        print()
        audit_cities(args.cities)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
