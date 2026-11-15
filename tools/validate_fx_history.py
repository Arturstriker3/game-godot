#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida data/definitions/economy/fx_history/*.json e fx_profiles.json.

Checa:
  1. toda moeda de currencies.json tem perfil e arquivo de histórico;
  2. anos estritamente crescentes, dentro de [1870, 2026], sem duplicata;
  3. taxa finita e > 0 (nenhum valor inventado/zero);
  4. toda linha declara uma fonte conhecida (source_refs) e regime do enum;
  5. linhas de reconstrução histórica (source=historical_reconstruction) trazem `method`,
     `ref` resolvida em `reconstruction.refs` com title/url/year, e `interpolated` só com
     method de interpolação (e vice-versa);
  6. o arquivo de fatos tools/data/fx_historical_facts.json é estruturalmente válido
     (anos, taxas, refs, regimes e limites de interpolação);
  7. estatísticas do perfil conferem com a série (volatilidade, mediana, p95, máxima
     depreciação, frequência de crise, cobertura e contagem de anos), com a mesma
     exclusão de `stat_breaks` usada pelo builder;
  8. confiança = base (cobertura) limitada pela fração reconstruída, e o resumo de
     cobertura (inclusive `historical_reconstruction`) confere com os perfis;
  9. arquivos são UTF-8 sem BOM e usam final de linha LF.

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
FACTS = os.path.join(ROOT, "tools", "data", "fx_historical_facts.json")

RECON_SOURCE = "historical_reconstruction"
INTERP_METHOD = "interpolated_between_documented_facts"
MAX_INTERP_GAP = 10
KNOWN_SOURCES = {"imf_ifs", "worldbank", "jst_r6", RECON_SOURCE}
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


def stdev(xs: list):
    if len(xs) < 2:
        return None
    mean = sum(xs) / len(xs)
    return math.sqrt(sum((c - mean) ** 2 for c in xs) / (len(xs) - 1))


def recompute(series: list, doc: dict) -> dict:
    values = {row["year"]: row["rate"] for row in series}
    years = [row["year"] for row in series]
    stat_breaks = {b["year"] for b in doc.get("breaks") or [] if b.get("definitional")}
    change_rows = [(y, math.log(values[y] / values[y - 1])) for y in years
                   if y - 1 in values and y not in stat_breaks
                   and values[y] > 0 and values[y - 1] > 0]
    changes = [c for _y, c in change_rows]
    documented = {row["year"] for row in series if not row.get("interpolated")}
    doc_changes = [c for y, c in change_rows if y in documented and (y - 1) in documented]
    crisis = (sum(1 for c in changes if c > math.log(1.25)) / len(changes)) if changes else None
    covered = sum(1 for y in range(1950, 2025) if y in values)
    recon = [row["year"] for row in series if row.get("source") == RECON_SOURCE]
    interp = [row["year"] for row in series if row.get("interpolated")]
    return {
        "n": len(years),
        "vol": stdev(changes),
        "vol_doc": stdev(doc_changes),
        "median": quantile(changes, 0.5),
        "p95": quantile([abs(c) for c in changes], 0.95),
        "max_dep": max(changes) if changes else None,
        "crisis": crisis,
        "coverage": covered / 75,
        "recon": recon,
        "interp": interp,
        "stat_breaks": stat_breaks,
        "values": values,
        "years": years,
    }


def close(a, b, tol=TOL) -> bool:
    if a is None and b is None:
        return True
    if a is None or b is None:
        return False
    return abs(a - b) <= max(tol, tol * abs(b))


def confidence_for(n_years: int, coverage: float, recon_fraction: float) -> tuple:
    """Mesma regra de tools/build_fx_history.py: base por cobertura, teto por reconstrução."""
    if n_years >= 60 and coverage >= 0.95:
        base = "complete"
    elif n_years >= 20 and coverage >= 0.5:
        base = "partial"
    else:
        base = "estimated"
    if recon_fraction >= 0.5:
        return base, "estimated"
    if recon_fraction >= 0.1 and base == "complete":
        return base, "partial"
    return base, base


