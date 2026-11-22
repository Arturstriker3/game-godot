"""Parque elétrico instalado por cidade e nação — base GPPD ~2021 (Parte E, E1).

Entrada: cache do Global Power Plant Database (WRI, v1.3.0, CC BY 4.0) em
tools/data/gppd/plants_normalized.csv, gerado por tools/fetch_gppd.py.

Atribuição geográfica: latitude/longitude -> pixel do mapa 5000x2500 ->
província admin-1 no raster map/map_data/atlas/world_province.png -> cidade dona
da província pela MESMA regra da simulação (tools/build_energy_common.py,
réplica de GeographyService.assign_provinces).

DECISÃO DO DONO (D-115, 2026-11-21):
  * a usina pertence ao CONTROLADOR DA CIDADE/PROVÍNCIA no mapa do jogo:
    `nation` é a nação do ATLAS (a cidade dona da província), NÃO a nação do
    GPPD. O ISO3 do GPPD fica preservado em `gppd_country` para rastreio;
  * o total por nação é a soma das cidades do atlas; usina sem cidade fica
    fora do agregado por nação e é declarada em `meta.counts.mw_without_city`;
  * conflitos de localização são classificados por causa:
      - `gppd_coord_error`: coordenada/país do GPPD comprovadamente errado.
        Corrigido com evidência (ex.: ESCOMBRERAS GRUPO 1, lat 7,5732 ->
        37,5732, mesmo complexo/usina irmã WRI1006415) ou DESCARTADO quando
        não há fonte para corrigir (declarado em `meta.discarded`);
      - `deleted_territory`: território do GPPD que não existe no jogo
        (ilhas/territórios ultramarinos simplificados no atlas, ou país fora
        da lista jogável). A usina fica na cidade do atlas (regra do jogo);
      - `occupied`: território disputado/ocupado; vale a regra (a) — fica com
        o controlador do atlas;
      - `border_pixel`: pixel de fronteira/costa do atlas; fica na cidade mais
        próxima da nação do atlas.
  * `gppd_country` é o ISO3 bruto do GPPD; `gppd_country_game` é o código
    traduzido para o jogo (aliases PSE->PSX e GUF->FRA); `province_nation` é a
    nação da província no atlas.

Honestidade do dado:
  * o GPPD é de ~2021; NADA é extrapolado para 2026 neste arquivo. Cada usina
    leva method="gppd_2021_unadjusted" e `meta.data_year = 2021`;
  * o escalonamento para a matriz Ember 2024-2025 é outro arquivo
    (power_plants_2026_scaled.json), com fator declarado por nação x fonte;
  * usina em pixel sem província (mar, ilha removida, país fora do atlas) fica
    com city_id/province_id nulos e o motivo registrado — nunca é descartada
    sem declaração.

Uso: python tools/build_energy_plants.py
Saída: data/definitions/energy/power_plants_base_2021_gppd.json
"""

from __future__ import annotations

import csv
import json
import os
import sys

import numpy as np

import build_energy_common as ec

ROOT = ec.ROOT
GPPD_CSV = os.path.join(ROOT, "tools", "data", "gppd", "plants_normalized.csv")
GPPD_MANIFEST = os.path.join(ROOT, "tools", "data", "gppd", "manifest.json")
OUT = os.path.join(ROOT, "data", "definitions", "energy",
                   "power_plants_base_2021_gppd.json")

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
GROUPS = ["hydro", "solar", "wind", "coal", "gas", "oil", "nuclear", "biomass",
          "other"]
REQUIRED_KEYS = {"id", "name", "gppd_country", "gppd_country_game",
                 "gppd_country_alias", "province_nation", "nation",
                 "capacity_mw", "lat", "lon", "province_id", "city_id",
                 "fuel_group", "source", "method", "confidence"}

# Códigos ISO3 do GPPD que o jogo escreve de outro jeito. NÃO é correção de
# país: é só a tradução do código (PSE/PSX, GUF/FRA).
GPPD_ISO3_ALIASES = {"PSE": "PSX", "GUF": "FRA"}

