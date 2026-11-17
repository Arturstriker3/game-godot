#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida data/definitions/economy/housing.json contra nations.csv e inequality.json.

Checa:

  1. o JSON cobre exatamente as 179 nações do nations.csv (nem uma a mais, nem a menos);
  2. homeownership_rate em [0,1] (ou null), slum_pct_urban em [0,100] (ou null apenas com
     slum_source="no_data"), household_size em [1,15] (ou null), rent_to_income_ratio em
     [0,1] (ou null), com `source`/`year` coerentes em cada campo;
  3. housing_stock_split tem as três chaves, cada uma em [0,1], soma 1,0 (tol. 0,01) e
     RECALCULA igual ao modelo lognormal do Gini de inequality.json (tol. 0,002) — garante
     que o estoque é derivado e coerente com a distribuição de renda do jogo;
  4. entrada observada (estimated=false) exige homeownership_source observada (oecd /
     national_census / un_habitat) e refs com url http(s) + year inteiro;
  5. entrada estimada (estimated=true) exige homeownership_source="regional_proxy",
     confidence="estimated" e method/nota;
  6. quando um campo veio de proxy de faixa, o valor bate com a tabela de proxy e o campo
     aparece em proxy_fields (nenhum proxy disfarçado de medição);
  7. o bloco `coverage` bate com a contagem recalculada.

Uso:
    python tools/validate_housing.py

