#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gera o histórico cambial anual (moeda local por USD) para as 136 moedas do jogo.

Fontes (citadas no JSON de saída e em docs/economy/fx_history.md):

  IMF IFS via DBnomics  A.<ISO2>.ENDA_XDC_USD_RATE  1950..2024  (primária)
  World Bank            PA.NUS.FCRF (LCU por USD)   1960..2025  (preenche lacuna / ano corrente)
  JST Macrohistory R6   xrusd                        1870..2020  (extensão 1925..1949, 18 países)
  Ilzetzki-Reinhart-Rogoff (238_data.xlsx, "Fine"/"Coarse")
                        regime de fato por país/ano   1940..2016
  Facts (curadoria)     tools/data/fx_historical_facts.json    últimos recursos, 1925..2025

Regras:
  * Nada é interpolado, exceto entre dois fatos documentados da camada de reconstrução
    (marcados `source: historical_reconstruction` + `interpolated: true`).
  * Mesmo ano em fontes diferentes: imf_ifs > worldbank > jst_r6 (a primeira vence);
    divergência > 2% vira entrada em `notes`.
  * Pré-1950 do JST só entra se a razão com o primeiro ano do IFS ficar em [0.5, 2.0]
    (proteção contra mudança de unidade não encadeada).
  * IFS e World Bank já vêm encadeados na unidade monetária corrente (BRL/ARS/TRY/ZWL
    conferidos); a calibração usa log-variações, robustas a escala de nível.
  * Reconstrução histórica preenche APENAS anos ausentes e nunca sobrescreve dado
    estatístico; cada ponto carrega `ref` que resolve em `reconstruction.refs`.

Uso:
    python tools/build_fx_history.py            # usa cache; baixa o que faltar
    python tools/build_fx_history.py --refresh  # ignora cache
    python tools/build_fx_history.py --check    # não escreve, só reporta cobertura
