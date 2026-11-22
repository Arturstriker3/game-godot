"""Parque elétrico instalado por cidade e nação (Parte E, E1).

Entrada: cache do Global Power Plant Database (WRI, v1.3.0, CC BY 4.0) em
tools/data/gppd/plants_normalized.csv, gerado por tools/fetch_gppd.py.

Atribuição geográfica: latitude/longitude -> pixel do mapa 5000x2500 ->
província admin-1 no raster map/map_data/atlas/world_province.png -> cidade dona
da província pela MESMA regra da simulação (tools/build_energy_common.py,
réplica de GeographyService.assign_provinces).

Honestidade do dado:
  * o GPPD é de ~2021; NADA é extrapolado para 2026. Cada usina leva
    method="gppd_2021_unadjusted" e a data de referência da base no meta;
  * a comparação do parque com o agregado nacional 2024-2025 (Ember, CC BY 4.0)
    NÃO fica aqui: ela é montada em tools/data/energy/build_national_capacity.py
    (national_capacity.json, `plant_comparison`), com desvio por fonte. A IRENA
    saiu das saídas versionadas porque os termos dela restringem uso comercial;
  * usina em pixel sem província (mar, ilha removida, país fora do atlas) fica
    com city_id/province_id nulos e o motivo registrado — nunca é descartada.

Uso: python tools/build_energy_plants.py
Saída: data/definitions/energy/power_plants_2026.json
"""

from __future__ import annotations

import csv
import os
import sys

import numpy as np

import build_energy_common as ec

ROOT = ec.ROOT
GPPD_CSV = os.path.join(ROOT, "tools", "data", "gppd", "plants_normalized.csv")
GPPD_MANIFEST = os.path.join(ROOT, "tools", "data", "gppd", "manifest.json")
OUT = os.path.join(ROOT, "data", "definitions", "energy", "power_plants_2026.json")

# GPPD primary_fuel -> grupo de energia do jogo. Grupos: hydro, solar, wind,
# coal, gas, oil, nuclear, biomass, other.
FUEL_GROUP = {
    "Hydro": "hydro",
    "Solar": "solar",
    "Wind": "wind",
    "Coal": "coal",
    "Gas": "gas",
    "Oil": "oil",
    "Nuclear": "nuclear",
    "Biomass": "biomass",
    "Waste": "biomass",
    "Geothermal": "other",
    "Storage": "other",
    "Cogeneration": "other",
    "Petcoke": "coal",
    "Wave and Tidal": "other",
    "Other": "other",
}
RENEWABLE_GROUPS = {"hydro", "solar", "wind", "biomass"}
# Petcoke é derivado do petróleo mas queima como carvão (coque de petróleo); o
# GPPD o separa de Coal. Agrupado em coal para a matriz do jogo, com a fonte
# original preservada em `fuel_source`.
GROUPS = ["hydro", "solar", "wind", "coal", "gas", "oil", "nuclear", "biomass", "other"]
# Sempre presentes, mesmo com valor null (a ausência de cidade/província é dado).
REQUIRED_KEYS = {"id", "name", "country_iso3", "capacity_mw", "lat", "lon",
                 "province_id", "city_id", "fuel_group", "source", "method",
                 "confidence"}


def load_plants():
    if not os.path.exists(GPPD_CSV):
        raise SystemExit("cache ausente: %s (rode tools/fetch_gppd.py)" % GPPD_CSV)
    with open(GPPD_CSV, encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle))
    rows.sort(key=lambda r: r["gppd_idnr"])
    return rows


def province_at(ids, lon, lat):
    """Id da província do raster no ponto; 0 fora da grade."""
    px, py = ec.lonlat_to_map(lon, lat)
    height, width = ids.shape
    rx = int(px / ec.MAP_W * width)
    ry = int(py / ec.MAP_H * height)
    if rx < 0 or ry < 0 or rx >= width or ry >= height:
        return 0, rx, ry
    return int(ids[ry, rx]), rx, ry