# Correções de coordenada COMPROVADAS. Cada entrada: (lat, lon, evidência).
GPPD_COORD_FIXES = {
    "WRI1006467": (
        37.5732, -0.9392,
        "lat 7.5732 -> 37.5732: mesmo complexo de ESCOMBRERAS, mesmo lon "
        "(-0.9392), mesmo dono (GDF SUEZ CARTAGENA ENERGIA) e mesmo ano (2006) "
        "da usina irmã WRI1006415 (CTCC ESCOMBRERAS), em ESP-VAL"),
}

# Usinas DESCARTADAS por erro de coordenada/país do GPPD sem fonte de correção.
# Nenhuma delas é inventada de volta: o motivo fica no meta.
GPPD_DISCARD = {
    "WRI1006494": "GARONA VERDA (ESP, 14,95 MW): lat 0,7046/lon 42,8025 fica na "
                  "Somália; dono espanhol, sem usina irmã para corrigir — descartada",
    "WRI1007043": "P.E. FERREIRA 2 (ESP, 49,5 MW): lat -2,9997/lon 37,2174 fica "
                  "na Tanzânia/Quênia; dono espanhol, sem usina irmã — descartada",
    "WRI1023535": "P.E. FOLCH II (ESP, 64,5 MW): lat -0,2108/lon 40,4682 fica no "
                  "Quênia/Somália; dono espanhol, sem usina irmã — descartada",
    "WRI1005410": "Outside the Northeast (NLD, 144 MW): lat 38,0211/lon -87,5172 "
                  "fica em Indiana (EUA); país do GPPD inconsistente com a "
                  "coordenada e sem fonte para corrigir — descartada",
}

# Pares (gppd_country_game, atlas_nation) de território disputado/ocupado.
OCCUPIED_PAIRS = {
    ("UKR", "RUS"),   # Crimeia/Donbas: controlador do atlas
    ("ISR", "PSX"),   # Gaza: controlador do atlas
    ("SRB", "XKX"),   # Kosovo: controlador do atlas
}

# Bboxes de territórios ocupados/disputados que não são país inteiro.
OCCUPIED_BBOXES = [
    ("melilla", 35.20, 35.40, -3.10, -2.80,
     "Melilla: enclave espanhol no Marrocos; controlador do atlas"),
]

# Bboxes de territórios ultramarinos que o atlas simplificou para outra nação.
# A usina fica na cidade do atlas (regra do jogo); a causa é declarada.
DELETED_BBOXES = [
    ("acores_madeira", 30.0, 40.0, -31.5, -15.0,
     "Açores/Madeira: província do atlas em ESP-LAS"),
    ("canarias", 27.0, 29.5, -18.5, -13.0,
     "Canárias: província do atlas em MAR-AGA"),
    ("reuniao_maiole", -22.0, -11.0, 44.0, 56.0,
     "Reunião/Mayotte: província do atlas em MDG-TOA"),
    ("antilhas_francesas", 14.0, 18.5, -62.5, -60.5,
     "Guadalupe/Martinica/São Martinho: província do atlas em DMA/LCA/ATG"),
    ("caribe_holandes", 17.0, 18.0, -63.5, -62.5,
     "Santo Eustáquio: província do atlas em KNA-BAS"),
    ("cayman", 18.0, 20.0, -82.0, -79.0,
     "Ilhas Cayman: província do atlas em CUB-HAV"),
    ("mauricio", -20.6, -19.9, 57.0, 58.0,
     "Maurício: país fora da lista jogável; província do atlas em MDG-TOA"),
    ("cabo_verde", 14.5, 17.2, -25.5, -22.5,
     "Cabo Verde: país fora da lista jogável; província do atlas em SEN-DAK"),
    ("sahara_ocidental", 20.0, 27.5, -17.5, -12.0,
     "Saara Ocidental: controlador do atlas (MAR)"),
]

# Países do GPPD sem nação jogável correspondente (além dos aliases acima).
DELETED_COUNTRIES = {"MUS", "CPV", "ATA", "ESH"}


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


def in_bbox(lat, lon, box):
    _name, lat_min, lat_max, lon_min, lon_max, _note = box
    return lat_min <= lat <= lat_max and lon_min <= lon <= lon_max


