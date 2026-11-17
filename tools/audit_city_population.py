#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Auditoria reprodutível da população das 834 cidades do cenário (B4 / Q-015).

POR QUE ESTE SCRIPT EXISTE
    O cenário carrega TRÊS populações por cidade e elas não são a mesma coisa. Sem uma
    auditoria reproduzível, qualquer leitura de "população da cidade" no jogo mistura
    as três silenciosamente e o erro só aparece no balanceamento. Este script lê os
    arquivos definitivos, recomputa os invariantes do contrato e lista onde a unidade de
    simulação se afasta da realidade.

AS POPULAÇÕES E AS FONTES
    * population (cities.json) — população REGIONAL: a população nacional real
      (nations.csv, mesma soma) rateada entre as cidades da nação **proporcionalmente à
      população urbana** (o fator population/urban_population é uniforme dentro da
      nação). A soma das cidades de cada nação é exatamente a população da nação. NÃO é
      a população da cidade: é o mercado/hinterland do nó.
    * sim_population (cities.json) — população PRÓPRIA da cidade na fonte do cenário
      (world cities). É a unidade de simulação do núcleo econômico (a habitação usa
      urban_population, não esta).
    * agglomeration_population (cities.json) — população da AGLOMERAÇÃO real,
      citypopulation.de (Thomas Brinkhoff, referência 2026-01-01), tabela
      tools/agglomerations_2026.csv; null para as 454 cidades fora da tabela, que só
      lista aglomerações de 1 milhão de habitantes ou mais. É REFERÊNCIA: pode ser
      transfronteiriça (ex.: SGP inclui Johor Bahru, Malásia).
    Derivadas (não são fonte independente):
      urban_land.json     urban_population vem de cities.csv (regra única de
                          tools/build_world_cities.py: min de agglomeration_population
                          com a população da nação quando há aglomeração, senão
                          sim_population; campo urban_population_source ∈
                          {agglomeration, agglomeration_capped, sim_population});
                          built_up_km2,
                          zoning_split.residential e height_class alimentam a capacidade.
      housing_city.json   housing_stock_total = round(urban_population / household_size
                          da nação em housing.json).

CAPACIDADE RESIDENCIAL (constantes do núcleo, repetidas aqui só para auditar)
    capacidade = built_up_km2 * zoning_split.residential * 1e6 * FAR / unit_m2
    FAR      = {low: 1.0, mid: 2.5, high: 6.0}
    unit_m2  = {low: 100.0, mid: 85.0, high: 75.0}
    ocupação = housing_stock_total / capacidade

ESCOPO
    Somente leitura dos dados. Escreve apenas nos caminhos passados em --json / --csv.
    Não corrige nada: aponta. Sai com código != 0 se faltar arquivo/campo ou se uma
    checagem de integridade (bloco H) falhar.

USO
    python tools/audit_city_population.py
    python tools/audit_city_population.py --json data/definitions/economy/_audit.json
    python tools/audit_city_population.py --csv  data/definitions/economy/_audit.csv
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import statistics
import sys
import unicodedata
from typing import Dict, List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CITIES_JSON = os.path.join(ROOT, "data", "definitions", "scenario", "cities.json")
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "scenario", "nations.csv")
URBAN_LAND_JSON = os.path.join(ROOT, "data", "definitions", "economy", "urban_land.json")
HOUSING_CITY_JSON = os.path.join(ROOT, "data", "definitions", "economy", "housing_city.json")
HOUSING_JSON = os.path.join(ROOT, "data", "definitions", "economy", "housing.json")
AGGLOMERATIONS_CSV = os.path.join(ROOT, "tools", "agglomerations_2026.csv")

EXPECTED_CITIES = 834

# Constantes do núcleo (ver core/economy: capacidade residencial por gabarito).
FAR_BY_HEIGHT = {"low": 1.0, "mid": 2.5, "high": 6.0}
UNIT_M2_BY_HEIGHT = {"low": 100.0, "mid": 85.0, "high": 75.0}

SPREAD_LIMIT = 0.01        # spread relativo (max-min)/mediana tolerado no fator regional
STOCK_TOLERANCE = 1        # arredondamento de housing_stock_total
SUM_TOLERANCE = 0.5        # arredondamento de population na soma por nação
SIM_RATIO_LOW = 0.5        # faixa aceitável de sim_population / aglomeração
SIM_RATIO_HIGH = 2.0

