#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida data/definitions/economy/housing_city.json contra cities.csv, nations.csv e
housing.json.

Checa (recalculando de forma independente, como os demais validadores):
  1. o JSON cobre exatamente as cidades do cities.csv (nem uma a mais, nem a menos);
  2. housing_stock_total = round(população / household_size da nação); a divisão
     housing_stock_by_tier (popular/medium/high) soma o total exato e popular/high batem
     com housing_stock_split da nação (tolerância de 1 unidade; medium fecha);
  3. tenure_by_tier: cada estrato soma 1,0 (tol. 0,01); o informal total reconstitui
     informal_share (favela/100); informal decresce com a renda (popular >= medium >=
     high) e casa própria cresce com a renda (high >= medium >= popular);
  4. aluguel/preço (proxy de renda): quando há PIB PPP per capita, rent = rent_ratio x
     per_capita / (12 x floor_m2) e price = rent x 12 x 20; quando não há, ambos null,
     price_source="no_data" e confidence="partial";
  5. flags estimated=true, confidence coerente, method presente, refs com url http(s);
  6. o bloco coverage bate com a contagem recalculada.

Uso:
    python tools/validate_housing_city.py

Retorna 0 quando está tudo consistente, 1 (com a lista de problemas) caso contrário.
"""

from __future__ import annotations

import csv
import json
import os
import sys
from typing import Dict, List, Optional

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")
ECONOMY = os.path.join(ROOT, "data", "definitions", "economy")
CITIES_CSV = os.path.join(SCENARIO, "cities.csv")
NATIONS_CSV = os.path.join(SCENARIO, "nations.csv")
HOUSING_JSON = os.path.join(ECONOMY, "housing.json")
HOUSING_CITY_JSON = os.path.join(ECONOMY, "housing_city.json")

BAND_HIGH_MIN = 25000.0
BAND_UPPER_MID_MIN = 12000.0
BAND_LOWER_MID_MIN = 5000.0

FLOOR_M2_PER_PERSON = {
    "low": 12.0, "lower_mid": 16.0, "upper_mid": 24.0, "high": 38.0, "unknown": 12.0,
}
PRICE_TO_RENT_RATIO = 20.0
OWNERSHIP_SPREAD = 0.15

TIERS = ("popular", "medium", "high")
TENURE_KEYS = ("own", "rent", "informal")
CONFIDENCE = {"estimated", "partial"}
STOCK_TOL = 1.5          # unidades de domicílio (arredondamento)
TENURE_SUM_TOL = 0.01
INFORMAL_TOL = 0.005

SAMPLES = ("USA-NEW", "BRA-SAO", "IND-MUM", "NGA-LAG", "DEU-BER", "JPN-TOK", "VNM-HOC",
           "AFG-KAB", "FJI-SUV", "ZAF-GQE")


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def load_csv(path: str) -> Dict[str, dict]:
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return {row["id"]: row for row in csv.DictReader(handle)}


def load_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def is_number(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def income_band(nation: dict) -> str:
    raw_pop = (nation.get("population") or "").strip()
    raw_gdp = (nation.get("gdp_ppp_bn") or "").strip()
    if not raw_pop or not raw_gdp:
        return "unknown"
    population = int(float(raw_pop))
    if population <= 0:
        return "unknown"
    per_capita = float(raw_gdp) * 1e9 / population
    if per_capita < BAND_LOWER_MID_MIN:
        return "low"
    if per_capita < BAND_UPPER_MID_MIN:
        return "lower_mid"
    if per_capita < BAND_HIGH_MIN:
        return "upper_mid"
    return "high"


def gdp_per_capita(nation: dict) -> Optional[float]:
    raw_pop = (nation.get("population") or "").strip()
    raw_gdp = (nation.get("gdp_ppp_bn") or "").strip()
    try:
        population = float(raw_pop)
        gdp = float(raw_gdp)
    except ValueError:
        return None
    if population <= 0.0 or gdp <= 0.0:
        return None
    return gdp * 1e9 / population


def expected_stock(population: int, household_size: float) -> int:
    if household_size <= 0:
        return 0
    return max(1, int(round(population / household_size)))


def expected_tiers(total: int, split: Dict[str, float]) -> Dict[str, int]:
    popular = int(round(total * split.get("popular", 0.0)))
    high = int(round(total * split.get("high", 0.0)))
    medium = total - popular - high
    if medium < 0:
        medium = 0
        high = total - popular
    return {"popular": popular, "medium": medium, "high": high}


def expected_tenure(own_rate: float, informal: float,
                    split: Dict[str, float]) -> Dict[str, Dict[str, float]]:
    p = split.get("popular", 0.0)
    m = split.get("medium", 0.0)
    h = split.get("high", 0.0)

    informal_by = {"popular": 0.0, "medium": 0.0, "high": 0.0}
    remaining = informal
    for tier in TIERS:
        share = split.get(tier, 0.0)
        if share <= 0.0:
            continue
        informal_by[tier] = min(1.0, remaining / share)
        remaining -= informal_by[tier] * share
        if remaining <= 1e-12:
            remaining = 0.0
            break

    own_grad_popular = clamp(own_rate - OWNERSHIP_SPREAD, 0.0, 1.0)
    own_grad_high = clamp(own_rate + OWNERSHIP_SPREAD, 0.0, 1.0)
    if m > 0.0:
        own_grad_medium = clamp(
            (own_rate - p * own_grad_popular - h * own_grad_high) / m, 0.0, 1.0)
    else:
        own_grad_medium = own_rate
    own_grad = {"popular": own_grad_popular, "medium": own_grad_medium, "high": own_grad_high}

    result: Dict[str, Dict[str, float]] = {}
    for tier in TIERS:
        informal_tier = informal_by[tier]
        formal = 1.0 - informal_tier
        grad = own_grad[tier]
        result[tier] = {
            "own": round(grad * formal, 4),
            "rent": round((1.0 - grad) * formal, 4),
            "informal": round(informal_tier, 4),
        }
    return result


def check_top_level(payload: dict, problems: List[str]) -> None:
    if payload.get("version") != 1:
        problems.append("version deveria ser 1, veio %r" % payload.get("version"))
    if not isinstance(payload.get("note"), str) or not payload["note"].strip():
        problems.append("note ausente ou vazia")
    for block in ("coverage", "sources", "method", "cities"):
        if not isinstance(payload.get(block), dict):
            problems.append("bloco %s ausente ou não é objeto" % block)
    method = payload.get("method") or {}
    for key in ("price_and_rent", "housing_stock", "tenure_by_tier"):
        if not isinstance(method.get(key), str) or len(method.get(key, "").strip()) < 40:
            problems.append("method.%s ausente ou curto demais" % key)


def check_refs(entry: dict, code: str, problems: List[str]) -> None:
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
        if not isinstance(ref.get("year"), int):
            problems.append("%s: refs[%d].year não é inteiro: %r"
                            % (code, position, ref.get("year")))


def check_entry(entry: dict, city: dict, nation: dict, housing: dict,
                problems: List[str]) -> str:
    code = city["id"]
    if not isinstance(entry, dict):
        problems.append("%s: entrada não é objeto" % code)
        return "invalid"

    if entry.get("name") != city["name"]:
        problems.append("%s: name %r != cities.csv %r" % (code, entry.get("name"), city["name"]))
    if entry.get("nation") != city["nation"]:
        problems.append("%s: nation %r != cities.csv %r"
                        % (code, entry.get("nation"), city["nation"]))

    population = int(city["population"])
    household_size = float(housing.get("household_size") or 0.0)
    own_rate = float(housing.get("homeownership_rate") or 0.0)
    rent_ratio = housing.get("rent_to_income_ratio")
    slum = housing.get("slum_pct_urban")
    split = housing.get("housing_stock_split") or {"popular": 0.0, "medium": 1.0, "high": 0.0}
    informal = (float(slum) / 100.0) if is_number(slum) else 0.0

    # (b) estoque
    total = entry.get("housing_stock_total")
    if not is_number(total) or int(total) != expected_stock(population, household_size):
        problems.append("%s: housing_stock_total %r != esperado %d"
                        % (code, total, expected_stock(population, household_size)))
    tiers = entry.get("housing_stock_by_tier")
    if not isinstance(tiers, dict) or set(tiers) != set(TIERS):
        problems.append("%s: housing_stock_by_tier inválido" % code)
    elif is_number(total):
        want_tiers = expected_tiers(int(total), split)
        for tier in TIERS:
            value = tiers.get(tier)
            if not is_number(value):
                problems.append("%s: housing_stock_by_tier.%s não numérico: %r"
                                % (code, tier, value))
            elif abs(int(value) - want_tiers[tier]) > STOCK_TOL:
                problems.append("%s: housing_stock_by_tier.%s %r != esperado %r"
                                % (code, tier, value, want_tiers[tier]))
        if is_number(tiers.get("popular")) and is_number(tiers.get("medium")) \
                and is_number(tiers.get("high")) and is_number(total):
            summed = int(tiers["popular"]) + int(tiers["medium"]) + int(tiers["high"])
            if summed != int(total):
                problems.append("%s: housing_stock_by_tier soma %d != total %d"
                                % (code, summed, int(total)))
    if entry.get("stock_split_source") != "derived:lognormal_from_gini":
        problems.append("%s: stock_split_source inválido: %r"
                        % (code, entry.get("stock_split_source")))

    # (c) posse
    tenure = entry.get("tenure_by_tier")
    if not isinstance(tenure, dict) or set(tenure) != set(TIERS):
        problems.append("%s: tenure_by_tier inválido" % code)
    else:
        want_tenure = expected_tenure(own_rate, informal, split)
        informal_reconstructed = 0.0
        own_by_tier: Dict[str, float] = {}
        for tier in TIERS:
            block = tenure.get(tier)
            if not isinstance(block, dict) or set(block) != set(TENURE_KEYS):
                problems.append("%s: tenure_by_tier.%s inválido" % (code, tier))
                continue
            subtotal = 0.0
            for key in TENURE_KEYS:
                value = block.get(key)
                if not is_number(value) or not (0.0 <= float(value) <= 1.0):
                    problems.append("%s: tenure_by_tier.%s.%s inválido: %r"
                                    % (code, tier, key, value))
                    continue
                subtotal += float(value)
                if key == "informal":
                    informal_reconstructed += float(value) * split.get(tier, 0.0)
                if key == "own":
                    own_by_tier[tier] = float(value)
                want = want_tenure[tier][key]
                if abs(float(value) - want) > 0.002:
                    problems.append("%s: tenure_by_tier.%s.%s %.4f != esperado %.4f"
                                    % (code, tier, key, float(value), want))
            if abs(subtotal - 1.0) > TENURE_SUM_TOL:
                problems.append("%s: tenure_by_tier.%s soma %.4f (esperado 1.0)"
                                % (code, tier, subtotal))
        if abs(informal_reconstructed - informal) > INFORMAL_TOL:
            problems.append("%s: informal reconstituído %.4f != informal_share %.4f"
                            % (code, informal_reconstructed, informal))
        # gradiente: informal decresce com a renda, casa própria cresce.
        if informal_by_values := [tenure.get(t, {}).get("informal")
                                  for t in TIERS
                                  if is_number(tenure.get(t, {}).get("informal"))]:
            if not (informal_by_values[0] + 1e-9 >= informal_by_values[-1]):
                problems.append("%s: informal não decresce com a renda %s"
                                % (code, informal_by_values))
        if own_by_tier and all(t in own_by_tier for t in TIERS):
            if not (own_by_tier["high"] + 1e-9 >= own_by_tier["popular"]):
                problems.append("%s: casa própria não cresce com a renda %s"
                                % (code, [round(own_by_tier[t], 4) for t in TIERS]))
    if entry.get("tenure_source") != "derived:homeownership+slum":
        problems.append("%s: tenure_source inválido: %r" % (code, entry.get("tenure_source")))

    # (a) aluguel/preço
    band = income_band(nation)
    floor_m2 = FLOOR_M2_PER_PERSON[band]
    per_capita = gdp_per_capita(nation)
    price = entry.get("price_per_m2_usd")
    rent = entry.get("rent_per_m2_usd")
    if per_capita is not None and is_number(rent_ratio):
        want_rent = round(float(rent_ratio) * per_capita / (12.0 * floor_m2), 2)
        want_price = round(want_rent * 12.0 * PRICE_TO_RENT_RATIO, 2)
        if not is_number(rent) or abs(float(rent) - want_rent) > 0.01:
            problems.append("%s: rent_per_m2_usd %r != esperado %.2f" % (code, rent, want_rent))
        if not is_number(price) or abs(float(price) - want_price) > 0.01:
            problems.append("%s: price_per_m2_usd %r != esperado %.2f" % (code, price, want_price))
        if entry.get("price_source") != "proxy_income":
            problems.append("%s: price_source %r != 'proxy_income'"
                            % (code, entry.get("price_source")))
        if entry.get("confidence") != "estimated":
            problems.append("%s: confidence %r != 'estimated'" % (code, entry.get("confidence")))
    else:
        if rent is not None or price is not None:
            problems.append("%s: sem PIB/rent mas com preço/aluguel não-nulo" % code)
        if entry.get("price_source") != "no_data":
            problems.append("%s: price_source %r != 'no_data'" % (code, entry.get("price_source")))
        if entry.get("confidence") != "partial":
            problems.append("%s: confidence %r != 'partial'" % (code, entry.get("confidence")))

    if entry.get("estimated") is not True:
        problems.append("%s: estimated deveria ser true" % code)
    if not entry.get("method"):
        problems.append("%s: method ausente" % code)
    if entry.get("confidence") not in CONFIDENCE:
        problems.append("%s: confidence inválida: %r" % (code, entry.get("confidence")))
    if not is_number(entry.get("price_to_rent_ratio")) \
            or float(entry["price_to_rent_ratio"]) != PRICE_TO_RENT_RATIO:
        problems.append("%s: price_to_rent_ratio inválido: %r"
                        % (code, entry.get("price_to_rent_ratio")))

    check_refs(entry, code, problems)
    return entry.get("confidence") or "invalid"


def main() -> int:
    cities = load_csv(CITIES_CSV)
    nations = load_csv(NATIONS_CSV)
    housing = load_json(HOUSING_JSON).get("nations", {})
    payload = load_json(HOUSING_CITY_JSON)
    problems: List[str] = []

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

    counts: Dict[str, int] = {}
    with_price = 0
    no_price = 0
    for code in sorted(cities):
        entry = entries.get(code)
        if entry is None:
            counts["invalid"] = counts.get("invalid", 0) + 1
            continue
        city_nation = cities[code]["nation"]
        nation = nations.get(city_nation)
        if nation is None:
            problems.append("%s: nação %r ausente em nations.csv" % (code, city_nation))
            continue
        hh = housing.get(city_nation)
        if hh is None:
            problems.append("%s: nação %r ausente em housing.json" % (code, city_nation))
            continue
        kind = check_entry(entry, cities[code], nation, hh, problems)
        counts[kind] = counts.get(kind, 0) + 1
        if entry.get("price_per_m2_usd") is not None:
            with_price += 1
        else:
            no_price += 1

    coverage = payload.get("coverage") or {}
    recomputed = {
        "cities_total": len(cities),
        "cities_with_price_rent": with_price,
        "cities_price_no_data": no_price,
    }
    for field, value in recomputed.items():
        if coverage.get(field) != value:
            problems.append("coverage.%s = %r, recalculado = %r" % (field, coverage.get(field), value))

    print("cidades no cities.csv: %d | entradas no JSON: %d" % (len(cities), len(entries)))
    print("com aluguel/preço (proxy de renda): %d | sem preço (sem PIB): %d"
          % (with_price, no_price))
    print("amostra:")
    for code in SAMPLES:
        entry = entries.get(code) or {}
        print("  %-9s %-16s stock=%-9s price=%-8s rent=%-6s conf=%-8s"
              % (code, entry.get("name"), entry.get("housing_stock_total"),
                 entry.get("price_per_m2_usd"), entry.get("rent_per_m2_usd"),
                 entry.get("confidence")))

    if problems:
        print()
        print("PROBLEMAS (%d):" % len(problems))
        for problem in problems:
            print(" -", problem)
        return 1

    print()
    print("OK: %d cidades, %d com aluguel/preço, %d sem preço"
          % (len(cities), with_price, no_price))
    return 0


if __name__ == "__main__":
    sys.exit(main())
