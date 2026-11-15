#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gera data/definitions/economy/inequality.json com o Gini REAL de cada nação do jogo.

Fontes:

  World Bank  SI.POV.GINI      Gini index (0..100)          2000..2025  (fonte única desta versão)
  World Bank  SI.DST.*         quintis/decis de renda       2000..2025  (só quando o país tem dado)
  Estimativa regional          faixa típica WB PIP/OECD IDD  (NÃO é medição; marcada como tal)

Regras:

  * Nenhum número é inventado: cada nação recebe o valor observado mais recente do
    World Bank ou uma estimativa regional explicitamente marcada (`estimated: true`,
    `method`, `confidence: "estimated"`).
  * Para cada nação usa-se o ANO MAIS RECENTE com valor não nulo.
  * NED e PSX não existem no World Bank: NED = código alternativo de NLD (Países
    Baixos) e PSX = PSX/PSE (Cisjordânia e Gaza). Os dois são resolvidos por
    `ISO_ALIASES` e a entrada registra `iso_worldbank`. Nenhum outro código é
    remapeado — em especial SOL é Somalilândia, NÃO Ilhas Salomão.
  * Quintis/decis vêm do mesmo ano do Gini quando existem; senão do ano mais recente
    disponível para aquele indicador (o campo `year` de cada entrada diz qual é).
  * Chamadas HTTP ficam em cache cru em tools/data/inequality_cache/ (gitignored).

Uso:
    python tools/build_inequality.py             # usa cache; baixa o que faltar
    python tools/build_inequality.py --refresh   # ignora o cache
    python tools/build_inequality.py --check     # não escreve, só reporta cobertura
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
import time
import urllib.request
from typing import Dict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "scenario", "nations.csv")
OUT_JSON = os.path.join(ROOT, "data", "definitions", "economy", "inequality.json")
CACHE = os.path.join(ROOT, "tools", "data", "inequality_cache")

API = "https://api.worldbank.org/v2/country/all/indicator/%s?format=json&per_page=30000&date=2000:2025"
COUNTRY_API = "https://api.worldbank.org/v2/country/%s/indicator/%s?format=json&date=%d"
USER_AGENT = "opengs-game/inequality-builder (python urllib)"

GINI_INDICATOR = "SI.POV.GINI"

# Chave no JSON -> indicador World Bank ("Income share held by ...")
DIST_INDICATORS = {
    "lowest_10": "SI.DST.FRST.10",   # 10% mais pobres
    "lowest_20": "SI.DST.FRST.20",   # 20% mais pobres
    "second_20": "SI.DST.02ND.20",
    "third_20": "SI.DST.03RD.20",
    "fourth_20": "SI.DST.04TH.20",
    "highest_20": "SI.DST.05TH.20",  # 20% mais ricos
    "highest_10": "SI.DST.10TH.10",  # 10% mais ricos
}

# Códigos do nations.csv que não existem (ou existem com outro nome) no World Bank.
ISO_ALIASES = {
    "NED": ("NLD", "nations.csv usa NED (código alternativo); World Bank usa NLD"),
    "PSX": ("PSE", "World Bank publica PSX como PSE (Cisjordânia e Gaza)"),
}

# Faixa típica regional (0..100). NÃO é medição de país: é o valor aplicado quando
# não existe Gini observado para a nação.
REGION_ESTIMATES = {
    "africa": 43.0,
    "asia": 38.0,
    "europe": 33.0,
    "middle_east": 39.0,
    "north_america": 42.0,
    "oceania": 36.0,
    "south_america": 47.0,
}

REGION_LABEL = {
    "africa": "África",
    "asia": "Ásia",
    "europe": "Europa",
    "middle_east": "Oriente Médio",
    "north_america": "América do Norte e Caribe",
    "oceania": "Oceania",
    "south_america": "América do Sul",
}