# Casamento cidade -> aglomeração: mesma regra de tools/build_urban_land.py
# (nome normalizado + ISO3 da nação, aliases abaixo, empate pelo menor rank).
ISO3_ALIASES = {
    "NED": ("NED", "NLD"),
    "PSX": ("PSX", "PSE"),
    # ATENÇÃO: nosso SOL é Somalilândia (nunca casar com "SLB", Ilhas Salomão);
    # a tabela publica Hargeisa/Berbera sob SOM.
    "SOL": ("SOL", "SOM"),
    "SAH": ("SAH", "MAR"),   # Saara Ocidental: El Aiún aparece sob MAR
    "CYN": ("CYN", "CYP"),
    "TMP": ("TMP", "TLS"),
}

CITY_EXONYMS = {
    "Ho Chi Minh City": "Saigon",
    "New Delhi": "Delhi",
}

COUNTRY_STOPWORDS = {
    "republic", "peoples", "people", "democratic", "federal", "kingdom", "state",
    "states", "arab", "islamic", "union", "socialist", "the", "and", "of", "saint",
    "st", "islands", "island", "territories", "territory",
}


class AuditError(Exception):
    """Falha de entrada (arquivo/campo ausente): aborta com exit 1."""


# ---------------------------------------------------------------------------
# Leitura e validação
# ---------------------------------------------------------------------------

def display_path(path: str) -> str:
    """Caminho relativo ao projeto quando possível; absoluto em outro volume."""
    try:
        return os.path.relpath(path, ROOT)
    except ValueError:
        return path


def read_json(path: str) -> dict:
    if not os.path.isfile(path):
        raise AuditError("arquivo não encontrado: %s" % display_path(path))
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except json.JSONDecodeError as exc:
        raise AuditError("JSON inválido em %s: %s" % (display_path(path), exc))


def read_csv(path: str) -> List[dict]:
    if not os.path.isfile(path):
        raise AuditError("arquivo não encontrado: %s" % display_path(path))
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def require(container: dict, key: str, where: str):
    if not isinstance(container, dict) or key not in container:
        raise AuditError("campo ausente: %s em %s" % (key, where))
    return container[key]


def as_number(value, where: str) -> float:
    if isinstance(value, bool) or value is None:
        raise AuditError("valor numérico esperado em %s (recebido %r)" % (where, value))
    try:
        return float(value)
    except (TypeError, ValueError):
        raise AuditError("valor numérico esperado em %s (recebido %r)" % (where, value))


def as_population(value, where: str) -> Optional[float]:
    """População: número positivo ou None (ausência explícita)."""
    if value is None:
        return None
    number = as_number(value, where)
    if number <= 0:
        raise AuditError("população não positiva em %s (%r)" % (where, value))
    return number


# ---------------------------------------------------------------------------
# Casamento cidade -> aglomeração (mesma regra do gerador)
#
# Reimplementação INDEPENDENTE de tools/build_urban_land.py: como é uma auditoria, a
# cópia é proposital — se a regra do gerador mudar e esta ficar para trás, o bloco D
# acusa divergência em vez de herdar o erro. Divergência atual: 0 nas 834 cidades.
# ---------------------------------------------------------------------------

def normalize_name(text: str) -> str:
    decomposed = unicodedata.normalize("NFKD", text or "")
    stripped = "".join(char for char in decomposed if not unicodedata.combining(char))
    cleaned = re.sub(r"[^a-z0-9]+", " ", stripped.lower())
    return " ".join(cleaned.split())


def compact_name(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "", normalize_name(text))


def name_variants(text: str) -> List[str]:
    key = normalize_name(text)
    variants = [key]
    if key.endswith(" city") and len(key) > 5:
        variants.append(key[:-5].strip())
    return [variant for variant in variants if variant]


def country_matches(nation_name: str, country: str) -> bool:
    left = normalize_name(nation_name)
    right = normalize_name(country or "")
    if not left or not right:
        return False
    if left == right or right in left or left in right:
        return True
    left_tokens = {t for t in left.split() if len(t) >= 4 and t not in COUNTRY_STOPWORDS}
    right_tokens = {t for t in right.split() if len(t) >= 4 and t not in COUNTRY_STOPWORDS}
    return bool(left_tokens & right_tokens)


def load_agglomerations() -> Tuple[Dict[str, List[dict]], Dict[str, List[dict]]]:
    by_name: Dict[str, List[dict]] = {}
    by_compact: Dict[str, List[dict]] = {}
    for row in read_csv(AGGLOMERATIONS_CSV):
        population = (row.get("population") or "").strip()
        if not population.isdigit():
            continue
        for key in {normalize_name(row.get("english", "")), normalize_name(row.get("name", ""))}:
            if key:
                by_name.setdefault(key, []).append(row)
        for key in {compact_name(row.get("english", "")), compact_name(row.get("name", ""))}:
            if key:
                by_compact.setdefault(key, []).append(row)
    return by_name, by_compact


