"""Teste de invariantes dos marcadores do mapa de recursos.

Valida `map/map_data/atlas/resource_points.json` contra
`data/definitions/natural/marker_budget.json`:

1. Total de marcadores por recurso <= orçamento.
2. Teto de 12% do orçamento por país (mesmo teto do build_resource_map.py).
3. Distância mínima >= 180 km entre marcadores do MESMO recurso.
4. Nenhum marcador no mar (nav_mask < 128), exceto offshore marcado
   (índice 9 == 1, petróleo/gás).
5. Nenhum marcador com reserva (mineral) ou aptidão (renovável) == 0.

Uso: python tools/resource_marker_test.py
Sai com código != 0 se alguma invariante falhar (para CI).

Não usa numpy/scipy — só a biblioteca padrão + PIL para o nav_mask.
"""

import json
import math
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATLAS = os.path.join(ROOT, "map", "map_data", "atlas")
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
NAV = os.path.join(ROOT, "map", "map_data", "nav_mask.png")

MAP_W, MAP_H = 5000.0, 2500.0
MIN_SAME_RESOURCE_DIST_KM = 120.0
CEILING_FRACTION = 0.12

MINERALS = ["coal", "copper", "gas", "iron", "oil",
            "bauxite", "lead", "gold", "phosphate", "tungsten",
            "rare_earths", "sulfur", "diamond", "uranium"]
RENEWABLES = ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]

failures = []


def fail(msg):
    failures.append(msg)
    print("FALHA: " + msg)


def km_distance(px1, py1, px2, py2):
    """Distância real (km) entre dois pontos do mapa (equirretangular)."""
    def lat(py):
        return 90.0 - py / MAP_H * 180.0

    def lon(px):
        return px / MAP_W * 360.0 - 180.0

    mean_lat = math.radians((lat(py1) + lat(py2)) / 2.0)
    dlon_km = (lon(px2) - lon(px1)) * 111.32 * math.cos(mean_lat)
    dlat_km = (lat(py2) - lat(py1)) * 110.57
    return math.hypot(dlon_km, dlat_km)


def nav_cell(nav, px, py):
    return (int(px / MAP_W * nav.width), int(py / MAP_H * nav.height))


def check_budget_and_ceiling(points, key_for, budgets, kind):
    """Total por recurso <= orçamento; teto 12% por país (region.country)."""
    totals = {}
    per_country = {}
    for point in points:
        resource = key_for(int(point[2]))
        totals[resource] = totals.get(resource, 0) + 1
        country = point[-1].get("country", "") if isinstance(point[-1], dict) else ""
        per_country.setdefault(resource, {}).setdefault(country, 0)
        per_country[resource][country] += 1
    for resource, count in totals.items():
        budget = int(budgets.get(resource, 0))
        if count > budget:
            fail("%s[%s]: total %d > orçamento %d" % (kind, resource, count, budget))
        ceiling = max(1, int(round(budget * CEILING_FRACTION)))
        for country, n in per_country[resource].items():
            if n > ceiling:
                fail("%s[%s]: país %s tem %d marcadores > teto %d (12%% de %d)"
                     % (kind, resource, country, n, ceiling, budget))


def check_distance(points, key_for, kind):
    """Distância mínima >= 180 km entre marcadores do MESMO recurso."""
    groups = {}
    for point in points:
        groups.setdefault(key_for(int(point[2])), []).append(point)
    for resource, group in groups.items():
        for i in range(len(group)):
            for j in range(i + 1, len(group)):
                d = km_distance(group[i][0], group[i][1], group[j][0], group[j][1])
                if d < MIN_SAME_RESOURCE_DIST_KM - 0.5:
                    fail("%s[%s]: distância %.1f km < %.0f km entre (%s,%s) e (%s,%s)"
                         % (kind, resource, d, MIN_SAME_RESOURCE_DIST_KM,
                            group[i][0], group[i][1], group[j][0], group[j][1]))
                    return  # uma falha por grupo basta


def check_sea(points, nav, key_for, kind):
    """Nenhum marcador no mar, exceto offshore (índice 9 == 1)."""
    for point in points:
        offshore = bool(point[9]) if len(point) > 9 else False
        if offshore:
            continue
        x, y = nav_cell(nav, float(point[0]), float(point[1]))
        if not (0 <= x < nav.width and 0 <= y < nav.height):
            fail("%s[%s]: marcador fora do nav_mask em (%s, %s)"
                 % (kind, key_for(int(point[2])), point[0], point[1]))
            continue
        if nav.getpixel((x, y)) < 128:
            fail("%s[%s]: marcador no mar em (%s, %s) sem offshore"
                 % (kind, key_for(int(point[2])), point[0], point[1]))


def check_nonzero(points, key_for, kind):
    """Nenhum marcador com reserva (mineral) ou aptidão (renovável) == 0."""
    for point in points:
        value = float(point[3])
        if value <= 0.0:
            fail("%s[%s]: valor %r <= 0 em (%s, %s)"
                 % (kind, key_for(int(point[2])), value, point[0], point[1]))


def main():
    with open(os.path.join(ATLAS, "resource_points.json"), encoding="utf-8") as fh:
        data = json.load(fh)
    with open(os.path.join(NATURAL, "marker_budget.json"), encoding="utf-8") as fh:
        budgets = json.load(fh)["budgets"]
    nav = Image.open(NAV).convert("L")

    minerals = data.get("minerals", [])
    renewables = data.get("renewables", [])

    print("minerais=%d renováveis=%d" % (len(minerals), len(renewables)))

    check_budget_and_ceiling(minerals, lambda i: MINERALS[i], budgets, "mineral")
    check_budget_and_ceiling(renewables, lambda i: RENEWABLES[i], budgets, "renovável")
    check_distance(minerals, lambda i: MINERALS[i], "mineral")
    check_distance(renewables, lambda i: RENEWABLES[i], "renovável")
    check_sea(minerals, nav, lambda i: MINERALS[i], "mineral")
    check_sea(renewables, nav, lambda i: RENEWABLES[i], "renovável")
    check_nonzero(minerals, lambda i: MINERALS[i], "mineral")
    check_nonzero(renewables, lambda i: RENEWABLES[i], "renovável")

    if failures:
        print("RESULTADO: %d falha(s)" % len(failures))
        return 1
    print("RESULTADO: OK (todas as invariantes mantidas)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
