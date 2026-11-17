"""Baixa o Global Power Plant Database (WRI) e gera um cache normalizado.

Fonte: https://github.com/wri/global-power-plant-database (release v1.3.0,
arquivo `output_database/global_power_plant_database.csv`).
Licença: CC BY 4.0. Citação: "Global Power Plant Database v1.3.0, World
Resources Institute (WRI), 2021, CC BY 4.0".

O CSV bruto e os derivados ficam em `tools/data/gppd/`, que não é versionado.
Saídas:
  - global_power_plant_database.csv  (download bruto, reaproveitado se existir)
  - plants_normalized.csv            (recorte tipado, ordenado por gppd_idnr)
  - manifest.json                    (URL, data de acesso, SHA-256, licença,
                                      contagens por combustível)

Uso:
  python tools/fetch_gppd.py

Idempotente: com o CSV já em cache, rodar de novo gera `plants_normalized.csv`
com o mesmo SHA-256.
"""

import csv
import hashlib
import json
import os
import sys
import urllib.request
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "tools", "data", "gppd")
SOURCE_CSV = os.path.join(OUT_DIR, "global_power_plant_database.csv")
NORMALIZED_CSV = os.path.join(OUT_DIR, "plants_normalized.csv")
MANIFEST_PATH = os.path.join(OUT_DIR, "manifest.json")

SOURCE_URL = (
    "https://raw.githubusercontent.com/wri/global-power-plant-database/"
    "master/output_database/global_power_plant_database.csv"
)
LICENSE = "CC BY 4.0"
CITATION = (
    "Global Power Plant Database v1.3.0, World Resources Institute (WRI), 2021, CC BY 4.0"
)
USER_AGENT = "Mozilla/5.0"
TIMEOUT = 600
CHUNK = 1 << 20

OUTPUT_COLUMNS = [
    "gppd_idnr",
    "name",
    "country",
    "country_long",
    "capacity_mw",
    "latitude",
    "longitude",
    "primary_fuel",
    "commissioning_year",
    "owner",
    "source",
    "url",
]


def utc_now_iso():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url, destination):
    """Baixa `url` para `destination` gravando num `.part` antes de renomear."""
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    partial = destination + ".part"
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    print("baixando %s" % url, flush=True)
    with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
        with open(partial, "wb") as handle:
            while True:
                block = response.read(CHUNK)
                if not block:
                    break
                handle.write(block)
    os.replace(partial, destination)


def ensure_source_csv():
    """Garante o CSV bruto; devolve True se baixou agora."""
    if os.path.exists(SOURCE_CSV) and os.path.getsize(SOURCE_CSV) > 0:
        print("cache reutilizado: %s" % os.path.relpath(SOURCE_CSV, ROOT), flush=True)
        return False
    download(SOURCE_URL, SOURCE_CSV)
    return True


def clean_text(value):
    """Remove quebras de linha e espaços sobrando."""
    if value is None:
        return ""
    text = value.replace("\r\n", " ").replace("\r", " ").replace("\n", " ")
    return " ".join(text.split())


def format_float(value, digits=6):
    """Formata como float com `digits` casas; vazio quando ausente/ inválido."""
    text = clean_text(value)
    if not text:
        return ""
    try:
        number = float(text)
    except ValueError:
        return ""
    if number != number:  # NaN
        return ""
    return "%.*f" % (digits, number)


def format_year(value):
    """Ano de comissionamento como inteiro; vazio quando ausente."""
    text = clean_text(value)
    if not text:
        return ""
    try:
        return str(int(float(text)))
    except ValueError:
        return ""