Retorna 0 quando está tudo consistente, 1 (com a lista de problemas) caso contrário.
"""

from __future__ import annotations

import csv
import datetime
import json
import math
import os
import sys
from typing import Dict, List, Optional

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "scenario", "nations.csv")
INEQUALITY_JSON = os.path.join(ROOT, "data", "definitions", "economy", "inequality.json")
HOUSING_JSON = os.path.join(ROOT, "data", "definitions", "economy", "housing.json")

# Mesmas faixas de proxy do gerador (o validador confere se o valor emitido é o da faixa).
HOMEOWNERSHIP_PROXY = {
    "low": 0.78, "lower_mid": 0.72, "upper_mid": 0.66, "high": 0.62, "unknown": 0.78,
}
RENT_PROXY = {
    "low": 0.35, "lower_mid": 0.32, "upper_mid": 0.28, "high": 0.25, "unknown": 0.35,
}
HOUSEHOLD_SIZE_PROXY = {
    "low": 5.0, "lower_mid": 4.2, "upper_mid": 3.2, "high": 2.3, "unknown": 5.0,
}

BANDS = set(HOMEOWNERSHIP_PROXY)

HOMEOWNERSHIP_SOURCES = {"oecd", "un_habitat", "national_census", "regional_proxy"}
RENT_SOURCES = {"oecd", "regional_proxy"}
SLUM_SOURCES = {"worldbank_sdg_11_1_1", "no_data"}
HOUSEHOLD_SOURCES = {"un_desa", "regional_proxy"}
CONFIDENCE = {"complete", "partial", "estimated", "no_data"}
REQUIRED_SOURCES = {
    "oecd_ahd_hm1_3_tenures", "oecd_egdna_hh_ownership", "national_census_jpn",
    "oecd_ahd_hc1_2_costs", "un_desa_hh_size", "worldbank_sdg_11_1_1",
    "regional_proxy_band", "derived_lognormal",
}
REQUIRED_METHOD = {"homeownership", "slum", "household_size", "rent_to_income",
                   "housing_stock_split"}
STOCK_KEYS = {"popular", "medium", "high"}

SAMPLES = ("USA", "DEU", "BRA", "CHN", "IND", "NGA", "ZAF", "JPN", "CHE", "TWN", "SGP", "HND")


def load_nations() -> Dict[str, dict]:
    with open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        return {row["id"]: row for row in csv.DictReader(handle)}


def load_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def sigma_from_gini(gini_pct: float) -> float:
    """sigma tal que erf(sigma/2) = gini (bisseção — independente da erfinv do gerador)."""
    gini = max(0.0, min(0.999999, gini_pct / 100.0))
    low, high = 0.0, 6.0
    for _ in range(80):
        mid = (low + high) / 2.0
        if math.erf(mid / 2.0) < gini:
            low = mid
        else:
            high = mid
    return (low + high) / 2.0


def expected_split(gini_pct: float, basic: float = 1.0, luxury: float = 7.0,
                   mean_baskets: float = 2.0) -> Dict[str, float]:
    sigma = sigma_from_gini(gini_pct)
    if sigma <= 1e-9:
        return {"popular": 0.0, "medium": 1.0, "high": 0.0}
    mu = math.log(mean_baskets) - sigma * sigma / 2.0
    phi = lambda z: 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))
    popular = phi((math.log(basic) - mu) / sigma)
    high = 1.0 - phi((math.log(luxury) - mu) / sigma)
    return {"popular": popular, "medium": 1.0 - popular - high, "high": high}


def check_top_level(payload: dict, problems: List[str]) -> None:
    if payload.get("version") != 1:
        problems.append("version deveria ser 1, veio %r" % payload.get("version"))
    if not isinstance(payload.get("note"), str) or not payload["note"].strip():
        problems.append("note ausente ou vazia")

    sources = payload.get("sources")
    if not isinstance(sources, dict):
        problems.append("bloco sources ausente")
    else:
        missing = sorted(REQUIRED_SOURCES - set(sources))
        if missing:
            problems.append("sources sem as fontes esperadas: %s" % ", ".join(missing))
        for name in sorted(set(sources) & REQUIRED_SOURCES):
            entry = sources.get(name) or {}
            if not entry.get("title"):
                problems.append("sources.%s sem title" % name)
            url = entry.get("url")
            if name in ("regional_proxy_band",):
                if url is not None and not str(url).startswith("http"):
                    problems.append("sources.%s.url inválida: %r" % (name, url))
            elif not isinstance(url, str) or not url.startswith("http"):
                problems.append("sources.%s sem url http(s)" % name)
            if name in ("regional_proxy_band", "derived_lognormal") and not entry.get("method"):
                problems.append("sources.%s.method ausente" % name)

    method = payload.get("method")
    if not isinstance(method, dict):
        problems.append("bloco method ausente")
    else:
        missing = sorted(REQUIRED_METHOD - set(method))
        if missing:
            problems.append("method sem as chaves esperadas: %s" % ", ".join(missing))
        for name in sorted(set(method) & REQUIRED_METHOD):
            text = method.get(name)
            if not isinstance(text, str) or len(text.strip()) < 40:
                problems.append("method.%s ausente ou curto demais" % name)


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
        year = ref.get("year")
        if year is not None and not isinstance(year, int):
            problems.append("%s: refs[%d].year não é inteiro: %r" % (code, position, year))


def is_number(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def check_entry(entry: dict, code: str, gini_by_code: Dict[str, Optional[float]],
                problems: List[str], current_year: int) -> str:
    """Valida uma nação e devolve o tipo: observed | partial | estimate | invalid."""
    if not isinstance(entry, dict):
        problems.append("%s: entrada não é objeto" % code)
        return "invalid"

    band = entry.get("band")
    if band not in BANDS:
        problems.append("%s: band desconhecida: %r" % (code, band))

    estimated = entry.get("estimated")
    confidence = entry.get("confidence")
    if not isinstance(estimated, bool):
        problems.append("%s: estimated ausente/não booleano" % code)
    if confidence not in CONFIDENCE:
        problems.append("%s: confidence inválida: %r" % (code, confidence))

    # homeownership
    home = entry.get("homeownership_rate")
    home_source = entry.get("homeownership_source")
    home_year = entry.get("homeownership_year")
    if home_source not in HOMEOWNERSHIP_SOURCES:
        problems.append("%s: homeownership_source desconhecida: %r" % (code, home_source))
    if home is None:
        if home_source != "regional_proxy":
            problems.append("%s: homeownership_rate null com fonte %r" % (code, home_source))
    elif not is_number(home):
        problems.append("%s: homeownership_rate não numérica: %r" % (code, home))
    elif not (0.0 <= float(home) <= 1.0):
        problems.append("%s: homeownership_rate fora de [0,1]: %r" % (code, home))
    if home_source == "regional_proxy":
        expected = HOMEOWNERSHIP_PROXY.get(band)
        if is_number(home) and expected is not None and abs(float(home) - expected) > 1e-6:
            problems.append("%s: homeownership proxy %r != faixa %r (%r)"
                            % (code, home, band, expected))
        if home_year is not None:
            problems.append("%s: proxy de homeownership com ano (%r)" % (code, home_year))
    else:
        if not isinstance(home_year, int):
            problems.append("%s: homeownership observada sem ano inteiro" % code)
        elif home_year > current_year:
            problems.append("%s: homeownership_year no futuro: %d" % (code, home_year))

    # aluguel/renda
    rent = entry.get("rent_to_income_ratio")
    rent_source = entry.get("rent_source")
    rent_year = entry.get("rent_year")
    if rent_source not in RENT_SOURCES:
        problems.append("%s: rent_source desconhecida: %r" % (code, rent_source))
    if rent is not None:
        if not is_number(rent):
            problems.append("%s: rent_to_income_ratio não numérico: %r" % (code, rent))
        elif not (0.0 <= float(rent) <= 1.0):
            problems.append("%s: rent_to_income_ratio fora de [0,1]: %r" % (code, rent))
    if rent_source == "regional_proxy":
        expected = RENT_PROXY.get(band)
        if is_number(rent) and expected is not None and abs(float(rent) - expected) > 1e-6:
            problems.append("%s: aluguel proxy %r != faixa %r" % (code, rent, band))
        if rent_year is not None:
            problems.append("%s: proxy de aluguel com ano (%r)" % (code, rent_year))
    elif not isinstance(rent_year, int):
        problems.append("%s: aluguel observado sem ano inteiro" % code)

    # favela
    slum = entry.get("slum_pct_urban")
    slum_source = entry.get("slum_source")
    slum_year = entry.get("slum_year")
    if slum_source not in SLUM_SOURCES:
        problems.append("%s: slum_source desconhecida: %r" % (code, slum_source))
    if slum is None:
        if slum_source != "no_data":
            problems.append("%s: slum null com fonte %r (esperado no_data)"
                            % (code, slum_source))
        if slum_year is not None:
            problems.append("%s: slum null com ano (%r)" % (code, slum_year))
    else:
        if slum_source != "worldbank_sdg_11_1_1":
            problems.append("%s: slum numérico com fonte %r" % (code, slum_source))
        if not is_number(slum):
            problems.append("%s: slum_pct_urban não numérico: %r" % (code, slum))
        elif not (0.0 <= float(slum) <= 100.0):
            problems.append("%s: slum_pct_urban fora de [0,100]: %r" % (code, slum))
        if not isinstance(slum_year, int):
            problems.append("%s: slum_year ausente/não inteiro" % code)

    # tamanho do domicílio
    size = entry.get("household_size")
    size_source = entry.get("household_size_source")
    size_year = entry.get("household_size_year")
    if size_source not in HOUSEHOLD_SOURCES:
        problems.append("%s: household_size_source desconhecida: %r" % (code, size_source))
    if size is None:
        problems.append("%s: household_size null (esperado valor observado ou proxy)" % code)
    elif not is_number(size):
        problems.append("%s: household_size não numérico: %r" % (code, size))
    elif not (1.0 <= float(size) <= 15.0):
        problems.append("%s: household_size fora de [1,15]: %r" % (code, size))
    if size_source == "regional_proxy":
        expected = HOUSEHOLD_SIZE_PROXY.get(band)
        if is_number(size) and expected is not None and abs(float(size) - expected) > 1e-6:
            problems.append("%s: household_size proxy %r != faixa %r" % (code, size, band))
        if size_year is not None:
            problems.append("%s: proxy de household_size com ano (%r)" % (code, size_year))
    elif not isinstance(size_year, int):
        problems.append("%s: household_size observado sem ano inteiro" % code)

    # proxy_fields coerente com as fontes
    proxy_fields = entry.get("proxy_fields")
    if not isinstance(proxy_fields, list):
        problems.append("%s: proxy_fields ausente/não lista" % code)
        proxy_fields = []
    expected_fields = set()
    if home_source == "regional_proxy":
        expected_fields.add("homeownership")
    if rent_source == "regional_proxy":
        expected_fields.add("rent")
    if size_source == "regional_proxy":
        expected_fields.add("household_size")
    if set(proxy_fields) != expected_fields:
        problems.append("%s: proxy_fields %r != fontes em proxy %r"
                        % (code, sorted(proxy_fields), sorted(expected_fields)))

    # estoque por padrão
    split = entry.get("housing_stock_split")
    if not isinstance(split, dict) or set(split) != STOCK_KEYS:
        problems.append("%s: housing_stock_split precisa das chaves %s"
                        % (code, sorted(STOCK_KEYS)))
    else:
        values = []
        for key in sorted(STOCK_KEYS):
            value = split[key]
            if not is_number(value) or not (0.0 <= float(value) <= 1.0):
                problems.append("%s: housing_stock_split.%s inválido: %r" % (code, key, value))
                values = []
                break
            values.append(float(value))
        if values:
            total = sum(values)
            if abs(total - 1.0) > 0.01:
                problems.append("%s: housing_stock_split soma %.4f (esperado 1.0)"
                                % (code, total))
            gini = gini_by_code.get(code)
            if gini is None:
                problems.append("%s: sem Gini em inequality.json para derivar o estoque" % code)
            else:
                want = expected_split(gini)
                for key in sorted(STOCK_KEYS):
                    if abs(float(split[key]) - want[key]) > 0.002:
                        problems.append(
                            "%s: housing_stock_split.%s=%.4f divergente do Gini %.1f "
                            "(modelo espera %.4f)" % (code, key, float(split[key]), gini,
                                                      want[key]))
    if entry.get("stock_split_source") != "derived:lognormal_from_gini":
        problems.append("%s: stock_split_source != derived:lognormal_from_gini (%r)"
                        % (code, entry.get("stock_split_source")))

    check_refs(entry, code, problems)

    # estimated <-> confidence <-> fonte
    if estimated is True:
        if home_source != "regional_proxy":
            problems.append("%s: estimated=true com homeownership observada (%r)"
                            % (code, home_source))
        if confidence != "estimated":
            problems.append("%s: estimated=true com confidence %r (esperado 'estimated')"
                            % (code, confidence))
        if not entry.get("method"):
            problems.append("%s: estimated=true sem method" % code)
    elif estimated is False:
        if home_source == "regional_proxy":
            problems.append("%s: estimated=false com proxy de homeownership" % code)
        if entry.get("method"):
            problems.append("%s: estimated=false não deveria ter method" % code)
        if confidence not in ("complete", "partial"):
            problems.append("%s: estimated=false com confidence %r" % (code, confidence))
        if not any(isinstance(ref.get("year"), int) for ref in (entry.get("refs") or [])
                   if isinstance(ref, dict)):
            problems.append("%s: observada sem refs com year inteiro" % code)
        if isinstance(home_year, int) and isinstance(slum_year, int):
            if abs(home_year - slum_year) > 15:
                problems.append("%s: anos muito distantes (home %d, favela %d)"
                                % (code, home_year, slum_year))

    return "estimate" if estimated is True else str(confidence)


def main() -> int:
    nations = load_nations()
    payload = load_json(HOUSING_JSON)
    inequality = load_json(INEQUALITY_JSON)
    problems: List[str] = []
    current_year = datetime.date.today().year

    gini_by_code: Dict[str, Optional[float]] = {}
    for code, item in (inequality.get("nations") or {}).items():
        gini = item.get("gini")
        gini_by_code[code] = float(gini) if is_number(gini) else None

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

    counts: Dict[str, int] = {}
    for code in sorted(nations):
        entry = entries.get(code)
        if entry is None:
            counts["invalid"] = counts.get("invalid", 0) + 1
            continue
        kind = check_entry(entry, code, gini_by_code, problems, current_year)
        counts[kind] = counts.get(kind, 0) + 1

    observed_home = sum(1 for e in entries.values()
                        if isinstance(e, dict) and e.get("homeownership_source") != "regional_proxy")
    observed_slum = sum(1 for e in entries.values()
                        if isinstance(e, dict) and e.get("slum_pct_urban") is not None)
    observed_hh = sum(1 for e in entries.values()
                      if isinstance(e, dict) and e.get("household_size_source") == "un_desa")

    coverage = payload.get("coverage") or {}
    recomputed = {
        "nations_total": len(nations),
        "nations_with_observed_homeownership": observed_home,
        "nations_with_observed_slum": observed_slum,
        "nations_with_observed_household_size": observed_hh,
    }
    for field, value in recomputed.items():
        declared = coverage.get(field)
        if declared != value:
            problems.append("coverage.%s = %r, recalculado = %r" % (field, declared, value))

    print("nações no nations.csv: %d | entradas no JSON: %d"
          % (len(nations), len(entries)))
    print("casa própria observada: %d | proxy: %d"
          % (observed_home, len(nations) - observed_home))
    print("favela observada (SDG 11.1.1): %d | sem dado: %d"
          % (observed_slum, len(nations) - observed_slum))
    print("tamanho do domicílio observado (UN DESA): %d" % observed_hh)
    print("confiança: %s" % ", ".join("%s=%d" % (k, counts[k]) for k in sorted(counts)))
    print("amostra:")
    for code in SAMPLES:
        entry = entries.get(code) or {}
        split = entry.get("housing_stock_split") or {}
        print("  %-4s casa=%-6s(%s/%s) aluguel=%-6s favela=%-6s(%s) dom=%-4s conf=%-9s"
              % (code,
                 entry.get("homeownership_rate"), entry.get("homeownership_source"),
                 entry.get("homeownership_year"),
                 entry.get("rent_to_income_ratio"),
                 entry.get("slum_pct_urban"), entry.get("slum_year"),
                 entry.get("household_size"), entry.get("confidence")))
        if split:
            print("       estoque popular=%.3f medium=%.3f high=%.3f soma=%.4f"
                  % (split.get("popular", 0.0), split.get("medium", 0.0),
                     split.get("high", 0.0),
                     sum(float(split.get(k, 0.0)) for k in STOCK_KEYS)))

    if problems:
        print()
        print("PROBLEMAS (%d):" % len(problems))
        for problem in problems:
            print(" -", problem)
        return 1

    print()
    print("OK: %d nações, %d com casa própria observada, %d com favela observada, "
          "%d com domicílio observado" % (len(nations), observed_home, observed_slum,
                                          observed_hh))
    return 0


if __name__ == "__main__":
    sys.exit(main())