def validate_facts(errors: list, codes: list) -> dict:
    """Valida o arquivo de fatos documentados (camada de reconstrução)."""
    if not os.path.exists(FACTS):
        err(errors, "tools/data/fx_historical_facts.json ausente (camada de reconstrução)")
        return {}
    raw = open(FACTS, "rb").read()
    if raw[:3] == b"\xef\xbb\xbf":
        err(errors, "fx_historical_facts.json tem BOM")
    if b"\r\n" in raw:
        err(errors, "fx_historical_facts.json usa CRLF")
    try:
        doc = json.loads(raw.decode("utf-8"))
    except Exception as exc:  # noqa: BLE001
        err(errors, f"fx_historical_facts.json invalido ({exc})")
        return {}
    if doc.get("source") != RECON_SOURCE:
        err(errors, f"fx_historical_facts.json: source deve ser '{RECON_SOURCE}'")

    currencies = doc.get("currencies") or {}
    known = set(codes)
    for code, block in sorted(currencies.items()):
        if code not in known:
            err(errors, f"fatos: moeda {code} nao esta em currencies.json")
        refs = block.get("refs") or {}
        if not refs:
            err(errors, f"fatos {code}: sem refs")
        for rid, ref in refs.items():
            for key in ("title", "url", "year"):
                if not ref.get(key):
                    err(errors, f"fatos {code} ref {rid}: falta {key}")
            url = str(ref.get("url") or "")
            if url and not url.startswith("http"):
                err(errors, f"fatos {code} ref {rid}: url nao e http(s): {url!r}")
            year = ref.get("year")
            if not isinstance(year, int) or not (1800 <= year <= 2026):
                err(errors, f"fatos {code} ref {rid}: year invalido {year!r}")

        points = block.get("points") or []
        if not points and not (block.get("regimes") or []):
            err(errors, f"fatos {code}: sem points e sem regimes (bloco vazio)")
        for point in points:
            y = point.get("year")
            if not isinstance(y, int) or not (1870 <= y <= 2026):
                err(errors, f"fatos {code}: ano invalido {y!r}")
                continue
            last = point.get("to_year", y)
            if not isinstance(last, int) or last < y or last > 2026:
                err(errors, f"fatos {code} {y}: to_year invalido {last!r}")
            rate = point.get("rate")
            if not isinstance(rate, (int, float)) or not math.isfinite(rate) or rate <= 0:
                err(errors, f"fatos {code} {y}: taxa invalida {rate!r}")
            if not isinstance(point.get("method"), str) or not point.get("method"):
                err(errors, f"fatos {code} {y}: sem method")
            if point.get("ref") not in refs:
                err(errors, f"fatos {code} {y}: ref {point.get('ref')!r} nao declarada")

        for spec in block.get("interpolate") or []:
            a, b = spec.get("from_year"), spec.get("to_year")
            if not isinstance(a, int) or not isinstance(b, int) or b <= a:
                err(errors, f"fatos {code}: interpolacao invalida {a!r}->{b!r}")
                continue
            if b - a - 1 > MAX_INTERP_GAP:
                err(errors, f"fatos {code}: interpolacao {a}->{b} acima do limite de {MAX_INTERP_GAP} anos")
            if spec.get("ref") not in refs:
                err(errors, f"fatos {code}: interpolacao {a}->{b} sem ref declarada")

        for spec in block.get("regimes") or []:
            a, b = spec.get("from"), spec.get("to")
            if not isinstance(a, int) or not isinstance(b, int) or b < a or not (1870 <= a <= 2026):
                err(errors, f"fatos {code}: regime com intervalo invalido {a!r}->{b!r}")
            if spec.get("regime") not in REGIMES:
                err(errors, f"fatos {code}: regime invalido {spec.get('regime')!r}")
            if spec.get("ref") not in refs:
                err(errors, f"fatos {code}: regime {a}->{b} sem ref declarada")

        for y in block.get("stat_breaks") or []:
            if not isinstance(y, int):
                err(errors, f"fatos {code}: stat_break invalido {y!r}")
    return currencies


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

    facts = validate_facts(errors, codes)

    stats = {"files": 0, "years": 0, "estimated": 0, "reconstructed": 0}
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

            if row.get("interpolated"):
                if row.get("method") != INTERP_METHOD:
                    err(errors, f"{code} {year}: interpolated com method {row.get('method')!r}")
                anchor = row.get("anchor")
                if not (isinstance(anchor, list) and len(anchor) == 2
                        and all(isinstance(a, int) for a in anchor) and anchor[0] < year < anchor[1]):
                    err(errors, f"{code} {year}: interpolated sem anchor valido {anchor!r}")
            elif str(row.get("method") or "").startswith("interpolat"):
                err(errors, f"{code} {year}: method de interpolacao sem interpolated=true")

        # --- camada de reconstrução
        recon_rows = [row for row in series if row.get("source") == RECON_SOURCE]
        block = doc.get("reconstruction")
        if recon_rows and not isinstance(block, dict):
            err(errors, f"{code}: linhas de reconstrucao sem bloco `reconstruction`")
        elif recon_rows:
            refs = block.get("refs") or {}
            if not refs:
                err(errors, f"{code}: reconstruction.refs vazio")
            for row in recon_rows:
                year = row["year"]
                method = row.get("method")
                if not isinstance(method, str) or not method:
                    err(errors, f"{code} {year}: reconstrucao sem method")
                ref_id = row.get("ref")
                if ref_id not in refs:
                    err(errors, f"{code} {year}: reconstrucao com ref ausente/nao declarada {ref_id!r}")
                    continue
                ref = refs[ref_id]
                for key in ("title", "url", "year"):
                    if not ref.get(key):
                        err(errors, f"{code} {year}: ref {ref_id} sem {key}")
                if ref.get("url") and not str(ref["url"]).startswith("http"):
                    err(errors, f"{code} {year}: ref {ref_id} com url invalida")
            if code in facts:
                declared = set(block.get("interpolated_years") or [])
                actual = {row["year"] for row in recon_rows if row.get("interpolated")}
                if declared != actual:
                    err(errors, f"{code}: reconstruction.interpolated_years != anos interpolados da serie")
                if not block.get("refs"):
                    err(errors, f"{code}: reconstruction sem refs")
        elif code in facts and (facts[code].get("points") or []):
            if doc.get("no_data"):
                err(errors, f"{code}: fatos declarados mas serie continua vazia")

        first, last = series[0]["year"], series[-1]["year"]
        if doc.get("first_year") != first or doc.get("last_year") != last:
            err(errors, f"{code}: first/last_year ({doc.get('first_year')},{doc.get('last_year')}) "
                        f"nao batem com a serie ({first},{last})")

        prof = profiles.get(code) or {}
        calc = recompute(series, doc)
        stats["years"] += calc["n"]
        stats["reconstructed"] += len(calc["recon"])
        if prof.get("years_with_data") != calc["n"]:
            err(errors, f"{code}: years_with_data {prof.get('years_with_data')} != {calc['n']}")
        if not close(prof.get("volatility_annual_log"), calc["vol"]):
            err(errors, f"{code}: volatilidade {prof.get('volatility_annual_log')} != {calc['vol']}")
        if not close(prof.get("volatility_documented_only_log"), calc["vol_doc"]):
            err(errors, f"{code}: volatilidade documentada "
                        f"{prof.get('volatility_documented_only_log')} != {calc['vol_doc']}")
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

        # confiança declarada == regra (cobertura + fração reconstruída)
        frac = len(calc["recon"]) / calc["n"]
        base, expected = confidence_for(calc["n"], calc["coverage"], frac)
        if prof.get("confidence") != expected:
            err(errors, f"{code}: confidence {prof.get('confidence')} != esperado {expected}")
        if prof.get("confidence_base") != base:
            err(errors, f"{code}: confidence_base {prof.get('confidence_base')} != {base}")
        if prof.get("reconstructed_years") != len(calc["recon"]):
            err(errors, f"{code}: reconstructed_years {prof.get('reconstructed_years')} != {len(calc['recon'])}")
        if not close(prof.get("reconstructed_fraction"), frac):
            err(errors, f"{code}: reconstructed_fraction {prof.get('reconstructed_fraction')} != {frac}")
        if prof.get("interpolated_years") != len(calc["interp"]):
            err(errors, f"{code}: interpolated_years {prof.get('interpolated_years')} != {len(calc['interp'])}")
        if sorted(prof.get("stat_break_years") or []) != sorted(calc["stat_breaks"]):
            err(errors, f"{code}: stat_break_years != quebras definicionais da serie")

        regime_latest = None
        for row in reversed(series):
            if "regime" in row:
                regime_latest = row["regime"]
                break
        if prof.get("regime_latest") != regime_latest:
            err(errors, f"{code}: regime_latest {prof.get('regime_latest')} != {regime_latest}")
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
    hr = summary.get("historical_reconstruction") or {}
    expected_hr = {
        "currencies": sum(1 for p in profiles.values() if p.get("reconstructed_years")),
        "years": sum(p.get("reconstructed_years") or 0 for p in profiles.values()),
        "interpolated_years": sum(p.get("interpolated_years") or 0 for p in profiles.values()),
        "downgraded": sum(1 for p in profiles.values() if p.get("confidence") != p.get("confidence_base")),
    }
    for key, value in expected_hr.items():
        if hr.get(key) != value:
            err(errors, f"coverage.historical_reconstruction.{key} {hr.get(key)} != {value}")
    meta = profiles_doc.get("reconstruction") or {}
    if meta.get("source") != RECON_SOURCE:
        err(errors, f"fx_profiles.json: reconstruction.source deve ser '{RECON_SOURCE}'")
    if (meta.get("rules") or {}).get("interpolation_max_gap_years") != MAX_INTERP_GAP:
        err(errors, "fx_profiles.json: reconstruction.rules.interpolation_max_gap_years divergente")

    if errors:
        print(f"FALHOU: {len(errors)} problema(s)")
        for e in errors[:60]:
            print("  -", e)
        if len(errors) > 60:
            print(f"  ... e mais {len(errors) - 60}")
        return 1

    if not quiet:
        print(f"OK: {stats['files']} arquivos, {stats['years']} pontos "
              f"({stats['reconstructed']} reconstruidos), {stats['estimated']} moedas estimated")
        print("cobertura:", json.dumps(summary, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
