#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gera data/definitions/population/age_structure_2026.json a partir do WPP2024.

Fonte unica:

  UN DESA, Population Division. World Population Prospects 2024 (WPP2024),
  Standard Projections, variante "Medium" (projecao), ano 2026.
  https://population.un.org/wpp/  (arquivos CSV em .../assets/Excel Files/1_Indicator (Standard)/CSV_FILES/)

Arquivos usados (baixados crus em tools/data/, gitignored; SHA-256 fica no JSON):

  WPP2024_PopulationByAge5GroupSex_Medium.csv.gz               faixas de 5 anos, ambos os sexos
  WPP2024_PopulationBySingleAgeSex_Medium_2024-2100.csv.gz     idade simples (fracao 18-49)
  WPP2024_Demographic_Indicators_Medium.csv.gz                 TFR, CBR, CDR, populacao total

Regras:

  * Nenhum numero e inventado. Todo valor vem de linha `LocTypeID == 4` (pais) e
    `Time == 2026` do WPP2024. Nada de estimativa regional.
  * Faixas: 0-14, 15-24, 25-44, 45-64, 65+ como FRACAO da populacao (soma = 1).
  * `frac_18_49` (reserva de recrutamento) vem da idade simples, nao das faixas de 5 anos.
  * Codigos do jogo que nao existem na ONU sao resolvidos explicitamente:
      NED -> NLD, PSX -> PSE, SAH -> ESH   method "iso_alias"
      SOL -> SOM (Somalilandia nao e entidade ONU) method "proxy_parent":
          estrutura etaria e taxas da Somalia; `parent_share` = populacao do jogo
          SOL / (SOL + SOM) para ratear a populacao-mae. confidence "low".
  * SAH -> ESH recebe confidence "medium": a populacao ONU de Western Sahara inclui
    os campos de refugiados de Tindouf, nao so o territorio.
  * `retrieved` e a data de COLETA do dado (constante), nao a data de geracao: o
    script e deterministico e o JSON nao muda entre execucoes com os mesmos arquivos.
  * Populacao total: `population_un_2026` e a do WPP2024 (pessoas). Os totais usados
    pela simulacao continuam vindo de data/definitions/scenario/nations.json
    (vintage diferente); este arquivo fornece FRACOES, nao totais.

Uso:
    python tools/build_age_structure.py            # escreve o JSON
    python tools/build_age_structure.py --check    # nao escreve; reporta cobertura