def main():
    cities = ec.load_cities()
    provinces = ec.load_provinces()
    areas = ec.raster_province_area_km2()
    tags, _ = ec.load_registry()
    tag_by_code = {ec.nation_code(tag): tag for tag in tags}
    tag_by_atlas = {ec.province_tag(tag): tag for tag in tags}
    city_by_id = {c["id"]: c for c in cities}
    province_by_id = {p["id"]: p for p in provinces}
    raw = ec.read_json(GPPD_MANIFEST) if os.path.exists(GPPD_MANIFEST) else {}

    owner = ec.province_city_index(cities, provinces)
    ids = ec.load_world_province_ids()
    rows = load_plants()

    plants = []
    for row in rows:
        lat = float(row["latitude"])
        lon = float(row["longitude"])
        pid, rx, ry = province_at(ids, lon, lat)
        city_id = ""
        city_share = None
        if pid in owner:
            city_id, city_share = owner[pid]
        elif pid not in province_by_id:
            pid = 0
        nation = tag_by_code.get(row["country"], "")
        fuel_source = row["primary_fuel"]
        group = FUEL_GROUP.get(fuel_source, "other")
        year = row["commissioning_year"]
        raster_nation = ""
        if pid:
            raster_nation = tag_by_atlas.get(
                province_by_id.get(pid, {}).get("nation", ""), "")
        record = {
            "id": row["gppd_idnr"],
            "name": row["name"],
            "country_iso3": row["country"],
            "nation": nation,
            "fuel_source": fuel_source,
            "fuel_group": group,
            "renewable": group in RENEWABLE_GROUPS,
            "capacity_mw": round(float(row["capacity_mw"]), 3),
            "lat": round(lat, 6),
            "lon": round(lon, 6),
            "province_id": pid or None,
            "city_id": city_id or None,
            "city_share": None if city_share is None else round(float(city_share), 6),
            "commissioning_year": int(year) if year else None,
            "owner": row["owner"] or None,
            "source": "GPPD v1.3.0 (WRI)",
            "method": "gppd_2021_unadjusted",
            "confidence": "high",
            "note": None if city_id else (
                "province_not_assigned" if pid else "no_province_at_point"),
        }
        if raster_nation and nation and raster_nation != nation:
            # O raster do atlas coloca o ponto em outra nação (fronteira, ilha sem
            # província própria, território ocupado). A usina continua na cidade dona
            # da província — a regra do jogo —, mas o conflito fica declarado.
            record["location_conflict"] = raster_nation
        plants.append({key: value for key, value in record.items()
                       if value is not None or key in REQUIRED_KEYS})

    by_city = {}
    by_nation = {}
    for plant in plants:
        mw = plant["capacity_mw"]
        city_entry = None
        if plant["city_id"]:
            city_id = plant["city_id"]
            city_entry = by_city.setdefault(city_id, {
                "city_id": city_id,
                "nation": city_by_id[city_id]["tag"],
                "total_mw": 0.0,
                "plants": 0,
                "by_fuel": {},
            })
            city_entry["total_mw"] += mw
            city_entry["plants"] += 1
            city_entry["by_fuel"][plant["fuel_group"]] = round(
                city_entry["by_fuel"].get(plant["fuel_group"], 0.0) + mw, 3)
        if plant["nation"]:
            nation_entry = by_nation.setdefault(plant["nation"], {
                "nation": plant["nation"],
                "total_mw": 0.0,
                "plants": 0,
                "by_fuel": {},
                "mw_without_city": 0.0,
                "area_km2": 0.0,
                "provinces": 0,
            })
            nation_entry["total_mw"] += mw
            nation_entry["plants"] += 1
            nation_entry["by_fuel"][plant["fuel_group"]] = round(
                nation_entry["by_fuel"].get(plant["fuel_group"], 0.0) + mw, 3)
            if not city_entry:
                nation_entry["mw_without_city"] += mw

    for entry in by_city.values():
        entry["total_mw"] = round(entry["total_mw"], 3)
    for tag, entry in by_nation.items():
        entry["total_mw"] = round(entry["total_mw"], 3)
        entry["mw_without_city"] = round(entry["mw_without_city"], 3)
        atlas_tag = ec.province_tag(tag)
        total_area = 0.0
        count = 0
        for province in provinces:
            if province["nation"] != atlas_tag:
                continue
            total_area += areas.get(province["id"], 0.0)
            count += 1
        entry["area_km2"] = round(total_area, 1)
        entry["provinces"] = count

    for entry in by_nation.values():
        entry["mw_per_1000_km2"] = (round(entry["total_mw"] / entry["area_km2"] * 1000.0, 3)
                                    if entry["area_km2"] > 0 else None)

    meta = {
        "generated_by": "tools/build_energy_plants.py",
        "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
        "park_source": {
            "dataset": "Global Power Plant Database v1.3.0",
            "publisher": "World Resources Institute (WRI)",
            "url": raw.get("source_url"),
            "accessed_at_utc": raw.get("accessed_at_utc"),
            "sha256_csv": raw.get("sha256"),
            "license": raw.get("license"),
            "citation": raw.get("citation"),
            "reference_year": 2021,
            "note": ("capacidade NÃO extrapolada; cada usina é o registro de ~2021 "
                     "do GPPD (method gppd_2021_unadjusted)"),
        },
        "geography": {
            "province_source": "map/map_data/atlas/provinces.json + world_province.png",
            "city_assignment": ("tools/build_energy_common.py — réplica de "
                                "GeographyService.assign_provinces (província -> cidade "
                                "da mesma nação e landmass, partição por área)"),
            "city_rule": ("quando a província é dividida entre cidades, a usina vai para "
                          "a cidade com maior fração (city_share registrado)"),
            "sea_rule": ("o raster world_province preenche o mar com a província de terra "
                         "mais próxima; usinas offshore (eólicas) herdam essa província"),
            "area_source": ("área da nação = soma da área das províncias contada no raster "
                            "do atlas (world_province + world_sdf), não province_areas.json"),
        },
        "methods": {
            "fuel_group": FUEL_GROUP,
            "groups": GROUPS,
            "renewable_groups": sorted(RENEWABLE_GROUPS),
            "confidence": "high (localização/MW do GPPD); atribuição cidade-província herda a do jogo",
        },
        "location_conflicts": {
            "count": sum(1 for p in plants if p.get("location_conflict")),
            "mw": round(sum(p["capacity_mw"] for p in plants
                            if p.get("location_conflict")), 3),
            "rule": ("`location_conflict` = nação do raster de província diferente do "
                     "`country_iso3` do GPPD (fronteira, ilha sem província própria, "
                     "território ocupado). A usina fica na cidade dona da província; o "
                     "conflito é registro, não correção — corrigir exige fonte."),
            "top_pairs": [],
        },
        "counts": {
            "plants": len(plants),
            "with_city": sum(1 for p in plants if p["city_id"]),
            "with_nation": sum(1 for p in plants if p["nation"]),
            "cities_with_plants": len(by_city),
            "nations_with_plants": len(by_nation),
            "total_mw": round(sum(p["capacity_mw"] for p in plants), 3),
        },
    }

    pairs = {}
    for plant in plants:
        other = plant.get("location_conflict")
        if not other:
            continue
        key = "%s>%s" % (plant["country_iso3"], other)
        pairs[key] = pairs.get(key, 0) + 1
    meta["location_conflicts"]["top_pairs"] = [
        {"pair": key, "plants": count}
        for key, count in sorted(pairs.items(), key=lambda kv: -kv[1])[:12]
    ]

    payload = {"meta": meta, "by_nation": by_nation, "by_city": by_city,
               "plants": plants}
    text = ['{', ' "meta": %s,' % _compact(meta),
            ' "by_nation": %s,' % _compact(by_nation),
            ' "by_city": %s,' % _compact(by_city),
            ' "plants": [']
    lines = []
    for plant in plants:
        lines.append("  " + _compact(plant))
    text.append(",\n".join(lines))
    text.append(' ]')
    text.append('}')
    ec.write_text(OUT, "\n".join(text) + "\n")

    print("usinas: %d | com cidade: %d | com nação: %d"
          % (meta["counts"]["plants"], meta["counts"]["with_city"], meta["counts"]["with_nation"]))
    print("total MW: %.1f | cidades com usina: %d | nações: %d"
          % (meta["counts"]["total_mw"], len(by_city), len(by_nation)))
    return 0


def _compact(obj):
    import json
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


if __name__ == "__main__":
    sys.exit(main())
