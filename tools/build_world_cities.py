"""Gera data/definitions/world/out/ a partir das fontes do mundo (2026).

Etapa de projeção e validação. Não roda Godot. Sem `--write-scenario` não
altera data/definitions/scenario/; com a flag, promove out/ para o cenário
(CSVs), migra os ids das cidades da fase 1 que continuam sendo a MESMA cidade
(nome/coordenada), completa `port_of` dos portos órfãos e mescla o i18n.

Entrada:
    data/definitions/world/nations_source.csv
    data/definitions/world/cities_source.csv
    data/definitions/world/ports_source.csv
    data/definitions/world/_import_report.csv          (raio de cobertura)
    data/definitions/world/_coverage_before_after.csv  (antes/depois)
    data/definitions/scenario/{cities,ports,buildings}.csv (contrato legado)

Saída:
    data/definitions/world/out/nations.csv
    data/definitions/world/out/cities.csv
    data/definitions/world/out/ports.csv
    data/definitions/world/out/buildings.csv
    data/definitions/world/out/i18n.csv
    data/definitions/world/out/_meta.json

Regras (docs/plans/world_all_data.md):
    cota = 1 + clamp(1 + floor(pop/25M) + floor(PIB_PPP_bn/1500)
                     + floor(área_km²/600k), 1, 24)
    cobertura de cidades-fonte ≤ 700 km; porto separado só com costa a ≥ 15 km.

Uso:
    python tools/build_world_cities.py            # gera out/
    python tools/build_world_cities.py --check    # valida sem escrever
    python tools/build_world_cities.py --write-scenario  # promove para o cenário
"""
from __future__ import annotations

import argparse
import colorsys
import csv
import json
import math
import os
import sys
import unicodedata
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
WORLD = os.path.join(ROOT, "data", "definitions", "world")
OUT = os.path.join(WORLD, "out")
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")
MAP_W, MAP_H = 5000.0, 2500.0
COVER_KM = 700.0

# Portos do cenário anterior à fase 1 (commit 57dc31e): continuam por contrato,
# com o mesmo id e as mesmas coordenadas, mesmo depois de o cenário crescer.
LEGACY_PORTS = [
    "GBR-DOV", "FRA-CAL", "DEU-HAM", "NED-AMS", "BEL-ANT", "FRA-CAY", "VEN-LAG",
    "SUR-PBM", "GUY-GEO", "FRA-LEH", "FRA-MRS", "FRA-NTE", "NED-RTM", "GBR-GLA",
    "GBR-BRS", "GBR-NCL", "VEN-MAR", "GBR-BFS",
]

# Cidade-estado com uma única cidade: a cota mínima de 2 não se aplica.
SINGLE_CITY = {"VAT"}

# Tom "papelaria": dessatura as cores do CSV sem mudar matiz nem luminosidade.
# O usuário achou as cores da F2 saturadas demais para o mapa de papel; 14% de
# saturação a menos mantém o ΔE entre vizinhos de FRONTEIRA ≥ 30 depois da
# auditoria feita com a adjacência de TERRA do mapa (ver docs/plans/world_all_data.md).
COLOR_SATURATION = 0.86
# Único par que continua abaixo de ΔE 30 na adjacência de terra do mapa depois
# do tom: BWA×AGO (29,6 antes). O novo tom fica a ΔE 33,1 do vizinho (o mais
# próximo) e a 8,4 do tom original, preservando a identidade da cor.
COLOR_TONE_OVERRIDE = {"BWA": "#a54062"}
# Distância máxima (km) para considerar que uma cidade da fase 1 e uma cidade
# re-derivada da fonte são a MESMA cidade. Nome normalizado igual aceita até
# 120 km (a fonte às vezes escolhe outro núcleo da mesma aglomeração); sem nome
# igual, só casamos até 15 km (mesma mancha urbana).
MATCH_NAME_KM = 120.0
MATCH_COORD_KM = 15.0


