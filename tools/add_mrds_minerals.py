"""Povoa globalmente os minerais sub-representados (ouro, chumbo, bauxita,
fosfato, tungstênio, terras raras, enxofre, diamante, urânio) a partir do MRDS.

O reclassify só re-rotula os depósitos já existentes; os registros MRDS dos
minerais novos foram descartados pelo classificador antigo. Este script os
re-adiciona, agrupados por (território, recurso, grade) — a mesma lógica do
build_deposits, mas com território por centróide mais próximo (não por
província, já que provinces.txt/history estão fora do repo).

Uso: python tools/add_mrds_minerals.py
Depois: python tools/build_resource_map.py
"""

import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import build_natural_geography as bng  # noqa: E402

DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")
NATURE = os.path.join(ROOT, "data", "definitions", "natural", "nature.json")
MRDS = os.path.join(ROOT, "tools", "data", "mrds_deposits.jsonl")

NEW_MINERALS = ["gold", "lead", "bauxite", "phosphate", "tungsten",
                "rare_earths", "sulfur", "diamond", "uranium"]


def main():
    nature = json.load(open(NATURE, encoding="utf-8"))["territories"]
    centroids = []
    for tid, t in nature.items():
        c = t.get("centroid_px", [])
        if len(c) >= 2:
            centroids.append((tid, float(c[0]), float(c[1])))

    def nearest(px, py):
        best, bd = None, 1e18
        for tid, cx, cy in centroids:
            d = (cx - px) ** 2 + (cy - py) ** 2
            if d < bd:
                bd, best = d, tid
        return best

    data = json.load(open(DEPOSITS, encoding="utf-8"))
    deposits = data["deposits"]
    existing_names = {str(d.get("names", [""])[0]).lower() for d in deposits}
    max_suffix = 0
    for d in deposits:
        tail = str(d.get("id", "")).rsplit("-", 1)[-1]
        if tail.isdigit():
            max_suffix = max(max_suffix, int(tail))

    groups = {}
    seen = set()
    with open(MRDS, encoding="utf-8") as handle:
        for line in handle:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            rid = str(record.get("id", ""))
            if not rid or rid in seen:
                continue
            seen.add(rid)
            resource = bng.resource_for_codes(record.get("codes", ""))
            if resource not in NEW_MINERALS:
                continue
            name = str(record.get("name", "") or "")
            if name and name.lower() in existing_names:
                continue
            lon, lat = record.get("lon"), record.get("lat")
            if lon is None or lat is None:
                continue
            px, py = bng.lonlat_to_pixel(lon, lat)
            if px < 0 or px >= bng.MAP_W or py < 0 or py >= bng.MAP_H:
                continue
            terr = nearest(px, py)
            if terr is None:
                continue
            key = (terr, resource, int(px // 50.0), int(py // 50.0))
            g = groups.setdefault(key, {"sites": 0, "px": 0.0, "py": 0.0,
                                        "names": [], "status": collections.Counter()})
            g["sites"] += 1
            g["px"] += px
            g["py"] += py
            if name and len(g["names"]) < 5:
                g["names"].append(name)
            g["status"][str(record.get("status", "Unknown") or "Unknown")] += 1

    added = 0
    next_id = max_suffix + 1
    for key, g in sorted(groups.items()):
        terr, resource, grid_x, grid_y = key
        cx = g["px"] / g["sites"]
        cy = g["py"] / g["sites"]
        status = max(g["status"].items(), key=lambda kv: (kv[1], kv[0]))[0]
        status_factor = bng.STATUS_FACTOR.get(status, 0.4)
        noise = 0.6 + 0.8 * bng.stable_unit(terr, resource, grid_x, grid_y)
        quantity = bng.RESOURCES[resource]["base_kt"] * g["sites"] * status_factor * noise
        quality = round(0.35 + 0.5 * bng.stable_unit(
            terr, resource, grid_x, grid_y, "q"), 3)
        depth = bng.RESOURCES[resource]["depth"] * (0.6 + 1.0 * bng.stable_unit(
            terr, resource, grid_x, grid_y, "d"))
        payload = bng.deposit_payload(terr, resource, cx, cy, g["sites"],
                                      g["names"], status, quantity, quality, depth, False)
        payload["id"] = "DEP-%s-%06d" % (resource.upper(), next_id)
        next_id += 1
        deposits.append(payload)
        added += 1

    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(DEPOSITS, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("minerais MRDS adicionados:", added, "| total depósitos:", len(deposits))


if __name__ == "__main__":
    main()