def classify_location(gppd_country, gppd_game, atlas_nation, lat, lon):
    """Causa do conflito (None quando não há conflito).

    ``gppd_game`` é o código do GPPD traduzido para o jogo (ou "" quando o
    país não existe no jogo). ``atlas_nation`` é a nação da província/cidade
    do atlas.
    """
    if not atlas_nation:
        return None, None
    if gppd_game and gppd_game == atlas_nation:
        return None, None

    # Territórios ocupados/disputados: vale o controlador do atlas.
    if (gppd_country, atlas_nation) in OCCUPIED_PAIRS:
        return "occupied", "controlador do atlas (%s)" % atlas_nation
    if gppd_country == "ESH":
        return "occupied", "Saara Ocidental: controlador do atlas (MAR)"
    for box in OCCUPIED_BBOXES:
        if in_bbox(lat, lon, box):
            return "occupied", box[5]

    # País do GPPD sem nação jogável: território deletado/simplificado.
    if not gppd_game:
        return "deleted_territory", "país do GPPD (%s) fora da lista jogável" % gppd_country
    if gppd_country in DELETED_COUNTRIES:
        return "deleted_territory", "país do GPPD (%s) fora da lista jogável" % gppd_country
    for box in DELETED_BBOXES:
        if in_bbox(lat, lon, box):
            return "deleted_territory", box[5]

    # Fronteira/costa: o pixel do atlas caiu no vizinho.
    return "border_pixel", "pixel de fronteira/costa do atlas"


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
    discarded = []
    fixed = []
    for row in rows:
        gppd_id = row["gppd_idnr"]
        if gppd_id in GPPD_DISCARD:
            discarded.append({
                "id": gppd_id,
                "name": row["name"],
                "gppd_country": row["country"],
                "capacity_mw": round(float(row["capacity_mw"]), 3),
                "lat": round(float(row["latitude"]), 6),
                "lon": round(float(row["longitude"]), 6),
                "reason": GPPD_DISCARD[gppd_id],
            })
            continue

        lat = float(row["latitude"])
        lon = float(row["longitude"])
        coord_note = None
        if gppd_id in GPPD_COORD_FIXES:
            lat, lon, coord_note = GPPD_COORD_FIXES[gppd_id]
            fixed.append({"id": gppd_id, "name": row["name"],
                          "before": {"lat": round(float(row["latitude"]), 6),
                                     "lon": round(float(row["longitude"]), 6)},
                          "after": {"lat": lat, "lon": lon},
                          "evidence": coord_note})
        pid, rx, ry = province_at(ids, lon, lat)
        city_id = ""
        city_share = None
        if pid in owner:
            city_id, city_share = owner[pid]
        elif pid not in province_by_id:
            pid = 0
        province_nation = ""
        if pid:
            province_nation = tag_by_atlas.get(
                province_by_id.get(pid, {}).get("nation", ""), "")
        city_nation = city_by_id[city_id]["tag"] if city_id else ""
        # Regra (a): a usina pertence ao controlador da cidade/província no mapa.
        nation = city_nation or province_nation
        # Sem cidade não há agregado por nação: fica declarado, nunca rateado.
        if not city_id:
            nation = ""
        gppd_country = row["country"]
        gppd_game = tag_by_code.get(gppd_country, "")
        alias = GPPD_ISO3_ALIASES.get(gppd_country)
        if not gppd_game and alias:
            gppd_game = tag_by_code.get(alias, "")
        cause, cause_note = classify_location(
            gppd_country, gppd_game, province_nation, lat, lon)
        if city_id and province_nation and city_nation and city_nation != province_nation:
            # Não deve acontecer com a réplica do jogo; se acontecer, declara.
            cause = cause or "border_pixel"
            cause_note = (cause_note or "") + "; cidade %s != província %s" % (
                city_nation, province_nation)
        fuel_source = row["primary_fuel"]
        group = FUEL_GROUP.get(fuel_source, "other")
        year = row["commissioning_year"]
        record = {
            "id": gppd_id,
            "name": row["name"],
            "gppd_country": gppd_country,
            "gppd_country_game": gppd_game or None,
            "gppd_country_alias": alias,
            "province_nation": province_nation or None,
            "nation": nation or None,
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
            "note": coord_note or (None if city_id else (
                "province_not_assigned" if pid else "no_province_at_point")),
        }
        if cause:
            record["location_conflict"] = province_nation or None
            record["location_cause"] = cause
            record["location_note"] = cause_note
        plants.append({key: value for key, value in record.items()
                       if value is not None or key in REQUIRED_KEYS})

    # Agregado por cidade = unidade da simulação. `nation` vem da cidade.
    by_city = {}
    for plant in plants:
        if not plant["city_id"]:
            continue
        city_id = plant["city_id"]
        mw = plant["capacity_mw"]
        entry = by_city.setdefault(city_id, {
            "city_id": city_id,
            "nation": city_by_id[city_id]["tag"],
            "total_mw": 0.0,
            "plants": 0,
            "by_fuel": {},
        })
        entry["total_mw"] += mw
        entry["plants"] += 1
        entry["by_fuel"][plant["fuel_group"]] = round(
            entry["by_fuel"].get(plant["fuel_group"], 0.0) + mw, 3)
    for entry in by_city.values():
        entry["total_mw"] = round(entry["total_mw"], 3)

    # Agregado por nação = soma EXATA das cidades do atlas (invariante QA).
    by_nation = {}
    for entry in by_city.values():
        tag = entry["nation"]
        nation_entry = by_nation.setdefault(tag, {
            "nation": tag,
            "total_mw": 0.0,
            "plants": 0,
            "by_fuel": {},
            "mw_without_city": 0.0,
            "area_km2": 0.0,
            "provinces": 0,
        })
        nation_entry["total_mw"] += entry["total_mw"]
        nation_entry["plants"] += entry["plants"]
        for fuel, value in entry["by_fuel"].items():
            nation_entry["by_fuel"][fuel] = round(
                nation_entry["by_fuel"].get(fuel, 0.0) + value, 3)
    for tag, entry in by_nation.items():
        entry["total_mw"] = round(entry["total_mw"], 3)
        entry["mw_without_city"] = 0.0
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
        entry["mw_per_1000_km2"] = (
            round(entry["total_mw"] / entry["area_km2"] * 1000.0, 3)
            if entry["area_km2"] > 0 else None)

    total_mw = round(sum(p["capacity_mw"] for p in plants), 3)
    by_city_mw = round(sum(e["total_mw"] for e in by_city.values()), 3)
    by_nation_mw = round(sum(e["total_mw"] for e in by_nation.values()), 3)
    without_city = [p for p in plants if not p["city_id"]]
    mw_without_city = round(sum(p["capacity_mw"] for p in without_city), 3)

    cause_counts = {}
    cause_mw = {}
    for plant in plants:
        cause = plant.get("location_cause")
        if not cause:
            continue
        cause_counts[cause] = cause_counts.get(cause, 0) + 1
        cause_mw[cause] = round(cause_mw.get(cause, 0.0) + plant["capacity_mw"], 3)

    pairs = {}
    for plant in plants:
        other = plant.get("location_conflict")
        if not other:
            continue
        key = "%s>%s" % (plant["gppd_country"], other)
        pairs[key] = pairs.get(key, 0) + 1

    meta = {
        "generated_by": "tools/build_energy_plants.py",
        "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
        "data_year": 2021,
        "owner_decision": "D-115(a): usina pertence ao controlador da cidade/província no mapa",
        "park_source": {
            "dataset": "Global Power Plant Database v1.3.0",
            "publisher": "World Resources Institute (WRI)",
            "url": raw.get("source_url"),
            "accessed_at_utc": raw.get("accessed_at_utc"),
            "sha256_csv": raw.get("sha256"),
            "license": {"name": "CC BY 4.0", "commercial_ok": True,
                        "url": "https://creativecommons.org/licenses/by/4.0/"},
            "citation": raw.get("citation"),
            "reference_year": 2021,
            "note": ("capacidade NÃO extrapolada; cada usina é o registro de ~2021 "
                     "do GPPD (method gppd_2021_unadjusted). O escalonamento para "
                     "2024-2025 é power_plants_2026_scaled.json"),
        },
        "geography": {
            "province_source": "map/map_data/atlas/provinces.json + world_province.png",
            "city_assignment": ("tools/build_energy_common.py — réplica de "
                                "GeographyService.assign_provinces (província -> cidade "
                                "da mesma nação e landmass, partição por área)"),
            "city_rule": ("quando a província é dividida entre cidades, a usina vai para "
                          "a cidade com maior fração (city_share registrado)"),
            "nation_rule": ("nation = tag da cidade dona da província no atlas; "
                            "gppd_country preserva o ISO3 bruto do GPPD"),
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
            "by_cause": {cause: {"plants": cause_counts[cause],
                                 "mw": cause_mw[cause]}
                         for cause in sorted(cause_counts)},
            "coord_error": {
                "cause": "gppd_coord_error",
                "plants": len(fixed) + len(discarded),
                "mw": round(sum(item["capacity_mw"] for item in discarded)
                            + sum(plant["capacity_mw"] for plant in plants
                                  if plant["id"] in GPPD_COORD_FIXES), 3),
                "corrected": len(fixed),
                "discarded": len(discarded),
                "note": ("não entra em `count`: a usina corrigida deixa de ser "
                         "conflito e a descartada sai do parque; o detalhe fica em "
                         "`coord_fixes` e `discarded`"),
            },
            "rule": ("conflito = nação da província/cidade do atlas != código do GPPD "
                     "traduzido para o jogo. A usina fica no controlador do atlas; "
                     "gppd_country preserva a origem e location_cause declara a causa"),
            "causes": ["gppd_coord_error", "deleted_territory", "border_pixel", "occupied"],
            "top_pairs": [
                {"pair": key, "plants": count}
                for key, count in sorted(pairs.items(), key=lambda kv: -kv[1])[:12]
            ],
        },
        "coord_fixes": fixed,
        "discarded": {
            "count": len(discarded),
            "mw": round(sum(d["capacity_mw"] for d in discarded), 3),
            "rule": ("gppd_coord_error sem fonte de correção: a usina é descartada, "
                     "nunca realocada por palpite"),
            "items": discarded,
        },
        "aliases": GPPD_ISO3_ALIASES,
        "counts": {
            "plants": len(plants),
            "with_city": sum(1 for p in plants if p["city_id"]),
            "with_nation": sum(1 for p in plants if p["nation"]),
            "cities_with_plants": len(by_city),
            "nations_with_plants": len(by_nation),
            "total_mw": total_mw,
            "mw_without_city": mw_without_city,
            "plants_without_city": len(without_city),
            "by_city_mw": by_city_mw,
            "by_nation_mw": by_nation_mw,
            "discarded_mw": round(sum(d["capacity_mw"] for d in discarded), 3),
        },
        "invariant": {
            "rule": "sum(by_city[].total_mw) == sum(by_nation[].total_mw) (<= 0.01 MW)",
            "by_city_mw": by_city_mw,
            "by_nation_mw": by_nation_mw,
            "difference_mw": round(by_city_mw - by_nation_mw, 6),
            "ok": abs(by_city_mw - by_nation_mw) <= 0.01,
            "total_mw": total_mw,
            "mw_without_city": mw_without_city,
            "total_matches": abs(total_mw - by_nation_mw - mw_without_city) <= 0.05,
            "total_note": ("o total das usinas e a soma dos agregados podem diferir "
                           "em até 0,05 MW pelo arredondamento de 3 casas por cidade; "
                           "a invariante QA é entre by_city e by_nation"),
        },
    }

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

    print("usinas: %d | com cidade: %d | com nação: %d | descartadas: %d"
          % (meta["counts"]["plants"], meta["counts"]["with_city"],
             meta["counts"]["with_nation"], len(discarded)))
    print("total MW: %.1f | cidades: %d | nações: %d | sem cidade: %.1f MW"
          % (total_mw, len(by_city), len(by_nation), mw_without_city))
    print("invariante: by_city %.3f == by_nation %.3f (dif %.6f)"
          % (by_city_mw, by_nation_mw, by_city_mw - by_nation_mw))
    print("conflitos: %d usinas / %.1f MW | causas: %s"
          % (meta["location_conflicts"]["count"], meta["location_conflicts"]["mw"],
             ", ".join("%s=%d" % (k, v["plants"])
                       for k, v in sorted(meta["location_conflicts"]["by_cause"].items()))))
    return 0


def _compact(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


if __name__ == "__main__":
    sys.exit(main())
