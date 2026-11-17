#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gera data/definitions/economy/housing_city.json: habitação POR CIDADE (834).

Preenche as lacunas do §19 que o housing.json (por nação) não cobre — itens (a), (b)
e (c) do pedido do gerente-dados:

  (a) aluguel e preço do m² residencial por cidade
      proxy de renda documentado (fonte: OECD AHD HC1.2 rent burden + HC2.1 living
      space + price-to-rent; NÃO é medição de mercado da cidade). Não há fonte
      gratuita/estável de preço por m² por cidade (Numbeo é scraping/ToS, Global
      Property Guide é parcialmente pago); por isso o campo é explicitamente um proxy.

  (b) estoque habitacional absoluto por cidade
      housing_stock_total = população da cidade (cities.csv) / tamanho médio do
      domicílio (housing.json, UN DESA); dividido por padrão popular/médio/alto pela
      housing_stock_split da nação (derivada do Gini de inequality.json).

  (c) posse (casa própria / aluguel / informal) por estrato de renda
      derivado da taxa nacional de casa própria (OECD AHD HM1.3, housing.json) com
      gradiente por estrato (diferença rico-pobre ~ 30 p.p., OECD HM1.3.A3) e do
      informal = população em favela (World Bank SDG 11.1.1, housing.json).

Tudo é proxy/derivado — nenhum valor é medição direta por cidade. Cada campo carrega
source/method/confidence; `estimated` é true em todo o arquivo. As 9 nações sem
gdp_ppp_bn (CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN, YEM) ficam com aluguel/preço
null (sem renda para o proxy), mas mantêm estoque e posse (que não dependem de renda).

Uso:
    python tools/build_housing_city.py            # escreve o JSON
    python tools/build_housing_city.py --check    # não escreve, só reporta cobertura
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from typing import Dict, Optional

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")
ECONOMY = os.path.join(ROOT, "data", "definitions", "economy")
CITIES_CSV = os.path.join(SCENARIO, "cities.csv")
NATIONS_CSV = os.path.join(SCENARIO, "nations.csv")
HOUSING_JSON = os.path.join(ECONOMY, "housing.json")
OUT_JSON = os.path.join(ECONOMY, "housing_city.json")

# Faixas de renda (PIB PPP per capita, USD) — mesmas de build_housing.py.
BAND_HIGH_MIN = 25000.0
BAND_UPPER_MID_MIN = 12000.0
BAND_LOWER_MID_MIN = 5000.0

# (a) âncoras do proxy de renda (estimativas de referência, NÃO medição por cidade).
# floor_m2_per_person: área de moradia per capita (m²/pessoa) por faixa de renda.
#   Referência: OECD Affordable Housing Database HC2.1 "Living space" (m²/pessoa
#   cresce com a renda; faixas: ~12 m²/pessoa em renda baixa até ~40 m²/pessoa em
#   renda alta) e UN-Habitat (padrão mínimo de moradia adequada). Estimativa de faixa.
FLOOR_M2_PER_PERSON = {
    "low": 12.0,
    "lower_mid": 16.0,
    "upper_mid": 24.0,
    "high": 38.0,
    "unknown": 12.0,
}
# price_to_rent_ratio: preço de compra em anos de aluguel (ordem publicada ~20–25
#   anos; OECD "house price-to-rent" / literatura). Constante de referência.
PRICE_TO_RENT_RATIO = 20.0

# (c) spread do gradiente de casa própria por estrato (OECD AHD HM1.3.A3: a
# diferença de homeownership entre o quintil mais rico e o mais pobre é ~25–35 p.p.;
# para 3 estratos, ±0,15 em torno da taxa nacional).
OWNERSHIP_SPREAD = 0.15

BAND_LABEL = {
    "low": "renda baixa (<5.000)",
    "lower_mid": "renda média-baixa (5.000-12.000)",
    "upper_mid": "renda média-alta (12.000-25.000)",
    "high": "renda alta (>25.000)",
    "unknown": "faixa desconhecida (sem PIB no nations.csv)",
}

