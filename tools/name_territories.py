"""Nomeia os territórios de nature.json com a região admin-1 mais próxima do
Natural Earth (localizada em pt/en/es), para a ficha não exibir "TRT000324".

Lê o shapefile ne_10m_admin_1_states_provinces (NE_DIR) e, para cada um dos 737
territórios, acha a região cujo ponto representativo (latitude/longitude do
próprio shapefile) está mais próximo do centróide. Grava name_pt/name_en/name_es
(e name nativo + adm0_a3) no nature.json.

Sem shapely: usa distância ao ponto representativo, não point-in-polygon.

Uso: python tools/name_territories.py
"""

import json
import math
import os

import shapefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATURE = os.path.join(ROOT, "data", "definitions", "natural", "nature.json")
NE_DIR = os.environ.get("NE_DIR", r"G:\tools\ne_extract")
SHP = os.path.join(NE_DIR, "ne_10m_admin_1_states_provinces",
                   "ne_10m_admin_1_states_provinces")


def pixel_to_lonlat(px, py):
    return px / 5000.0 * 360.0 - 180.0, 90.0 - py / 2500.0 * 180.0


def _clean(name):
    """Remove hífens do nome (ex.: 'Île-de-France' -> 'Île de France')."""
    return str(name).replace("-", " ")


def main():
    if not os.path.exists(SHP + ".shp"):
        raise SystemExit("shapefile admin-1 ausente em " + NE_DIR)

    sf = shapefile.Reader(SHP)
    fields = [f[0] for f in sf.fields[1:]]
    need = ["name", "name_pt", "name_en", "name_es", "adm0_a3", "latitude", "longitude"]
    missing = [f for f in need if f not in fields]
    if missing:
        raise SystemExit("campos ausentes no shapefile: " + str(missing))

    regions = []
    for record in sf.iterRecords():
        lat = record[fields.index("latitude")]
        lon = record[fields.index("longitude")]
        if lat is None or lon is None:
            continue
        regions.append({
            "name": record[fields.index("name")] or "",
            "name_pt": record[fields.index("name_pt")] or "",
            "name_en": record[fields.index("name_en")] or "",
            "name_es": record[fields.index("name_es")] or "",
            "adm0_a3": record[fields.index("adm0_a3")] or "",
            "lat": float(lat), "lon": float(lon),
        })

    data = json.load(open(NATURE, encoding="utf-8"))
    territories = data["territories"]

    named = 0
    for territory_id, entry in territories.items():
        centroid = entry.get("centroid_px", [])
        if len(centroid) < 2:
            continue
        lon, lat = pixel_to_lonlat(float(centroid[0]), float(centroid[1]))
        best = None
        best_dist = float("inf")
        for region in regions:
            dlon = lon - region["lon"]
            dlat = lat - region["lat"]
            dist = dlon * dlon + dlat * dlat
            if dist < best_dist:
                best_dist = dist
                best = region
        if best is None:
            continue
        entry["name"] = _clean(best["name"])
        entry["name_pt"] = _clean(best["name_pt"] or best["name"])
        entry["name_en"] = _clean(best["name_en"] or best["name"])
        entry["name_es"] = _clean(best["name_es"] or best["name"])
        entry["adm0_a3"] = best["adm0_a3"]
        named += 1

    text = json.dumps(data, indent="\t", ensure_ascii=False).replace("\n", "\r\n") + "\r\n"
    with open(NATURE, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("territórios nomeados:", named, "de", len(territories))


if __name__ == "__main__":
    main()