def tone_color(hexcolor):
    """Dessatura `hexcolor` mantendo matiz e luminosidade (HSL)."""
    value = hexcolor.lstrip("#")
    rgb = tuple(int(value[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    hue, light, sat = colorsys.rgb_to_hls(*rgb)
    r, g, b = colorsys.hls_to_rgb(hue, light, sat * COLOR_SATURATION)
    return "#%02x%02x%02x" % tuple(round(c * 255) for c in (r, g, b))


def scenario_color(iso3, hexcolor):
    """Cor final do cenário: tom uniforme + correção pontual documentada."""
    return COLOR_TONE_OVERRIDE.get(iso3, tone_color(hexcolor))


def norm_name(text):
    text = unicodedata.normalize("NFKD", text)
    return " ".join("".join(c for c in text if not unicodedata.combining(c))
                    .lower().replace("-", " ").split())


def km_between_px(a, b):
    """Distância aproximada em km entre dois pontos x/y da projeção global."""
    dx = (float(a[0]) - float(b[0])) * 40075.0 / MAP_W
    dy = (float(a[1]) - float(b[1])) * 20037.0 / MAP_H
    return math.hypot(dx, dy)


def lonlat_to_xy(lon, lat):
    return (lon + 180.0) / 360.0 * MAP_W, (90.0 - lat) / 180.0 * MAP_H


def read_csv(path):
    with open(path, encoding="utf-8-sig", newline="") as fh:
        return list(csv.DictReader(fh))


def write_csv(directory, name, rows, fields):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, name), "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: row.get(k, "") for k in fields})


def quota(population, gdp_bn, area_km2):
    extra = 1 + int(population // 25_000_000) + int(gdp_bn // 1500) + int(area_km2 // 600_000)
    return 1 + max(1, min(24, extra))


def building_code(city_id, legacy):
    if legacy:
        return city_id.split("-", 1)[1].lower()
    return city_id.lower().replace("-", "_")


def plan_city_ids(cities_out, scenario_cities, legacy_ids):
    """Mapeia o id da fonte (out) para o id FINAL do cenário.

    Contrato: as 40 cidades legadas mantêm o id (já são o mesmo id em out). As
    demais cidades da fase 1 mantêm o id anterior quando a cidade física é a
    mesma — nome normalizado igual a ≤120 km ou coordenada a ≤15 km, mesma
    nação. Uma cidade antiga só pode corresponder a uma cidade nova (casamento
    guloso e determinístico por menor distância).
    """
    by_nation = defaultdict(list)
    for row in scenario_cities.values():
        if row["id"] not in legacy_ids:
            by_nation[row["nation"]].append(row)
    pairs = []
    for row in cities_out:
        if row["id"] in legacy_ids:
            continue
        for old in by_nation.get(row["nation"], []):
            distance = km_between_px((row["x"], row["y"]), (old["x"], old["y"]))
            same_name = norm_name(row["name"]) == norm_name(old["name"])
            if same_name and distance <= MATCH_NAME_KM:
                pairs.append(((0, distance, old["id"], row["id"]), row, old))
            elif distance <= MATCH_COORD_KM:
                pairs.append(((1, distance, old["id"], row["id"]), row, old))
    pairs.sort(key=lambda item: item[0])
    used_old, used_new, matches = set(), set(), {}
    for _score, row, old in pairs:
        if old["id"] in used_old or row["id"] in used_new:
            continue
        used_old.add(old["id"])
        used_new.add(row["id"])
        matches[row["id"]] = old["id"]
    mapping = {}
    taken = set(legacy_ids)
    for row in cities_out:
        out_id = row["id"]
        final_id = matches.get(out_id, out_id)
        if final_id in taken:
            final_id = out_id
        taken.add(final_id)
        mapping[out_id] = final_id
    dropped = [cid for cid in scenario_cities if cid not in legacy_ids
               and cid not in used_old]
    stats = {
        "legacy": len(legacy_ids),
        "kept_old_id": len(matches),
        "same_name": sum(1 for row in cities_out if row["id"] in matches
                         and norm_name(row["name"]) == norm_name(
                             scenario_cities[matches[row["id"]]]["name"])),
        "dropped_phase1": len(dropped),
        "dropped_ids": sorted(dropped),
        "new_id": sum(1 for row in cities_out if mapping[row["id"]] == row["id"]
                      and row["id"] not in legacy_ids),
    }
    return mapping, stats


def assign_port_of(cities, ports):
    """Completa `port_of` com a cidade da MESMA nação mais próxima (km)."""
    by_nation = defaultdict(list)
    for city in cities:
        by_nation[city["nation"]].append(city)
    result = {}
    for port in ports:
        existing = port.get("port_of", "").strip()
        if existing:
            result[port["id"]] = existing
            continue
        best_id, best_d = "", float("inf")
        for city in by_nation.get(port["nation"], []):
            distance = km_between_px((port["x"], port["y"]), (city["x"], city["y"]))
            if distance < best_d or (distance == best_d and city["id"] < best_id):
                best_id, best_d = city["id"], distance
        result[port["id"]] = best_id
    return result


def merge_i18n(existing, generated, old_nations, old_cities, old_ports):
    """Mescla o i18n gerado no CSV existente, preservando as chaves de UI.

    Remove só as chaves de nação/cidade/porto que deixaram de existir (e as que
    serão reescritas) e mantém todo o resto intacto.
    """
    def is_entity_key(key):
        if key.startswith("city.name.") or key.startswith("port.name."):
            return True
        if key.startswith("nation.name."):
            return True
        if key.startswith("nation.") and key.split(".", 1)[1] in old_nations:
            return True
        return False

    merged = {row["key"]: row for row in existing if not is_entity_key(row["key"])}
    for row in generated:
        merged[row["key"]] = row
    return [merged[key] for key in sorted(merged)]


def promote_scenario(nations_out, cities_out, ports_out, buildings_out, i18n_out,
                     scenario_cities, scenario_ports, scenario_nations):
    """Escreve os CSVs do cenário com ids migrados e `port_of` completo."""
    source_legacy = read_csv(os.path.join(WORLD, "cities_source.csv"))
    legacy_ids = {row["id"] for row in source_legacy if row["legacy"] == "yes"}
    mapping, stats = plan_city_ids(cities_out, scenario_cities, legacy_ids)

    scenario_cities_out = []
    for row in cities_out:
        final = row["id"] if row["id"] in legacy_ids else mapping[row["id"]]
        scenario_cities_out.append({
            "id": final, "nation": row["nation"], "home_nation": row["home_nation"],
            "name": row["name"], "population": row["population"], "x": row["x"],
            "y": row["y"], "landmass": row["landmass"], "capital": row["capital"],
        })
    scenario_cities_out.sort(key=lambda r: r["id"])

    # `port_of` usa os ids FINAIS das cidades.
    final_cities = [{"id": (row["id"] if row["id"] in legacy_ids else mapping[row["id"]]),
                     "nation": row["nation"], "x": row["x"], "y": row["y"]}
                    for row in cities_out]
    port_of = assign_port_of(final_cities, ports_out)
    ports_final = []
    for row in ports_out:
        ports_final.append({
            "id": row["id"], "name": row["name"], "nation": row["nation"],
            "x": row["x"], "y": row["y"], "landmass": row["landmass"],
            "port_of": port_of.get(row["id"], ""),
        })
    ports_final.sort(key=lambda r: r["id"])

    buildings_final = []
    for row in buildings_out:
        city_id = row["city_id"]
        final = city_id if city_id in legacy_ids else mapping[city_id]
        code = building_code(final, final in legacy_ids)
        buildings_final.append({
            "city_id": final,
            "building_id": f"{code}_wheat",
            "name": row["name"],
            "output_tons_per_day": row["output_tons_per_day"],
        })
    buildings_final.sort(key=lambda r: r["city_id"])

    # i18n: a chave da cidade acompanha o id final; o valor não muda.
    generated_i18n = []
    for row in i18n_out:
        entry = dict(row)
        if entry["key"].startswith("city.name."):
            out_id = entry["key"].split(".", 2)[2]
            final = out_id if out_id in legacy_ids else mapping.get(out_id, out_id)
            entry["key"] = f"city.name.{final}"
        generated_i18n.append(entry)

    scenario_fields = ["id", "name", "flag", "color", "capital", "currency",
                       "government", "ai_profile", "region", "population",
                       "gdp_ppp_bn"]
    city_fields = ["id", "nation", "home_nation", "name", "population", "x", "y",
                   "landmass", "capital"]
    port_fields = ["id", "name", "nation", "x", "y", "landmass", "port_of"]
    building_fields = ["city_id", "building_id", "name", "output_tons_per_day"]
    i18n_fields = ["key", "pt_BR", "en", "es"]

    write_csv(SCENARIO, "nations.csv", nations_out, scenario_fields)
    write_csv(SCENARIO, "cities.csv", scenario_cities_out, city_fields)
    write_csv(SCENARIO, "ports.csv", ports_final, port_fields)
    write_csv(SCENARIO, "buildings.csv", buildings_final, building_fields)

    i18n_path = os.path.join(ROOT, "data", "definitions", "i18n", "i18n.csv")
    existing_i18n = read_csv(i18n_path) if os.path.exists(i18n_path) else []
    merged = merge_i18n(existing_i18n, generated_i18n,
                        set(scenario_nations), set(scenario_cities),
                        set(scenario_ports))
    with open(i18n_path, "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=i18n_fields)
        writer.writeheader()
        for row in merged:
            writer.writerow({k: row.get(k, "") for k in i18n_fields})

    return stats, ports_final, merged


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="valida sem escrever")
    parser.add_argument("--write-scenario", action="store_true",
                        help="promove out/ para data/definitions/scenario (CSV) e mescla i18n")
    args = parser.parse_args()

    nations_src = read_csv(os.path.join(WORLD, "nations_source.csv"))
    cities_src = read_csv(os.path.join(WORLD, "cities_source.csv"))
    ports_src = read_csv(os.path.join(WORLD, "ports_source.csv"))
    scenario_cities = {r["id"]: r for r in read_csv(os.path.join(SCENARIO, "cities.csv"))}
    scenario_ports = {r["id"]: r for r in read_csv(os.path.join(SCENARIO, "ports.csv"))}
    scenario_nations = {r["id"]: r for r in read_csv(os.path.join(SCENARIO, "nations.csv"))}
    scenario_buildings = {r["city_id"]: r for r in
                          read_csv(os.path.join(SCENARIO, "buildings.csv"))}
    report = {}
    report_path = os.path.join(WORLD, "_import_report.csv")
    if os.path.exists(report_path):
        report = {r["tag"]: r for r in read_csv(report_path)}

    problems = []
    nations_by_id = {r["iso3"]: r for r in nations_src}
    if len(nations_by_id) != len(nations_src):
        problems.append("nações: iso3 duplicado")
    for row in nations_src:
        if not row["name_en"] or not row["name_pt"] or not row["name_es"]:
            problems.append(f"{row['iso3']}: nome i18n vazio")
        if not row["iso2"]:
            problems.append(f"{row['iso3']}: sem iso2")
    for row in cities_src:
        if row["nation"] not in nations_by_id:
            problems.append(f"cidade {row['id']}: nação desconhecida {row['nation']}")
        if not row["name_pt"] or not row["name_es"]:
            problems.append(f"cidade {row['id']}: sem pt/es")
    for row in ports_src:
        if row["nation"] not in nations_by_id:
            problems.append(f"porto {row['id']}: nação desconhecida {row['nation']}")

    by_nation = defaultdict(list)
    for row in cities_src:
        by_nation[row["nation"]].append(row)

    for nation in nations_src:
        iso3 = nation["iso3"]
        cities = by_nation.get(iso3, [])
        if not cities:
            problems.append(f"{iso3}: nenhuma cidade")
            continue
        capitals = [c for c in cities if c["capital"] == "1"]
        if len(capitals) != 1:
            problems.append(f"{iso3}: {len(capitals)} capitais (esperado 1)")
        pop = float(nation["population"] or 0)
        gdp = float(nation["gdp_ppp_bn"] or 0)
        area = float(nation["area_km2"] or 0)
        expected = quota(pop, gdp, area)
        if iso3 in SINGLE_CITY:
            expected = 1
        if int(nation["quota"]) != expected:
            problems.append(f"{iso3}: cota da fonte {nation['quota']} != recalculada {expected}")
        if report.get(iso3):
            if float(report[iso3]["settle_km"]) > COVER_KM + 1:
                problems.append(f"{iso3}: cobertura {report[iso3]['settle_km']} km")

    # contrato legado: as 40 cidades com legacy=yes (id, coordenadas, população
    # e nação iguais às do cenário). VEN-PAY foi substituída por VEN-CBL, que
    # ainda não existe no cenário (a promoção é outra tarefa).
    legacy_cities = [r for r in cities_src if r["legacy"] == "yes"]
    if len(legacy_cities) != 40:
        problems.append(f"legado: {len(legacy_cities)} cidades com legacy=yes (esperado 40)")
    found_ids = {r["id"] for r in legacy_cities}
    for cid in found_ids:
        if cid not in scenario_cities:
            problems.append(f"legado: {cid} não está no cenário")
    for cid in ("VEN-PAY",):
        if cid in found_ids or any(r["id"] == cid for r in cities_src):
            problems.append(f"legado: {cid} deveria ter sido substituída")
    if "VEN-CBL" not in {r["id"] for r in cities_src}:
        problems.append("legado: VEN-CBL (Ciudad Bolívar) ausente da fonte")
    for row in legacy_cities:
        src = scenario_cities.get(row["id"])
        if not src:
            continue
        x, y = lonlat_to_xy(float(row["lon"]), float(row["lat"]))
        for field, value in (("x", x), ("y", y)):
            if abs(float(src[field]) - value) > 0.2:
                problems.append(f"legado {row['id']}: {field} divergente")
        if int(src["population"]) != int(row["population"]):
            problems.append(f"legado {row['id']}: população divergente")
        if src["nation"] != row["nation"]:
            problems.append(f"legado {row['id']}: nação divergente")

    # portos: os 18 do cenário anterior à fase 1 continuam intactos
    source_ports = {r["id"]: r for r in ports_src}
    for pid in LEGACY_PORTS:
        row = source_ports.get(pid)
        if not row:
            problems.append(f"porto legado {pid} ausente da fonte")
            continue
        src = scenario_ports.get(pid)
        if not src:
            problems.append(f"porto legado {pid} fora do cenário")
            continue
        x, y = lonlat_to_xy(float(row["lon"]), float(row["lat"]))
        if abs(float(src["x"]) - x) > 0.2 or abs(float(src["y"]) - y) > 0.2:
            problems.append(f"porto legado {pid}: coordenada divergente")
    scenario_ports_src = [r for r in ports_src
                          if r["source"].startswith("scenario/ports.csv")]
    if len(scenario_ports_src) != len(scenario_ports):
        problems.append(f"portos do cenário: {len(scenario_ports_src)} carregados "
                        f"de {len(scenario_ports)}")

    # saída
    nations_out, cities_out, ports_out, buildings_out, i18n_out = [], [], [], [], []
    for nation in sorted(nations_src, key=lambda r: r["iso3"]):
        iso3 = nation["iso3"]
        nations_out.append({
            "id": iso3,
            "name": nation["name_en"],
            "flag": nation["iso2"].lower(),
            "color": scenario_color(iso3, nation["color"]),
            "capital": nation["capital_en"],
            "currency": nation["currency"],
            "government": nation["government"],
            "ai_profile": nation["ai_profile"],
            "region": nation["region"],
            "population": nation["population"],
            "gdp_ppp_bn": nation["gdp_ppp_bn"],
        })
        i18n_out.append({"key": f"nation.name.{iso3}", "pt_BR": nation["name_pt"],
                         "en": nation["name_en"], "es": nation["name_es"]})
        i18n_out.append({"key": f"nation.{iso3}", "pt_BR": nation["name_pt"],
                         "en": nation["name_en"], "es": nation["name_es"]})
        for city in sorted(by_nation.get(iso3, []),
                           key=lambda r: (r["capital"] != "1", -int(r["population"]),
                                          r["id"])):
            x, y = lonlat_to_xy(float(city["lon"]), float(city["lat"]))
            cities_out.append({
                "id": city["id"], "nation": iso3, "home_nation": iso3,
                "name": city["name_en"], "population": int(city["population"]),
                "x": round(x, 1), "y": round(y, 1),
                "landmass": city["landmass"], "capital": city["capital"],
            })
            legacy = city["legacy"] == "yes"
            fm = city["initial_farm_tons"] or f"{int(city['population']) * 0.0003:.1f}"
            buildings_out.append({
                "city_id": city["id"],
                "building_id": f"{building_code(city['id'], legacy)}_wheat",
                "name": f"{city['name_pt']} — trigo",
                "output_tons_per_day": fm,
            })
            i18n_out.append({"key": f"city.name.{city['id']}", "pt_BR": city["name_pt"],
                             "en": city["name_en"], "es": city["name_es"]})

    for port in sorted(ports_src, key=lambda r: r["id"]):
        x, y = lonlat_to_xy(float(port["lon"]), float(port["lat"]))
        ports_out.append({
            "id": port["id"], "name": port["name"], "nation": port["nation"],
            "x": round(x, 1), "y": round(y, 1), "landmass": port["landmass"],
        })
        i18n_out.append({"key": f"port.name.{port['id']}", "pt_BR": port["name"],
                         "en": port["name"], "es": port["name"]})

    for label, rows in (("cidades", cities_out), ("portos", ports_out)):
        ids = [r["id"] for r in rows]
        if len(ids) != len(set(ids)):
            problems.append(f"{label}: ids duplicados")
    i18n_out.sort(key=lambda r: r["key"])

    counts = Counter(r["nation"] for r in cities_out)
    top10 = counts.most_common(10)
    print(f"nações: {len(nations_out)}")
    print(f"cidades: {len(cities_out)} (legado: {sum(1 for c in cities_src if c['legacy'] == 'yes')})")
    print(f"portos: {len(ports_out)} (do cenário: {len(scenario_ports_src)}, "
          f"originais: {len(LEGACY_PORTS)})")
    print(f"construções: {len(buildings_out)}")
    print(f"chaves i18n: {len(i18n_out)}")
    print("top 10 nações por cidades:", ", ".join(f"{k} {v}" for k, v in top10))
    for p in problems:
        print(f"AVISO: {p}")

    if args.check:
        return 1 if problems else 0

    nation_fields = ["id", "name", "flag", "color", "capital", "currency", "government",
                     "ai_profile", "region", "population", "gdp_ppp_bn"]
    city_fields = ["id", "nation", "home_nation", "name", "population", "x", "y",
                   "landmass", "capital"]
    port_fields = ["id", "name", "nation", "x", "y", "landmass"]
    building_fields = ["city_id", "building_id", "name", "output_tons_per_day"]
    i18n_fields = ["key", "pt_BR", "en", "es"]

    write_csv(OUT, "nations.csv", nations_out, nation_fields)
    write_csv(OUT, "cities.csv", cities_out, city_fields)
    write_csv(OUT, "ports.csv", ports_out, port_fields)
    write_csv(OUT, "buildings.csv", buildings_out, building_fields)
    write_csv(OUT, "i18n.csv", i18n_out, i18n_fields)

    coverage = read_csv(os.path.join(WORLD, "_coverage_before_after.csv")) \
        if os.path.exists(os.path.join(WORLD, "_coverage_before_after.csv")) else []
    meta = {
        "phase": "F2",
        "generated_by": "tools/build_world_cities.py",
        "projection": "x=(lon+180)/360*5000, y=(90-lat)/180*2500",
        "sources": {
            "population_nation": "World Bank WDI SP.POP.TOTL (último ano; ver nations_source.csv)",
            "gdp_ppp": "World Bank WDI NY.GDP.MKTP.PP.CD (último ano; ver nations_source.csv)",
            "cities": "GeoNames cities15000 + cities500 (2026, city proper)",
            "city_names": "Wikidata: rótulo pt-br (fallback pt, inglês); es (fallback inglês)",
            "area": "GeoNames countryInfo (2026)",
            "borders": "map/map_data/atlas/world_province.png",
            "coast": "map/map_data/atlas/world_sdf.png",
            "flags": "flagcdn.com w40 (domínio público)",
        },
        "rules": {
            "quota": "1 + clamp(1 + floor(pop/25M) + floor(gdp_bn/1500) + floor(area_km2/600k), 1, 24)",
            "coverage_km": COVER_KM,
            "border_km": 40,
            "border_small_km": 25,
            "small_area_km2": 50_000,
            "min_spacing_km": 25,
            "port_city_coast_km": 25,
            "port_point_km": 15,
            "legacy": "40 cidades do cenário preservadas, exceto VEN-PAY (trocada por VEN-CBL)",
            "color_min_delta_e": 30,
            "color_note": "ΔE(Lab) entre vizinhos de fronteira; auditoria e menor par em docs/plans/world_all_data.md",
        },
        "counts": {
            "nations": len(nations_out),
            "cities": len(cities_out),
            "ports": len(ports_out),
            "buildings": len(buildings_out),
            "i18n_keys": len(i18n_out),
            "legacy_cities": sum(1 for c in cities_src if c["legacy"] == "yes"),
            "legacy_ports": len(LEGACY_PORTS),
            "scenario_ports": len(scenario_ports_src),
            "cities_with_pt_exonym": sum(1 for c in cities_src
                                         if c["name_pt"] != c["name_en"]),
            "cities_with_es_exonym": sum(1 for c in cities_src
                                         if c["name_es"] != c["name_en"]),
            "cities_pt_same_as_en": sum(1 for c in cities_src
                                        if c["legacy"] == "no"
                                        and c["name_pt"] == c["name_en"]),
        },
        "top10": [{"nation": k, "cities": v} for k, v in top10],
        "coverage": coverage,
    }
    with open(os.path.join(OUT, "_meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    print(f"OK -> {os.path.relpath(OUT, ROOT)}")

    if args.write_scenario:
        stats, ports_final, merged = promote_scenario(
            nations_out, cities_out, ports_out, buildings_out, i18n_out,
            scenario_cities, scenario_ports, scenario_nations)
        missing = [row for row in ports_final if not row["port_of"]]
        print("")
        print("== cenário promovido ==")
        print(f"nações: {len(nations_out)} | cidades: {len(cities_out)} | "
              f"portos: {len(ports_final)} | construções: {len(buildings_out)}")
        print(f"ids mantidos da fase 1: {stats['kept_old_id']} "
              f"(nome igual: {stats['same_name']}) | ids novos: {stats['new_id']} "
              f"| legados: {stats['legacy']}")
        print(f"cidades antigas fora da seleção nova: {stats['dropped_phase1']}")
        if stats["dropped_ids"]:
            print("  " + ", ".join(stats["dropped_ids"]))
        print(f"portos com port_of: {len(ports_final) - len(missing)}/{len(ports_final)}")
        print(f"i18n: {len(merged)} chaves em data/definitions/i18n/i18n.csv")
        if missing:
            problems.append(f"portos sem port_of: {len(missing)}")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
