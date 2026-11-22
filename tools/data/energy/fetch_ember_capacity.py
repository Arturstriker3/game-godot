"""Baixa a base anual da Ember (capacidade instalada por pais e fonte) para o cache.

Fonte: Ember, *Yearly Electricity Data* (release global). Licenca CC BY 4.0
(https://ember-energy.org/creative-commons/). O CSV tem uma linha por
(area, ano, fonte eletrica) e a coluna `Capacity (GW)`.

O arquivo tem ~16 MB e NAO e versionado: fica em `tools/data/energy/cache/`
com um `manifest.json` de procedencia (URL, SHA-256, data de acesso, licenca).
Este script so baixa e valida o cabecalho; a transformacao para o jogo esta em
`build_national_capacity.py`.

Uso:
    python tools/data/energy/fetch_ember_capacity.py [--force]
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import sys
import urllib.request
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
CACHE_DIR = os.path.join(ROOT, "tools", "data", "energy", "cache")
CSV_PATH = os.path.join(CACHE_DIR, "ember_yearly_global.csv")
MANIFEST_PATH = os.path.join(CACHE_DIR, "ember_yearly_global.manifest.json")

# O endpoint antigo (yearly_full_release_long_format.csv) responde com um
# aviso de 320 bytes apontando para este arquivo desde a mudanca de formato de
# julho/2026. O aviso esta registrado no doc E1.
SOURCE_URL = ("https://files.ember-energy.org/public-downloads/generation/outputs/"
              "release_generation_yearly_global.csv")
LANDING_URL = "https://ember-energy.org/data/yearly-electricity-data/"
LICENSE = {
    "name": "CC BY 4.0",
    "url": "https://ember-energy.org/creative-commons/",
    "commercial_ok": True,
    "notes": "Atribuicao obrigatoria. Sem restricao de uso comercial.",
}
CITATION = ("Ember (2026). Yearly Electricity Data. Ember Energy Research CIC. "
            "https://ember-energy.org/data/yearly-electricity-data/")
REQUIRED_COLUMNS = ("Area", "ISO 3 code", "Year", "Area type",
                    "Electricity source", "Capacity (GW)", "Generation (TWh)")
HTTP_TIMEOUT = 600


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download() -> tuple[str, str, int]:
    request = urllib.request.Request(SOURCE_URL, headers={
        "User-Agent": "opengs-game data collection (research; contact: project repo)"})
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
        body = response.read()
        last_modified = response.headers.get("Last-Modified", "")
    text = body.decode("utf-8-sig")
    first_line = text.split("\n", 1)[0]
    if not all(column in first_line for column in REQUIRED_COLUMNS):
        raise SystemExit("cabecalho inesperado na base da Ember: %r" % first_line[:300])
    os.makedirs(CACHE_DIR, exist_ok=True)
    with open(CSV_PATH, "wb") as handle:
        handle.write(body)
    return hashlib.sha256(body).hexdigest(), last_modified, len(body)


def validate() -> dict:
    """Conta linhas, anos e areas para provar que o arquivo abre e tem dado 2024+."""
    years: set[int] = set()
    countries: set[str] = set()
    rows = 0
    with open(CSV_PATH, encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            rows += 1
            try:
                years.add(int(row["Year"]))
            except (TypeError, ValueError):
                continue
            if row.get("ISO 3 code"):
                countries.add(row["ISO 3 code"])
    if rows < 50000:
        raise SystemExit("arquivo da Ember pequeno demais (%d linhas)" % rows)
    missing = sorted({2024, 2025} - years)
    if missing:
        raise SystemExit("anos ausentes na base da Ember: %s" % missing)
    return {"rows": rows, "years": [min(years), max(years)],
            "countries_with_iso3": len(countries)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--force", action="store_true",
                        help="baixa de novo mesmo com o cache presente")
    args = parser.parse_args()

    if os.path.exists(CSV_PATH) and os.path.exists(MANIFEST_PATH) and not args.force:
        print("cache presente: %s (use --force para baixar de novo)" % CSV_PATH)
        with open(MANIFEST_PATH, encoding="utf-8") as handle:
            manifest = json.load(handle)
        print(json.dumps(manifest, indent=1, ensure_ascii=False))
        return 0

    accessed_at = datetime.now(timezone.utc).replace(microsecond=0)
    sha256, last_modified, size = download()
    stats = validate()
    manifest = {
        "source": "Ember, Yearly Electricity Data (global release)",
        "publisher": "Ember Energy Research CIC",
        "url": SOURCE_URL,
        "landing_url": LANDING_URL,
        "accessed_at_utc": accessed_at.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "http_last_modified": last_modified,
        "sha256": sha256,
        "bytes": size,
        "license": LICENSE,
        "citation": CITATION,
        "file": os.path.relpath(CSV_PATH, ROOT).replace("\\", "/"),
        "transform": ("nenhuma no download; build_national_capacity.py usa a linha "
                      "Capacity (GW) do ultimo ano nao vazio entre 2024 e 2025 por pais"),
        "validation": stats,
        "note": ("O endpoint antigo yearly_full_release_long_format.csv foi descontinuado "
                 "em julho/2026 e responde um aviso de 320 bytes com a URL nova."),
    }
    os.makedirs(CACHE_DIR, exist_ok=True)
    with open(MANIFEST_PATH, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(manifest, indent=1, ensure_ascii=False) + "\n")
    print(json.dumps(manifest, indent=1, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