SOURCES = {
    "worldbank_si_pov_gini": {
        "title": "World Bank — Gini index (SI.POV.GINI)",
        "url": "https://data.worldbank.org/indicator/SI.POV.GINI",
        "unit": "0..100",
    },
    "worldbank_si_dst": {
        "title": "World Bank — Income share by quintile/decile (SI.DST.FRST.10, FRST.20, 02ND.20, 03RD.20, 04TH.20, 05TH.20, 10TH.10)",
        "url": "https://data.worldbank.org/indicator/SI.DST.FRST.20",
        "unit": "% da renda total",
    },
    "oecd_idd": {
        "title": "OECD Income Distribution Database",
        "url": "https://www.oecd.org/en/data/datasets/income-distribution-database.html",
        "unit": "0..1 (convertida para 0..100)",
    },
    "swiid": {
        "title": "Standardized World Income Inequality Database",
        "url": "https://fsolt.org/swiid/",
        "unit": "0..100",
    },
    "national_statistics": {
        "title": "instituto nacional de estatística (ver refs por nação)",
        "unit": "0..100",
    },
    "regional_estimate": {
        "title": "estimativa regional (faixa típica World Bank PIP / OECD IDD)",
        "unit": "0..100",
        "method": "valor regional fixo aplicado quando não há Gini observado; NÃO é medição da nação",
    },
}

NOTE = (
    "Gini por nação. Fonte primária: World Bank SI.POV.GINI (ano mais recente com valor "
    "não nulo entre 2000 e 2025); nações sem observação recebem estimativa regional "
    "explicitamente marcada (estimated=true) — nunca um valor apresentado como medição. "
    "NED e PSX foram resolvidos pelos códigos World Bank NLD e PSE (campo iso_worldbank). "
    "Nenhuma fonte alternativa (OECD IDD, SWIID, instituto nacional) forneceu valor "
    "verificável nesta versão, por isso essas chaves existem mas não são usadas. "
    "Em entradas estimated, refs[].year é o último ano com dado observado na região "
    "(referência da faixa), não uma medição da nação."
)


def fetch_indicator(indicator: str, refresh: bool = False) -> list:
    """Baixa (ou lê do cache) uma série inteira do World Bank."""
    path = os.path.join(CACHE, "wb_%s.json" % indicator.lower())
    payload = None
    if not refresh and os.path.exists(path):
        with open(path, "r", encoding="utf-8") as handle:
            payload = json.load(handle)
    if payload is None:
        last_error = None
        for attempt in range(3):
            try:
                request = urllib.request.Request(
                    API % indicator,
                    headers={"User-Agent": USER_AGENT, "Accept": "application/json"},
                )
                with urllib.request.urlopen(request, timeout=180) as response:
                    payload = json.loads(response.read().decode("utf-8"))
                break
            except Exception as exc:  # rede instável: tenta de novo
                last_error = exc
                time.sleep(3 * (attempt + 1))
        if payload is None:
            raise SystemExit("falha ao baixar %s: %s" % (indicator, last_error))
        os.makedirs(CACHE, exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle)

    if not isinstance(payload, list) or len(payload) < 2 or not payload[1]:
        raise SystemExit("resposta inesperada do World Bank para %s" % indicator)
    return payload[1]


def index_series(rows: list) -> Dict[str, Dict[int, float]]:
    """indexa {codigo_pais: {ano: valor}} descartando nulos."""
    series: Dict[str, Dict[int, float]] = {}
    for row in rows:
        value = row.get("value")
        if value is None:
            continue
        code = (row.get("countryiso3code") or "").strip().upper()
        if not code:
            continue
        series.setdefault(code, {})[int(row["date"])] = float(value)
    return series


def dist_source_name(indicator: str) -> str:
    return "worldbank_" + indicator.lower().replace(".", "_")


def build_distribution(wb_code: str, dist_index: dict, gini_year) -> dict:
    """Quintis/decis do World Bank: mesmo ano do Gini quando existir, senão o mais recente."""
    out = {}
    for key, indicator in DIST_INDICATORS.items():
        years = dist_index.get(indicator, {}).get(wb_code)
        if not years:
            continue
        if gini_year is not None and gini_year in years:
            year = gini_year
        else:
            year = max(years)
        out[key] = {
            "value": round(years[year], 1),
            "year": year,
            "source": dist_source_name(indicator),
        }
    return out


