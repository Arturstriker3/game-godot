"""Monta a ancora NACIONAL de demanda e a geracao real por fonte (Ember).

Entrada: cache da Ember (`fetch_ember_capacity.py`), o agregado nacional de
capacidade ja versionado (`national_capacity.json`, mesma Ember) e o acesso do
WDI (`access_losses.json`, que define a lista de nacoes jogaveis com energia).
Saida: `data/definitions/energy/national_demand.json`.

Regra de dado (E2/E3, D-116, ancoragem por nacao):
  * `demand_twh` = serie "Demand" da Ember no ANO ESCOLHIDO por nacao. A Ember
    define demanda como geracao + importacao - exportacao (lado da geracao,
    ja inclui as perdas T&D) - nao e consumo medido no medidor;
  * `generation_twh` = serie "Generation (TWh)" por fonte no MESMO ano, com o
    mapa ja usado pelo E1 (Hydro/Solar/Wind/Coal/Gas/Nuclear/Bioenergy/
    Other fossil + Other renewables -> other; o oleo nao existe separado);
  * `net_imports_twh` = serie "Net imports" (positivo = importador liquido);
  * ano: o ano da capacidade nacional (`national_capacity.json`) quando ele tem
    demanda e geracao; senao o ultimo ano com as duas series. Nenhuma
    interpolacao nem extrapolacao; `year_reason` declara a escolha e
    `demand_year`/`generation_year` ficam explicitos (podem diferir da
    capacidade so quando o pais nao tem capacidade nacional publicada);
  * `low_coverage` repete a regra declarada em `power_params.json`
    (`meta.demand_calibration.excluded.rule`): Ember < 1,37 kWh/pessoa/dia.
    A ancora NAO exclui essas nacoes (o dado existe); a marca e so declaracao.

Uso:
    python tools/data/energy/build_national_demand.py
"""

from __future__ import annotations

import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import build_energy_common as ec  # noqa: E402

CACHE = os.path.join(ROOT, "tools", "data", "energy", "cache")
TEMP_CSV = os.path.join(os.environ.get("TEMP", ""), "ember_gen.csv")
EMBER_CSV = os.path.join(CACHE, "ember_yearly_global.csv")
EMBER_MANIFEST = os.path.join(CACHE, "ember_yearly_global.manifest.json")
CAPACITY_PATH = os.path.join(ec.DEFS, "energy", "national_capacity.json")
ACCESS_PATH = os.path.join(ec.DEFS, "energy", "access_losses.json")
NATIONS_PATH = os.path.join(ec.DEFS, "scenario", "nations.json")
OUT_PATH = os.path.join(ec.DEFS, "energy", "national_demand.json")

GAME_SOURCES = ("hydro", "solar", "wind", "coal", "gas", "oil", "nuclear",
                "biomass", "other")
# Correcao D-115: a Ember nao publica "Oil" separado, mas separa
# "Other fossil" de "Other renewables". O nucleo TEM grupo oleo proprio, entao
# `Other fossil` ancora o oleo e `Other renewables` (geotermia/mare) ancora
# `other`. Sem isso a geracao fossil ficava no grupo `other` e o parque de oleo
# do GPPD gerava POR CIMA do alvo, contando duas vezes (IRN, GBR, EGY...).
# A quebra crua fica preservada em `other_generation_twh` de cada nacao.
EMBER_TO_GAME = {
    "Hydro": "hydro",
    "Solar": "solar",
    "Wind": "wind",
    "Coal": "coal",
    "Gas": "gas",
    "Nuclear": "nuclear",
    "Bioenergy": "biomass",
    "Other fossil": "oil",
    "Other renewables": "other",
}
LOW_COVERAGE_KWH_DAY = 1.37
DAYS_PER_YEAR = 365.0


def _csv_path() -> str:
    """Cache versionado; o CSV solto no TEMP e so conveniencia local."""
    if os.path.exists(EMBER_CSV):
        return EMBER_CSV
    return TEMP_CSV