def normalize():
    """Lê o CSV bruto e grava `plants_normalized.csv`; devolve estatísticas."""
    with open(SOURCE_CSV, "r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        missing = [column for column in OUTPUT_COLUMNS if column not in (reader.fieldnames or [])]
        if missing:
            raise SystemExit("colunas ausentes no CSV de origem: %s" % ", ".join(missing))

        rows = []
        skipped_no_id = 0
        skipped_no_fuel = 0
        missing_capacity = 0
        missing_coords = 0
        missing_year = 0
        source_rows = 0

        for record in reader:
            source_rows += 1
            plant_id = clean_text(record.get("gppd_idnr"))
            fuel = clean_text(record.get("primary_fuel"))
            if not plant_id:
                skipped_no_id += 1
                continue
            if not fuel:
                skipped_no_fuel += 1
                continue

            capacity = format_float(record.get("capacity_mw"))
            latitude = format_float(record.get("latitude"))
            longitude = format_float(record.get("longitude"))
            year = format_year(record.get("commissioning_year"))

            if not capacity:
                missing_capacity += 1
            if not latitude or not longitude:
                missing_coords += 1
            if not year:
                missing_year += 1

            rows.append([
                plant_id,
                clean_text(record.get("name")),
                clean_text(record.get("country")),
                clean_text(record.get("country_long")),
                capacity,
                latitude,
                longitude,
                fuel,
                year,
                clean_text(record.get("owner")),
                clean_text(record.get("source")),
                clean_text(record.get("url")),
            ])

    rows.sort(key=lambda row: row[0])

    with open(NORMALIZED_CSV, "w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(OUTPUT_COLUMNS)
        writer.writerows(rows)

    fuel_rows = {}
    fuel_capacity = {}
    countries = set()
    total_capacity = 0.0
    for row in rows:
        fuel = row[7]
        capacity = float(row[4]) if row[4] else 0.0
        fuel_rows[fuel] = fuel_rows.get(fuel, 0) + 1
        fuel_capacity[fuel] = fuel_capacity.get(fuel, 0.0) + capacity
        total_capacity += capacity
        if row[2]:
            countries.add(row[2])

    return {
        "source_rows": source_rows,
        "rows": len(rows),
        "countries": len(sorted(countries)),
        "total_capacity_mw": total_capacity,
        "fuel_rows": fuel_rows,
        "fuel_capacity": fuel_capacity,
        "skipped_no_id": skipped_no_id,
        "skipped_no_fuel": skipped_no_fuel,
        "missing_capacity": missing_capacity,
        "missing_coords": missing_coords,
        "missing_year": missing_year,
    }


def write_manifest(stats, downloaded, source_sha256, source_bytes):
    accessed_at = utc_now_iso()
    if not downloaded and os.path.exists(MANIFEST_PATH):
        try:
            with open(MANIFEST_PATH, "r", encoding="utf-8") as handle:
                previous = json.load(handle)
            previous_access = previous.get("accessed_at_utc")
            if previous_access and previous.get("sha256") == source_sha256:
                accessed_at = previous_access
        except (OSError, ValueError):
            pass

    manifest = {
        "source_url": SOURCE_URL,
        "accessed_at_utc": accessed_at,
        "sha256": source_sha256,
        "bytes": source_bytes,
        "license": LICENSE,
        "citation": CITATION,
        "files": {
            "global_power_plant_database.csv": {
                "role": "download bruto",
                "sha256": source_sha256,
                "bytes": source_bytes,
            },
            "plants_normalized.csv": {
                "role": "derivado normalizado",
                "sha256": sha256_file(NORMALIZED_CSV),
                "bytes": os.path.getsize(NORMALIZED_CSV),
                "rows": stats["rows"],
                "columns": OUTPUT_COLUMNS,
            },
        },
        "script": os.path.basename(__file__),
        "total_rows": stats["rows"],
        "source_rows": stats["source_rows"],
        "countries": stats["countries"],
        "total_capacity_mw": round(stats["total_capacity_mw"], 6),
        "primary_fuel_counts": dict(sorted(stats["fuel_rows"].items())),
        "primary_fuel_capacity_mw": {
            fuel: round(value, 6)
            for fuel, value in sorted(stats["fuel_capacity"].items())
        },
        "dropped_rows_without_primary_fuel": stats["skipped_no_fuel"],
        "dropped_rows_without_id": stats["skipped_no_id"],
        "missing_capacity_mw": stats["missing_capacity"],
        "missing_latitude_or_longitude": stats["missing_coords"],
        "missing_commissioning_year": stats["missing_year"],
    }

    with open(MANIFEST_PATH, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2, sort_keys=False)
        handle.write("\n")
    return manifest


def main():
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except (OSError, ValueError):
            pass
    os.makedirs(OUT_DIR, exist_ok=True)
    downloaded = ensure_source_csv()

    source_sha256 = sha256_file(SOURCE_CSV)
    source_bytes = os.path.getsize(SOURCE_CSV)
    print("origem: %s (%d bytes, sha256 %s)" % (
        os.path.relpath(SOURCE_CSV, ROOT), source_bytes, source_sha256), flush=True)

    stats = normalize()
    manifest = write_manifest(stats, downloaded, source_sha256, source_bytes)

    normalized_sha = manifest["files"]["plants_normalized.csv"]["sha256"]
    print("usinas: %d (linhas no CSV de origem: %d)" % (stats["rows"], stats["source_rows"]))
    print("países distintos: %d" % stats["countries"])
    print("capacidade total: %.0f MW" % stats["total_capacity_mw"])
    print("soma de capacity_mw por primary_fuel (top 5):")
    order = sorted(stats["fuel_capacity"].items(), key=lambda item: (-item[1], item[0]))
    for fuel, capacity in order[:5]:
        print("  %-12s %12.0f MW  (%d usinas)" % (fuel, capacity, stats["fuel_rows"][fuel]))
    print("plantas sem primary_fuel descartadas: %d | sem id: %d" % (
        stats["skipped_no_fuel"], stats["skipped_no_id"]))
    print("capacity_mw ausente: %d | lat/lon ausente: %d | commissioning_year ausente: %d" % (
        stats["missing_capacity"], stats["missing_coords"], stats["missing_year"]))
    print("manifest: %s" % os.path.relpath(MANIFEST_PATH, ROOT))
    print("plants_normalized.csv sha256: %s" % normalized_sha)
    return 0


if __name__ == "__main__":
    sys.exit(main())