"""

from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import os
import sys
from typing import Dict, List, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "tools", "data")
NATIONS_JSON = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
OUT_JSON = os.path.join(ROOT, "data", "definitions", "population", "age_structure_2026.json")

YEAR = "2026"
RETRIEVED = "2026-11-17"

WPP_BASE = (
    "https://population.un.org/wpp/assets/Excel%20Files/"
    "1_Indicator%20(Standard)/CSV_FILES"
)
WPP_LANDING = "https://population.un.org/wpp/"

FILES: Dict[str, Tuple[str, str]] = {
    "age5": (
        "WPP2024_PopulationByAge5GroupSex_Medium.csv.gz",
        "faixas de 5 anos por sexo, ambos os sexos usados (PopTotal)",
    ),
    "single_age": (
        "WPP2024_PopulationBySingleAgeSex_Medium_2024-2100.csv.gz",
        "idade simples 0..100+, usado so para a fracao 18-49",
    ),
    "indicators": (
        "WPP2024_Demographic_Indicators_Medium.csv.gz",
        "TFR, CBR, CDR, populacao total de 1 de julho",
    ),
}

# Codigos do jogo -> codigo ONU + metodo + confianca.
ALIASES: Dict[str, Tuple[str, str, str]] = {
    "NED": ("NLD", "iso_alias", "high"),   # NED e codigo alternativo de Paises Baixos
    "PSX": ("PSE", "iso_alias", "high"),   # Palestine
    "SAH": ("ESH", "iso_alias", "medium"), # inclui campos de refugiados de Tindouf
}
# Nacao sem entidade na ONU: estrutura da nacao-mae + fatia de populacao.
PROXIES: Dict[str, Tuple[str, str, str, str]] = {
    "SOL": ("SOM", "proxy_parent", "low", "Somalilandia nao e entidade WPP; estrutura e taxas da Somalia"),
}

BANDS: List[Tuple[str, int, int]] = [
    ("age_0_14", 0, 14),
    ("age_15_24", 15, 24),
    ("age_25_44", 25, 44),
    ("age_45_64", 45, 64),
    ("age_65_plus", 65, 200),
]

BAND_TOLERANCE = 1e-6


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _year_rows(kind: str):
    """Le um arquivo WPP2024 e devolve so linhas de pais/ano, como dicionarios."""
    name = FILES[kind][0]
    path = os.path.join(DATA_DIR, name)
    if not os.path.exists(path):
        raise SystemExit("arquivo ausente: %s (baixe de %s/%s)" % (path, WPP_BASE, name))
    with gzip.open(path, "rt", encoding="utf-8-sig", newline="") as fh:
        for row in csv.DictReader(fh):
            if row.get("LocTypeID") == "4" and row.get("Time") == YEAR:
                yield row


def read_age5() -> Dict[str, Dict[str, object]]:
    """{ISO3: {'ages': {idade_inicial: pop}, 'locid': str, 'location': str}}"""
    out: Dict[str, Dict[str, object]] = {}
    for row in _year_rows("age5"):
        iso = row["ISO3_code"]
        if not iso:
            continue
        entry = out.setdefault(iso, {"ages": {}, "locid": row["LocID"], "location": row["Location"]})
        entry["ages"][int(row["AgeGrpStart"])] = float(row["PopTotal"])
    return out


def read_single_age() -> Dict[str, Dict[int, float]]:
    out: Dict[str, Dict[int, float]] = {}
    for row in _year_rows("single_age"):
        iso = row["ISO3_code"]
        if not iso:
            continue
        out.setdefault(iso, {})[int(row["AgeGrpStart"])] = float(row["PopTotal"])
    return out


def read_indicators() -> Dict[str, Dict[str, float]]:
    out: Dict[str, Dict[str, float]] = {}
    for row in _year_rows("indicators"):
        iso = row["ISO3_code"]
        if not iso:
            continue
        out[iso] = {
            "tfr": float(row["TFR"]),
            "cbr": float(row["CBR"]),
            "cdr": float(row["CDR"]),
            "pop": float(row["TPopulation1July"]),
        }
    return out


def band_of(age_start: int) -> str:
    for key, lo, hi in BANDS:
        if lo <= age_start <= hi:
            return key
    raise ValueError("idade fora das faixas: %s" % age_start)


def build_nation(
    iso: str,
    method: str,
    confidence: str,
    age5: Dict[str, Dict[str, object]],
    single: Dict[str, Dict[int, float]],
    ind: Dict[str, Dict[str, float]],
    parent_share: float | None,
    note: str | None,
) -> Dict[str, object]:
    src = age5[iso]
    ages: Dict[int, float] = src["ages"]
    totals = {key: 0.0 for key, _, _ in BANDS}
    for start, pop in ages.items():
        totals[band_of(start)] += pop
    total = sum(totals.values())
    if total <= 0:
        raise ValueError("%s: populacao zero" % iso)

    bands = {key: round(totals[key] / total, 9) for key, _, _ in BANDS}
    band_sum = sum(bands.values())
    if abs(band_sum - 1.0) > BAND_TOLERANCE:
        raise ValueError("%s: soma das faixas %.12f" % (iso, band_sum))

    single_ages = single[iso]
    rec_total = sum(single_ages.values())
    rec_1849 = sum(p for age, p in single_ages.items() if 18 <= age <= 49)
    frac_18_49 = round(rec_1849 / rec_total, 9)

    parent_total = round(ind[iso]["pop"] * 1000.0)  # WPP traz em milhares
    pop_un = parent_total if parent_share is None else round(parent_total * parent_share)
    entry: Dict[str, object] = {
        "bands": bands,
        "frac_18_49": frac_18_49,
        "tfr_2026": round(ind[iso]["tfr"], 4),
        "cbr_per_1000": round(ind[iso]["cbr"], 3),
        "cdr_per_1000": round(ind[iso]["cdr"], 3),
        "population_un_2026": pop_un,
        "un_code": iso,
        "un_location": src["location"],
        "un_locid": src["locid"],
        "source": "UN WPP2024 (Medium, 2026)",
        "method": method,
        "confidence": confidence,
    }
    if parent_share is not None:
        entry["parent_share"] = parent_share
        entry["population_parent_2026"] = parent_total
        entry["population_share_basis"] = (
            "nations.json: pop(SOL) / (pop(SOL) + pop(SOM))"
        )
    if note:
        entry["note"] = note
    return entry


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="nao escreve; reporta cobertura")
    args = ap.parse_args()

    with open(NATIONS_JSON, encoding="utf-8") as fh:
        game_nations = json.load(fh)
    game_ids = sorted(n["id"] for n in game_nations)
    game_pop = {n["id"]: int(n.get("population") or 0) for n in game_nations}

    # Fatia de populacao para as nacoes proxy (nations.json, nao inventada).
    sol_share = None
    if "SOL" in game_pop and "SOM" in game_pop and game_pop["SOL"] + game_pop["SOM"] > 0:
        sol_share = round(game_pop["SOL"] / (game_pop["SOL"] + game_pop["SOM"]), 6)

    age5 = read_age5()
    single = read_single_age()
    ind = read_indicators()

    files_meta = []
    for kind, (name, role) in FILES.items():
        path = os.path.join(DATA_DIR, name)
        files_meta.append(
            {
                "file": name,
                "role": role,
                "url": WPP_BASE + "/" + name,
                "bytes": os.path.getsize(path),
                "sha256": sha256_of(path),
            }
        )

    nations: Dict[str, object] = {}
    counts = {"direct": 0, "iso_alias": 0, "proxy_parent": 0}
    problems: List[str] = []

    for code in game_ids:
        if code in ALIASES:
            un_iso, method, confidence = ALIASES[code]
            note = None
            if code == "SAH":
                note = "WPP ESH inclui campos de refugiados de Tindouf; territorio e menor"
            share = None
        elif code in PROXIES:
            un_iso, method, confidence, note = PROXIES[code]
            if sol_share is None:
                problems.append("SOL: sem populacao no nations.json para a fatia")
            share = sol_share
        else:
            un_iso, method, confidence, note, share = code, "direct", "high", None, None

        missing = [k for k, table in (("age5", age5), ("single", single), ("indicators", ind)) if un_iso not in table]
        if missing:
            problems.append("%s -> %s ausente em %s" % (code, un_iso, ",".join(missing)))
            continue

        entry = build_nation(
            un_iso, method, confidence, age5, single, ind, share, note
        )
        nations[code] = entry
        counts[method] = counts.get(method, 0) + 1

    if problems:
        for p in problems:
            print("ERRO: " + p, file=sys.stderr)
        return 1

    out = {
        "version": 1,
        "dataset": "population_age_structure_2026",
        "source": (
            "UN DESA Population Division, World Population Prospects 2024 (WPP2024), "
            "Standard Projections, Medium variant, ano 2026"
        ),
        "url": WPP_LANDING,
        "retrieved": RETRIEVED,
        "year": int(YEAR),
        "variant": "Medium",
        "units": {
            "bands": "fracao da populacao total (soma = 1)",
            "frac_18_49": "fracao da populacao total com 18 a 49 anos",
            "tfr_2026": "nascimentos por mulher",
            "cbr_per_1000": "nascimentos por mil habitantes",
            "cdr_per_1000": "obitos por mil habitantes",
            "population_un_2026": "pessoas (WPP2024, 1 de julho)",
        },
        "bands": [{"key": k, "min_age": lo, "max_age": None if hi > 200 else hi} for k, lo, hi in BANDS],
        "files": files_meta,
        "sha256": {f["file"]: f["sha256"] for f in files_meta},
        "coverage": {
            "nations_total": len(game_ids),
            "nations_with_data": len(nations),
            "by_method": counts,
        },
        "code_resolution": {
            code: {"un_code": spec[0], "method": spec[1]}
            for code, spec in {**ALIASES, **{k: v[:2] for k, v in PROXIES.items()}}.items()
        },
        "nations": nations,
    }

    print(
        "cobertura: %d/%d nacoes (direct=%d, iso_alias=%d, proxy_parent=%d)"
        % (
            len(nations),
            len(game_ids),
            counts.get("direct", 0),
            counts.get("iso_alias", 0),
            counts.get("proxy_parent", 0),
        )
    )
    if args.check:
        return 0

    os.makedirs(os.path.dirname(OUT_JSON), exist_ok=True)
    with open(OUT_JSON, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(out, fh, ensure_ascii=False, indent=2, sort_keys=False)
        fh.write("\n")
    print("escrito: %s" % OUT_JSON)
    print("sha256(json): %s" % sha256_of(OUT_JSON))
    return 0


if __name__ == "__main__":
    sys.exit(main())
