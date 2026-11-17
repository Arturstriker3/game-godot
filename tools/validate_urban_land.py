#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida data/definitions/economy/urban_land.json contra data/definitions/scenario/.

Checa:
  1. o JSON cobre exatamente as cidades do cities.csv (nem uma a mais, nem a menos);
  2. toda cidade tem built_up_km2>0 e density_hab_km2>0; nenhuma densidade passa de
     60.000 hab/km2 (ERRO, não aviso — o máximo do Atlas é 55.200 e o proxy 15.000);
  3. a densidade OBSERVADA é a densidade do próprio Atlas: confere, com tolerância de
     1 hab/km2, contra a coluna 'Built-up Area Density (persons/ha)' da Tabela 1
     (cache tools/data/urban_land_cache/atlas_aue2016_table1_density.tsv, x100);
  4. a densidade DERIVADA é o DENSITY_PROXY da faixa de renda e built_up_km2 bate com
     população usada / densidade;
  5. zoning_split tem as três chaves, cada valor em [0,1] e soma 1.0 (tolerância 0.01);
  6. height_class em {low, mid, high}; coastal é booleano; reserve_method conhecido e
     expandable_reserve_km2 dentro de (0, 3 x built_up_km2];
  7. cidade OBSERVADA (built_up_source != derived_density_proxy): estimated=false,
     built_up_year inteiro, confidence complete (ano >= 2015) ou partial (ano < 2015),
     refs não-vazio com title/url http/year;
  8. cidade DERIVADA (derived_density_proxy): estimated=true, built_up_year null,
     confidence "estimated";
  9. population e name batem com cities.csv;
 10. o bloco coverage bate com a contagem recalculada.

Uso:
    python tools/validate_urban_land.py