SOURCES = {
    "oecd_ahd_hc1_2_costs": {
        "title": "OECD Affordable Housing Database — HC1.2 Housing costs over income "
                 "(median rent burden as share of disposable income)",
        "url": "https://webfs.oecd.org/els-com/Affordable_Housing_Database/"
               "HC1-2-Housing-costs-over-income.xlsx",
        "unit": "fração da renda disponível, 0..1",
    },
    "oecd_ahd_hc2_1_living_space": {
        "title": "OECD Affordable Housing Database — HC2.1 Living space "
                 "(floor space per person, m²/pessoa)",
        "url": "https://webfs.oecd.org/els-com/Affordable_Housing_Database/"
               "HC2-1-Living-space.pdf",
        "unit": "m²/pessoa (faixa de referência por renda)",
        "method": "valor de faixa por nível de renda; NÃO é medição da cidade",
    },
    "oecd_ahd_hm1_3_tenures": {
        "title": "OECD Affordable Housing Database — HM1.3 Housing tenures "
                 "(share of households by tenure type, incl. by income quintile HM1.3.A3)",
        "url": "https://webfs.oecd.org/els-com/Affordable_Housing_Database/"
               "HM1-3-Housing-tenures.xlsx",
        "unit": "share de domicílios, 0..1",
    },
    "worldbank_sdg_11_1_1": {
        "title": "World Bank — SDG 11.1.1: Population living in slums (EN.POP.SLUM.UR.ZS)",
        "url": "https://data.worldbank.org/indicator/EN.POP.SLUM.UR.ZS",
        "unit": "% da população urbana (proxy de posse informal)",
    },
    "un_desa_hh_size": {
        "title": "UN DESA, Population Division — Database on Household Size and "
                 "Composition 2022 (average household size per survey)",
        "url": "https://www.un.org/development/desa/pd/sites/www.un.org.development.desa.pd/"
               "files/undesa_pd_2022_hh-size-composition.xlsx",
        "unit": "moradores por domicílio",
    },
    "derived_gini_stock_split": {
        "title": "housing.json housing_stock_split — derivado do Gini de inequality.json "
                 "(lognormal, média = 2,0 cestas)",
        "url": "https://data.worldbank.org/indicator/SI.POV.GINI",
        "unit": "fração do estoque (popular/medium/high)",
        "method": "modelo reprodutível a partir do Gini; NÃO é medição de estoque",
    },
    "proxy_income": {
        "title": "proxy de renda por faixa (PIB PPP per capita, nations.csv)",
        "url": "https://data.worldbank.org/indicator/NY.GDP.PCAP.PP.CD",
        "unit": "USD PPP per capita/ano",
        "method": "renda da nação aplicada a todas as cidades da nação (sem dado "
                  "subnacional de renda no projeto)",
    },
}

