"""Re-atribui depósitos cujo território está absurdamente longe (>=16°) do ponto.

O `nearest_territory` por centróide deixa ilhas remotas (Açores, Galápagos,
Pohnpei, Bataan...) "capturarem" depósitos de continentes distantes — ex.: mina
da Argélia no Açores (a 35°), mina do Japão nas Filipinas (27°). Re-atribui pelo
território mais próximo real. Idempotente: só move se a distância atual >= 16°.

Uso: python tools/fix_territory_misassign.py
Depois: python tools/build_resource_map.py
"""

import json
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")
NATURE = os.path.join(ROOT, "data", "definitions", "natural", "nature.json")

THRESHOLD = 16.0  # graus


def dist_deg(lon1, lat1, lon2, lat2):
    return math.sqrt((lon1 - lon2) ** 2 + (lat1 - lat2) ** 2)


def main():
    nature = json.load(open(NATURE, encoding="utf-8"))["territories"]
    centroids = []
    for tid, t in nature.items():
        lon, lat = t.get("lon"), t.get("lat")
        if lon is not None and lat is not None:
            centroids.append((tid, float(lon), float(lat)))

    def nearest(lon, lat):
        best, bd = None, float("inf")
        for tid, clon, clat in centroids:
            d = dist_deg(lon, lat, clon, clat)
            if d < bd:
                bd, best = d, tid
        return best, bd

    data = json.load(open(DEPOSITS, encoding="utf-8"))
    deposits = data["deposits"]
    fixed = 0
    for d in deposits:
        terr = d.get("territory", "")
        if not terr or terr not in nature:
            continue
        lon, lat = d.get("lon"), d.get("lat")
        if lon is None or lat is None:
            continue
        t = nature[terr]
        tlon, tlat = t.get("lon"), t.get("lat")
        if tlon is None or tlat is None:
            continue
        cur = dist_deg(lon, lat, tlon, tlat)
        if cur < THRESHOLD:
            continue
        new_terr, new_dist = nearest(lon, lat)
        if new_terr and new_terr != terr:
            old = nature[terr].get("name_pt", "?")
            new = nature[new_terr].get("name_pt", "?")
            d["territory"] = new_terr
            print("  %-32s %s -> %s (%.1f -> %.1f graus)" % (
                d.get("names", [""])[0], old, new, cur, new_dist))
            fixed += 1

    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(DEPOSITS, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("re-atribuidos:", fixed)


if __name__ == "__main__":
    main()
