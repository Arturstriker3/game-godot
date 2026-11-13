"""Audita cidades faltantes nas nações grandes/populosas (item 14).

Regra (docs/plans/world_all_data.md, §21):
  para cada nação com área >= 500.000 km² OU população >= 30 M, compara as
  cidades do cenário com as 10 maiores aglomerações de 2026-01-01
  (`tools/agglomerations_2026.csv`, snapshot de Thomas Brinkhoff,
  Major Agglomerations of the World, citypopulation.de) e lista as ausentes
  que tenham > 1.000.000 hab. e estejam a >= 150 km de QUALQUER cidade do
  cenário (distância geodésica no elipsoide, a mesma do build).

Não altera dados. A decisão de entrar no cenário é sempre explícita em
`tools/world_city_curation.py` (FORCED_CITIES / FORCED_PORTS).

Uso:
    python tools/audit_missing_cities.py
    python tools/audit_missing_cities.py --all
    python tools/audit_missing_cities.py --top=10 --min-pop=1000000 --min-km=150
"""
from __future__ import annotations

import argparse
import csv
import difflib
import math
import os
import re
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
WORLD = os.path.join(ROOT, "data", "definitions", "world")
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")
AGGLOMERATIONS = os.path.join(HERE, "agglomerations_2026.csv")

AREA_KM2 = 500_000.0
POP = 30_000_000


def key(text):
    text = unicodedata.normalize("NFKD", text)
    text = "".join(c for c in text if not unicodedata.combining(c))
    return re.sub(r"[^a-z0-9]", "", text.lower())


def haversine_km(lat1, lon1, lat2, lon2):
    r = 6371.0088
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def read_csv(path):
    with open(path, encoding="utf-8-sig", newline="") as fh:
        return list(csv.DictReader(fh))


# Coordenadas do centro urbano (Wikidata) para as aglomerações que não estão
# na fonte de cidades do cenário (as que entraram na r8 e as que ficaram de
# fora por < 150 km). Chave = nome/English normalizado.
COORDS = {
    # r8 — adicionadas
    "nuremberg": (49.4539, 11.0775),
    "nurnberg": (49.4539, 11.0775),
    "boston": (42.3603, -71.0578),
    "fez": (34.0333, -5.0000),
    "fes": (34.0333, -5.0000),
    "padua": (45.4078, 11.8733),
    "padova": (45.4078, 11.8733),
    "miami": (25.7742, -80.1936),
    "georgetown": (5.4145, 100.3292),
    "mendoza": (-32.8897, -68.8444),
    "samarkand": (39.6547, 66.9758),
    "samarqand": (39.6547, 66.9758),
    "cochabamba": (-17.3935, -66.1570),
    "bordeaux": (44.8378, -0.5794),
    "marrakesh": (31.6295, -7.9811),
    "marrakech": (31.6295, -7.9811),
    "konya": (37.8728, 32.4921),
    "krasnodar": (45.0333, 38.9833),
    "benguela": (-12.5500, 13.4167),
    "santacruz": (-17.7892, -63.1975),
    "xiamen": (24.4797, 118.0819),
    "kermanshah": (34.3167, 47.0686),
    "bucaramanga": (7.1186, -73.1161),
    "sendai": (38.2682, 140.8694),
    "antalya": (36.9081, 30.6956),
    "torreon": (25.5439, -103.4190),
    "cucuta": (7.9075, -72.5047),
    "agadir": (30.4214, -9.5831),
    "ciudadjuarez": (31.6685, -106.4200),
    "seville": (37.3886, -5.9950),
    "sevilla": (37.3886, -5.9950),
    "malaga": (36.7167, -4.4167),
    # casos de borda (>= 1 M, mas < 150 km) e nomes já presentes no cenário
    "basra": (30.5150, 47.8100),
    "najaf": (32.0290, 44.3396),
    "hofuf": (25.3608, 49.5997),
    "barquisimeto": (10.0678, -69.3467),
    "tangier": (35.7695, -5.8045),
    "tanger": (35.7695, -5.8045),
    "nice": (43.7102, 7.2620),
    "goma": (-1.6641, 29.2229),
    "bukavu": (-2.5000, 28.8667),
    "okayama": (34.6551, 133.9196),
    "vancouver": (49.2827, -123.1207),
    "valparaiso": (-33.0472, -71.6127),
    "cartagena": (10.3910, -75.4794),
    "mannheim": (49.4875, 8.4660),
    "hannover": (52.3759, 9.7320),
    "bielefeld": (52.0302, 8.5325),
    "southampton": (50.9097, -1.4044),
    "yogyakarta": (-7.7956, 110.3695),
    "surakarta": (-7.5755, 110.8243),
    "kirkuk": (35.4681, 44.3922),
    "kitakyushu": (33.8834, 130.8751),
    "hamamatsu": (34.7108, 137.7261),
    "ulsan": (35.5384, 129.3114),
    "changwon": (35.2280, 128.6811),
    "toluca": (19.2826, -99.6557),
    "angeles": (15.1450, 120.5880),
    "katowice": (50.2649, 19.0238),
    "namangan": (41.0011, 71.6673),
    "maracay": (10.2469, -67.5958),
    "sanfrancisco": (37.7749, -122.4194),
    "chittagong": (22.3569, 91.7832),
    "delhi": (28.6139, 77.2090),
    "hochiminh": (10.8231, 106.6297),
    "cebu": (10.3157, 123.8854),
    "queretaro": (20.5888, -100.3899),
    "leon": (21.1219, -101.6833),
    "tucuman": (-26.8241, -65.2226),
    "portelizabeth": (-33.9608, 25.6022),
    "donetsk": (48.0159, 37.8028),
    "newyork": (40.7128, -74.0060),
    "newcastle": (54.9783, -1.6178),
    "newcastleupontyne": (54.9783, -1.6178),
    "rawalpindi": (33.5971, 73.0479),
    "guadalajara": (20.6597, -103.3496),
    "saigon": (10.8231, 106.6297),
    "thanhphohochiminh": (10.8231, 106.6297),
    "rheinruhr": (51.4500, 7.0100),
}