METHOD = {
    "price_and_rent": (
        "Proxy de renda documentado (price_source='proxy_income', não é medição de "
        "mercado da cidade). rent_per_m2_usd (mensal) = rent_to_income_ratio x "
        "gdp_ppp_per_capita / (12 x floor_m2_per_person). price_per_m2_usd = "
        "rent_per_m2_usd x 12 x price_to_rent_ratio (20 anos). rent_to_income_ratio "
        "vem de housing.json (OECD AHD HC1.2 mediana do ônus de aluguel por nação ou "
        "proxy de faixa); gdp_ppp_per_capita = gdp_ppp_bn*1e9/population (nations.csv, "
        "a MESMA renda para todas as cidades da nação — limitação documentada, sem dado "
        "subnacional de renda); floor_m2_per_person por faixa (low 12 | lower_mid 16 | "
        "upper_mid 24 | high 38 m²/pessoa, referência OECD AHD HC2.1/UN-Habitat). "
        "Nações sem gdp_ppp_bn ficam com rent/price null (source='no_data')."
    ),
    "housing_stock": (
        "housing_stock_total = round(população da cidade / household_size da nação). "
        "housing_stock_by_tier = housing_stock_total x housing_stock_split da nação "
        "(housing.json, derivado do Gini de inequality.json); popular/medium/high em "
        "unidades de domicílio, medium ajustado para fechar a soma exata. A população "
        "é a de CENÁRIO (cities.csv), coerente com o que o núcleo consome."
    ),
    "tenure_by_tier": (
        "Posse (own/rent/informal) por estrato de renda (popular/medium/high), "
        "derivada — não é medição. Entradas: homeownership_rate (O, OECD HM1.3) e "
        "informal (I = slum_pct_urban/100, proxy de posse informal pelo SDG 11.1.1). "
        "1) informal: distribuído do estrato mais pobre para o mais rico até esgotar I. "
        "2) casa própria FORMAL com gradiente de renda: own_grad = clamp(O ± 0,15) "
        "(popular −0,15, high +0,15, medium fecha a média O; spread documentado via "
        "OECD HM1.3.A3). 3) own_tier = own_grad x (1 − informal_tier); rent_tier = "
        "(1 − own_grad) x (1 − informal_tier). Soma por estrato = 1. A taxa nacional O "
        "se aplica à parcela formal; o informal reduz a casa própria efetiva do estrato "
        "popular. Países de alta renda sem medição de favela têm informal = 0."
    ),
}

NOTE = (
    "Habitação POR CIDADE (834 do cenário), complementar ao housing.json (por nação). "
    "Três blocos, todos proxy/derivado (nenhum é medição direta por cidade, "
    "`estimated` = true em todo o arquivo): (a) aluguel e preço do m² residencial "
    "(proxy de renda documentado: rent burden OECD AHD HC1.2 x renda PIB PPP per "
    "capita / área de moradia per capita HC2.1; preço = aluguel x 20 anos de "
    "price-to-rent); (b) estoque habitacional absoluto (população / domicílio, "
    "dividido por popular/médio/alto pela split derivada do Gini de inequality.json); "
    "(c) posse por estrato de renda (casa própria OECD HM1.3 com gradiente de renda, "
    "informal = favela SDG 11.1.1, aluguel = resto). Não há fonte gratuita estável de "
    "preço por m² por cidade (Numbeo é scraping/ToS; Global Property Guide é "
    "parcialmente pago), por isso o preço é proxy, não medição. A renda é a da NAÇÃO "
    "(sem dado subnacional de renda no projeto). As 9 nações sem gdp_ppp_bn no "
    "nations.csv (CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN, YEM) ficam com aluguel/preço "
    "null (sem renda para o proxy), mantendo estoque e posse."
)


def clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, value))


def read_csv(path: str) -> list:
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def load_housing() -> Dict[str, dict]:
    with open(HOUSING_JSON, "r", encoding="utf-8") as handle:
        return json.load(handle).get("nations", {})


def income_band(nation: dict) -> str:
    """Faixa de PIB PPP per capita; 'unknown' quando o nations.csv não traz PIB."""
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


def stock_total(population: int, household_size: float) -> int:
    if household_size <= 0:
        return 0
    return max(1, int(round(population / household_size)))


def stock_by_tier(total: int, split: Dict[str, float]) -> Dict[str, int]:
    """Divide o estoque em popular/medium/high; medium fecha a soma exata."""
    popular = int(round(total * split.get("popular", 0.0)))
    high = int(round(total * split.get("high", 0.0)))
    medium = total - popular - high
    if medium < 0:  # proteção contra arredondamento extremo
        medium = 0
        high = total - popular
    return {"popular": popular, "medium": medium, "high": high}