def match_agglomeration(city: dict, nation_name: str,
                        by_name: Dict[str, List[dict]],
                        by_compact: Dict[str, List[dict]]) -> Optional[dict]:
    iso_codes = ISO3_ALIASES.get(city["nation"], (city["nation"],))
    variants = name_variants(city["name"])
    exonym = CITY_EXONYMS.get(city["name"])
    if exonym:
        variants = variants + name_variants(exonym)
    candidates: List[dict] = []
    for variant in variants:
        candidates = [row for row in by_name.get(variant, []) if row.get("iso3") in iso_codes]
        if candidates:
            break
    if not candidates:
        for variant in variants:
            candidates = [row for row in by_compact.get(compact_name(variant), [])
                          if row.get("iso3") in iso_codes]
            if candidates:
                break
    if not candidates:
        for variant in variants:
            candidates = [row for row in by_name.get(variant, [])
                          if country_matches(nation_name, row.get("country", ""))]
            if candidates:
                break
    if not candidates:
        return None
    return min(candidates, key=lambda row: int(row.get("rank") or 10 ** 6))


# ---------------------------------------------------------------------------
# Carga dos dados definitivos
# ---------------------------------------------------------------------------

def load_dataset() -> dict:
    cities_raw = read_json(CITIES_JSON)
    if not isinstance(cities_raw, list):
        raise AuditError("cities.json deveria ser uma lista de cidades")

    cities: List[dict] = []
    for index, entry in enumerate(cities_raw):
        where = "cities.json[%d]" % index
        city_id = require(entry, "id", where)
        cities.append({
            "id": city_id,
            "name": require(entry, "name", where),
            "nation": require(entry, "nation", where),
            "population": as_population(require(entry, "population", where),
                                        "%s.population" % where),
            "sim_population": as_population(require(entry, "sim_population", where),
                                            "%s.sim_population" % where),
            "agglomeration_population": as_population(
                entry.get("agglomeration_population"),
                "%s.agglomeration_population" % where),
        })

    nations: Dict[str, dict] = {}
    for row in read_csv(NATIONS_CSV):
        nation_id = (row.get("id") or "").strip()
        if not nation_id:
            raise AuditError("nations.csv com linha sem id")
        nations[nation_id] = {
            "id": nation_id,
            "name": (row.get("name") or "").strip(),
            "population": as_number(row.get("population"), "nations.csv[%s].population" % nation_id),
        }

    urban_raw = read_json(URBAN_LAND_JSON)
    urban_cities = require(urban_raw, "cities", "urban_land.json")
    urban: Dict[str, dict] = {}
    for city_id, entry in urban_cities.items():
        where = "urban_land.json.cities[%s]" % city_id
        residential = as_number(require(require(entry, "zoning_split", where), "residential", where),
                                "%s.zoning_split.residential" % where)
        height_class = require(entry, "height_class", where)
        if height_class not in FAR_BY_HEIGHT:
            raise AuditError("height_class desconhecido em %s: %r" % (where, height_class))
        if residential <= 0 or residential > 1:
            raise AuditError("zoning_split.residential fora de (0,1] em %s: %r" % (where, residential))
        urban[city_id] = {
            "urban_population": as_population(require(entry, "urban_population", where),
                                              "%s.urban_population" % where),
            "urban_population_source": require(entry, "urban_population_source", where),
            "built_up_km2": as_number(require(entry, "built_up_km2", where),
                                      "%s.built_up_km2" % where),
            "residential_share": residential,
            "height_class": height_class,
        }

    housing_raw = read_json(HOUSING_CITY_JSON)
    housing_cities = require(housing_raw, "cities", "housing_city.json")
    housing: Dict[str, dict] = {}
    for city_id, entry in housing_cities.items():
        where = "housing_city.json.cities[%s]" % city_id
        housing[city_id] = {
            "housing_stock_total": as_number(require(entry, "housing_stock_total", where),
                                             "%s.housing_stock_total" % where),
            "urban_population": as_population(require(entry, "urban_population", where),
                                              "%s.urban_population" % where),
        }

    housing_nation_raw = read_json(HOUSING_JSON)
    housing_nations_raw = require(housing_nation_raw, "nations", "housing.json")
    household_size: Dict[str, float] = {}
    for nation_id, entry in housing_nations_raw.items():
        value = require(entry, "household_size", "housing.json.nations[%s]" % nation_id)
        if value is None:
            raise AuditError("housing.json.nations[%s].household_size ausente" % nation_id)
        household_size[nation_id] = as_number(value,
                                             "housing.json.nations[%s].household_size" % nation_id)

    return {
        "cities": cities,
        "nations": nations,
        "urban": urban,
        "housing": housing,
        "household_size": household_size,
    }