def load_ember() -> dict:
    """{iso3: {year: {source: TWh}}} para demanda, geracao por fonte e importacao."""
    series: dict[str, dict[int, dict[str, float]]] = {}
    path = _csv_path()
    with open(path, encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("Area type") != "Country or economy":
                continue
            iso3 = (row.get("ISO 3 code") or "").strip().upper()
            if not iso3:
                continue
            source = row.get("Electricity source") or ""
            if source not in ("Demand", "Net imports", "Total generation") \
                    and source not in EMBER_TO_GAME:
                continue
            raw = (row.get("Generation (TWh)") or "").strip()
            if raw == "":
                continue
            try:
                year = int(row["Year"])
                value = float(raw)
            except (TypeError, ValueError):
                continue
            series.setdefault(iso3, {}).setdefault(year, {})[source] = value
    return series


def load_manifest() -> dict:
    if os.path.exists(EMBER_MANIFEST):
        with open(EMBER_MANIFEST, encoding="utf-8") as handle:
            return json.load(handle)
    return {}


def load_population() -> dict:
    rows = ec.read_json(NATIONS_PATH)
    return {str(row.get("id")): float(row.get("population") or 0.0)
            for row in rows}


def playable_nations() -> dict:
    """tag do jogo -> ISO3 do WDI (keyed by the access file)."""
    payload = ec.read_json(ACCESS_PATH)
    out = {}
    for iso3, row in payload.get("by_country", {}).items():
        tag = (row.get("nation") or "").strip()
        if not tag or not bool(row.get("playable", True)):
            continue
        access = row.get("electricity_access_pct") or {}
        if not isinstance(access, dict) or access.get("value") is None:
            continue
        out[tag] = iso3
    return out


def choose_year(years: dict, capacity_year: int | None) -> tuple[int | None, str]:
    usable = [year for year, row in years.items()
              if "Demand" in row and "Total generation" in row]
    if not usable:
        return None, "no_demand_and_total_generation"
    if capacity_year is not None and capacity_year in usable:
        return capacity_year, "capacity_year"
    latest = max(usable)
    if capacity_year is not None:
        return latest, "latest_with_demand_and_total"
    return latest, "no_national_capacity_latest_with_demand_and_total"


def build_nation(tag: str, iso3: str, years: dict, capacity: dict | None,
                 population: float) -> dict:
    capacity_year = int(capacity["year"]) if capacity else None
    year, year_reason = choose_year(years, capacity_year)
    row = years.get(year, {}) if year is not None else {}
    demand = row.get("Demand")
    total = row.get("Total generation")
    generation = {source: None for source in GAME_SOURCES}
    for ember_source, value in row.items():
        group = EMBER_TO_GAME.get(ember_source)
        if group is None:
            continue
        current = generation[group] or 0.0
        generation[group] = round(current + value, 4)
    sum_sources = round(sum(value for value in generation.values()
                            if value is not None), 4)
    total_value = round(total, 4) if total is not None else None
    other_fossil = row.get("Other fossil")
    other_renewables = row.get("Other renewables")
    net_imports = row.get("Net imports")
    per_capita = None
    if demand is not None and population > 0.0:
        per_capita = demand * 1e9 / DAYS_PER_YEAR / population
    return {
        "nation": tag,
        "country_iso3": iso3,
        "year": year,
        "demand_year": year if demand is not None else None,
        "generation_year": year if total is not None else None,
        "year_reason": year_reason,
        "capacity_year": capacity_year,
        "demand_twh": round(demand, 4) if demand is not None else None,
        "total_generation_twh": total_value,
        "total_generation_source": ("ember_total" if total is not None
                                    else ("sum_sources" if year is not None
                                          else None)),
        "generation_twh": generation,
        "other_generation_twh": {
            "fossil": (round(other_fossil, 4)
                       if other_fossil is not None else None),
            "renewables": (round(other_renewables, 4)
                           if other_renewables is not None else None),
        },
        "generation_sum_twh": sum_sources if year is not None else None,
        "generation_check_delta_twh": (
            round(sum_sources - total_value, 4)
            if total_value is not None else None),
        "net_imports_twh": (round(net_imports, 4)
                            if net_imports is not None else None),
        "net_imports_share": (
            round(net_imports / demand, 4)
            if net_imports is not None and demand not in (None, 0.0) else None),
        "population": round(population, 0) if population > 0.0 else None,
        "demand_per_capita_kwh_day": (round(per_capita, 4)
                                      if per_capita is not None else None),
        "low_coverage": (per_capita is not None
                         and per_capita < LOW_COVERAGE_KWH_DAY),
        "has_national_capacity": capacity is not None,
        "method": "ember_demand_and_generation_same_year",
    }


def main() -> int:
    if not os.path.exists(_csv_path()):
        raise SystemExit("cache da Ember ausente: rode "
                         "tools/data/energy/fetch_ember_capacity.py")
    series = load_ember()
    manifest = load_manifest()
    capacity_payload = ec.read_json(CAPACITY_PATH)
    capacity_nations = capacity_payload.get("nations", {})
    populations = load_population()
    nations = playable_nations()

    by_nation = {}
    missing = []
    for tag, iso3 in sorted(nations.items()):
        years = series.get(iso3.upper(), {})
        if not years:
            missing.append(tag)
            continue
        capacity = capacity_nations.get(tag)
        entry = build_nation(tag, iso3, years, capacity,
                             populations.get(tag, 0.0))
        if entry["year"] is None:
            missing.append(tag)
            continue
        by_nation[tag] = entry

    reason_counts: dict[str, int] = {}
    for entry in by_nation.values():
        reason = entry["year_reason"]
        reason_counts[reason] = reason_counts.get(reason, 0) + 1
    year_counts: dict[str, int] = {}
    for entry in by_nation.values():
        key = str(entry["year"])
        year_counts[key] = year_counts.get(key, 0) + 1

    with_demand = [e for e in by_nation.values() if e["demand_twh"] is not None]
    with_generation = [e for e in by_nation.values()
                       if e["total_generation_twh"] is not None]
    importers = [e for e in by_nation.values()
                 if e["net_imports_twh"] is not None
                 and e["net_imports_twh"] > 0.0]
    exporters = [e for e in by_nation.values()
                 if e["net_imports_twh"] is not None
                 and e["net_imports_twh"] < 0.0]
    low_coverage = sorted(e["nation"] for e in by_nation.values()
                          if e["low_coverage"])
    no_capacity = sorted(e["nation"] for e in by_nation.values()
                         if not e["has_national_capacity"])
    mismatch = sorted(e["nation"] for e in by_nation.values()
                      if e["generation_check_delta_twh"] is not None
                      and abs(e["generation_check_delta_twh"]) > 0.5)
    world_demand = sum(e["demand_twh"] for e in with_demand)
    world_generation = sum(e["total_generation_twh"] for e in with_generation)
    world_imports = sum(e["net_imports_twh"] for e in importers)
    world_exports = sum(-e["net_imports_twh"] for e in exporters)
    zero_park_sources = sorted(
        f"{e['nation']}:{source}" for e in by_nation.values()
        for source in GAME_SOURCES
        if e["generation_twh"].get(source) == 0.0)

    payload = {
        "meta": {
            "generated_by": "tools/data/energy/build_national_demand.py",
            "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E2/E3",
            "decision": "D-116 - ancoragem por nacao (oferta e demanda no ano-base)",
            "sources": {
                "document": {
                    "dataset": "Ember, Yearly Electricity Data (global release)",
                    "url": manifest.get("url"),
                    "landing_url": manifest.get("landing_url"),
                    "sha256": manifest.get("sha256"),
                    "accessed_at_utc": manifest.get("accessed_at_utc"),
                    "license": manifest.get("license"),
                    "citation": manifest.get("citation"),
                },
                "capacity": {
                    "path": "data/definitions/energy/national_capacity.json",
                    "note": "mesmo arquivo/ano usado pelo parque; a ancora de "
                            "geracao divide pela capacidade do MESMO dado",
                },
                "nation_list": {
                    "path": "data/definitions/energy/access_losses.json",
                    "note": "universo = nacoes jogaveis com acesso do WDI "
                            "(PSE->PSX pelo alias do proprio arquivo)",
                },
            },
            "method": (
                "por nacao: ano = ano da capacidade nacional quando ele tem "
                "'Demand' e 'Total generation'; senao o ultimo ano com as duas "
                "series (sem interpolacao/extrapolacao). demand_twh = serie "
                "'Demand'; generation_twh = 'Generation (TWh)' por fonte no "
                "mesmo ano (correcao D-115: 'Other fossil' ancora oil, "
                "'Other renewables' ancora other; a quebra crua fica em "
                "other_generation_twh); net_imports_twh = 'Net imports' "
                "(positivo = importador). O nucleo usa demanda_base = "
                "demand_twh x 1e6 / 365 MWh/dia x fracao simulada, e calibra o "
                "fator de capacidade por nacao x fonte para gerar "
                "generation_twh x fracao simulada no ano-base."
            ),
            "semantics": {
                "demand": ("Ember 'Demand' = geracao + importacao - exportacao "
                           "(lado da geracao, ja inclui perdas T&D). Nao e "
                           "consumo medido no medidor."),
                "imports": ("net_imports > 0 = importador liquido. Sem E5 nao "
                            "ha comercio entre redes: o deficit de um "
                            "importador = sua importacao liquida."),
            },
            "low_coverage_rule": {
                "kwh_per_person_day": LOW_COVERAGE_KWH_DAY,
                "note": ("mesma regra declarada em power_params.json "
                         "(meta.demand_calibration.excluded.rule); aqui a "
                         "nacao NAO e excluida da ancora - so marcada"),
                "nations": low_coverage,
            },
            "counts": {
                "game_nations": len(nations),
                "nations_with_data": len(by_nation),
                "nations_with_demand": len(with_demand),
                "nations_with_generation": len(with_generation),
                "nations_without_demand_data": missing,
                "nations_without_national_capacity": no_capacity,
                "nations_net_importers": len(importers),
                "nations_net_exporters": len(exporters),
                "nations_generation_sum_mismatch": len(mismatch),
                "nations_low_coverage": len(low_coverage),
                "nations_year": dict(sorted(year_counts.items())),
                "nations_year_reason": dict(sorted(reason_counts.items())),
                "zero_generation_sources": len(zero_park_sources),
            },
            "checks": {
                "world_demand_twh": round(world_demand, 3),
                "world_generation_twh": round(world_generation, 3),
                "world_demand_generation_delta_twh": round(
                    world_demand - world_generation, 3),
                "world_net_imports_twh": round(world_imports, 3),
                "world_net_exports_twh": round(world_exports, 3),
                "generation_sum_mismatch_nations": mismatch,
                "zero_generation_sources_sample":
                    zero_park_sources[:20],
            },
            "gaps": {
                "oil": ("a Ember nao publica 'Oil' separado; 'Other fossil' "
                        "(oleo e outros fosseis) ancora o grupo oil e "
                        "'Other renewables' ancora other (correcao D-115). O "
                        "oleo nao e dado medido: e o bucket fossil da Ember, "
                        "declarado em other_generation_twh.fossil."),
                "zero_generation": ("geracao publicada = 0,0 com parque > 0 "
                                    "zera a disponibilidade daquela fonte "
                                    "(dado, nao ausencia)."),
                "year_mix": ("pais sem capacidade nacional (ex. LKA, PSE) usa "
                             "demanda/geracao do ultimo ano com as duas series; "
                             "nao ha usina semeada para calibrar forca."),
            },
        },
        "by_nation": by_nation,
    }
    ec.write_json(OUT_PATH, payload)
    print("nacoes com demanda+geracao: %d/%d (faltam %d)"
          % (len(by_nation), len(nations), len(missing)))
    print("ano: %s | motivo: %s"
          % (", ".join("%s=%d" % item for item in sorted(year_counts.items())),
             ", ".join("%s=%d" % item for item in sorted(reason_counts.items()))))
    print("mundo: demanda %.1f TWh/ano, geracao %.1f TWh/ano, delta %.3f"
          % (world_demand, world_generation, world_demand - world_generation))
    print("importadores liquidos: %d | exportadores: %d | cobertura fraca: %d"
          % (len(importers), len(exporters), len(low_coverage)))
    if missing:
        print("sem dado: %s" % " ".join(missing))
    if no_capacity:
        print("sem capacidade nacional: %s" % " ".join(no_capacity))
    return 0


if __name__ == "__main__":
    sys.exit(main())
