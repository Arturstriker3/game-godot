"""Baixa a ancora nacional de potencial hidreletrico tecnico do Banco Mundial.

Indicador: `WB_CLEAR_HP_TECHNFEASIBLE_PT` — "Technically feasible hydropower
potential developed (%)", base WB_CLEAR, ano 2021. Licenca CC BY 4.0 (metadata
oficial do Data360). O indicador NAO publica o potencial em MW: ele da a fracao
(% do potencial tecnico) ja desenvolvida e instalada. O potencial nacional em MW
e DERIVADO em `build_hydro_national.py` como:

    potencial_mw = capacidade_hidreletrica_instalada_2021_mw / (pct_desenvolvido / 100)

onde a capacidade instalada de 2021 vem da Ember (CC BY 4.0). A derivacao esta
declarada no dado e no doc E1; o denominador (a usina instalada) e de fonte
diferente da usada pelo Banco Mundial, entao o valor carrega esse desvio.

Uso:
    python tools/data/energy/fetch_wb_hydro_anchor.py [--force]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import urllib.parse
import urllib.request
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
CACHE_DIR = os.path.join(ROOT, "tools", "data", "energy", "cache")
DATA_PATH = os.path.join(CACHE_DIR, "wb_hydro_developed_pct.json")
MANIFEST_PATH = os.path.join(CACHE_DIR, "wb_hydro_developed_pct.manifest.json")

API_URL = "https://data360api.worldbank.org/data360/data"
DATABASE_ID = "WB_CLEAR"
INDICATOR = "WB_CLEAR_HP_TECHNFEASIBLE_PT"
PAGE_SIZE = 500
LANDING_URL = ("https://data360.worldbank.org/en/indicator/"
               "WB_CLEAR_HP_TECHNFEASIBLE_PT")
METADATA_URL = ("https://data360files.worldbank.org/data360-data/metadata/WB_CLEAR/"
                "WB_CLEAR_HP_TECHNFEASIBLE_PT.pdf")
LICENSE = {
    "name": "CC BY 4.0",
    "url": "https://creativecommons.org/licenses/by/4.0/",
    "commercial_ok": True,
    "notes": ("Licenca declarada no metadata oficial do indicador; inclui a clausula "
              "de mediacao WIPO em disputas."),
}
CITATION = ("World Bank (2026). Technically feasible hydropower potential developed (%) "
            "[WB_CLEAR_HP_TECHNFEASIBLE_PT]. Data360.")
HTTP_TIMEOUT = 120


def http_json(url: str) -> object:
    request = urllib.request.Request(url, headers={
        "User-Agent": "opengs-game data collection (research; contact: project repo)"})
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
        return json.load(response)


def fetch_rows() -> tuple[list[dict], int]:
    rows: list[dict] = []
    declared = None
    skip = 0
    while True:
        query = urllib.parse.urlencode({
            "DATABASE_ID": DATABASE_ID, "INDICATOR": INDICATOR,
            "top": PAGE_SIZE, "skip": skip,
        })
        payload = http_json("%s?%s" % (API_URL, query))
        if not isinstance(payload, dict):
            raise SystemExit("payload inesperado do Data360: %s" % type(payload).__name__)
        declared = int(payload.get("count") or 0)
        page = payload.get("value") or []
        rows.extend(page)
        if not page or len(rows) >= declared:
            break
        skip += PAGE_SIZE
    unique = {(row.get("REF_AREA"), row.get("TIME_PERIOD")) for row in rows}
    if len(unique) != len(rows) or len(rows) != declared:
        raise SystemExit("paginacao inconsistente no Data360 (linhas=%d, unicas=%d, count=%d)"
                         % (len(rows), len(unique), declared))
    return rows, declared


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    if os.path.exists(DATA_PATH) and os.path.exists(MANIFEST_PATH) and not args.force:
        print("cache presente: %s (use --force para baixar de novo)" % DATA_PATH)
        with open(MANIFEST_PATH, encoding="utf-8") as handle:
            print(json.dumps(json.load(handle), indent=1, ensure_ascii=False))
        return 0

    rows, declared = fetch_rows()
    countries = {}
    for row in rows:
        iso3 = str(row.get("REF_AREA") or "").strip()
        value = row.get("OBS_VALUE")
        period = str(row.get("TIME_PERIOD") or "").strip()
        if not iso3 or value in (None, ""):
            continue
        try:
            numeric = float(value)
        except (TypeError, ValueError):
            continue
        countries[iso3] = {
            "developed_pct": numeric,
            "year": int(period) if period.isdigit() else None,
            "obs_status": row.get("OBS_STATUS"),
        }
    if len(countries) < 140:
        raise SystemExit("poucos paises com potencial desenvolvido (%d)" % len(countries))

    payload = {
        "meta": {
            "source": "World Bank Data360, WB_CLEAR",
            "indicator": INDICATOR,
            "indicator_name": "Technically feasible hydropower potential developed (%)",
            "url": LANDING_URL,
            "metadata_url": METADATA_URL,
            "license": LICENSE,
            "citation": CITATION,
            "definition": ("(Installed hydropower capacity / Technically feasible hydropower "
                           "potential) x 100; potencial tecnico = parcela da capacidade bruta "
                           "teorica exploravel com a tecnologia atual, sem filtro economico "
                           "ou ambiental (Hydropower & Dams World Atlas 2021; complementado "
                           "por World Energy Council quando ausente)."),
            "derivation": ("potencial_mw = capacidade_hidreletrica_instalada_2021_mw / "
                           "(developed_pct / 100); a capacidade instalada de 2021 vem da "
                           "Ember, nao da mesma publicacao do Banco Mundial"),
            "counts": {"rows_api": declared, "countries": len(countries)},
        },
        "countries": dict(sorted(countries.items())),
    }
    os.makedirs(CACHE_DIR, exist_ok=True)
    text = json.dumps(payload, indent=1, ensure_ascii=False) + "\n"
    with open(DATA_PATH, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    sha256 = hashlib.sha256(text.encode("utf-8")).hexdigest()
    manifest = {
        "source": payload["meta"]["source"],
        "indicator": INDICATOR,
        "url": LANDING_URL,
        "api_url": API_URL,
        "accessed_at_utc": datetime.now(timezone.utc).replace(microsecond=0)
        .strftime("%Y-%m-%dT%H:%M:%SZ"),
        "license": LICENSE,
        "citation": CITATION,
        "file": os.path.relpath(DATA_PATH, ROOT).replace("\\", "/"),
        "sha256": sha256,
        "validation": payload["meta"]["counts"],
    }
    with open(MANIFEST_PATH, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n")
    print(json.dumps(manifest, indent=1, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