# ---------------------------------------------------------------------------
# Blocos
# ---------------------------------------------------------------------------

def capacity_res(entry: dict) -> float:
    height = entry["height_class"]
    return (entry["built_up_km2"] * entry["residential_share"] * 1e6
            * FAR_BY_HEIGHT[height] / UNIT_M2_BY_HEIGHT[height])


def build_blocks(data: dict) -> dict:
    cities = data["cities"]
    nations = data["nations"]
    urban = data["urban"]
    housing = data["housing"]
    household_size = data["household_size"]

    # ---- A. cobertura -----------------------------------------------------
    with_agg = [city for city in cities if city["agglomeration_population"]]
    src_sim = [city_id for city_id, entry in urban.items()
               if entry["urban_population_source"] == "sim_population"]
    block_a = {
        "cities": len(cities),
        "with_agglomeration": len(with_agg),
        "without_agglomeration": len(cities) - len(with_agg),
        "urban_population_source_sim": len(src_sim),
        "sum_population": sum(city["population"] for city in cities),
        "sum_sim_population": sum(city["sim_population"] for city in cities),
        "sum_urban_population": sum(entry["urban_population"] for entry in urban.values()),
    }

    # ---- B. soma por nação ------------------------------------------------
    sums: Dict[str, float] = {}
    for city in cities:
        sums[city["nation"]] = sums.get(city["nation"], 0.0) + city["population"]
    divergences = []
    for nation_id in sorted(sums):
        expected = nations[nation_id]["population"] if nation_id in nations else None
        if expected is None:
            divergences.append({"nation": nation_id, "cities_sum": sums[nation_id],
                                "nations_csv": None, "diff": None,
                                "reason": "nação ausente em nations.csv"})
            continue
        diff = sums[nation_id] - expected
        if abs(diff) > SUM_TOLERANCE:
            divergences.append({"nation": nation_id, "cities_sum": sums[nation_id],
                                "nations_csv": expected, "diff": diff,
                                "reason": "soma das cidades != nations.csv"})
    block_b = {
        "nations_compared": len(sums),
        "divergences": divergences,
        "world_total_nations_csv": sum(entry["population"] for entry in nations.values()),
        "world_total_cities": sum(sums.values()),
    }

    # ---- C. fator regional -------------------------------------------------
    factor_by_city: Dict[str, float] = {}
    factors_by_nation: Dict[str, List[float]] = {}
    for city in cities:
        urban_pop = urban[city["id"]]["urban_population"]
        factor = city["population"] / urban_pop if urban_pop else None
        if factor is None:
            continue
        factor_by_city[city["id"]] = factor
        factors_by_nation.setdefault(city["nation"], []).append(factor)
    flagged = []
    nation_medians: List[float] = []
    for nation_id in sorted(factors_by_nation):
        values = factors_by_nation[nation_id]
        med = statistics.median(values)
        nation_medians.append(med)
        spread = (max(values) - min(values)) / med if med > 0 else float("inf")
        if spread > SPREAD_LIMIT:
            flagged.append({"nation": nation_id, "min": min(values), "median": med,
                            "max": max(values), "relative_spread": spread})
    all_factors = list(factor_by_city.values())
    block_c = {
        "nations": len(factors_by_nation),
        "cities": len(all_factors),
        "cities_factor": {"min": min(all_factors), "median": statistics.median(all_factors),
                          "max": max(all_factors)},
        "nation_medians": {"min": min(nation_medians),
                           "median": statistics.median(nation_medians),
                           "max": max(nation_medians)},
        "flagged_nations": flagged,
    }

    # ---- D. aglomeração real ----------------------------------------------
    by_name, by_compact = load_agglomerations()
    agg_divergences = []
    capped = []
    unused_matches = []
    for city in cities:
        entry = urban[city["id"]]
        if city["agglomeration_population"]:
            if entry["urban_population"] != city["agglomeration_population"]:
                # Corte DECLARADO da regra (aglomeração transfronteiriça limitada à
                # população da nação, urban_population_source='agglomeration_capped'):
                # esperado, não é divergência de dado.
                target = (capped if entry["urban_population_source"] == "agglomeration_capped"
                          else agg_divergences)
                target.append({
                    "id": city["id"], "name": city["name"], "nation": city["nation"],
                    "urban_population": entry["urban_population"],
                    "agglomeration_population": city["agglomeration_population"],
                })
        else:
            row = match_agglomeration(city, nations.get(city["nation"], {}).get("name", ""),
                                      by_name, by_compact)
            if row is not None:
                unused_matches.append({
                    "id": city["id"], "name": city["name"], "nation": city["nation"],
                    "sim_population": city["sim_population"],
                    "table_population": float(row["population"]),
                    "table_name": row.get("english") or row.get("name"),
                    "table_rank": int(row.get("rank") or 0),
                })
    block_d = {
        "with_agglomeration": len(with_agg),
        "urban_vs_agglomeration_divergences": agg_divergences,
        "urban_population_capped": capped,
        "unused_matches": unused_matches,
    }

    # ---- E. qualidade da unidade de simulação ------------------------------
    sim_quality = []
    for city in with_agg:
        agg = city["agglomeration_population"]
        ratio = city["sim_population"] / agg
        if ratio < SIM_RATIO_LOW or ratio > SIM_RATIO_HIGH:
            sim_quality.append({
                "id": city["id"], "name": city["name"], "nation": city["nation"],
                "sim_population": city["sim_population"],
                "agglomeration_population": agg,
                "ratio": ratio,
            })
    sim_quality.sort(key=lambda item: (-abs(item["ratio"] - 1.0), item["id"]))
    block_e = {
        "cities_with_agglomeration": len(with_agg),
        "out_of_range": len(sim_quality),
        "below_half": sum(1 for item in sim_quality if item["ratio"] < SIM_RATIO_LOW),
        "above_double": sum(1 for item in sim_quality if item["ratio"] > SIM_RATIO_HIGH),
        "items": sim_quality,
    }

    # ---- F. ocupação do solo residencial ----------------------------------
    occupancy = []
    for city in cities:
        entry = urban[city["id"]]
        cap = capacity_res(entry)
        if cap <= 0:
            raise AuditError("capacidade residencial não positiva em %s" % city["id"])
        stock = housing[city["id"]]["housing_stock_total"]
        occupancy.append({
            "id": city["id"], "name": city["name"], "nation": city["nation"],
            "housing_stock_total": stock,
            "capacity": cap,
            "occupancy": stock / cap,
        })
    occupancy.sort(key=lambda item: (-item["occupancy"], item["id"]))
    block_f = {
        "cities": len(occupancy),
        "median": statistics.median(item["occupancy"] for item in occupancy),
        "above_one": [{"id": item["id"], "occupancy": item["occupancy"]}
                      for item in occupancy if item["occupancy"] > 1.0],
        "top10": occupancy[:10],
        "bottom10": list(reversed(occupancy[-10:])),
    }

    # ---- G. maiores razões para hinterland --------------------------------
    hinterland = []
    for city in with_agg:
        factor = factors_by_nation.get(city["nation"], [])
        hinterland.append({
            "id": city["id"], "name": city["name"], "nation": city["nation"],
            "population": city["population"],
            "agglomeration_population": city["agglomeration_population"],
            "ratio": city["population"] / city["agglomeration_population"],
            "nation_factor": statistics.median(factor) if factor else None,
        })
    hinterland.sort(key=lambda item: (-item["ratio"], item["id"]))
    block_g = {"top15": hinterland[:15]}

    # ---- H. integridade ---------------------------------------------------
    checks = []

    checks.append({
        "id": "i",
        "label": "soma de population por nação == nations.csv",
        "ok": not divergences,
        "detail": "%d nações conferidas" % len(sums),
        "violations": divergences,
    })

    stock_bad = []
    nation_by_city = {city["id"]: city["nation"] for city in cities}
    for city_id, entry in housing.items():
        nation_id = nation_by_city.get(city_id)
        if nation_id is None or nation_id not in household_size:
            stock_bad.append({"id": city_id, "reason": "household_size da nação ausente"})
            continue
        expected = round(entry["urban_population"] / household_size[nation_id])
        if abs(entry["housing_stock_total"] - expected) > STOCK_TOLERANCE:
            stock_bad.append({
                "id": city_id,
                "housing_stock_total": entry["housing_stock_total"],
                "expected": expected,
                "diff": entry["housing_stock_total"] - expected,
            })
    checks.append({
        "id": "ii",
        "label": "housing_stock_total == round(urban_population / household_size), tol. %d"
                 % STOCK_TOLERANCE,
        "ok": not stock_bad,
        "detail": "%d cidades conferidas" % len(housing),
        "violations": stock_bad,
    })

    urban_bad = []
    for city in cities:
        entry = urban[city["id"]]
        nation_pop = nations.get(city["nation"], {}).get("population")
        if entry["urban_population"] is None or entry["urban_population"] <= 0:
            urban_bad.append({"id": city["id"], "urban_population": entry["urban_population"],
                              "reason": "urban_population <= 0"})
        elif nation_pop is not None and entry["urban_population"] > nation_pop:
            violation = {"id": city["id"], "name": city["name"],
                         "urban_population": entry["urban_population"],
                         "nation": city["nation"], "nation_population": nation_pop,
                         "reason": "urban_population > população nacional"}
            # Contexto da fonte quando o valor vem de aglomeração (perímetro pode
            # cruzar fronteira: p.ex. a aglomeração de Cingapura inclui Johor Bahru).
            row = match_agglomeration(city, nations.get(city["nation"], {}).get("name", ""),
                                      by_name, by_compact)
            if row is not None:
                violation["agglomeration_remark"] = (row.get("remark") or "").strip()
                violation["agglomeration_rank"] = int(row.get("rank") or 0)
            urban_bad.append(violation)
    checks.append({
        "id": "iii",
        "label": "urban_population > 0 e <= população nacional da nação",
        "ok": not urban_bad,
        "detail": "%d cidades conferidas" % len(cities),
        "violations": urban_bad,
    })

    city_ids = {city["id"] for city in cities}
    id_problems = []
    for label, collection in (("urban_land.cities", urban),
                              ("housing_city.cities", housing)):
        missing = sorted(city_ids - set(collection))
        extra = sorted(set(collection) - city_ids)
        if missing or extra:
            id_problems.append({"file": label, "missing": missing, "extra": extra})
    if len(city_ids) != EXPECTED_CITIES:
        id_problems.append({"file": "cities.json", "count": len(city_ids),
                            "expected": EXPECTED_CITIES})
    checks.append({
        "id": "iv",
        "label": "%d cidades e mesmos ids nos três arquivos" % EXPECTED_CITIES,
        "ok": not id_problems,
        "detail": "cities.json=%d urban_land=%d housing_city=%d"
                  % (len(city_ids), len(urban), len(housing)),
        "violations": id_problems,
    })

    block_h = {
        "ok": all(check["ok"] for check in checks),
        "checks": checks,
    }

    return {
        "A_coverage": block_a,
        "B_nation_sums": block_b,
        "C_regional_factor": block_c,
        "D_agglomeration": block_d,
        "E_sim_quality": block_e,
        "F_res_occupancy": block_f,
        "G_hinterland": block_g,
        "H_integrity": block_h,
    }


