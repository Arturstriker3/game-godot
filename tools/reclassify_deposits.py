"""Reclassifica os depósitos commitados com o classificador corrigido (código
primário) e os novos recursos, re-estimando quantidade/qualidade/profundidade.

O MRDS compacto não traz as coordenadas de vários recursos, então este script
CORRIGE o rótulo + re-estima os 2.249 depósitos já commitados (que já têm
território/px/py), sem regenerar do zero. Depósitos curados (carvão/petróleo/gás
do CURATED_DEPOSITS e suplementos) são preservados — eles não têm match no MRDS.

Reusa resource_for_codes/stable_unit/STATUS_FACTOR/RESOURCES de
build_natural_geography.py. Idempotente: reaplicar não muda nada.

Uso: python tools/reclassify_deposits.py
Depois: python tools/build_resource_map.py  (regenera resource_points.json)
"""

import collections
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import build_natural_geography as bng  # noqa: E402

DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")
MRDS = os.path.join(ROOT, "tools", "data", "mrds_deposits.jsonl")


def load_name_codes():
    name2codes = collections.defaultdict(list)
    if not os.path.exists(MRDS):
        raise SystemExit("mrds_deposits.jsonl ausente; rode tools/fetch_usgs_deposits.py")
    with open(MRDS, encoding="utf-8") as handle:
        for line in handle:
            record = json.loads(line)
            name = record.get("name")
            if name:
                name2codes[name].append((record.get("codes") or "").strip())
    return name2codes


def reestimate(dep, resource):
    """Re-estima os campos derivados do recurso, replicando build_deposits."""
    spec = bng.RESOURCES[resource]
    territory = str(dep.get("territory", ""))
    px = float(dep.get("px", 0.0))
    py = float(dep.get("py", 0.0))
    grid_x = int(px // 50.0)
    grid_y = int(py // 50.0)
    sites = int(dep.get("sites", 1))
    status = str(dep.get("status", "Unknown"))
    status_factor = bng.STATUS_FACTOR.get(status, 0.4)

    noise = 0.6 + 0.8 * bng.stable_unit(territory, resource, grid_x, grid_y)
    quantity = spec["base_kt"] * sites * status_factor * noise
    quality = round(0.35 + 0.5 * bng.stable_unit(
        territory, resource, grid_x, grid_y, "q"), 3)
    depth = spec["depth"] * (0.6 + 1.0 * bng.stable_unit(
        territory, resource, grid_x, grid_y, "d"))
    recoverable = quantity * spec["recovery"]
    extraction_cost = round(20.0 + depth * 0.02 + (1.0 - quality) * 30.0, 1)
    tech = 1 if depth < 500.0 and quality > 0.5 else 2 if depth < 2000.0 else 3
    max_rate = max(1.0, round(recoverable / (25.0 * 365.0), 1))
    discovery = round(0.15 if status == "Producer" else
                      0.35 if status == "Past Producer" else
                      0.6 if status == "Prospect" else 0.75, 2)

    dep["resource"] = resource
    dep["quantity_kt"] = round(quantity, 1)
    dep["recoverable_kt"] = round(recoverable, 1)
    dep["quality"] = quality
    dep["depth_m"] = round(depth)
    dep["extraction_cost"] = extraction_cost
    dep["required_tech"] = tech
    dep["max_rate_kt_day"] = max_rate
    dep["discovery_difficulty"] = discovery


def write_json(path, data):
    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def main():
    name2codes = load_name_codes()
    data = json.load(open(DEPOSITS, encoding="utf-8"))
    deposits = data["deposits"]

    changed = 0
    skipped_curated = 0
    for dep in deposits:
        names = dep.get("names") or []
        votes = collections.Counter()
        for name in names:
            for codes in name2codes.get(name, []):
                resource = bng.resource_for_codes(codes)
                if resource:
                    votes[resource] += 1
        if not votes:
            skipped_curated += 1  # curado/suplemento: sem match no MRDS
            continue
        new_resource = votes.most_common(1)[0][0]
        if new_resource == dep.get("resource"):
            continue
        reestimate(dep, new_resource)
        changed += 1

    # Regera ids (prefixo do recurso) e garante unicidade.
    ids = collections.Counter()
    for dep in deposits:
        resource = str(dep["resource"]).upper()
        ids[resource] += 1
        dep["id"] = "DEP-%s-%06d" % (resource, ids[resource])

    write_json(DEPOSITS, data)
    print("reclassificados:", changed,
          "| preservados (curados/sem match):", skipped_curated)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
