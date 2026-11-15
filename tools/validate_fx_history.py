#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida data/definitions/economy/fx_history/*.json e fx_profiles.json.

Checa:
  1. toda moeda de currencies.json tem perfil e arquivo de histórico;
  2. anos estritamente crescentes, dentro de [1870, 2026], sem duplicata;
  3. taxa finita e > 0 (nenhum valor inventado/zero);
  4. toda linha declara uma fonte conhecida (source_refs) e regime do enum;
  5. estatísticas do perfil conferem com a série (volatilidade, mediana, p95,
     máxima depreciação, frequência de crise, cobertura e contagem de anos);
  6. resumo de cobertura de fx_profiles.json confere com os perfis;
  7. arquivos são UTF-8 sem BOM e usam final de linha LF.

Uso: python tools/validate_fx_history.py [--quiet]
Retorna != 0 se houver erro.
"""

from __future__ import annotations

import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CURRENCIES = os.path.join(ROOT, "data", "definitions", "economy", "currencies.json")
FX_DIR = os.path.join(ROOT, "data", "definitions", "economy", "fx_history")
PROFILES = os.path.join(ROOT, "data", "definitions", "economy", "fx_profiles.json")

KNOWN_SOURCES = {"imf_ifs", "worldbank", "jst_r6"}
REGIMES = {"peg", "band", "float", "free_fall", "union", "dual_market"}
TOL = 5e-6


def err(errors: list, msg: str) -> None:
    errors.append(msg)


def quantile(xs: list, q: float):
    if not xs:
        return None
    s = sorted(xs)
    pos = (len(s) - 1) * q
    lo, hi = int(math.floor(pos)), int(math.ceil(pos))
    return s[lo] if lo == hi else s[lo] + (s[hi] - s[lo]) * (pos - lo)


def recompute(series: list) -> dict:
    values = {row["year"]: row["rate"] for row in series}
    years = [row["year"] for row in series]
    changes = [math.log(values[y] / values[y - 1])
               for y in years if y - 1 in values and values[y] > 0 and values[y - 1] > 0]
    vol = None
    if len(changes) >= 2:
        mean = sum(changes) / len(changes)
        vol = math.sqrt(sum((c - mean) ** 2 for c in changes) / (len(changes) - 1))
    crisis = (sum(1 for c in changes if c > math.log(1.25)) / len(changes)) if changes else None
    covered = sum(1 for y in range(1950, 2025) if y in values)
    return {
        "n": len(years),
        "vol": vol,
        "median": quantile(changes, 0.5),
        "p95": quantile([abs(c) for c in changes], 0.95),
        "max_dep": max(changes) if changes else None,
        "crisis": crisis,
        "coverage": covered / 75,
    }


def close(a, b, tol=TOL) -> bool:
    if a is None and b is None:
        return True
    if a is None or b is None:
        return False
    return abs(a - b) <= max(tol, tol * abs(b))


def main(argv=None) -> int:
    argv = argv or sys.argv[1:]
    quiet = "--quiet" in argv
    errors: list = []

    with open(CURRENCIES, "r", encoding="utf-8-sig") as fh:
        currencies = json.load(fh)["currencies"]
    with open(PROFILES, "r", encoding="utf-8") as fh:
        profiles_doc = json.load(fh)
    profiles = profiles_doc.get("currencies", {})

    with open(PROFILES, "rb") as fh:
        if fh.read(3) == b"\xef\xbb\xbf":
            err(errors, "fx_profiles.json tem BOM")
    if b"\r\n" in open(PROFILES, "rb").read():
        err(errors, "fx_profiles.json usa CRLF")

    codes = [c["code"] for c in currencies]
    if len(codes) != len(set(codes)):
        err(errors, "currencies.json tem moeda duplicada")
    for code in codes:
        if code not in profiles:
            err(errors, f"{code}: sem perfil em fx_profiles.json")
        if not os.path.exists(os.path.join(FX_DIR, f"{code}.json")):
            err(errors, f"{code}: fx_history/{code}.json ausente")

    extra = sorted(set(profiles) - set(codes))
    for code in extra:
        err(errors, f"{code}: perfil sem moeda em currencies.json")

    stats = {"files": 0, "years": 0, "estimated": 0}
    for code in codes:
        path = os.path.join(FX_DIR, f"{code}.json")
        if not os.path.exists(path):
            continue
        raw = open(path, "rb").read()
        if raw[:3] == b"\xef\xbb\xbf":
            err(errors, f"{code}: arquivo com BOM")
        if b"\r\n" in raw:
            err(errors, f"{code}: arquivo com CRLF")
        try:
            doc = json.loads(raw.decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            err(errors, f"{code}: JSON invalido ({exc})")
            continue

        stats["files"] += 1
        if doc.get("code") != code:
            err(errors, f"{code}: campo code = {doc.get('code')}")
        series = doc.get("series") or []
        if not series:
            if not doc.get("no_data"):
                err(errors, f"{code}: serie vazia sem flag no_data")
            prof0 = profiles.get(code) or {}
            if prof0.get("years_with_data") != 0:
                err(errors, f"{code}: serie vazia mas years_with_data={prof0.get('years_with_data')}")
            if "no_data" not in (prof0.get("flags") or []):
                err(errors, f"{code}: serie vazia sem flag no_data no perfil")
            if doc.get("first_year") is not None or doc.get("last_year") is not None:
                err(errors, f"{code}: serie vazia com first/last_year")
            continue

        seen = set()
        prev = None
        for row in series:
            year, rate = row.get("year"), row.get("rate")
            if not isinstance(year, int):
                err(errors, f"{code}: ano invalido {year!r}")
                continue
            if not (1870 <= year <= 2026):
                err(errors, f"{code}: ano fora de [1870, 2026]: {year}")
            if year in seen:
                err(errors, f"{code}: ano duplicado {year}")
            if prev is not None and year <= prev:
                err(errors, f"{code}: anos nao crescentes ({prev} -> {year})")
            prev = year
            seen.add(year)
            if not isinstance(rate, (int, float)) or not math.isfinite(rate) or rate <= 0:
                err(errors, f"{code} {year}: taxa invalida {rate!r}")
            src = row.get("source", "")
            if src.startswith("derived:"):
                if src.split(":", 1)[1].upper() not in {"DEM", "EUR", "GBP", "FRF", "ITL"}:
                    err(errors, f"{code} {year}: fonte derivada nao documentada {src}")
            elif src not in KNOWN_SOURCES:
                err(errors, f"{code} {year}: fonte desconhecida {src!r}")
            if "regime" in row and row["regime"] not in REGIMES:
                err(errors, f"{code} {year}: regime invalido {row['regime']!r}")

        first, last = series[0]["year"], series[-1]["year"]
        if doc.get("first_year") != first or doc.get("last_year") != last:
            err(errors, f"{code}: first/last_year ({doc.get('first_year')},{doc.get('last_year')}) "
                        f"nao batem com a serie ({first},{last})")

        prof = profiles.get(code) or {}
        calc = recompute(series)
        stats["years"] += calc["n"]
        if prof.get("years_with_data") != calc["n"]:
            err(errors, f"{code}: years_with_data {prof.get('years_with_data')} != {calc['n']}")
        if not close(prof.get("volatility_annual_log"), calc["vol"]):
            err(errors, f"{code}: volatilidade {prof.get('volatility_annual_log')} != {calc['vol']}")
        if not close(prof.get("median_annual_log_change"), calc["median"]):
            err(errors, f"{code}: mediana {prof.get('median_annual_log_change')} != {calc['median']}")
        if not close(prof.get("p95_abs_annual_log_change"), calc["p95"]):
            err(errors, f"{code}: p95 {prof.get('p95_abs_annual_log_change')} != {calc['p95']}")
        if not close(prof.get("max_1y_depreciation_log"), calc["max_dep"]):
            err(errors, f"{code}: max dep {prof.get('max_1y_depreciation_log')} != {calc['max_dep']}")
        if not close(prof.get("crisis_frequency"), calc["crisis"]):
            err(errors, f"{code}: crise {prof.get('crisis_frequency')} != {calc['crisis']}")
        if not close(prof.get("coverage_1950_2024"), calc["coverage"]):
            err(errors, f"{code}: cobertura {prof.get('coverage_1950_2024')} != {calc['coverage']}")
        if prof.get("confidence") not in {"complete", "partial", "estimated"}:
            err(errors, f"{code}: confidence invalida {prof.get('confidence')!r}")
        if prof.get("confidence") == "estimated":
            stats["estimated"] += 1
        if prof.get("regime_recent") is not None and prof["regime_recent"] not in REGIMES:
            err(errors, f"{code}: regime_recent invalido {prof['regime_recent']!r}")

    summary = profiles_doc.get("coverage") or {}
    if summary.get("currencies") != len(codes):
        err(errors, f"coverage.currencies {summary.get('currencies')} != {len(codes)}")
    for key, threshold in (("at_least_50_years", 50), ("at_least_75_years", 75),
                           ("at_least_100_years", 100)):
        expected = sum(1 for p in profiles.values() if (p.get("years_with_data") or 0) >= threshold)
        if summary.get(key) != expected:
            err(errors, f"coverage.{key} {summary.get(key)} != {expected}")

    if errors:
        print(f"FALHOU: {len(errors)} problema(s)")
        for e in errors[:60]:
            print("  -", e)
        if len(errors) > 60:
            print(f"  ... e mais {len(errors) - 60}")
        return 1

    if not quiet:
        print(f"OK: {stats['files']} arquivos, {stats['years']} pontos, "
              f"{stats['estimated']} moedas estimated")
        print("cobertura:", json.dumps(summary, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