# ---------------------------------------------------------------------------
# Formatação
# ---------------------------------------------------------------------------

def f_pop(value: Optional[float]) -> str:
    if value is None:
        return "null"
    return "%d" % round(value)


def f_ratio(value: Optional[float]) -> str:
    if value is None:
        return "-"
    text = "%.4f" % value
    return text.replace(".", ",")


def f_bound(value: float) -> str:
    """Limiar em forma curta (0,5 · 2 · 0,01)."""
    return ("%g" % value).replace(".", ",")


def print_report(blocks: dict) -> None:
    a = blocks["A_coverage"]
    b = blocks["B_nation_sums"]
    c = blocks["C_regional_factor"]
    d = blocks["D_agglomeration"]
    e = blocks["E_sim_quality"]
    f = blocks["F_res_occupancy"]
    g = blocks["G_hinterland"]
    h = blocks["H_integrity"]

    print("=== Auditoria de população das 834 cidades (B4 / Q-015) ===")
    print("fontes: cities.json, nations.csv, urban_land.json, housing_city.json,")
    print("        housing.json, tools/agglomerations_2026.csv")
    print()
    print("A. COBERTURA")
    print("   cidades: %d" % a["cities"])
    print("   com agglomeration_population: %d" % a["with_agglomeration"])
    print("   sem aglomeração (urban_population_source=sim_population): %d"
          % a["urban_population_source_sim"])
    print("   soma population (regional): %s" % f_pop(a["sum_population"]))
    print("   soma sim_population (cidade própria): %s" % f_pop(a["sum_sim_population"]))
    print("   soma urban_population (unidade de simulação): %s"
          % f_pop(a["sum_urban_population"]))
    print()

    print("B. SOMA POR NAÇÃO vs nations.csv")
    print("   nações comparadas: %d" % b["nations_compared"])
    print("   divergências: %s" % ("nenhuma" if not b["divergences"] else len(b["divergences"])))
    for item in b["divergences"][:10]:
        print("     ! %s soma_cidades=%s nations.csv=%s (%s)"
              % (item["nation"], f_pop(item["cities_sum"]), f_pop(item["nations_csv"]),
                 item["reason"]))
    print("   total mundial (nations.csv): %s" % f_pop(b["world_total_nations_csv"]))
    print("   total mundial (soma das cidades): %s" % f_pop(b["world_total_cities"]))
    print()

    print("C. FATOR REGIONAL (population / urban_population)")
    print("   nações: %d · cidades: %d" % (c["nations"], c["cities"]))
    print("   fator por cidade: min %s · mediana %s · max %s"
          % (f_ratio(c["cities_factor"]["min"]), f_ratio(c["cities_factor"]["median"]),
             f_ratio(c["cities_factor"]["max"])))
    print("   mediana por nação: min %s · mediana %s · max %s"
          % (f_ratio(c["nation_medians"]["min"]), f_ratio(c["nation_medians"]["median"]),
             f_ratio(c["nation_medians"]["max"])))
    if c["flagged_nations"]:
        print("   nações com spread relativo > %s: %d" % (f_bound(SPREAD_LIMIT),
                                                          len(c["flagged_nations"])))
        for item in c["flagged_nations"]:
            print("     ! %s min %s · mediana %s · max %s · spread %s"
                  % (item["nation"], f_ratio(item["min"]), f_ratio(item["median"]),
                     f_ratio(item["max"]), f_ratio(item["relative_spread"])))
    else:
        print("   nações com spread relativo > %s: nenhuma" % f_bound(SPREAD_LIMIT))
    print()

    print("D. urban_population vs AGLOMERAÇÃO REAL")
    print("   cidades com aglomeração: %d · divergências (urban != aglomeração): %d"
          % (d["with_agglomeration"], len(d["urban_vs_agglomeration_divergences"])))
    for item in d["urban_vs_agglomeration_divergences"][:10]:
        print("     ! %s %s urban=%s aglomeracao=%s"
              % (item["id"], item["name"], f_pop(item["urban_population"]),
                 f_pop(item["agglomeration_population"])))
    print("   cortadas na população da nação (urban_population_source=agglomeration_capped): %d"
          % len(d["urban_population_capped"]))
    for item in d["urban_population_capped"][:10]:
        print("     - %s %s urban=%s aglomeracao=%s"
              % (item["id"], item["name"], f_pop(item["urban_population"]),
                 f_pop(item["agglomeration_population"])))
    print("   sem aglomeração com linha na tabela não usada: %d (esperado 0)"
          % len(d["unused_matches"]))
    for item in d["unused_matches"][:10]:
        print("     ! %s %s sim=%s tabela=%s (rank %d)"
              % (item["id"], item["name"], f_pop(item["sim_population"]),
                 f_pop(item["table_population"]), item["table_rank"]))
    print()

    print("E. QUALIDADE DA UNIDADE DE SIMULAÇÃO (sim_population fora de [%sx, %sx] da aglomeração)"
          % (f_bound(SIM_RATIO_LOW), f_bound(SIM_RATIO_HIGH)))
    print("   cidades com aglomeração: %d · fora da faixa: %d (abaixo de %sx: %d · acima de %sx: %d)"
          % (e["cities_with_agglomeration"], e["out_of_range"],
             f_bound(SIM_RATIO_LOW), e["below_half"],
             f_bound(SIM_RATIO_HIGH), e["above_double"]))
    print("   ordenadas pelo desvio |razão-1| (maior primeiro):")
    for index, item in enumerate(e["items"], 1):
        print("   %3d. %-8s %-24s %-4s sim %10s · aglom %10s · razão %s"
              % (index, item["id"], item["name"][:24], item["nation"],
                 f_pop(item["sim_population"]), f_pop(item["agglomeration_population"]),
                 f_ratio(item["ratio"])))
    print()

    print("F. OCUPAÇÃO DO SOLO RESIDENCIAL (housing_stock_total / capacidade)")
    print("   cidades: %d · mediana %s" % (f["cities"], f_ratio(f["median"])))
    print("   >1: %d (%s)" % (len(f["above_one"]),
                              ", ".join(item["id"] for item in f["above_one"]) or "nenhuma"))
    print("   10 maiores:")
    for index, item in enumerate(f["top10"], 1):
        print("   %2d. %-8s %-22s %-4s estoque %10s · capacidade %12s · ocupação %s"
              % (index, item["id"], item["name"][:22], item["nation"],
                 f_pop(item["housing_stock_total"]), f_pop(item["capacity"]),
                 f_ratio(item["occupancy"])))
    print("   10 menores:")
    for index, item in enumerate(f["bottom10"], 1):
        print("   %2d. %-8s %-22s %-4s estoque %10s · capacidade %12s · ocupação %s"
              % (index, item["id"], item["name"][:22], item["nation"],
                 f_pop(item["housing_stock_total"]), f_pop(item["capacity"]),
                 f_ratio(item["occupancy"])))
    print()

    print("G. MAIORES population / agglomeration_population (top %d)" % len(g["top15"]))
    for index, item in enumerate(g["top15"], 1):
        print("   %2d. %-8s %-22s %-4s pop %12s · aglom %11s · razão %s · fator_nação %s"
              % (index, item["id"], item["name"][:22], item["nation"],
                 f_pop(item["population"]), f_pop(item["agglomeration_population"]),
                 f_ratio(item["ratio"]), f_ratio(item["nation_factor"])))
    print()

    print("H. INTEGRIDADE")
    for check in h["checks"]:
        status = "ok" if check["ok"] else "FALHOU %d" % len(check["violations"])
        print("   (%s) %s: %s" % (check["id"], check["label"], status))
        for item in check["violations"][:10]:
            print("       ! %s" % json.dumps(item, ensure_ascii=False))
    print("   resultado: %s" % ("ok" if h["ok"] else "FALHOU"))