Retorna 0 quando está tudo consistente, 1 (com a lista de problemas) caso contrário.
"""

from __future__ import annotations

import csv
import json
import os
import sys
from typing import Dict, List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")
CITIES_CSV = os.path.join(SCENARIO, "cities.csv")
PORTS_CSV = os.path.join(SCENARIO, "ports.csv")
URBAN_LAND = os.path.join(ROOT, "data", "definitions", "economy", "urban_land.json")
ATLAS_DENSITY_CACHE = os.path.join(TOOLS, "data", "urban_land_cache",
                                   "atlas_aue2016_table1_density.tsv")

if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)

CONFIDENCE_YEAR = 2015
DERIVED_SOURCE = "derived_density_proxy"
OBSERVED_SOURCES = {"atlas_urban_expansion", "ghsl_ucdb", "oecd_fua"}
KNOWN_SOURCES = OBSERVED_SOURCES | {DERIVED_SOURCE}
HEIGHT_CLASSES = {"low", "mid", "high"}
ZONING_KEYS = {"residential", "commercial", "industrial"}
RESERVE_METHODS = {"derived:coast+landmass"}
DENSITY_SOURCES = {"atlas_density", DERIVED_SOURCE}
DENSITY_PROXY_VALUES = {15000.0, 11000.0, 8500.0, 6000.0}
DENSITY_TOLERANCE = 1.0
BUILT_UP_TOLERANCE = 0.1  # built_up_km2 é arredondado a 0,1 km2
ZONING_TOLERANCE = 0.01
RESERVE_CAP_MULTIPLE = 3.0
# Teto de plausibilidade: ERRO. Atlas (máx. 55.200 hab/km2) e proxy (máx. 15.000).
MAX_DENSITY = 60000.0

SAMPLES = ("IND-MUM", "AFG-KAB", "JPN-TOK", "USA-NEW", "NGA-LAG", "ALB-TIR", "CAN-LON")


def load_atlas_density() -> Dict[Tuple[str, str], float]:
    """(cidade, país) do Atlas -> densidade T3 em persons/ha (cache da Tabela 1)."""
    records: Dict[Tuple[str, str], float] = {}
    with open(ATLAS_DENSITY_CACHE, "r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            density = (row.get("density_per_ha_t3") or "").strip().replace(",", "")
            name = (row.get("city") or "").strip()
            country = (row.get("country") or "").strip()
            if not name or not country or not density:
                continue
            try:
                records[(name, country)] = float(density)
            except ValueError:
                continue
    return records


def load_observed_names() -> Dict[str, Tuple[str, str]]:
    """id da cidade -> (cidade, país) da linha do Atlas (fonte: build_urban_land)."""
    try:
        from build_urban_land import OBSERVED_AREAS
    except ImportError:
        return {}
    return {code: (row[0], row[1]) for code, row in OBSERVED_AREAS.items()}


def load_cities() -> Dict[str, dict]:
    with open(CITIES_CSV, "r", encoding="utf-8", newline="") as handle:
        return {row["id"]: row for row in csv.DictReader(handle)}


def load_payload() -> dict:
    with open(URBAN_LAND, "r", encoding="utf-8") as handle:
        return json.load(handle)


def check_top_level(payload: dict, problems: List[str]) -> None:
    if payload.get("version") != 1:
        problems.append("version deveria ser 1, veio %r" % payload.get("version"))
    if not isinstance(payload.get("note"), str) or not payload["note"].strip():
        problems.append("note ausente ou vazia")
    units = payload.get("units")
    if not isinstance(units, dict) or units.get("area") != "km2" \
            or units.get("density") != "hab/km2":
        problems.append("units deveria ser {area: km2, density: hab/km2}, veio %r" % (units,))
    for block in ("sources", "method"):
        value = payload.get(block)
        if not isinstance(value, dict) or not value:
            problems.append("bloco %s ausente ou vazio" % block)
    method = payload.get("method") or {}
    if isinstance(method, dict):
        for key in ("built_up_area", "density", "expandable_reserve",
                    "zoning_split", "height_class", "coastal"):
            if not str(method.get(key, "")).strip():
                problems.append("method.%s ausente" % key)
    sources = payload.get("sources") or {}
    if isinstance(sources, dict):
        for name, entry in sources.items():
            if not isinstance(entry, dict):
                problems.append("sources.%s não é objeto" % name)
                continue
            if not entry.get("title"):
                problems.append("sources.%s sem title" % name)
            url = entry.get("url")
            if not isinstance(url, str) or not url.startswith("http"):
                problems.append("sources.%s sem url http(s)" % name)


def check_refs(entry: dict, code: str, problems: List[str], current_year: int) -> None:
    refs = entry.get("refs")
    if not isinstance(refs, list) or not refs:
        problems.append("%s: refs vazio ou ausente" % code)
        return
    for position, ref in enumerate(refs):
        if not isinstance(ref, dict):
            problems.append("%s: refs[%d] não é objeto" % (code, position))
            continue
        if not ref.get("title"):
            problems.append("%s: refs[%d].title ausente" % (code, position))
        url = ref.get("url")
        if not isinstance(url, str) or not url.startswith("http"):
            problems.append("%s: refs[%d].url não é http(s): %r" % (code, position, url))
        year = ref.get("year")
        if not isinstance(year, int):
            problems.append("%s: refs[%d].year não é inteiro: %r" % (code, position, year))
        elif year > current_year:
            problems.append("%s: refs[%d].year no futuro: %d" % (code, position, year))


def check_zoning(entry: dict, code: str, problems: List[str]) -> None:
    zoning = entry.get("zoning_split")
    if not isinstance(zoning, dict):
        problems.append("%s: zoning_split ausente ou não-objeto" % code)
        return
    if set(zoning) != ZONING_KEYS:
        problems.append("%s: zoning_split com chaves %s (esperado %s)"
                        % (code, sorted(zoning), sorted(ZONING_KEYS)))
    total = 0.0
    for key in sorted(set(zoning)):
        value = zoning[key]
        if not isinstance(value, (int, float)):
            problems.append("%s: zoning_split.%s não numérico: %r" % (code, key, value))
            continue
        if not (0.0 <= float(value) <= 1.0):
            problems.append("%s: zoning_split.%s fora de [0,1]: %r" % (code, key, value))
        if key in ZONING_KEYS:
            total += float(value)
    if abs(total - 1.0) > ZONING_TOLERANCE:
        problems.append("%s: zoning_split soma %.3f (esperado 1.0)" % (code, total))


def check_entry(entry: dict, city: dict, problems: List[str], current_year: int,
                atlas_density: Dict[Tuple[str, str], float],
                observed_names: Dict[str, Tuple[str, str]]) -> str:
    """Valida uma cidade e devolve o tipo: observed | derived | invalid."""
    code = city["id"]
    if not isinstance(entry, dict):
        problems.append("%s: entrada não é objeto" % code)
        return "invalid"

    if entry.get("name") != city["name"]:
        problems.append("%s: name %r != cities.csv %r"
                        % (code, entry.get("name"), city["name"]))
    if entry.get("nation") != city["nation"]:
        problems.append("%s: nation %r != cities.csv %r"
                        % (code, entry.get("nation"), city["nation"]))
    population = entry.get("population")
    if not isinstance(population, int) or population != int(city["population"]):
        problems.append("%s: population %r != cities.csv %r"
                        % (code, population, city["population"]))

    agglomeration = entry.get("agglomeration_population")
    if agglomeration is not None:
        if isinstance(agglomeration, bool) or not isinstance(agglomeration, int) \
                or agglomeration <= 0:
            problems.append("%s: agglomeration_population inválida: %r" % (code, agglomeration))
    density_population = (agglomeration if isinstance(agglomeration, int) and agglomeration > 0
                          else population)
    density_source = entry.get("density_source")
    if density_source not in DENSITY_SOURCES:
        problems.append("%s: density_source inválida: %r" % (code, density_source))

    built_up = entry.get("built_up_km2")
    if not isinstance(built_up, (int, float)) or float(built_up) <= 0.0:
        problems.append("%s: built_up_km2 inválido: %r" % (code, built_up))
        return "invalid"
    built_up = float(built_up)

    density = entry.get("density_hab_km2")
    if not isinstance(density, (int, float)) or float(density) <= 0.0:
        problems.append("%s: density_hab_km2 inválido: %r" % (code, density))
        density = None
    else:
        density = float(density)
        if density > MAX_DENSITY:
            problems.append("%s: density_hab_km2 %.0f acima do teto de %g hab/km2"
                            % (code, density, MAX_DENSITY))

    reserve = entry.get("expandable_reserve_km2")
    if not isinstance(reserve, (int, float)) or float(reserve) <= 0.0:
        problems.append("%s: expandable_reserve_km2 inválido: %r" % (code, reserve))
    elif float(reserve) > built_up * RESERVE_CAP_MULTIPLE + 0.1:
        problems.append("%s: expandable_reserve_km2 %.2f > 3x built_up (%.2f)"
                        % (code, float(reserve), built_up * RESERVE_CAP_MULTIPLE))
    if entry.get("reserve_method") not in RESERVE_METHODS:
        problems.append("%s: reserve_method desconhecido: %r"
                        % (code, entry.get("reserve_method")))
    if not isinstance(entry.get("coastal"), bool):
        problems.append("%s: coastal não booleano: %r" % (code, entry.get("coastal")))
    if entry.get("height_class") not in HEIGHT_CLASSES:
        problems.append("%s: height_class inválido: %r" % (code, entry.get("height_class")))
    if not entry.get("height_class_source"):
        problems.append("%s: height_class_source ausente" % code)
    if not entry.get("zoning_source"):
        problems.append("%s: zoning_source ausente" % code)
    check_zoning(entry, code, problems)

    source = entry.get("built_up_source")
    estimated = entry.get("estimated")
    confidence = entry.get("confidence")
    year = entry.get("built_up_year")

    if source not in KNOWN_SOURCES:
        problems.append("%s: built_up_source desconhecida: %r" % (code, source))
        return "invalid"
    if not isinstance(estimated, bool):
        problems.append("%s: estimated ausente/não booleano" % code)

    if source == DERIVED_SOURCE:
        if estimated is not True:
            problems.append("%s: derivada com estimated=%r (esperado true)" % (code, estimated))
        if year is not None:
            problems.append("%s: derivada com built_up_year=%r (esperado null)" % (code, year))
        if confidence != "estimated":
            problems.append("%s: derivada com confidence=%r (esperado 'estimated')"
                            % (code, confidence))
        if density_source != DERIVED_SOURCE:
            problems.append("%s: derivada com density_source=%r (esperado %r)"
                            % (code, density_source, DERIVED_SOURCE))
        if density is not None:
            # built_up = população usada / proxy, e a densidade É o proxy da faixa.
            if density not in DENSITY_PROXY_VALUES:
                problems.append(
                    "%s: derivada com density_hab_km2 %.0f fora do DENSITY_PROXY %s"
                    % (code, density, sorted(DENSITY_PROXY_VALUES, reverse=True)))
            elif isinstance(density_population, int) and density_population > 0:
                expected_built_up = density_population / density
                if abs(built_up - expected_built_up) > BUILT_UP_TOLERANCE:
                    problems.append(
                        "%s: built_up_km2 %.1f != população usada/densidade %.1f"
                        % (code, built_up, expected_built_up))
        return "derived"

    # observada
    if estimated is not False:
        problems.append("%s: observada com estimated=%r (esperado false)" % (code, estimated))
    if not isinstance(year, int):
        problems.append("%s: observada sem built_up_year inteiro: %r" % (code, year))
    else:
        if year > current_year:
            problems.append("%s: built_up_year no futuro: %d" % (code, year))
        expected = "complete" if year >= CONFIDENCE_YEAR else "partial"
        if confidence != expected:
            problems.append("%s: confidence %r != %r para ano %d"
                            % (code, confidence, expected, year))
    check_refs(entry, code, problems, current_year)

    if source == "atlas_urban_expansion":
        # Densidade OBSERVADA = densidade do próprio Atlas (persons/ha * 100), não
        # população de aglomeração / área do Atlas (perímetros diferentes).
        if density_source != "atlas_density":
            problems.append("%s: observada com density_source=%r (esperado 'atlas_density')"
                            % (code, density_source))
        if atlas_density:
            # Conferência de fidelidade à fonte (só quando o cache do Atlas está presente;
            # sem ele, a checagem de faixa <= MAX_DENSITY já garante a plausibilidade).
            key = observed_names.get(code)
            if key is None:
                problems.append("%s: cidade observada sem linha em OBSERVED_AREAS" % code)
            else:
                per_ha = atlas_density.get(key)
                if per_ha is None:
                    problems.append("%s: Atlas sem 'Built-up Area Density' T3 para %s (%s)"
                                    % (code, key[0], key[1]))
                elif density is not None:
                    expected_density = round(per_ha * 100.0, 0)
                    if abs(density - expected_density) > DENSITY_TOLERANCE:
                        problems.append(
                            "%s: density_hab_km2 %.0f != Atlas %g persons/ha = %.0f hab/km2"
                            % (code, density, per_ha, expected_density))
    return "observed"


def main() -> int:
    import datetime

    cities = load_cities()
    payload = load_payload()
    problems: List[str] = []
    current_year = datetime.date.today().year

    check_top_level(payload, problems)

    entries = payload.get("cities")
    if not isinstance(entries, dict):
        print("ERRO: bloco cities ausente")
        return 1

    extra = sorted(set(entries) - set(cities))
    missing = sorted(set(cities) - set(entries))
    if extra:
        problems.append("cidades no JSON que não estão no cities.csv: %s" % ", ".join(extra))
    if missing:
        problems.append("cidades do cities.csv ausentes no JSON: %s" % ", ".join(missing))

    counts = {"observed": 0, "derived": 0, "invalid": 0}
    coastal = 0
    with_agglomeration = 0
    sources_used: Dict[str, int] = {}
    density_sources: Dict[str, int] = {}
    max_density = 0.0
    if not os.path.exists(ATLAS_DENSITY_CACHE):
        print("NOTA: cache do Atlas ausente (%s); a conferência de densidade vs. a fonte "
              "fica desativada (rode tools/build_urban_land.py para baixá-lo)."
              % os.path.relpath(ATLAS_DENSITY_CACHE, ROOT))
        atlas_density: Dict[Tuple[str, str], float] = {}
    else:
        atlas_density = load_atlas_density()
        if not atlas_density:
            print("NOTA: cache do Atlas sem densidade legível (%s); conferência de "
                  "densidade desativada." % os.path.relpath(ATLAS_DENSITY_CACHE, ROOT))
    observed_names = load_observed_names()
    for code in sorted(cities):
        entry = entries.get(code)
        if entry is None:
            counts["invalid"] += 1
            continue
        kind = check_entry(entry, cities[code], problems, current_year,
                           atlas_density, observed_names)
        counts[kind] += 1
        if isinstance(entry.get("coastal"), bool) and entry["coastal"]:
            coastal += 1
        if isinstance(entry.get("agglomeration_population"), int):
            with_agglomeration += 1
        source = entry.get("built_up_source")
        sources_used[source] = sources_used.get(source, 0) + 1
        density_source = entry.get("density_source")
        density_sources[density_source] = density_sources.get(density_source, 0) + 1
        density = entry.get("density_hab_km2")
        if isinstance(density, (int, float)):
            max_density = max(max_density, float(density))

    coverage = payload.get("coverage") or {}
    recomputed = {
        "cities_total": len(cities),
        "cities_with_observed_area": counts["observed"],
        "cities_with_derived_area": counts["derived"],
        "cities_with_agglomeration_population": with_agglomeration,
    }
    for field, value in recomputed.items():
        declared = coverage.get(field)
        if declared != value:
            problems.append("coverage.%s = %r, recalculado = %r" % (field, declared, value))

    print("cidades no cities.csv: %d | entradas no JSON: %d" % (len(cities), len(entries)))
    print("observadas: %d | derivadas: %d | costeiras: %d"
          % (counts["observed"], counts["derived"], coastal))
    print("população real da aglomeração: %d | rateada de cenário: %d"
          % (with_agglomeration, len(cities) - with_agglomeration))
    print("density_source: %s" % ", ".join(
        "%s=%d" % (name, density_sources[name]) for name in sorted(density_sources, key=str)))
    print("densidade máxima: %.0f hab/km2 (teto %g = erro)" % (max_density, MAX_DENSITY))
    print("fontes de área: %s" % ", ".join(
        "%s=%d" % (name, sources_used[name]) for name in sorted(sources_used, key=str)))
    print("amostra:")
    for code in SAMPLES:
        entry = entries.get(code) or {}
        print("  %-8s %-18s pop=%-11s agg=%-10s built_up=%-9s dens=%-9s %s"
              % (code, entry.get("name"), entry.get("population"),
                 entry.get("agglomeration_population"), entry.get("built_up_km2"),
                 entry.get("density_hab_km2"), entry.get("density_source")))

    if problems:
        print()
        print("PROBLEMAS (%d):" % len(problems))
        for problem in problems:
            print(" -", problem)
        return 1

    print()
    print("OK: %d cidades, %d com área observada, %d com área derivada"
          % (len(cities), counts["observed"], counts["derived"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