def resolve_coords(row, source_by_nation, scen_by_nation):
    """(lat, lon, origem) da aglomeração; None se não houver coordenada."""
    names = [row["name"], row["english"]]
    for name in names:
        coords = COORDS.get(key(name))
        if coords:
            return coords[0], coords[1], "wikidata"
    for name in names:
        k = key(name)
        exact = [c for c in source_by_nation.get(row["iso3"], [])
                 if key(c["name_en"]) == k]
        if exact:
            c = max(exact, key=lambda r: int(float(r["population"] or 0)))
            return float(c["lat"]), float(c["lon"]), "source"
    # nome igual a uma cidade do cenário = já presente
    for name in names:
        k = key(name)
        for s in scen_by_nation.get(row["iso3"], []):
            if key(s["name"]) == k:
                return s["lat"], s["lon"], "scenario"
    # fuzzy na fonte (mesma nação)
    best, score = None, 0.0
    for c in source_by_nation.get(row["iso3"], []):
        for name in names:
            s = difflib.SequenceMatcher(None, key(c["name_en"]), key(name)).ratio()
            if s > score:
                best, score = c, s
    if best and score >= 0.86:
        return float(best["lat"]), float(best["lon"]), "source-fuzzy"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--min-pop", type=int, default=1_000_000)
    ap.add_argument("--min-km", type=float, default=150.0)
    ap.add_argument("--all", action="store_true",
                    help="imprime todas as top-N, não só as ausentes")
    ap.add_argument("--scenario", default=os.path.join(SCENARIO, "cities.csv"),
                    help="CSV de cidades do cenário a auditar (padrão: o atual)")
    args = ap.parse_args()

    nations = read_csv(os.path.join(WORLD, "nations_source.csv"))
    source = read_csv(os.path.join(WORLD, "cities_source.csv"))
    scenario = read_csv(args.scenario)
    agglomerations = read_csv(AGGLOMERATIONS)

    source_by_nation = {}
    for c in source:
        source_by_nation.setdefault(c["nation"], []).append(c)
    scen_by_nation = {}
    scen = []
    for row in scenario:
        x, y = float(row["x"]), float(row["y"])
        lat = 90.0 - y / 2500.0 * 180.0
        lon = x / 5000.0 * 360.0 - 180.0
        scen.append((row["id"], row["nation"], lat, lon))
        scen_by_nation.setdefault(row["nation"], []).append(
            {"id": row["id"], "name": row["name"], "lat": lat, "lon": lon})

    targets = [n for n in nations
               if float(n.get("area_km2") or 0) >= AREA_KM2
               or int(float(n.get("population") or 0)) >= POP]

    print("fonte: citypopulation.de, Major Agglomerations of the World "
          "(2026-01-01), snapshot tools/agglomerations_2026.csv")
    print(f"nações alvo (área >= {AREA_KM2:,.0f} km² ou pop >= {POP:,.0f}): "
          f"{len(targets)}")
    print()
    missing = 0
    unresolved = 0
    for nation in sorted(targets, key=lambda r: -int(float(r["population"]))):
        iso = nation["iso3"]
        top = [r for r in agglomerations if r["iso3"] == iso][: args.top]
        rows = []
        for row in top:
            coords = resolve_coords(row, source_by_nation, scen_by_nation)
            if coords is None:
                unresolved += 1
                if args.all:
                    rows.append((row, None, None))
                continue
            lat, lon, origin = coords
            near = min(scen, key=lambda s: haversine_km(lat, lon, s[2], s[3]))
            km = haversine_km(lat, lon, near[2], near[3])
            is_missing = (int(row["population"]) >= args.min_pop
                          and km >= args.min_km)
            if is_missing:
                missing += 1
            if args.all or is_missing:
                rows.append((row, near, km))
        if not rows:
            continue
        print(f"[{iso}] {nation['name_en']}")
        for row, near, km in rows:
            if near is None:
                print(f"  {row['name']:<24} {int(row['population']):>10,}  "
                      "(sem coordenada na fonte)")
                continue
            flag = ("AUSENTE" if int(row["population"]) >= args.min_pop
                    and km >= args.min_km else "coberto")
            print(f"  {flag:<7} {row['name']:<24} "
                  f"{int(row['population']):>10,}  {km:7.1f} km de "
                  f"{near[0]} ({near[1]})")
        print()
    print(f"ausentes (> {args.min_pop:,} hab. e >= {args.min_km:.0f} km): "
          f"{missing}")
    if unresolved:
        print(f"sem coordenada (provavelmente já presente/fora do roster): "
              f"{unresolved}")


if __name__ == "__main__":
    main()