def tenure_by_tier(own_rate: float, informal: float,
                   split: Dict[str, float]) -> Dict[str, Dict[str, float]]:
    """own/rent/informal por estrato (frações, soma 1 por estrato). Ver METHOD."""
    p = split.get("popular", 0.0)
    m = split.get("medium", 0.0)
    h = split.get("high", 0.0)

    # 1) informal distribuído do estrato mais pobre para o mais rico.
    informal_by = {"popular": 0.0, "medium": 0.0, "high": 0.0}
    remaining = informal
    for tier in ("popular", "medium", "high"):
        share = split.get(tier, 0.0)
        if share <= 0.0:
            continue
        informal_by[tier] = min(1.0, remaining / share)
        remaining -= informal_by[tier] * share
        if remaining <= 1e-12:
            remaining = 0.0
            break

    # 2) gradiente de casa própria formal.
    own_grad_popular = clamp(own_rate - OWNERSHIP_SPREAD, 0.0, 1.0)
    own_grad_high = clamp(own_rate + OWNERSHIP_SPREAD, 0.0, 1.0)
    if m > 0.0:
        own_grad_medium = clamp(
            (own_rate - p * own_grad_popular - h * own_grad_high) / m, 0.0, 1.0)
    else:
        own_grad_medium = own_rate
    own_grad = {"popular": own_grad_popular, "medium": own_grad_medium, "high": own_grad_high}

    # 3) compor own/rent sobre a parcela formal de cada estrato.
    result: Dict[str, Dict[str, float]] = {}
    for tier in ("popular", "medium", "high"):
        informal_tier = informal_by[tier]
        formal = 1.0 - informal_tier
        grad = own_grad[tier]
        own = round(grad * formal, 4)
        rent = round((1.0 - grad) * formal, 4)
        result[tier] = {"own": own, "rent": rent, "informal": round(informal_tier, 4)}
    return result


