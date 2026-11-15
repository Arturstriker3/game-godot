#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida data/definitions/economy/inequality.json contra data/definitions/scenario/nations.csv.

Checa:
  1. o JSON cobre exatamente as nações do nations.csv (nem uma a mais, nem a menos);
  2. todo `gini` numérico está em [0, 100] (ou é null com `no_data` + `reason`);
  3. toda nação com Gini numérico tem `source` conhecido e `refs` não-vazio com
     title/url/year;
  4. `estimated: true` <-> `source == regional_estimate` + `method` não-vazio +
     `confidence == "estimated"`; `estimated: false` -> fonte observada e
     `confidence` coerente com o ano (complete >= 2015, partial < 2015);
  5. quintis/decis, quando presentes, têm value/year/source e faixa plausível;
  6. o bloco `coverage` bate com a contagem recalculada.

Uso:
    python tools/validate_inequality.py

Retorna 0 quando está tudo consistente, 1 (com a lista de problemas) caso contrário.
"""

from __future__ import annotations

import csv
import json
import os
import sys
from typing import Dict, List

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "scenario", "nations.csv")
INEQUALITY = os.path.join(ROOT, "data", "definitions", "economy", "inequality.json")

CONFIDENCE_YEAR = 2015

OBSERVED_SOURCES = {
    "worldbank_si_pov_gini",
    "oecd_idd",
    "swiid",
    "national_statistics",
}
ESTIMATE_SOURCE = "regional_estimate"
KNOWN_SOURCES = OBSERVED_SOURCES | {ESTIMATE_SOURCE}

# Fontes "guarda-chuva": não têm uma URL única (a referência real fica em refs por nação)
NO_URL_SOURCES = {"national_statistics", ESTIMATE_SOURCE}

DECILE_KEYS = {"lowest_10", "lowest_20", "second_20", "third_20", "fourth_20",
               "highest_20", "highest_10"}
QUINTILE_KEYS = {"lowest_20", "second_20", "third_20", "fourth_20", "highest_20"}

SAMPLES = ("USA", "DEU", "BRA", "CHN", "IND", "NGA", "ZAF", "NOR", "SAU", "RUS")


def load_nations() -> Dict[str, dict]:
    with open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        return {row["id"]: row for row in csv.DictReader(handle)}


def load_payload() -> dict:
    with open(INEQUALITY, "r", encoding="utf-8") as handle:
        return json.load(handle)


def check_top_level(payload: dict, problems: List[str]) -> List[str]:
    """Valida o cabeçalho e devolve a lista de fontes declaradas."""
    if payload.get("version") != 1:
        problems.append("version deveria ser 1, veio %r" % payload.get("version"))
    if not isinstance(payload.get("gini_unit"), str) or "0..100" not in payload["gini_unit"]:
        problems.append("gini_unit deveria declarar a escala 0..100")
    if not isinstance(payload.get("note"), str) or not payload["note"].strip():
        problems.append("note ausente ou vazia")
    sources = payload.get("sources")
    if not isinstance(sources, dict):
        problems.append("bloco sources ausente")
        return []
    missing = sorted(KNOWN_SOURCES - set(sources))
    if missing:
        problems.append("sources sem as fontes esperadas: %s" % ", ".join(missing))
    for name in sorted(KNOWN_SOURCES):
        entry = sources.get(name)
        if not isinstance(entry, dict):
            problems.append("sources.%s não é objeto" % name)
            continue
        if not entry.get("title"):
            problems.append("sources.%s sem title" % name)
        url = entry.get("url")
        if name in NO_URL_SOURCES:
            if url is not None and not str(url).startswith("http"):
                problems.append("sources.%s.url inválida: %r" % (name, url))
        elif not isinstance(url, str) or not url.startswith("http"):
            problems.append("sources.%s sem url http(s)" % name)
    estimate = sources.get(ESTIMATE_SOURCE, {})
    if isinstance(estimate, dict) and not estimate.get("method"):
        problems.append("sources.%s.method ausente" % ESTIMATE_SOURCE)
    return sorted(sources)


def check_refs(entry: dict, code: str, problems: List[str]) -> None:
    refs = entry.get("refs")
    if not isinstance(refs, list) or not refs:
        problems.append("%s: refs vazio ou ausente" % code)
        return
    for position, ref in enumerate(refs):
        if not isinstance(ref, dict):
            problems.append("%s: refs[%d] não é objeto" % (code, position))
            continue
        for field in ("title", "url", "year"):
            if ref.get(field) in (None, ""):
                problems.append("%s: refs[%d].%s ausente" % (code, position, field))
        url = ref.get("url")
        if isinstance(url, str) and not url.startswith("http"):
            problems.append("%s: refs[%d].url não é http(s): %r" % (code, position, url))
        year = ref.get("year")
        if year is not None and not isinstance(year, int):
            problems.append("%s: refs[%d].year não é inteiro: %r" % (code, position, year))


def check_deciles(entry: dict, code: str, problems: List[str]) -> None:
    deciles = entry.get("deciles")
    if deciles is None:
        return
    if not isinstance(deciles, dict) or not deciles:
        problems.append("%s: deciles deveria ser objeto não-vazio" % code)
        return
    unknown = sorted(set(deciles) - DECILE_KEYS)
    if unknown:
        problems.append("%s: deciles com chaves desconhecidas: %s" % (code, ", ".join(unknown)))
    for key, item in deciles.items():
        if not isinstance(item, dict):
            problems.append("%s: deciles.%s não é objeto" % (code, key))
            continue
        value = item.get("value")
        if not isinstance(value, (int, float)) or not (0.0 <= float(value) <= 100.0):
            problems.append("%s: deciles.%s.value fora de [0,100]: %r" % (code, key, value))
        if not isinstance(item.get("year"), int):
            problems.append("%s: deciles.%s.year ausente/não inteiro" % (code, key))
        if not item.get("source"):
            problems.append("%s: deciles.%s.source ausente" % (code, key))
    if QUINTILE_KEYS <= set(deciles):
        total = sum(float(deciles[key]["value"]) for key in QUINTILE_KEYS
                    if isinstance(deciles.get(key, {}).get("value"), (int, float)))
        if abs(total - 100.0) > 2.0:
            problems.append("%s: quintis somam %.1f%% (esperado ~100)" % (code, total))


def check_entry(entry: dict, code: str, problems: List[str], current_year: int) -> str:
    """Valida uma nação e devolve o tipo: observed | estimate | no_data | invalid."""
    if not isinstance(entry, dict):
        problems.append("%s: entrada não é objeto" % code)
        return "invalid"

    gini = entry.get("gini")
    estimated = entry.get("estimated")
    source = entry.get("source")
    confidence = entry.get("confidence")

    if not isinstance(estimated, bool):
        problems.append("%s: estimated ausente/não booleano" % code)

    if gini is None:
        if entry.get("no_data") is not True:
            problems.append("%s: gini null sem no_data=true" % code)
        if not entry.get("reason"):
            problems.append("%s: gini null sem reason" % code)
        if entry.get("refs"):
            check_refs(entry, code, problems)
        return "no_data"

    if not isinstance(gini, (int, float)):
        problems.append("%s: gini não numérico: %r" % (code, gini))
        return "invalid"
    if not (0.0 <= float(gini) <= 100.0):
        problems.append("%s: gini fora de [0,100]: %r" % (code, gini))

    if source not in KNOWN_SOURCES:
        problems.append("%s: source desconhecida: %r" % (code, source))

    check_refs(entry, code, problems)
    check_deciles(entry, code, problems)

    year = entry.get("gini_year")
    if estimated:
        if source != ESTIMATE_SOURCE:
            problems.append("%s: estimated=true com source %r (esperado %r)"
                            % (code, source, ESTIMATE_SOURCE))
        if not entry.get("method"):
            problems.append("%s: estimated=true sem method" % code)
        if confidence != "estimated":
            problems.append("%s: estimated=true com confidence %r (esperado 'estimated')"
                            % (code, confidence))
        if year is not None:
            problems.append("%s: estimated=true não deveria ter gini_year (%r)" % (code, year))
        return "estimate"

    if source not in OBSERVED_SOURCES:
        problems.append("%s: estimated=false com source %r" % (code, source))
    if entry.get("method"):
        problems.append("%s: estimated=false não deveria ter method" % code)
    if not isinstance(year, int):
        problems.append("%s: gini_year ausente/não inteiro" % code)
    else:
        if year > current_year:
            problems.append("%s: gini_year no futuro: %d" % (code, year))
        expected = "complete" if year >= CONFIDENCE_YEAR else "partial"
        if confidence != expected:
            problems.append("%s: confidence %r != %r para ano %d"
                            % (code, confidence, expected, year))
    return "observed"


def main() -> int:
    import datetime

    nations = load_nations()
    payload = load_payload()
    problems: List[str] = []
    current_year = datetime.date.today().year

    check_top_level(payload, problems)

    entries = payload.get("nations")
    if not isinstance(entries, dict):
        print("ERRO: bloco nations ausente")
        return 1

    extra = sorted(set(entries) - set(nations))
    missing = sorted(set(nations) - set(entries))
    if extra:
        problems.append("nações no JSON que não estão no nations.csv: %s" % ", ".join(extra))
    if missing:
        problems.append("nações do nations.csv ausentes no JSON: %s" % ", ".join(missing))

    counts = {"observed": 0, "estimate": 0, "no_data": 0, "invalid": 0}
    by_region_estimate: Dict[str, int] = {}
    for code in sorted(nations):
        entry = entries.get(code)
        if entry is None:
            counts["invalid"] += 1
            continue
        kind = check_entry(entry, code, problems, current_year)
        counts[kind] += 1
        if kind == "estimate":
            region = nations[code].get("region", "?")
            by_region_estimate[region] = by_region_estimate.get(region, 0) + 1

    coverage = payload.get("coverage") or {}
    recomputed = {
        "nations_total": len(nations),
        "nations_with_observed_gini": counts["observed"],
        "nations_with_estimate": counts["estimate"],
    }
    for field, value in recomputed.items():
        declared = coverage.get(field)
        if declared != value:
            problems.append("coverage.%s = %r, recalculado = %r" % (field, declared, value))

    print("nações no nations.csv: %d | entradas no JSON: %d" % (len(nations), len(entries)))
    print("observados: %d | estimados: %d | sem dado: %d"
          % (counts["observed"], counts["estimate"], counts["no_data"]))
    if by_region_estimate:
        print("estimativas por região: %s" % ", ".join(
            "%s=%d" % (region, by_region_estimate[region]) for region in sorted(by_region_estimate)))
    print("amostra:")
    for code in SAMPLES:
        entry = entries.get(code) or {}
        gini = entry.get("gini")
        year = entry.get("gini_year")
        print("  %-4s gini=%s ano=%s fonte=%-26s confiança=%s" % (
            code, gini, year if year is not None else "-",
            entry.get("source"), entry.get("confidence")))

    if problems:
        print()
        print("PROBLEMAS (%d):" % len(problems))
        for problem in problems:
            print(" -", problem)
        return 1

    print()
    print("OK: %d nações, %d observados, %d estimados"
          % (len(nations), counts["observed"], counts["estimate"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