def build_nation(nation: dict, gini_index: dict, dist_index: dict,
                 region_ref_year: dict) -> dict:
    code = nation["id"]
    wb_code, alias_note = ISO_ALIASES.get(code, (code, ""))
    years = gini_index.get(wb_code, {})
    entry: dict = {}

    if years:
        year = max(years)
        entry["gini"] = round(years[year], 1)
        entry["gini_year"] = year
        entry["source"] = "worldbank_si_pov_gini"
        entry["estimated"] = False
        entry["confidence"] = "complete" if year >= 2015 else "partial"
        if alias_note:
            entry["iso_worldbank"] = wb_code
            entry["note"] = alias_note
        entry["refs"] = [{
            "title": "World Bank SI.POV.GINI — %s (%d)" % (nation["name"], year),
            "url": COUNTRY_API % (wb_code, GINI_INDICATOR, year),
            "year": year,
        }]
        distribution = build_distribution(wb_code, dist_index, year)
        if distribution:
            entry["deciles"] = distribution
        return entry

    region = nation.get("region", "")
    if region not in REGION_ESTIMATES:
        entry["gini"] = None
        entry["gini_year"] = None
        entry["source"] = None
        entry["estimated"] = False
        entry["confidence"] = "no_data"
        entry["no_data"] = True
        entry["reason"] = "sem Gini observado e sem faixa regional definida para '%s'" % region
        entry["refs"] = []
        return entry

    value = REGION_ESTIMATES[region]
    entry["gini"] = value
    entry["gini_year"] = None
    entry["source"] = "regional_estimate"
    entry["estimated"] = True
    entry["confidence"] = "estimated"
    entry["method"] = (
        "regional_estimate: %s = %g (faixa típica World Bank PIP/OECD IDD, não é medição)"
        % (region, value)
    )
    entry["refs"] = [{
        "title": ("World Bank PIP — Gini index: faixa de referência de %s "
                  "(estimativa regional, não medição da nação)" % REGION_LABEL.get(region, region)),
        "url": "https://data.worldbank.org/indicator/SI.POV.GINI",
        "year": region_ref_year.get(region),
    }]
    return entry


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Gera inequality.json a partir do World Bank.")
    parser.add_argument("--refresh", action="store_true", help="ignora o cache local")
    parser.add_argument("--check", action="store_true", help="não escreve o JSON")
    args = parser.parse_args(argv)

    with open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        nations = list(csv.DictReader(handle))

    gini_index = index_series(fetch_indicator(GINI_INDICATOR, args.refresh))
    dist_index = {}
    for indicator in DIST_INDICATORS.values():
        dist_index[indicator] = index_series(fetch_indicator(indicator, args.refresh))

    observed_by_region: Dict[str, int] = {}
    for nation in nations:
        wb_code = ISO_ALIASES.get(nation["id"], (nation["id"], ""))[0]
        years = gini_index.get(wb_code, {})
        if years:
            region = nation.get("region", "")
            observed_by_region[region] = max(observed_by_region.get(region, 0), max(years))

    entries = {}
    for nation in nations:
        entries[nation["id"]] = build_nation(nation, gini_index, dist_index, observed_by_region)

    observed = sum(1 for e in entries.values() if not e["estimated"] and e["gini"] is not None)
    estimated = sum(1 for e in entries.values() if e["estimated"])
    without = len(entries) - observed - estimated

    payload = {
        "version": 1,
        "note": NOTE,
        "gini_unit": "index 0..100 (escala nativa World Bank SI.POV.GINI)",
        "coverage": {
            "nations_total": len(entries),
            "nations_with_observed_gini": observed,
            "nations_with_estimate": estimated,
        },
        "sources": SOURCES,
        "nations": entries,
    }

    print("nações: %d | observados: %d | estimados: %d | sem dado: %d"
          % (len(entries), observed, estimated, without))
    if without:
        print("AVISO: %d nação(ões) sem dado nem estimativa" % without)

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