def build_city(city: dict, nation: dict, housing: dict, band: str) -> dict:
    code = city["id"]
    name = city["name"]
    nation_code = city["nation"]
    population = int(city["population"])

    hh = housing or {}
    household_size = float(hh.get("household_size") or 0.0)
    own_rate = float(hh.get("homeownership_rate") or 0.0)
    rent_ratio = hh.get("rent_to_income_ratio")
    slum = hh.get("slum_pct_urban")
    split = hh.get("housing_stock_split") or {"popular": 0.0, "medium": 1.0, "high": 0.0}

    informal = (float(slum) / 100.0) if isinstance(slum, (int, float)) else 0.0

    total = stock_total(population, household_size)
    tiers = stock_by_tier(total, split)
    tenure = tenure_by_tier(own_rate, informal, split)

    per_capita = gdp_per_capita(nation)
    floor_m2 = FLOOR_M2_PER_PERSON[band]

    refs = [
        {
            "title": ("OECD AHD HC2.1 Living space — floor space per person: faixa de "
                      "referência %s (%.0f m²/pessoa) — %s"
                      % (BAND_LABEL[band], floor_m2, nation["name"])),
            "url": SOURCES["oecd_ahd_hc2_1_living_space"]["url"],
            "year": 2025,
        },
        {
            "title": ("OECD AHD HM1.3 Housing tenures — casa própria e gradiente por "
                      "renda (HM1.3.A3) — %s" % nation["name"]),
            "url": SOURCES["oecd_ahd_hm1_3_tenures"]["url"],
            "year": 2024,
        },
        {
            "title": ("housing.json — estoque por padrão derivado do Gini de "
                      "inequality.json (split %.4f/%.4f/%.4f) — %s"
                      % (split.get("popular", 0.0), split.get("medium", 0.0),
                         split.get("high", 0.0), nation["name"])),
            "url": SOURCES["derived_gini_stock_split"]["url"],
            "year": 2025,
        },
    ]

    if per_capita is not None and isinstance(rent_ratio, (int, float)):
        rent_per_m2 = round(float(rent_ratio) * per_capita / (12.0 * floor_m2), 2)
        price_per_m2 = round(rent_per_m2 * 12.0 * PRICE_TO_RENT_RATIO, 2)
        price_source = "proxy_income"
        confidence = "estimated"
        refs.append({
            "title": ("OECD AHD HC1.2 Housing costs over income — mediana do ônus de "
                      "aluguel %s (proxy de renda) — %s"
                      % (rent_ratio, nation["name"])),
            "url": SOURCES["oecd_ahd_hc1_2_costs"]["url"],
            "year": 2024,
        })
    else:
        rent_per_m2 = None
        price_per_m2 = None
        price_source = "no_data"
        confidence = "partial"

    entry = {
        "name": name,
        "nation": nation_code,
        "population": population,
        "household_size": household_size,
        "household_size_source": hh.get("household_size_source", "no_data"),
        "housing_stock_total": total,
        "housing_stock_by_tier": tiers,
        "stock_split_source": "derived:lognormal_from_gini",
        "tenure_by_tier": tenure,
        "tenure_source": "derived:homeownership+slum",
        "homeownership_rate": round(own_rate, 4),
        "informal_share": round(informal, 4),
        "rent_per_m2_usd": rent_per_m2,
        "price_per_m2_usd": price_per_m2,
        "price_to_rent_ratio": PRICE_TO_RENT_RATIO,
        "floor_m2_per_person": floor_m2,
        "rent_to_income_ratio": round(float(rent_ratio), 4) if isinstance(rent_ratio, (int, float)) else None,
        "gdp_ppp_per_capita_usd": round(per_capita, 1) if per_capita is not None else None,
        "price_source": price_source,
        "estimated": True,
        "confidence": confidence,
        "refs": refs,
    }
    if confidence == "partial":
        entry["method"] = (
            "Estoque e posse derivados (população/domicílio, Gini e taxa nacional de "
            "casa própria + favela); aluguel/preço null: nação sem gdp_ppp_bn no "
            "nations.csv (renda indeterminada para o proxy de renda)."
        )
    else:
        entry["method"] = (
            "Proxy de renda documentado para aluguel/preço (rent burden OECD AHD HC1.2 "
            "x PIB PPP per capita / área HC2.1; preço = aluguel x 20 anos); estoque = "
            "população/domicílio dividido pela split do Gini; posse = taxa nacional de "
            "casa própria com gradiente de renda + informal (favela)."
        )
    return entry


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Gera housing_city.json (habitação por cidade).")
    parser.add_argument("--check", action="store_true", help="não escreve o JSON")
    args = parser.parse_args(argv)

    cities = read_csv(CITIES_CSV)
    nations = {row["id"]: row for row in read_csv(NATIONS_CSV)}
    housing = load_housing()

    entries: Dict[str, dict] = {}
    for city in cities:
        nation = nations.get(city["nation"])
        if nation is None:
            raise SystemExit("nação %r da cidade %s ausente em nations.csv"
                             % (city["nation"], city["id"]))
        band = income_band(nation)
        entries[city["id"]] = build_city(city, nation, housing.get(city["nation"], {}), band)

    with_price = sum(1 for e in entries.values() if e["price_per_m2_usd"] is not None)
    no_price = len(entries) - with_price

    payload = {
        "version": 1,
        "note": NOTE,
        "units": {
            "rent_per_m2_usd": "USD/m²/mês",
            "price_per_m2_usd": "USD/m²",
            "housing_stock_total": "domicílios",
            "tenure": "fração (0..1) por estrato",
        },
        "coverage": {
            "cities_total": len(entries),
            "cities_with_price_rent": with_price,
            "cities_price_no_data": no_price,
        },
        "sources": SOURCES,
        "method": METHOD,
        "cities": entries,
    }

    print("cidades: %d | com aluguel/preço (proxy de renda): %d | sem preço (sem PIB): %d"
          % (len(entries), with_price, no_price))

    if args.check:
        print("--check: JSON não foi escrito")
        return 0

    os.makedirs(os.path.dirname(OUT_JSON), exist_ok=True)
    with open(OUT_JSON, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    print("escrito:", os.path.relpath(OUT_JSON, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
