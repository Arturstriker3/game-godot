"""Diagnóstico de sobreposição de marcadores no mapa de recursos.

Lê map/map_data/atlas/resource_points.json (minerais e renováveis) e lista
pares de marcadores de recursos DIFERENTES a menos de D px de mapa entre si.

Regra do projeto (GAME_STATE): 1 marcador por (território × recurso), com
offset por recurso — recursos diferentes não podem colar/sobrepor.

Uso: python tools/resource_overlap_check.py
"""

import json
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POINTS = os.path.join(ROOT, "map", "map_data", "atlas", "resource_points.json")

## Mesmas ordens de resource_points.json (ver tools/build_resource_map.py e
## map/map_mode_layer.gd).
MINERALS = ["coal", "copper", "gas", "iron", "oil",
            "bauxite", "lead", "gold", "phosphate", "tungsten",
            "rare_earths", "sulfur", "diamond", "uranium"]
RENEWABLES = ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]

## Diâmetro do marcador em px de mapa (MARKER_SIZE=1.0 u = 5 px; folga => D=7).
D = 7.0


def load_points():
    with open(POINTS, encoding="utf-8") as handle:
        data = json.load(handle)
    markers = []
    for row in data.get("minerals", []):
        # [px, py, mineral_index, ...]
        markers.append({
            "px": float(row[0]),
            "py": float(row[1]),
            "resource": MINERALS[int(row[2])],
            "territory": str(row[7]),
        })
    for row in data.get("renewables", []):
        # [px, py, renewable_index, value, territory]
        markers.append({
            "px": float(row[0]),
            "py": float(row[1]),
            "resource": RENEWABLES[int(row[2])],
            "territory": str(row[4]),
        })
    return markers


def main():
    markers = load_points()
    pairs = []
    for i in range(len(markers)):
        for j in range(i + 1, len(markers)):
            a, b = markers[i], markers[j]
            if a["resource"] == b["resource"]:
                continue  # mesmo recurso já funde por território
            dist = math.hypot(a["px"] - b["px"], a["py"] - b["py"])
            if dist < D:
                pairs.append((a, b, dist))

    print("marcadores totais: %d" % len(markers))
    print("pares sobrepostos (recursos diferentes, dist < %g px): %d" % (D, len(pairs)))

    # Separa sobreposição no MESMO território (viola a regra de 1 marcador por
    # território×recurso) da sobreposição entre territórios vizinhos (limite
    # inevitável quando territórios são pequenos/adjacentes).
    within = [(a, b, d) for a, b, d in pairs if a["territory"] == b["territory"]]
    cross = [(a, b, d) for a, b, d in pairs if a["territory"] != b["territory"]]
    print("  no MESMO território: %d" % len(within))
    print("  entre territórios vizinhos: %d" % len(cross))

    # Exemplos: agrupa por par de recursos e mostra até 3 exemplos de cada.
    from collections import Counter
    by_pair = Counter()
    examples = {}
    for a, b, dist in pairs:
        key = tuple(sorted((a["resource"], b["resource"])))
        by_pair[key] += 1
        examples.setdefault(key, []).append((a, b, dist))

    shown = 0
    for key, count in sorted(by_pair.items(), key=lambda kv: -kv[1]):
        r1, r2 = key
        print("  %s × %s: %d par(es)" % (r1, r2, count))
        for a, b, dist in examples[key][:3]:
            same = "MESMO" if a["territory"] == b["territory"] else "vizinho"
            print("    [%s] %s (%s) vs %s (%s) dist=%.1f px  (%.1f,%.1f)-(%.1f,%.1f)"
                  % (same, a["resource"], a["territory"], b["resource"], b["territory"],
                     dist, a["px"], a["py"], b["px"], b["py"]))
        shown += 1
        if shown >= 25:
            print("  ...")
            break

    # Confere explicitamente o caso reportado: ouro × trigo.
    gold_wheat = [(a, b, d) for a, b, d in pairs
                  if {a["resource"], b["resource"]} == {"gold", "wheat"}]
    print("ouro × trigo: %d par(es)" % len(gold_wheat))

    if not within:
        print("PASS: nenhum par sobreposto no MESMO território")
    else:
        print("FAIL: %d par(es) sobrepostos no MESMO território" % len(within))


if __name__ == "__main__":
    main()
