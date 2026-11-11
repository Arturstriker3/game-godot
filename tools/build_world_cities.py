"""Gera o cenário F1 (Europa + G20) a partir dos snapshots de fonte.

Fonte de edição (versionada, com `source` e `year` por linha):
    data/definitions/world/nations_source.csv
    data/definitions/world/cities_source.csv
    data/definitions/world/ports_source.csv

Saída (não consumida pelo jogo ainda — a F1 é só dado):
    data/definitions/world/out/nations.csv   schema do plano §5
    data/definitions/world/out/cities.csv    schema de data/definitions/scenario/cities.csv
    data/definitions/world/out/ports.csv     schema de data/definitions/scenario/ports.csv
    data/definitions/world/out/buildings.csv schema de data/definitions/scenario/buildings.csv
    data/definitions/world/out/i18n.csv      key,pt_BR,en,es (nation.name.*, city.name.*, port.name.*)
    data/definitions/world/out/_meta.json    contagens, fontes e anos-base

Regras (plano docs/plans/world_nations_plan.md §4):

    cidades = 1 (capital) + clamp(1 + floor(pop/25M) + floor(PIB_PPP/1,5e12), 1, 12)

O plano diz que a capital não entra no clamp, mas o §8 (A2) e o §9 fixam o teto em
12 cidades por nação. Com o literal `1 + clamp(...,1,12)` as cinco maiores nações
dariam 13, contra o teto declarado. Aqui o resultado é limitado a 12
(`min(12, 1 + clamp(...))`) e o desvio está registrado em
docs/plans/world_f1_data.md. Nenhuma cidade é inventada: se a fonte acaba, a
nação fica com menos cidades (ex.: Andorra, Luxemburgo, Malta, Suriname = 1).

As 40 cidades e os 18 portos do cenário atual são preservados por contrato
(coluna `legacy` = yes): mesmo id, mesmas coordenadas e mesma população.

Uso:
    python tools/build_world_cities.py            # gera out/
    python tools/build_world_cities.py --check    # só valida (não escreve)
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
WORLD = os.path.join(ROOT, "data", "definitions", "world")
OUT = os.path.join(WORLD, "out")
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")

MAP_W, MAP_H = 5000.0, 2500.0
MIN_SPACING_KM = 25.0
POP_PER_TON = 0.0003
MAX_CITIES = 12


def lonlat_to_xy(lon: float, lat: float) -> tuple[float, float]:
    """Projeção equirretangular do projeto (build_natural_geography.py)."""
    return (lon + 180.0) / 360.0 * MAP_W, (90.0 - lat) / 180.0 * MAP_H


def km_between(a: tuple[float, float], b: tuple[float, float]) -> float:
    lon1, lat1 = a
    lon2, lat2 = b
    dx = (lon2 - lon1) * 111.32 * math.cos(math.radians((lat1 + lat2) / 2.0))
    dy = (lat2 - lat1) * 110.57
    return math.hypot(dx, dy)


def read_csv(name: str) -> list[dict]:
    path = os.path.join(WORLD, name)
    with open(path, encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def quota(population: float, gdp_bn: float) -> int:
    """Cota de cidades da nação (capital incluída). 1 quando falta dado."""
    if not population or not gdp_bn:
        return 1
    extra = 1 + int(population // 25_000_000) + int(gdp_bn // 1500.0)
    return min(MAX_CITIES, 1 + max(1, min(MAX_CITIES, extra)))


def select_cities(rows: list[dict], target: int) -> list[dict]:
    """Capital + legado obrigatórios; o resto por população, com espaçamento."""
    must = [r for r in rows if r["legacy"] == "yes" or r["capital"] == "1"]
    rest = sorted(
        (r for r in rows if r not in must),
        key=lambda r: (-float(r["population"]), r["id"]),
    )
    chosen = list(must)
    for row in rest:
        if len(chosen) >= max(target, len(must)):
            break
        point = (float(row["lon"]), float(row["lat"]))
        if any(
            km_between(point, (float(c["lon"]), float(c["lat"]))) < MIN_SPACING_KM
            for c in chosen
        ):
            continue
        chosen.append(row)
    return chosen


def building_code(city_id: str) -> str:
    return city_id.split("-", 1)[1].lower()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="valida sem escrever")
    args = parser.parse_args()

    nations_src = read_csv("nations_source.csv")
    cities_src = read_csv("cities_source.csv")
    ports_src = read_csv("ports_source.csv")

    problems: list[str] = []
    if not nations_src or not cities_src:
        print("ERRO: fontes vazias em data/definitions/world/", file=sys.stderr)
        return 2

    known = {n["iso3"] for n in nations_src}
    for c in cities_src:
        if c["nation"] not in known:
            problems.append(f"cidade {c['id']} em nação desconhecida {c['nation']}")
        if not c["name_pt"] or not c["name_es"]:
            problems.append(f"cidade {c['id']} sem tradução pt/es")
    for p in ports_src:
        if p["nation"] not in known:
            problems.append(f"porto {p['id']} em nação desconhecida {p['nation']}")

    by_nation: dict[str, list[dict]] = {}
    for c in cities_src:
        by_nation.setdefault(c["nation"], []).append(c)

    nations_out: list[dict] = []
    cities_out: list[dict] = []
    buildings_out: list[dict] = []
    i18n_out: list[dict] = []

    for n in sorted(nations_src, key=lambda r: r["iso3"]):
        iso3 = n["iso3"]
        pop = float(n["population"]) if n["population"] else 0.0
        gdp = float(n["gdp_ppp_bn"]) if n["gdp_ppp_bn"] else 0.0
        target = quota(pop, gdp)
        if n["quota"] and int(n["quota"]) != target:
            problems.append(
                f"{iso3}: cota da fonte ({n['quota']}) != recalculada ({target})"
            )
        rows = by_nation.get(iso3, [])
        if not rows:
            problems.append(f"{iso3}: nenhuma cidade na fonte")
            continue
        chosen = select_cities(rows, target)
        chosen.sort(key=lambda r: (r["capital"] != "1", -float(r["population"]), r["id"]))
        for c in chosen:
            x, y = lonlat_to_xy(float(c["lon"]), float(c["lat"]))
            cities_out.append({
                "id": c["id"],
                "nation": iso3,
                "home_nation": iso3,
                "name": c["name_en"],
                "population": int(float(c["population"])),
                "x": round(x, 1),
                "y": round(y, 1),
                "landmass": c["landmass"],
                "capital": c["capital"],
            })
            farm = c["initial_farm_tons"]
            if not farm:
                farm = f"{float(c['population']) * POP_PER_TON:.1f}"
            buildings_out.append({
                "city_id": c["id"],
                "building_id": f"{building_code(c['id'])}_wheat",
                "name": f"{c['name_pt']} — trigo",
                "output_tons_per_day": farm,
            })
            i18n_out.append({"key": f"city.name.{c['id']}", "pt_BR": c["name_pt"],
                             "en": c["name_en"], "es": c["name_es"]})
        nations_out.append({
            "id": iso3,
            "name": n["name_en"],
            "flag": n["iso2"].lower(),
            "color": n["color"],
            "capital": n["capital_en"],
            "currency": n["currency"],
            "government": n["government"],
            "ai_profile": n["ai_profile"],
            "region": n["region"],
            "population": n["population"],
            "gdp_ppp_bn": n["gdp_ppp_bn"],
        })
        i18n_out.append({"key": f"nation.name.{iso3}", "pt_BR": n["name_pt"],
                         "en": n["name_en"], "es": n["name_es"]})
        extra = len(chosen) - (len(rows))
        if extra:
            problems.append(f"{iso3}: seleção inconsistente")
        if len(chosen) > MAX_CITIES:
            problems.append(f"{iso3}: {len(chosen)} cidades acima do teto {MAX_CITIES}")

    selected_ids = {c["id"] for c in cities_out}
    ports_out: list[dict] = []
    seen_ports: set[str] = set()
    for p in ports_src:
        if p["nation"] not in known:
            continue
        legacy_port = p["source"].startswith("scenario/")
        if not legacy_port and p["id"] not in selected_ids:
            continue  # porto de cidade que a cota cortou
        if p["id"] in seen_ports:
            problems.append(f"porto {p['id']} repetido na fonte")
            continue
        seen_ports.add(p["id"])
        x, y = lonlat_to_xy(float(p["lon"]), float(p["lat"]))
        ports_out.append({
            "id": p["id"],
            "name": p["name"],
            "nation": p["nation"],
            "x": round(x, 1),
            "y": round(y, 1),
            "landmass": p["landmass"],
        })
        i18n_out.append({"key": f"port.name.{p['id']}", "pt_BR": p["name"],
                         "en": p["name"], "es": p["name"]})

    # duplicidade de ids
    for label, rows in (("cidades", cities_out), ("portos", ports_out)):
        ids = [r["id"] for r in rows]
        if len(ids) != len(set(ids)):
            problems.append(f"{label}: ids duplicados")
    i18n_out.sort(key=lambda r: r["key"])

    print(f"nações: {len(nations_out)}")
    print(f"cidades: {len(cities_out)} (legado: "
          f"{sum(1 for c in cities_src if c['legacy'] == 'yes')})")
    print(f"portos: {len(ports_out)}")
    print(f"construções: {len(buildings_out)}")
    print(f"chaves i18n: {len(i18n_out)}")
    for p in problems:
        print(f"AVISO: {p}")

    if args.check:
        return 1 if problems else 0

    os.makedirs(OUT, exist_ok=True)

    def write(name: str, rows: list[dict], fields: list[str]) -> None:
        with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=fields)
            writer.writeheader()
            for row in rows:
                writer.writerow({k: row.get(k, "") for k in fields})

    write("nations.csv", nations_out,
          ["id", "name", "flag", "color", "capital", "currency", "government",
           "ai_profile", "region", "population", "gdp_ppp_bn"])
    write("cities.csv", cities_out,
          ["id", "nation", "home_nation", "name", "population", "x", "y", "landmass", "capital"])
    write("ports.csv", ports_out, ["id", "name", "nation", "x", "y", "landmass"])
    write("buildings.csv", buildings_out,
          ["city_id", "building_id", "name", "output_tons_per_day"])
    write("i18n.csv", i18n_out, ["key", "pt_BR", "en", "es"])

    meta = {
        "phase": "F1",
        "generated_by": "tools/build_world_cities.py",
        "projection": "x=(lon+180)/360*5000, y=(90-lat)/180*2500",
        "sources": {
            "population_nation": "World Bank WDI SP.POP.TOTL (2025)",
            "gdp_ppp": "World Bank WDI NY.GDP.MKTP.PP.CD (2025, US$ PPP correntes)",
            "cities": "citypopulation.de Major Agglomerations (2026-01-01); "
                      "GeoNames cities15000 (2026) para aglomerações < 1 M",
            "ports": "scenario/ports.csv (18 legados) + snap na costa do world_sdf.png",
        },
        "rules": {
            "quota": "min(12, 1 + clamp(1 + floor(pop/25M) + floor(gdp/1.5e12), 1, 12))",
            "min_spacing_km": MIN_SPACING_KM,
            "pop_per_ton": POP_PER_TON,
            "legacy": "40 cidades e 18 portos do cenário atual preservados",
        },
        "counts": {
            "nations": len(nations_out),
            "cities": len(cities_out),
            "ports": len(ports_out),
            "buildings": len(buildings_out),
            "i18n_keys": len(i18n_out),
        },
    }
    with open(os.path.join(OUT, "_meta.json"), "w", encoding="utf-8") as fh:
        json.dump(meta, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    print(f"OK -> {os.path.relpath(OUT, ROOT)}")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