"""

from __future__ import annotations

import argparse
import difflib
import json
import math
import os
import ssl
import sys
import time
import urllib.request
from typing import Dict, Iterable, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CURRENCIES = os.path.join(ROOT, "data", "definitions", "economy", "currencies.json")
OUT_DIR = os.path.join(ROOT, "data", "definitions", "economy", "fx_history")
OUT_PROFILES = os.path.join(ROOT, "data", "definitions", "economy", "fx_profiles.json")
CACHE = os.path.join(ROOT, "tools", "data", "fx_cache")
FACTS = os.path.join(ROOT, "tools", "data", "fx_historical_facts.json")

TARGET_START, TARGET_END = 1925, 2025
PRIMARY_START, PRIMARY_END = 1950, 2024
CRISIS_THRESHOLD = 0.25
SPLICE_MIN, SPLICE_MAX = 0.5, 2.0
CONFLICT_TOL = 0.02
RECON_SOURCE = "historical_reconstruction"
MAX_INTERP_GAP = 10  # anos ausentes máximos cobertos por interpolação entre dois fatos

SOURCES = {
    "imf_ifs": {
        "name": "IMF International Financial Statistics (via DBnomics)",
        "indicator": "ENDA_XDC_USD_RATE",
        "description": "Exchange rate, domestic currency per US dollar, period average, annual.",
        "url": "https://api.db.nomics.world/v22/series/IMF/IFS",
        "homepage": "https://data.imf.org/",
        "coverage": "1950-2024 (varia por pais)",
    },
    "worldbank": {
        "name": "World Bank Open Data",
        "indicator": "PA.NUS.FCRF",
        "description": "Official exchange rate (LCU per US$, period average).",
        "url": "https://api.worldbank.org/v2/indicator/PA.NUS.FCRF",
        "homepage": "https://data.worldbank.org/indicator/PA.NUS.FCRF",
        "coverage": "1960-2025 (varia por pais)",
    },
    "jst_r6": {
        "name": "Jorda-Schularick-Taylor Macrohistory Database, release 6",
        "indicator": "xrusd",
        "description": "Exchange rate, national currency per USD, annual.",
        "url": "https://www.macrohistory.net/database/",
        "homepage": "https://www.macrohistory.net/database/",
        "coverage": "1870-2020, 18 paises",
    },
    "irr": {
        "name": "Ilzetzki, Reinhart & Rogoff - Exchange Rate Arrangements (de facto)",
        "indicator": "fine/coarse classification",
        "description": "De facto exchange rate arrangement classification by country and year.",
        "url": "https://carmenreinhart.com/exchange-rate/",
        "homepage": "https://carmenreinhart.com/exchange-rate/",
        "coverage": "1940-2016",
    },
    RECON_SOURCE: {
        "name": "Reconstrucao historica documentada (curadoria do projeto)",
        "indicator": "pontos anuais documentados (paridades, desvalorizacoes, reformas, taxas de mercado)",
        "description": ("Ultimo recurso para lacunas: cada ponto tem method + ref; refs com title/url/year "
                        "ficam em fx_history/<CODE>.json:reconstruction.refs. Nunca sobrescreve dado estatistico."),
        "url": "tools/data/fx_historical_facts.json",
        "homepage": "docs/economy/fx_history.md#reconstrucao-historica",
        "coverage": "1925-2025 (9 moedas com lacuna) + regime de fato 2017-2025",
    },
}

IRR_FINE = {
    1: "No separate legal tender or currency union",
    2: "Pre-announced peg or currency board",
    3: "Pre-announced horizontal band <= +/-2%",
    4: "De facto peg",
    5: "Pre-announced crawling peg",
    6: "Pre-announced crawling band <= +/-2%",
    7: "De facto crawling peg",
    8: "De facto crawling band <= +/-2%",
    9: "Pre-announced crawling band >= +/-2%",
    10: "De facto crawling band <= +/-5%",
    11: "Moving band <= +/-2%",
    12: "De facto moving band +/-5% / managed floating",
    13: "Freely floating",
    14: "Freely falling",
    15: "Dual market (parallel data missing)",
}

FINE_TO_COARSE = {1: 1, 2: 1, 3: 1, 4: 1, 5: 2, 6: 2, 7: 2, 8: 2, 9: 3, 10: 3, 11: 3, 12: 3,
                  13: 4, 14: 5, 15: 6}

COARSE_TO_REGIME = {1: "peg", 2: "peg", 3: "band", 4: "float", 5: "free_fall", 6: "dual_market"}

REGIME_ENUM = ["peg", "band", "float", "free_fall", "union", "dual_market"]

NATION_ALIASES = {"NED": "NLD"}

# áreas que não estão na lista de países do World Bank mas têm série no IFS
NATION_ISO2_FALLBACK = {"TWN": "TW", "PSX": "PS", "XKX": "XK", "SAH": "", "SLS": ""}

SHARED_SERIES = {
    "EUR": {"legacy_from": "DEM", "legacy_nation": "DEU", "factor": 1.95583,
            "area": "U2", "area_from": 1999,
            "note": ("pre-1999 = DEM/USD dividido pela paridade irrevogavel 1 EUR = 1,95583 DEM; "
                     "1999+ = area euro do IFS (U2)")}
}

# moeda usada por vários países: a série canônica é a do emissor/âncora. Os demais países
# só contribuem em anos ausentes E se a taxa for compatível (união monetária / peg).
PRIMARY_NATION = {
    "USD": "USA", "EUR": "DEU", "ILS": "ISR", "XOF": "SEN", "XAF": "CMR",
    "XCD": "ATG", "MAD": "MAR", "ZAR": "ZAF",
}
CONSISTENT_YEARS = 2
CONSISTENT_TOL = 0.05


def log(msg: str) -> None:
    print(msg, flush=True)


def cache_path(*parts: str) -> str:
    p = os.path.join(CACHE, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def http_get(url: str, timeout: int = 90) -> bytes:
    ctx = ssl._create_unverified_context()
    req = urllib.request.Request(url, headers={"User-Agent": "opengs-fx-builder/1.0"})
    last = None
    for attempt in range(4):
        try:
            return urllib.request.urlopen(req, timeout=timeout, context=ctx).read()
        except Exception as exc:  # noqa: BLE001 - rede instavel, registra e tenta de novo
            last = exc
            time.sleep(2 + 3 * attempt)
    raise RuntimeError(f"falha ao baixar {url}: {last}")


def fetch_cached(url: str, cache_file: str, refresh: bool) -> bytes:
    if not refresh and os.path.exists(cache_file) and os.path.getsize(cache_file) > 0:
        with open(cache_file, "rb") as fh:
            return fh.read()
    data = http_get(url)
    with open(cache_file, "wb") as fh:
        fh.write(data)
    return data


def fetch_json(url: str, cache_file: str, refresh: bool):
    return json.loads(fetch_cached(url, cache_file, refresh).decode("utf-8"))


def load_currencies() -> dict:
    with open(CURRENCIES, "r", encoding="utf-8-sig") as fh:
        return json.load(fh)


# --------------------------------------------------------------------------- fontes


def fetch_wb_countries(refresh: bool) -> Dict[str, dict]:
    url = "https://api.worldbank.org/v2/country?format=json&per_page=400"
    data = fetch_json(url, cache_path("worldbank", "countries.json"), refresh)
    out = {}
    for row in data[1]:
        iso3 = row.get("id")
        if not iso3:
            continue
        out[iso3] = {"iso2": (row.get("iso2Code") or "").strip(), "name": row.get("name")}
    return out


def fetch_ifs_all(refresh: bool) -> Dict[str, Dict[int, float]]:
    """Todas as séries anuais de câmbio do IFS numa única chamada (226 áreas)."""
    url = ("https://api.db.nomics.world/v22/series/IMF/IFS?limit=250&observations=1&dimensions="
           "%7B%22FREQ%22%3A%5B%22A%22%5D%2C%22INDICATOR%22%3A%5B%22ENDA_XDC_USD_RATE%22%5D%7D")
    data = fetch_json(url, cache_path("ifs", "bulk_annual.json"), refresh)
    docs = data.get("series", {}).get("docs") or []
    out: Dict[str, Dict[int, float]] = {}
    for doc in docs:
        parts = (doc.get("series_code") or "").split(".")
        if len(parts) < 3:
            continue
        values: Dict[int, float] = {}
        for period, value in zip(doc.get("period", []), doc.get("value", [])):
            if value in (None, "", "NA"):
                continue
            try:
                year, rate = int(str(period)[:4]), float(value)
            except (TypeError, ValueError):
                continue
            if rate > 0:
                values[year] = rate
        out[parts[1]] = values
    return out


def fetch_wb_all(iso3_list: Iterable[str], refresh: bool) -> Dict[str, Dict[int, float]]:
    """PA.NUS.FCRF de todos os países necessários, em lotes de 40 (evita 502 do WB)."""
    codes = sorted({c for c in iso3_list if c})
    out: Dict[str, Dict[int, float]] = {}
    for start in range(0, len(codes), 40):
        chunk = codes[start:start + 40]
        url = ("https://api.worldbank.org/v2/country/" + ";".join(chunk) +
               "/indicator/PA.NUS.FCRF?format=json&per_page=20000&date=1950:2025")
        name = f"bulk_fcrf_{start:04d}.json"
        path = os.path.join(CACHE, "worldbank", name)
        try:
            data = fetch_json(url, path, refresh)
            if not (isinstance(data, list) and len(data) > 1 and data[1] is not None):
                raise RuntimeError(f"resposta invalida: {str(data)[:120]}")
        except RuntimeError as exc:
            if os.path.exists(path):
                os.remove(path)  # nao guarda resposta de erro no cache
            log(f"  aviso: lote {start} do World Bank falhou ({exc}); seguindo")
            continue
        for row in data[1]:
            value = row.get("value")
            country = row.get("country") or {}
            iso3 = row.get("countryiso3code") or country.get("id")
            if value is None or not iso3:
                continue
            try:
                year, rate = int(row["date"]), float(value)
            except (TypeError, ValueError, KeyError):
                continue
            if rate > 0:
                out.setdefault(iso3, {})[year] = rate
    return out


def load_jst(refresh: bool) -> Dict[str, Dict[int, float]]:
    """xrusd = moeda local por USD (conferido contra IFS 1950)."""
    import openpyxl

    path = cache_path("JSTdatasetR6.xlsx")
    if refresh or not os.path.exists(path):
        url = "https://www.macrohistory.net/app/download/9834512569/JSTdatasetR6.xlsx"
        with open(path, "wb") as fh:
            fh.write(http_get(url, timeout=180))
    wb = openpyxl.load_workbook(path, read_only=True, data_only=True)
    rows = list(wb[wb.sheetnames[0]].iter_rows(values_only=True))
    wb.close()
    ix = {h: i for i, h in enumerate(list(rows[0]))}
    out: Dict[str, Dict[int, float]] = {}
    for row in rows[1:]:
        iso3, year, rate = row[ix["iso"]], row[ix["year"]], row[ix["xrusd"]]
        if iso3 is None or year is None or rate is None:
            continue
        try:
            y, r = int(year), float(rate)
        except (TypeError, ValueError):
            continue
        if r > 0:
            out.setdefault(str(iso3), {})[y] = r
    return out


def _irr_columns(rows: list) -> Dict[int, Optional[str]]:
    """Mapeia coluna -> ISO3 no worksheet de classificação (nomes quebrados em 2 linhas)."""
    import openpyxl

    wb = openpyxl.load_workbook(cache_path("irr", "236_data.xlsx"), read_only=True, data_only=True)
    master = list(wb["Master"].iter_rows(values_only=True))
    wb.close()
    pairs = [(str(c).strip(), str(n).strip()) for c, n in zip(master[6][1:], master[7][1:]) if c]
    names = [p[1] for p in pairs]

    label_row, name_row = rows[4], rows[5]
    cols = []
    for col in range(2, len(name_row)):
        a = str(label_row[col]).strip() if col < len(label_row) and label_row[col] else ""
        b = str(name_row[col]).strip() if name_row[col] else ""
        if not a and not b:
            continue
        nm = f"{a} {b}" if b.lower().startswith("and ") and a else (b or a)
        cols.append((col, nm))

    mapping: Dict[int, Optional[str]] = {}
    k = 0
    for col, nm in cols:
        best, bi = 0.0, None
        for cand in range(k, min(len(names), k + 6)):
            ratio = difflib.SequenceMatcher(None, nm.lower(), names[cand].lower()).ratio()
            if nm.lower() in names[cand].lower() or names[cand].lower() in nm.lower():
                ratio = max(ratio, 0.95)
            if ratio > best:
                best, bi = ratio, cand
        if best >= 0.8:
            mapping[col] = pairs[bi][0]
            k = bi + 1
        else:
            mapping[col] = None

    # preenche as colunas não casadas com os ISO3 "pulados" da lista de referência
    ordered = sorted(mapping)
    anchor_idx = {code: i for i, (code, _) in enumerate(pairs)}
    matched = [(c, mapping[c]) for c in ordered if mapping[c] is not None]
    for (c1, code1), (c2, code2) in zip(matched, matched[1:]):
        between = [c for c in ordered if c1 < c < c2]
        skipped = [pairs[i][0] for i in range(anchor_idx[code1] + 1, anchor_idx[code2])]
        if len(between) == len(skipped):
            for c, code in zip(between, skipped):
                mapping[c] = code
    return mapping


def load_irr(refresh: bool) -> Dict[str, Dict[int, int]]:
    """ISO3 -> ano -> código fine (1..15)."""
    import openpyxl

    irr_dir = os.path.join(CACHE, "irr")
    os.makedirs(irr_dir, exist_ok=True)
    for fname in ("236_data.xlsx", "238_data.xlsx"):
        path = os.path.join(irr_dir, fname)
        if refresh or not os.path.exists(path):
            url = f"https://carmenreinhart.com/wp-content/uploads/2020/02/{fname}"
            with open(path, "wb") as fh:
                fh.write(http_get(url, timeout=180))

    wb = openpyxl.load_workbook(os.path.join(irr_dir, "238_data.xlsx"), read_only=True, data_only=True)
    rows = list(wb["Fine"].iter_rows(values_only=True))
    wb.close()

    mapping = _irr_columns(rows)
    year_rows = {}
    for row in rows[6:]:
        label = row[1]
        if isinstance(label, (int, float)) and 1940 <= int(label) <= 2016:
            year_rows[int(label)] = row

    out: Dict[str, Dict[int, int]] = {}
    for col, iso3 in mapping.items():
        if not iso3:
            continue
        for year, row in year_rows.items():
            if col >= len(row):
                continue
            code = row[col]
            if isinstance(code, (int, float)) and int(code) in IRR_FINE:
                out.setdefault(iso3, {})[year] = int(code)
    return out


# --------------------------------------------------------------------------- montagem


def rnd(value: float) -> float:
    """12 algarismos significativos: JSON estável sem perder 1e-14."""
    if value == 0:
        return 0.0
    return float(f"{value:.12g}")


# ------------------------------------------------------- reconstrução histórica


def load_facts(path: str = FACTS) -> Dict[str, dict]:
    """Fatos documentados por moeda (tools/data/fx_historical_facts.json)."""
    if not os.path.exists(path):
        log(f"aviso: {path} ausente; reconstrucao historica desativada")
        return {}
    with open(path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    if doc.get("source") != RECON_SOURCE:
        raise RuntimeError(f"{path}: campo source deve ser '{RECON_SOURCE}'")
    return doc.get("currencies") or {}


def reconstruct_currency(code: str, facts: dict, merged: Dict[int, Tuple[float, str]]) -> Tuple[Dict[int, dict], dict]:
    """Expande os fatos documentados em anos ausentes de `merged`.

    Regras: (a) ano que já existe na fonte estatística nunca é tocado; (b) interpolação só
    entre dois anos com fato documentado, marcada com `interpolated`; (c) lacuna maior que
    MAX_INTERP_GAP anos não é interpolada (fica ausente, e o motivo vai para `notes`).
    """
    refs = facts.get("refs") or {}
    used_refs: Dict[str, dict] = {}
    notes: list = []

    def ref_of(ref_id: Optional[str], where: str) -> str:
        if not ref_id or ref_id not in refs:
            raise ValueError(f"{code}: ref {ref_id!r} usada em {where} nao existe em refs")
        used_refs[ref_id] = refs[ref_id]
        return ref_id

    rows: Dict[int, dict] = {}
    for point in facts.get("points") or []:
        where = f"points/{point.get('year')}"
        ref = ref_of(point.get("ref"), where)
        first = int(point["year"])
        last = int(point.get("to_year", first))
        rate = float(point["rate"])
        if rate <= 0:
            raise ValueError(f"{code}: taxa nao positiva em {where}")
        for year in range(first, last + 1):
            if year in merged or year in rows:
                continue  # dado estatístico (ou fato anterior) vence
            rows[year] = {
                "rate": rate,
                "method": point["method"],
                "ref": ref,
                "basis": point.get("basis", "documented"),
                "note": point.get("note", ""),
            }

    def rate_at(year: int) -> Optional[float]:
        if year in merged:
            return merged[year][0]
        if year in rows:
            return rows[year]["rate"]
        return None

    interpolated_years = []
    for spec in facts.get("interpolate") or []:
        a, b = int(spec["from_year"]), int(spec["to_year"])
        ref = ref_of(spec.get("ref"), f"interpolate/{a}-{b}")
        va, vb = rate_at(a), rate_at(b)
        if va is None or vb is None or va <= 0 or vb <= 0:
            notes.append(f"interpolacao {a}-{b} descartada (falta fato em uma das pontas)")
            continue
        missing = [y for y in range(a + 1, b) if y not in merged and y not in rows]
        if not missing:
            continue
        if len(missing) > MAX_INTERP_GAP:
            notes.append(f"interpolacao {a}-{b} descartada ({len(missing)} anos > limite {MAX_INTERP_GAP})")
            continue
        la, lb = math.log(va), math.log(vb)
        for year in missing:
            t = (year - a) / float(b - a)
            rows[year] = {
                "rate": rnd(math.exp(la + t * (lb - la))),
                "method": "interpolated_between_documented_facts",
                "ref": ref,
                "basis": "interpolated",
                "note": spec.get("note", ""),
                "interpolated": True,
                "anchor": [a, b],
            }
            interpolated_years.append(year)

    regimes = []
    for spec in facts.get("regimes") or []:
        if spec.get("regime") not in REGIME_ENUM:
            raise ValueError(f"{code}: regime invalido {spec.get('regime')!r}")
        regimes.append({
            "from": int(spec["from"]),
            "to": int(spec["to"]),
            "regime": spec["regime"],
            "note": spec.get("note", ""),
            "ref": ref_of(spec.get("ref"), f"regimes/{spec['from']}"),
        })

    meta = {
        "source": RECON_SOURCE,
        "unit_note": facts.get("unit_note", ""),
        "basis_note": facts.get("basis_note", ""),
        "notes": list(facts.get("notes") or []) + notes,
        "refs": used_refs,
        "stat_breaks": sorted({int(y) for y in facts.get("stat_breaks") or []}),
        "interpolated_years": sorted(interpolated_years),
        "regimes": regimes,
    }
    return rows, meta


def nation_series(nation: str, ctx: dict) -> Tuple[Dict[int, Tuple[float, str]], list]:
    notes = []
    info = ctx["wb_countries"].get(nation)
    iso2 = info["iso2"] if info else NATION_ISO2_FALLBACK.get(nation, "")
    series: Dict[int, Tuple[float, str]] = {}
    ifs_series = ctx["ifs"].get(iso2, {}) if iso2 else {}
    wb_series = ctx["wb"].get(nation, {})
    jst_series = ctx["jst"].get(nation, {})

    for year, rate in ifs_series.items():
        series[year] = (rate, "imf_ifs")
    for year in sorted(wb_series):
        rate = wb_series[year]
        if year in series:
            old, _ = series[year]
            if abs(rate - old) / old > CONFLICT_TOL:
                notes.append(f"{nation} {year}: worldbank {rate:.6g} difere de imf_ifs {old:.6g}")
            continue
        series[year] = (rate, "worldbank")

    if series and jst_series:
        anchor_year = min(series)
        anchor = series[anchor_year][0]
        candidates = {y: r for y, r in jst_series.items() if TARGET_START <= y < anchor_year}
        if candidates:
            probe = max(candidates)
            ratio = candidates[probe] / anchor if anchor else 0
            if SPLICE_MIN <= ratio <= SPLICE_MAX:
                for year, rate in candidates.items():
                    series[year] = (rate, "jst_r6")
            else:
                notes.append(f"{nation}: extensao JST descartada (razao {ratio:.3f} com {anchor_year})")
    return series, notes


def build_currency(code: str, entry: dict, ctx: dict) -> Tuple[dict, dict]:
    nations = [NATION_ALIASES.get(n, n) for n in entry.get("nations", [])]
    missing_area, notes = [], []
    per_nation: Dict[str, Dict[int, Tuple[float, str]]] = {}
    for nation in nations:
        series, nation_notes = nation_series(nation, ctx)
        notes.extend(nation_notes)
        per_nation[nation] = series
        if not (ctx["wb_countries"].get(nation, {}).get("iso2") or NATION_ISO2_FALLBACK.get(nation)):
            missing_area.append(nation)

    primary = PRIMARY_NATION.get(code)
    if primary not in per_nation or not per_nation.get(primary):
        primary = next((n for n in nations if per_nation.get(n)), nations[0] if nations else "")
    merged: Dict[int, Tuple[float, str]] = dict(per_nation.get(primary, {}))

    for nation in nations:
        if nation == primary:
            continue
        for year, (rate, src) in per_nation.get(nation, {}).items():
            if year in merged:
                continue
            near = [(abs(year - y), y) for y in merged if abs(year - y) <= CONSISTENT_YEARS]
            if near:
                _, y0 = min(near)
                if abs(math.log(rate / merged[y0][0])) > CONSISTENT_TOL:
                    notes.append(f"{nation} {year}: {rate:.6g} incompativel com {primary} {merged[y0][0]:.6g}; ignorado")
                    continue
            merged[year] = (rate, src)

    shared = SHARED_SERIES.get(code)
    if shared:
        legacy = ctx["legacy"].get(shared["legacy_from"], {})
        if legacy:
            factor = shared["factor"]
            for year, (rate, _src) in legacy.items():
                if year < 1999:
                    merged[year] = (rate / factor, f"derived:{shared['legacy_from'].lower()}")
            notes.append(shared["note"])
        else:
            notes.append("encadeamento legado indisponivel")
        area = shared.get("area")
        if area:
            area_series = ctx["ifs"].get(area, {})
            if area_series:
                for year, rate in area_series.items():
                    if year >= shared.get("area_from", 0):
                        merged[year] = (rate, "imf_ifs")
            else:
                notes.append(f"area {area} ausente no IFS")

    regime_by_year: Dict[int, dict] = {}
    for year in sorted(merged):
        for nation in nations:
            fine = ctx["irr"].get(nation, {}).get(year)
            if fine:
                coarse = FINE_TO_COARSE[fine]
                regime = "union" if fine == 1 else COARSE_TO_REGIME[coarse]
                regime_by_year[year] = {"regime": regime, "fine": fine, "coarse": coarse,
                                        "source": "irr"}
                break

    recon_rows, recon_meta = {}, None
    facts = ctx.get("facts", {}).get(code)
    if facts:
        recon_rows, recon_meta = reconstruct_currency(code, facts, merged)
        notes.extend(recon_meta["notes"])
        for spec in recon_meta["regimes"]:
            for year in range(spec["from"], spec["to"] + 1):
                if year in regime_by_year:
                    continue  # IRR (estatístico) vence
                regime_by_year[year] = {"regime": spec["regime"], "fine": None,
                                        "note": spec["note"], "source": RECON_SOURCE}

    stat_break_years = set(recon_meta["stat_breaks"]) if recon_meta else set()
    series_out, breaks, prev = [], [], None
    for year in sorted(set(merged) | set(recon_rows)):
        if year in merged:
            rate, src = merged[year]
            row = {"year": year, "rate": rnd(rate), "source": src}
        else:
            spec = recon_rows[year]
            rate, src = spec["rate"], RECON_SOURCE
            row = {"year": year, "rate": rnd(rate), "source": RECON_SOURCE,
                   "method": spec["method"], "ref": spec["ref"], "basis": spec["basis"]}
            if spec.get("interpolated"):
                row["interpolated"] = True
                row["anchor"] = spec["anchor"]
        regime = regime_by_year.get(year)
        if regime:
            row["regime"] = regime["regime"]
            if regime.get("fine"):
                row["regime_fine"] = regime["fine"]
            if regime.get("source") and regime["source"] != "irr":
                row["regime_source"] = regime["source"]
            if regime.get("note"):
                row["regime_note"] = regime["note"]
        series_out.append(row)
        if prev and prev[0] == year - 1 and prev[1] > 0:
            change = math.log(rate / prev[1])
            if year in stat_break_years:
                breaks.append({"year": year, "log_change": rnd(change), "definitional": True,
                               "note": ("mudanca de base/fonte (oficial->mercado ou reforma monetaria): "
                                        "variacao excluida das estatisticas do perfil")})
            elif src == prev[2] and abs(change) > math.log(3):
                breaks.append({"year": year, "log_change": rnd(change),
                               "note": "salto >3x entre anos consecutivos na mesma fonte"})
        prev = (year, rate, src)

    history = {
        "version": 1,
        "code": code,
        "nations": entry.get("nations", []),
        "unit": "LCU per USD (annual average, unidade monetaria corrente da fonte)",
        "first_year": series_out[0]["year"] if series_out else None,
        "last_year": series_out[-1]["year"] if series_out else None,
        "sources": sorted({row["source"] for row in series_out}),
        "source_refs": SOURCES,
        "regime_enum": REGIME_ENUM,
        "regime_note": ("Ilzetzki-Reinhart-Rogoff (fine -> coarse); 'union' quando fine=1. "
                        "2017-2025: regime de fato documentado (historical_reconstruction), "
                        "ver `reconstruction.regimes`; linha sem `regime` fica sem classificacao."),
        "breaks": breaks,
        "notes": notes[:40],
        "missing_source_area": missing_area,
        "no_data": not series_out,
        "series": series_out,
    }
    if recon_meta:
        history["reconstruction"] = recon_meta
    return history, {"merged": merged, "regime": regime_by_year}


def compute_profile(code: str, entry: dict, history: dict) -> dict:
    series = history["series"]
    years = [row["year"] for row in series]
    values = {row["year"]: row["rate"] for row in series}
    stat_breaks = {b["year"] for b in history.get("breaks") or [] if b.get("definitional")}

    change_rows = [(y, math.log(values[y] / values[y - 1])) for y in years
                   if y - 1 in values and y not in stat_breaks
                   and values[y] > 0 and values[y - 1] > 0]
    changes = [c for _y, c in change_rows]

    def stdev(xs: Iterable[float]) -> Optional[float]:
        xs = list(xs)
        if len(xs) < 2:
            return None
        mean = sum(xs) / len(xs)
        return math.sqrt(sum((x - mean) ** 2 for x in xs) / (len(xs) - 1))

    def quantile(xs: list, q: float) -> Optional[float]:
        if not xs:
            return None
        s = sorted(xs)
        pos = (len(s) - 1) * q
        lo, hi = int(math.floor(pos)), int(math.ceil(pos))
        return s[lo] if lo == hi else s[lo] + (s[hi] - s[lo]) * (pos - lo)

    # meia-vida de reversão: AR(1) dos desvios do log do nível contra a média móvel
    # centrada de 10 anos (proxy nominal do cambio real; nao usa nivel absoluto)
    half_life = None
    logs = {y: math.log(values[y]) for y in years if values[y] > 0}
    if len(logs) >= 25:
        devs: Dict[int, float] = {}
        for y in sorted(logs):
            window = [logs[k] for k in range(y - 5, y + 6) if k in logs]
            if len(window) >= 7:
                devs[y] = logs[y] - sum(window) / len(window)
        pairs = [(devs[y - 1], devs[y]) for y in devs if y - 1 in devs]
        if len(pairs) >= 10:
            den = sum(a * a for a, _ in pairs)
            phi = sum(a * b for a, b in pairs) / den if den > 0 else 0
            if 0 < phi < 1:
                half_life = math.log(0.5) / math.log(phi)

    regime_dist: Dict[str, int] = {}
    for row in series:
        if "regime" in row:
            regime_dist[row["regime"]] = regime_dist.get(row["regime"], 0) + 1
    recent = [row["regime"] for row in series if 2007 <= row["year"] <= 2016 and "regime" in row]
    regime_recent = None
    if recent:
        counts: Dict[str, int] = {}
        for r in recent:
            counts[r] = counts.get(r, 0) + 1
        regime_recent = max(sorted(counts), key=lambda k: counts[k])

    span = PRIMARY_END - PRIMARY_START + 1
    coverage = sum(1 for y in range(PRIMARY_START, PRIMARY_END + 1) if y in values) / span
    if len(years) >= 60 and coverage >= 0.95:
        conf_base = "complete"
    elif len(years) >= 20 and coverage >= 0.5:
        conf_base = "partial"
    else:
        conf_base = "estimated"

    recon_years = [row["year"] for row in series if row["source"] == RECON_SOURCE]
    interp_years = [row["year"] for row in series if row.get("interpolated")]
    recon_fraction = (len(recon_years) / len(series)) if series else 0.0
    recon_fraction_window = (sum(1 for y in recon_years if PRIMARY_START <= y <= PRIMARY_END) / span)

    # a confiança nunca sobe por reconstrução: fração reconstruída limita o teto
    if recon_fraction >= 0.5:
        conf = "estimated"
    elif recon_fraction >= 0.1 and conf_base == "complete":
        conf = "partial"
    else:
        conf = conf_base

    flags = []
    if not series:
        flags.append("no_data")
    if history["first_year"] and history["first_year"] < PRIMARY_START:
        flags.append("extended_pre1950:jst_r6")
    if any(src.startswith("derived:") for src in history["sources"]):
        flags.append("chained:legacy_currency")
    if history["missing_source_area"]:
        flags.append("missing_source_area:" + ",".join(history["missing_source_area"]))
    if history["breaks"]:
        flags.append("structural_breaks")
    if recon_years:
        flags.append(f"reconstructed:{len(recon_years)}y")
    if interp_years:
        flags.append(f"interpolated:{len(interp_years)}y")

    regime_latest = None
    for row in reversed(series):
        if "regime" in row:
            regime_latest = row["regime"]
            break

    vol = stdev(changes)
    # volatilidade só com anos documentados (estatística ou fato): isola o efeito da interpolação
    documented = {row["year"] for row in series if not row.get("interpolated")}
    doc_changes = [c for y, c in change_rows if y in documented and (y - 1) in documented]
    vol_doc = stdev(doc_changes)
    median = quantile(changes, 0.5)
    p95 = quantile([abs(c) for c in changes], 0.95)
    return {
        "nations": entry.get("nations", []),
        "years_with_data": len(years),
        "first_year": history["first_year"],
        "last_year": history["last_year"],
        "coverage_1950_2024": rnd(coverage),
        "volatility_annual_log": rnd(vol) if vol is not None else None,
        "volatility_documented_only_log": rnd(vol_doc) if vol_doc is not None else None,
        "median_annual_log_change": rnd(median) if median is not None else None,
        "p95_abs_annual_log_change": rnd(p95) if p95 is not None else None,
        "max_1y_depreciation_log": rnd(max(changes)) if changes else None,
        "crisis_frequency": rnd(sum(1 for c in changes if c > math.log(1 + CRISIS_THRESHOLD)) / len(changes))
        if changes else None,
        "crisis_threshold_log": rnd(math.log(1 + CRISIS_THRESHOLD)),
        "mean_reversion_half_life_years": rnd(half_life) if half_life else None,
        "regime_recent": regime_recent,
        "regime_window": [2007, 2016],
        "regime_latest": regime_latest,
        "regime_distribution": dict(sorted(regime_dist.items())),
        "reconstructed_years": len(recon_years),
        "reconstructed_fraction": rnd(recon_fraction),
        "reconstructed_fraction_1950_2024": rnd(recon_fraction_window),
        "interpolated_years": len(interp_years),
        "stat_break_years": sorted(stat_breaks),
        "confidence_base": conf_base,
        "confidence": conf,
        "flags": flags,
    }


def main(argv: Optional[list] = None) -> int:
    ap = argparse.ArgumentParser(description="Gera fx_history/<CODE>.json e fx_profiles.json")
    ap.add_argument("--refresh", action="store_true", help="ignora o cache e rebaixa")
    ap.add_argument("--check", action="store_true", help="nao escreve nada, so reporta cobertura")
    ap.add_argument("--only", default="", help="restringe a uma moeda (debug)")
    args = ap.parse_args(argv)

    os.makedirs(CACHE, exist_ok=True)
    data = load_currencies()
    currencies = data.get("currencies", [])
    log(f"moedas em currencies.json: {len(currencies)}")

    wb_countries = fetch_wb_countries(args.refresh)
    log(f"paises World Bank: {len(wb_countries)}")

    needed = sorted({NATION_ALIASES.get(n, n) for c in currencies for n in c.get("nations", [])})
    for extra in ("DEU",):
        if extra not in needed:
            needed.append(extra)
    ifs = fetch_ifs_all(args.refresh)
    log(f"IFS: {len(ifs)} areas")
    wb = fetch_wb_all([n for n in needed if n in wb_countries], args.refresh)
    log(f"World Bank: {len(wb)} paises")

    jst = load_jst(args.refresh)
    log(f"JST xrusd: {len(jst)} paises")
    irr = load_irr(args.refresh)
    log(f"IRR classificacao: {len(irr)} paises")

    ctx = {"wb_countries": wb_countries, "ifs": ifs, "wb": wb, "jst": jst, "irr": irr, "legacy": {},
           "facts": load_facts()}
    log(f"fatos de reconstrucao historica: {len(ctx['facts'])} moedas")
    for shared in SHARED_SERIES.values():
        ctx["legacy"][shared["legacy_from"]], _n = nation_series(shared["legacy_nation"], ctx)

    histories, profiles = {}, {}
    for entry in currencies:
        code = entry["code"]
        if args.only and code != args.only:
            continue
        history, _extra = build_currency(code, entry, ctx)
        histories[code] = history
        profiles[code] = compute_profile(code, entry, history)

    def at_least(n: int) -> int:
        return sum(1 for p in profiles.values() if (p["years_with_data"] or 0) >= n)

    summary = {
        "currencies": len(profiles),
        "at_least_50_years": at_least(50),
        "at_least_75_years": at_least(75),
        "at_least_100_years": at_least(100),
        "complete": sum(1 for p in profiles.values() if p["confidence"] == "complete"),
        "partial": sum(1 for p in profiles.values() if p["confidence"] == "partial"),
        "estimated": sum(1 for p in profiles.values() if p["confidence"] == "estimated"),
        "target_window": [TARGET_START, TARGET_END],
        "primary_window": [PRIMARY_START, PRIMARY_END],
        "historical_reconstruction": {
            "currencies": sum(1 for p in profiles.values() if p["reconstructed_years"]),
            "years": sum(p["reconstructed_years"] for p in profiles.values()),
            "interpolated_years": sum(p["interpolated_years"] for p in profiles.values()),
            "downgraded": sum(1 for p in profiles.values() if p["confidence"] != p["confidence_base"]),
        },
    }
    log("cobertura: " + json.dumps(summary, ensure_ascii=False))

    if args.check:
        return 0

    os.makedirs(OUT_DIR, exist_ok=True)
    for code, history in histories.items():
        with open(os.path.join(OUT_DIR, f"{code}.json"), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(history, fh, ensure_ascii=False, indent=1)
            fh.write("\n")

    out = {
        "version": 1,
        "generated_by": "tools/build_fx_history.py",
        "unit": "LCU per USD",
        "method": {
            "volatility_annual_log": "desvio-padrao amostral de ln(taxa_t / taxa_{t-1})",
            "volatility_documented_only_log": ("o mesmo desvio-padrao, excluindo anos interpolados "
                                               "(isola o efeito da interpolacao; None se sobrar <2 variacoes)"),
            "max_1y_depreciation_log": "maior ln(taxa_t / taxa_{t-1}) (positivo = depreciacao)",
            "crisis_frequency": "fracao dos anos com depreciacao > 25%",
            "mean_reversion_half_life_years": "AR(1) no log do nivel nominal; None se phi fora de (0,1)",
            "regime_recent": "moda do regime de fato 2007-2016 (IRR)",
            "regime_latest": "regime do ultimo ano com classificacao (IRR ate 2016; fatos documentados em 2017-2025)",
            "confidence": ("base: complete = >=60 anos e >=95% de 1950-2024; partial = >=20 anos e >=50%; "
                           "estimated = fora disso. Teto por reconstrucao: fracao reconstruida >= 0,5 -> "
                           "estimated; >= 0,1 -> no maximo partial (confianca nunca sobe por reconstrucao)"),
            "reconstructed_fraction": "anos com source=historical_reconstruction / years_with_data",
            "stat_break_years": "anos cuja variacao e mudanca de base/fonte e fica fora de volatilidade/crise",
        },
        "reconstruction": {
            "facts_file": "tools/data/fx_historical_facts.json",
            "source": RECON_SOURCE,
            "rules": {
                "never_overwrite_statistical_data": True,
                "interpolation_max_gap_years": MAX_INTERP_GAP,
                "interpolation": "somente entre dois fatos documentados, marcada com interpolated=true",
            },
        },
        "sources": SOURCES,
        "irr_fine_codes": {str(k): v for k, v in IRR_FINE.items()},
        "coverage": summary,
        "currencies": dict(sorted(profiles.items())),
    }
    with open(OUT_PROFILES, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, ensure_ascii=False, indent=1)
        fh.write("\n")
    log(f"escritos {len(histories)} arquivos em {OUT_DIR} e {OUT_PROFILES}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
