#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida data/definitions/economy/municipal_finance.json contra
data/definitions/scenario/nations.csv.

Checa:
  1. o JSON cobre exatamente as nações do nations.csv (nem uma a mais, nem a menos);
  2. faixas: subnational_revenue_pct_gdp em [0, 30] quando proxy (e [0, 40] quando
     observado — a faixa 31,4/31,8 de DNK/CAN é dado real, não erro de proxy);
     property_tax_pct_total_tax em [0, 20]; central_transfers_pct_subnational em [0, 100];
     qualquer um pode ser null;
  3. flags POR CAMPO: toda entrada traz `subnational_estimated`, `property_tax_estimated` e
     `transfers_estimated` (boolean). Campo com source "proxy" exige flag true; campo observado
     (oecd_*/imf_gfs) exige flag false; campo null exige flag false e source "no_data";
  4. campo observado exige `*_year` inteiro e `refs` com url http(s) + year inteiro;
     campo proxy não leva ano (o year da ref é o ano de referência da faixa, não medição);
  5. `confidence` coerente com a mistura: complete = os três campos observados; partial = misto
     (ao menos um observado e ao menos um proxy); estimated = os três proxy; no_data = os três
     null (com `reason`). `estimated` (entrada) é true SOMENTE quando os três campos são proxy;
  6. o bloco `coverage` bate com a contagem recalculada, que é POR CAMPO
     (observado / proxy / null) mais as entradas 100% observadas.

Uso:
    python tools/validate_municipal_finance.py

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
MUNICIPAL_FINANCE = os.path.join(ROOT, "data", "definitions", "economy", "municipal_finance.json")

# (campo, chave de source, chave de ano, chave da flag por campo,
#  limite inferior, limite superior para proxy, limite superior para observado)
FIELDS = (
    ("subnational_revenue_pct_gdp", "subnational_source", "subnational_year",
     "subnational_estimated", 0.0, 30.0, 40.0),
    ("property_tax_pct_total_tax", "property_tax_source", "property_tax_year",
     "property_tax_estimated", 0.0, 20.0, 20.0),
    ("central_transfers_pct_subnational", "transfers_source", "transfers_year",
     "transfers_estimated", 0.0, 100.0, 100.0),
)

SOURCE_BY_FIELD = {
    "subnational_revenue_pct_gdp": {
        "imf_gfs", "oecd_subnational",
    },
    "property_tax_pct_total_tax": {
        "oecd_revenue_statistics", "imf_gfs",
    },
    "central_transfers_pct_subnational": {
        "oecd_subnational", "imf_gfs",
    },
}

CONFIDENCE_KINDS = ("complete", "partial", "estimated", "no_data")

SAMPLES = ("USA", "DEU", "BRA", "IND", "CHN", "NGA", "CAN", "DNK", "JPN", "ZAF")


def load_nations() -> Dict[str, dict]:
    with open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        return {row["id"]: row for row in csv.DictReader(handle)}


def load_payload() -> dict:
    with open(MUNICIPAL_FINANCE, "r", encoding="utf-8") as handle:
        return json.load(handle)


def check_top_level(payload: dict, problems: List[str]) -> None:
    if payload.get("version") != 1:
        problems.append("version deveria ser 1, veio %r" % payload.get("version"))
    if not isinstance(payload.get("note"), str) or not payload["note"].strip():
        problems.append("note ausente ou vazia")
    for block in ("coverage", "sources", "method", "nations"):
        if not isinstance(payload.get(block), dict):
            problems.append("bloco %s ausente ou não é objeto" % block)
    sources = payload.get("sources") or {}
    for name in ("oecd_subnational", "oecd_revenue_statistics"):
        entry = sources.get(name)
        if not isinstance(entry, dict):
            problems.append("sources.%s ausente" % name)
            continue
        if not entry.get("title"):
            problems.append("sources.%s sem title" % name)
        if not isinstance(entry.get("url"), str) or not entry["url"].startswith("http"):
            problems.append("sources.%s sem url http(s)" % name)
    for name in ("subnational_revenue", "property_tax", "central_transfers"):
        if not (payload.get("method") or {}).get(name):
            problems.append("method.%s ausente" % name)


def check_refs(entry: dict, code: str, problems: List[str]) -> None:
    """Todo campo observado precisa de pelo menos uma ref http+ano; proxy aceita a guia."""
    refs = entry.get("refs")
    if not isinstance(refs, list):
        problems.append("%s: refs ausente ou não é lista" % code)
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
        if not isinstance(ref.get("year"), int):
            problems.append("%s: refs[%d].year não é inteiro: %r" % (code, position, ref.get("year")))


