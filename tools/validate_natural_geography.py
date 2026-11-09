"""Valida as definições naturais geradas e imprime amostras por cidade.

Uso:
  python tools/validate_natural_geography.py
"""

import hashlib
import json
import os
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")

CITIES = {
    "London": (2498.2, 534.6),
    "Paris": (2532.6, 571.4),
    "Berlin": (2686.2, 520.6),
    "Amsterdam": (2568.0, 522.7),
    "Brussels": (2560.4, 543.8),
}

RANGES = {
    "temp_c": (-35.0, 40.0),
    "precip_mm": (0.0, 5000.0),
    "fertility": (0.0, 1.0),
    "water": (0.0, 1.0),
    "altitude_m": (0.0, 9000.0),
    "forest_cover": (0.0, 1.0),
    "fishing": (0.0, 1.0),
    "cultivable": (0.0, 1.0),
}


def load(name):
    with open(os.path.join(NATURAL, name), "r", encoding="utf-8") as handle:
        return json.load(handle)


def check_ranges(nature, agriculture):
    problems = []
    for territory_id, entry in nature["territories"].items():
        for field, (low, high) in RANGES.items():
            value = entry.get(field)
            if value is None or not (low <= value <= high):
                problems.append("%s.%s=%r fora de [%s, %s]" % (
                    territory_id, field, value, low, high))
        for crop, value in agriculture["territories"].get(territory_id, {}).items():
            if not (0.0 <= value <= 1.0):
                problems.append("%s.%s=%r fora de [0,1]" % (territory_id, crop, value))
    return problems


def check_deposits(deposits):
    problems = []
    for deposit in deposits["deposits"]:
        if deposit["quantity_kt"] <= 0 or deposit["recoverable_kt"] <= 0:
            problems.append("%s quantidade não positiva" % deposit["id"])
        if deposit["recoverable_kt"] > deposit["quantity_kt"]:
            problems.append("%s recuperável > quantidade" % deposit["id"])
        if deposit["required_tech"] not in (1, 2, 3):
            problems.append("%s tecnologia inválida" % deposit["id"])
        if not (0.0 <= deposit["quality"] <= 1.0):
            problems.append("%s qualidade inválida" % deposit["id"])
        if deposit["resource"] not in ("iron", "copper", "coal", "oil", "gas"):
            problems.append("%s recurso inválido" % deposit["id"])
    return problems


def nearest_territory(nature, px, py):
    best = None
    for territory_id, entry in nature["territories"].items():
        cx, cy = entry["centroid_px"]
        distance = (cx - px) ** 2 + (cy - py) ** 2
        if best is None or distance < best[0]:
            best = (distance, territory_id, entry)
    return best[1], best[2]


def digest(payload):
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha1(canonical.encode("utf-8")).hexdigest()[:16]


def main():
    nature = load("nature.json")
    agriculture = load("agriculture.json")
    deposits = load("deposits.json")

    problems = check_ranges(nature, agriculture) + check_deposits(deposits)

    print("Territórios: %d | depósitos: %d" % (
        len(nature["territories"]), len(deposits["deposits"])))
    print("Assinaturas: nature=%s agriculture=%s deposits=%s" % (
        digest(nature), digest(agriculture), digest(deposits)))
    print()
    for name, (px, py) in CITIES.items():
        territory_id, entry = nearest_territory(nature, px, py)
        print("%-10s %s %-4s clima=%-22s temp=%5.1f precip=%4d solo=%-10s "
              "fertilidade=%.2f trigo=%.2f" % (
                  name, territory_id, entry["owner"], entry["climate"],
                  entry["temp_c"], entry["precip_mm"], entry["soil_type"],
                  entry["fertility"], agriculture["territories"][territory_id]["wheat"]))

    print()
    global_counts = Counter(d["resource"] for d in deposits["deposits"])
    europe = [d for d in deposits["deposits"] if -12 <= d["lon"] <= 32 and 34 <= d["lat"] <= 61]
    europe_counts = Counter(d["resource"] for d in europe)
    print("Recursos globais:", dict(global_counts))
    print("Recursos na Europa:", dict(europe_counts))

    print()
    for resource in ("iron", "copper", "coal", "oil", "gas"):
        samples = [d for d in europe if d["resource"] == resource][:3]
        if not samples:
            continue
        print(resource, [(d["names"][0] if d["names"] else "?",
                          round(d["lat"], 1), round(d["lon"], 1),
                          round(d["quantity_kt"])) for d in samples])

    if problems:
        print()
        print("PROBLEMAS (%d):" % len(problems))
        for problem in problems[:20]:
            print(" -", problem)
        return 1
    print()
    print("OK: nenhuma inconsistência encontrada.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
