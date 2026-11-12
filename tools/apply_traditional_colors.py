"""Aplica as cores tradicionais das nações e repara vizinhos (só a cor muda).

Lê `data/definitions/world/traditional_colors.csv` (prioridade) e a cor atual do
cenário (paleta automática); resolve conflitos na adjacência de TERRA e regrava
APENAS a coluna `color` de `data/definitions/scenario/nations.csv`. Também
espelha os mesmos valores em `nations.json` (campo `color`) e em
`provinces.json` -> `nation_colors` (as 8 jogáveis, usadas como fallback do
atlas e pelo editor).

Nada mais é tocado: cidades, portos, construções, i18n e as colunas de nome/
população/PIB ficam byte a byte iguais.

Uso:
    <python> tools/apply_traditional_colors.py            # regrava
    <python> tools/apply_traditional_colors.py --check    # valida sem escrever
    <python> tools/apply_traditional_colors.py --refresh-adjacency
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import sys

import nation_colors

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")
WORLD = os.path.join(ROOT, "data", "definitions", "world")
ATLAS = os.path.join(ROOT, "map", "map_data", "atlas")
NATIONS_CSV = os.path.join(SCENARIO, "nations.csv")
NATIONS_JSON = os.path.join(SCENARIO, "nations.json")
PROVINCES_JSON = os.path.join(ATLAS, "provinces.json")


def read_rows(path):
    with open(path, encoding="utf-8-sig", newline="") as fh:
        return list(csv.DictReader(fh))


def areas_by_tag():
    """Área (km²) de cada nação: fonte do mundo; fallback no atlas."""
    path = os.path.join(WORLD, "nations_source.csv")
    if os.path.exists(path):
        return {row["iso3"]: float(row["area_km2"] or 0) for row in read_rows(path)}
    provinces = json.load(open(PROVINCES_JSON, encoding="utf-8"))["provinces"]
    areas = {}
    for province in provinces:
        areas[province["nation"]] = areas.get(province["nation"], 0.0) + province["area_global"]
    return areas


def rewrite_color_column(path, colors):
    """Troca só a coluna `color`, preservando CRLF, ordem e todas as outras."""
    with open(path, "rb") as fh:
        text = fh.read().decode("utf-8")
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    header = lines[0].split(",")
    column = header.index("color")
    id_column = header.index("id")
    changed = []
    for index in range(1, len(lines)):
        if not lines[index]:
            continue
        fields = lines[index].split(",")
        tag = fields[id_column]
        if tag in colors and fields[column] != colors[tag]:
            changed.append((tag, fields[column], colors[tag]))
            fields[column] = colors[tag]
            lines[index] = ",".join(fields)
    with open(path, "wb") as fh:
        fh.write(newline.join(lines).encode("utf-8"))
    return changed


def sync_nations_json(colors):
    data = json.load(open(NATIONS_JSON, encoding="utf-8"))
    changed = []
    for nation in data:
        tag = nation.get("id")
        if tag in colors and nation.get("color") != colors[tag]:
            changed.append((tag, nation.get("color"), colors[tag]))
            nation["color"] = colors[tag]
    with open(NATIONS_JSON, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)
    return changed


def sync_provinces_json(colors):
    """`nation_colors` repete a cor das jogáveis (fallback do atlas/editor).

    As cores por província dessas nações são legado do atlas estático e não
    entram no mapa dinâmico; ficam como estão para não misturar convenções.
    """
    data = json.load(open(PROVINCES_JSON, encoding="utf-8"))
    changed = []
    for tag in list(data.get("nation_colors", {})):
        if tag not in colors:
            continue
        value = [int(colors[tag][i:i + 2], 16) for i in (1, 3, 5)]
        if data["nation_colors"][tag] != value:
            changed.append((tag, data["nation_colors"][tag], value))
            data["nation_colors"][tag] = value
    with open(PROVINCES_JSON, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent="\t")
        fh.write("\n")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="não escreve; falha se divergir")
    parser.add_argument("--refresh-adjacency", action="store_true",
                        help="recalcula a adjacência de terra a partir dos rasters")
    args = parser.parse_args()

    rows = read_rows(NATIONS_CSV)
    base = {row["id"]: row["color"].strip().lower() for row in rows}
    areas = areas_by_tag()
    order = sorted(base, key=lambda tag: (-areas.get(tag, 0.0), tag))
    traditional = nation_colors.load_traditional()
    unknown = sorted(tag for tag in traditional if tag not in base)
    if unknown:
        raise SystemExit(f"traditional_colors.csv: tags fora do cenário: {', '.join(unknown)}")

    graph = (nation_colors.refresh_adjacency() if args.refresh_adjacency
             else nation_colors.load_or_refresh_adjacency())
    colors, report = nation_colors.assign_colors(base, traditional, graph, order)

    pending_csv = [(tag, base[tag], colors[tag]) for tag in sorted(base)
                   if base[tag] != colors[tag]]
    print(f"nações: {len(base)} | tradicionais: {report['traditional_count']} | "
          f"alteradas: {len(pending_csv)}")
    print(f"menor ΔE entre vizinhos: {report['min_delta_e']} "
          f"({report['min_pair'][1]}-{report['min_pair'][2]}) de {report['pairs']} pares")
    print(f"abaixo do alvo 30: {len(report['below_target'])} | "
          f"abaixo do piso 25: {len(report['below_floor'])} | "
          f"RGB < 0,16: {len(report['rgb_below_min'])}")
    for delta, a, b in report["below_target"]:
        print(f"  <30 ΔE {delta}: {a} {colors[a]} x {b} {colors[b]}")

    if pending_csv:
        print("tons ajustados (pedido -> aplicado):")
        for item in report["changed"]:
            print(f"  {item['tag']} {item['kind']:11s} {item['from']} -> {item['to']} "
                  f"(min ΔE {item['min_delta_e']})")

    if args.check:
        failed = bool(pending_csv or report["below_floor"] or report["rgb_below_min"])
        return 1 if failed else 0

    metadata_changes = []
    if pending_csv:
        rewrite_color_column(NATIONS_CSV, colors)
        metadata_changes += sync_nations_json(colors)
        metadata_changes += sync_provinces_json(colors)
        print("gravado: nations.csv (só color), nations.json, provinces.json/nation_colors")
    else:
        print("sem mudanças: cores já aplicadas")
    for tag, old, new in metadata_changes:
        print(f"  json {tag}: {old} -> {new}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