def check_entry(entry: dict, code: str, problems: List[str], current_year: int) -> str:
    """Valida uma nação e devolve complete | partial | estimated | no_data | invalid."""
    if not isinstance(entry, dict):
        problems.append("%s: entrada não é objeto" % code)
        return "invalid"

    for key in ("name", "subnational_source", "property_tax_source", "transfers_source",
                "subnational_estimated", "property_tax_estimated", "transfers_estimated",
                "estimated", "confidence"):
        if key not in entry:
            problems.append("%s: campo %s ausente" % (code, key))

    if not isinstance(entry.get("estimated"), bool):
        problems.append("%s: estimated ausente/não booleano" % code)

    observed_count = 0
    proxy_count = 0
    null_count = 0

    for value_key, source_key, year_key, flag_key, low, proxy_high, observed_high in FIELDS:
        value = entry.get(value_key)
        source = entry.get(source_key)
        year = entry.get(year_key)
        flag = entry.get(flag_key)

        if not isinstance(flag, bool):
            problems.append("%s: %s ausente/não booleano: %r" % (code, flag_key, flag))
            flag = None

        if value is None:
            null_count += 1
            if year is not None:
                problems.append("%s: %s null mas %s=%r" % (code, value_key, year_key, year))
            if source != "no_data":
                problems.append("%s: %s null exige source 'no_data', veio %r"
                                % (code, value_key, source))
            if flag is True:
                problems.append("%s: %s null não é proxy, mas %s=true"
                                % (code, value_key, flag_key))
            continue

        if not isinstance(value, (int, float)) or isinstance(value, bool):
            problems.append("%s: %s não numérico: %r" % (code, value_key, value))
            continue

        if source == "proxy":
            proxy_count += 1
            if flag is False:
                problems.append("%s: %s com source proxy exige %s=true"
                                % (code, value_key, flag_key))
            if not (low <= float(value) <= proxy_high):
                problems.append("%s: %s (proxy) fora de [%g,%g]: %r"
                                % (code, value_key, low, proxy_high, value))
            if year is not None:
                problems.append("%s: %s é proxy e não deveria ter %s" % (code, value_key, year_key))
            continue

        if source not in SOURCE_BY_FIELD[value_key]:
            problems.append("%s: %s com fonte desconhecida: %r" % (code, value_key, source))
            continue

        observed_count += 1
        if flag is True:
            problems.append("%s: %s observado (%s) mas %s=true"
                            % (code, value_key, source, flag_key))
        if not (low <= float(value) <= observed_high):
            problems.append("%s: %s (observado) fora de [%g,%g]: %r"
                            % (code, value_key, low, observed_high, value))
        if not isinstance(year, int) or isinstance(year, bool):
            problems.append("%s: %s observado sem %s inteiro: %r" % (code, value_key, year_key, year))
        elif year > current_year:
            problems.append("%s: %s com ano futuro: %d" % (code, value_key, year))

    confidence = entry.get("confidence")
    estimated = entry.get("estimated")

    if observed_count == 0 and proxy_count == 0:
        if null_count != len(FIELDS):
            problems.append("%s: nenhum campo com valor mas %d campo(s) não null"
                            % (code, len(FIELDS) - null_count))
        if confidence != "no_data":
            problems.append("%s: três campos null com confidence %r (esperado 'no_data')"
                            % (code, confidence))
        if estimated is not False:
            problems.append("%s: sem dado, estimated deveria ser false (veio %r)"
                            % (code, estimated))
        if not entry.get("reason"):
            problems.append("%s: no_data sem reason" % code)
        return "no_data"

    check_refs(entry, code, problems)

    method = entry.get("method")
    if proxy_count > 0:
        if not isinstance(method, str) or not method.strip():
            problems.append("%s: %d campo(s) proxy sem method por entrada" % (code, proxy_count))
    elif confidence == "complete" and method is not None:
        problems.append("%s: entrada 100%% observada com method (%r)" % (code, method))

    if observed_count == len(FIELDS):
        expected = "complete"
    elif proxy_count == len(FIELDS):
        expected = "estimated"
    elif observed_count and proxy_count:
        expected = "partial"
    else:
        problems.append("%s: mistura de campo com valor e null não prevista pelo contrato "
                        "(observados=%d, proxy=%d, null=%d)"
                        % (code, observed_count, proxy_count, null_count))
        expected = None

    if expected and confidence != expected:
        problems.append("%s: confidence %r != %r (observados=%d, proxy=%d)"
                        % (code, confidence, expected, observed_count, proxy_count))

    want_estimated = proxy_count == len(FIELDS) and observed_count == 0
    if estimated is not want_estimated:
        problems.append("%s: estimated=%r, esperado %r (proxy=%d, observado=%d)"
                        % (code, estimated, want_estimated, proxy_count, observed_count))

    return expected or "invalid"


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

    counts = {kind: 0 for kind in CONFIDENCE_KINDS}
    counts["invalid"] = 0
    per_field_observed = {key: 0 for key, *_ in FIELDS}
    per_field_proxy = {key: 0 for key, *_ in FIELDS}
    per_field_null = {key: 0 for key, *_ in FIELDS}
    over_thirty: List[str] = []

    for code in sorted(nations):
        entry = entries.get(code)
        if entry is None:
            counts["invalid"] += 1
            continue
        kind = check_entry(entry, code, problems, current_year)
        counts[kind] += 1
        if entry.get("name") != nations[code]["name"]:
            problems.append("%s: name %r != %r do nations.csv"
                            % (code, entry.get("name"), nations[code]["name"]))
        for value_key, _source_key, _year_key, flag_key, _low, _proxy_high, _obs_high in FIELDS:
            value = entry.get(value_key)
            if value is None:
                per_field_null[value_key] += 1
            elif entry.get(flag_key) is True:
                per_field_proxy[value_key] += 1
            else:
                per_field_observed[value_key] += 1
            if value_key == "subnational_revenue_pct_gdp" and isinstance(value, (int, float)) \
                    and float(value) > 30.0:
                over_thirty.append("%s=%.2f" % (code, float(value)))

    coverage = payload.get("coverage") or {}
    recomputed = {
        "nations_total": len(nations),
        "nations_fully_observed": counts["complete"],
        "nations_by_confidence": {kind: counts[kind] for kind in CONFIDENCE_KINDS},
        "fields": {
            value_key: {
                "observed": per_field_observed[value_key],
                "proxy": per_field_proxy[value_key],
                "null": per_field_null[value_key],
            }
            for value_key, *_ in FIELDS
        },
    }
    for field, value in recomputed.items():
        if field == "fields":
            declared_fields = coverage.get("fields")
            if not isinstance(declared_fields, dict):
                problems.append("coverage.fields ausente ou não é objeto")
                continue
            for value_key, per_field in value.items():
                declared = declared_fields.get(value_key) or {}
                for counter, expected in per_field.items():
                    if declared.get(counter) != expected:
                        problems.append("coverage.fields.%s.%s = %r, recalculado = %r"
                                        % (value_key, counter, declared.get(counter), expected))
            continue
        if field == "nations_by_confidence":
            declared = coverage.get(field)
            if not isinstance(declared, dict):
                problems.append("coverage.%s ausente ou não é objeto" % field)
                continue
            for kind, expected in value.items():
                if declared.get(kind) != expected:
                    problems.append("coverage.%s.%s = %r, recalculado = %r"
                                    % (field, kind, declared.get(kind), expected))
            continue
        if coverage.get(field) != value:
            problems.append("coverage.%s = %r, recalculado = %r" % (field, coverage.get(field), value))

    print("nações no nations.csv: %d | entradas no JSON: %d" % (len(nations), len(entries)))
    print("entradas: 100%% observadas: %d | mistas (observado+proxy): %d | 100%% proxy: %d | sem dado: %d"
          % (counts["complete"], counts["partial"], counts["estimated"], counts["no_data"]))
    print("por campo (observado / proxy / null):")
    for value_key, *_ in FIELDS:
        print("  %-36s %3d / %3d / %3d"
              % (value_key, per_field_observed[value_key], per_field_proxy[value_key],
                 per_field_null[value_key]))
    if over_thirty:
        print("nota: receita subnacional observada acima de 30%% do PIB (dado real, faixa "
              "específica de CAN/DNK): %s" % ", ".join(over_thirty))
    print("amostra (* = campo proxy, não medição):")
    for code in SAMPLES:
        entry = entries.get(code) or {}

        def cell(value_key: str, flag_key: str) -> str:
            value = entry.get(value_key)
            if value is None:
                return "null"
            return "%s%s" % (value, "*" if entry.get(flag_key) is True else "")

        print("  %-4s sub=%7s | imovel=%6s | transf=%7s | estimated=%-5s | %s"
              % (code,
                 cell("subnational_revenue_pct_gdp", "subnational_estimated"),
                 cell("property_tax_pct_total_tax", "property_tax_estimated"),
                 cell("central_transfers_pct_subnational", "transfers_estimated"),
                 entry.get("estimated"),
                 entry.get("confidence")))

    if problems:
        print()
        print("PROBLEMAS (%d):" % len(problems))
        for problem in problems:
            print(" -", problem)
        return 1

    print()
    print("OK: %d nações, %d com os três campos observados, %d mistas (partial), "
          "%d totalmente proxy, %d sem dado"
          % (len(nations), counts["complete"], counts["partial"], counts["estimated"],
             counts["no_data"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