def write_json(path: str, blocks: dict) -> None:
    payload = {
        "generated_by": "tools/audit_city_population.py",
        "sources": {
            "cities": os.path.relpath(CITIES_JSON, ROOT).replace("\\", "/"),
            "nations": os.path.relpath(NATIONS_CSV, ROOT).replace("\\", "/"),
            "urban_land": os.path.relpath(URBAN_LAND_JSON, ROOT).replace("\\", "/"),
            "housing_city": os.path.relpath(HOUSING_CITY_JSON, ROOT).replace("\\", "/"),
            "housing": os.path.relpath(HOUSING_JSON, ROOT).replace("\\", "/"),
            "agglomerations": os.path.relpath(AGGLOMERATIONS_CSV, ROOT).replace("\\", "/"),
        },
        "capacity_constants": {
            "far_by_height": FAR_BY_HEIGHT,
            "unit_m2_by_height": UNIT_M2_BY_HEIGHT,
        },
    }
    payload.update(blocks)
    directory = os.path.dirname(os.path.abspath(path))
    if directory and not os.path.isdir(directory):
        raise AuditError("diretório de saída inexistente: %s" % directory)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def write_csv(path: str, blocks: dict) -> None:
    header = ["bloco", "ordem", "id", "name", "nation", "sim_population", "population",
              "agglomeration_population", "ratio", "nation_factor"]
    directory = os.path.dirname(os.path.abspath(path))
    if directory and not os.path.isdir(directory):
        raise AuditError("diretório de saída inexistente: %s" % directory)
    with open(path, "w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.writer(handle, delimiter=";", lineterminator="\n")
        writer.writerow(header)
        for index, item in enumerate(blocks["E_sim_quality"]["items"], 1):
            writer.writerow(["E", index, item["id"], item["name"], item["nation"],
                             f_pop(item["sim_population"]), "",
                             f_pop(item["agglomeration_population"]),
                             "%.6f" % item["ratio"], ""])
        for index, item in enumerate(blocks["G_hinterland"]["top15"], 1):
            writer.writerow(["G", index, item["id"], item["name"], item["nation"], "",
                             f_pop(item["population"]),
                             f_pop(item["agglomeration_population"]),
                             "%.6f" % item["ratio"],
                             "" if item["nation_factor"] is None
                             else "%.6f" % item["nation_factor"]])


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Auditoria reprodutível da população das 834 cidades (B4 / Q-015). "
                    "Somente leitura dos dados: escreve apenas com --json/--csv.")
    parser.add_argument("--json", metavar="PATH", dest="json_path",
                        help="grava os blocos A-H e as listas em JSON")
    parser.add_argument("--csv", metavar="PATH", dest="csv_path",
                        help="grava CSV (separador ';', utf-8-sig) dos blocos E e G")
    args = parser.parse_args(argv)

    try:
        data = load_dataset()
        blocks = build_blocks(data)
        print_report(blocks)
        if args.json_path:
            write_json(args.json_path, blocks)
            print("\nJSON gravado em: %s" % args.json_path)
        if args.csv_path:
            write_csv(args.csv_path, blocks)
            print("CSV gravado em: %s" % args.csv_path)
    except AuditError as exc:
        print("ERRO: %s" % exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print("ERRO de E/S: %s" % exc, file=sys.stderr)
        return 1

    return 0 if blocks["H_integrity"]["ok"] else 1


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
