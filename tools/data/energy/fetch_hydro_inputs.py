"""Baixa as entradas hídricas livres do E1 (sem WorldClim):

  * CHIRPS v3.0 anual 2025 (precipitação; domínio público / CC BY 4.0);
  * ETOPO1 Ice Surface GeoTIFF (relevo; domínio público, NOAA/NCEI);
  * Global Aridity Index and PET Database v3.1 — PET anual (CC BY 4.0,
    figshare 7504448).

Os arquivos ficam no cache gitignored `tools/data/energy/cache/hydro/`, com
manifest de URL, SHA-256, bytes e data de acesso. Nada é versionado.

Uso:
    python tools/data/energy/fetch_hydro_inputs.py [--force]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import zipfile
from datetime import datetime, timezone

import requests

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
CACHE = os.path.join(ROOT, "tools", "data", "energy", "cache", "hydro")
MANIFEST = os.path.join(CACHE, "manifest.json")

CHIRPS_URL = ("https://data.chc.ucsb.edu/products/CHIRPS/v3.0/annual/global/"
              "tifs/chirps-v3.0.2025.tif")
ETOPO_URL = ("https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO1/data/"
             "ice_surface/grid_registered/georeferenced_tiff/"
             "ETOPO1_Ice_g_geotiff.zip")
PET_URL = "https://api.figshare.com/v2/file/download/56300327"

SOURCES = {
    "chirps_v3_2025": {
        "url": CHIRPS_URL,
        "file": "chirps-v3.0.2025.tif",
        "dataset": "CHIRPS v3.0 annual 2025 (Climate Hazards Center, UCSB)",
        "license": {
            "name": "domínio público / CC BY 4.0",
            "url": "https://creativecommons.org/licenses/by/4.0/",
            "commercial_ok": True,
            "evidence": ("CHC declara CHIRPS3 em domínio público, registrado "
                         "como CC BY 4.0 (chc.ucsb.edu/data/chirps3)"),
        },
        "citation": ("Funk et al. (2026), CHIRPS v3, Sci Data 13:718; "
                     "https://doi.org/10.15780/G2JQ0P"),
    },
    "etopo1": {
        "url": ETOPO_URL,
        "file": "ETOPO1_Ice_g_geotiff.zip",
        "dataset": "ETOPO1 Ice Surface 1 arc-minute global relief (NOAA/NCEI)",
        "license": {
            "name": "domínio público (NOAA/NCEI)",
            "url": "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO1/docs/ETOPO1.pdf",
            "commercial_ok": True,
            "evidence": "NOAA/NCEI distribui ETOPO1 sem restrição de uso",
        },
        "citation": "Amante & Eakins (2009), ETOPO1 1 Arc-Minute Global Relief Model, NOAA",
    },
    "cgiar_pet_v3_1": {
        "url": PET_URL,
        "file": "Global-AI_ET0__annual_v3_1.zip",
        "dataset": "Global Aridity Index and PET Database v3.1 — PET annual",
        "license": {
            "name": "CC BY 4.0",
            "url": "https://creativecommons.org/licenses/by/4.0/",
            "commercial_ok": True,
            "evidence": ("figshare 7504448 declara license CC BY 4.0; o dado é "
                         "um produto derivado, não o WorldClim bruto"),
        },
        "citation": ("Zomer et al. (2008); Trabucco & Zomer (2019), Global "
                     "Aridity Index and PET Database v3.1"),
    },
}


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def download(url, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".part"
    with requests.get(url, stream=True, timeout=300,
                      headers={"User-Agent": "opengs-game energy data collection"}) as response:
        response.raise_for_status()
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        with open(tmp, "wb") as handle:
            for block in response.iter_content(1 << 20):
                handle.write(block)
                done += len(block)
                if total and done % (50 << 20) < (1 << 20):
                    print("  %s: %.0f/%.0f MB" % (os.path.basename(path),
                                                  done / 1e6, total / 1e6))
    os.replace(tmp, path)


def extract_zip(path, prefix=""):
    with zipfile.ZipFile(path) as archive:
        names = [n for n in archive.namelist() if n.lower().endswith((".tif", ".tiff"))]
        if not names:
            raise SystemExit("zip sem GeoTIFF: %s" % path)
        out = []
        for name in names:
            target = os.path.join(os.path.dirname(path),
                                  os.path.basename(name))
            if not os.path.exists(target):
                with archive.open(name) as source, open(target, "wb") as sink:
                    sink.write(source.read())
            out.append(target)
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    manifest = {}
    if os.path.exists(MANIFEST) and not args.force:
        with open(MANIFEST, encoding="utf-8") as handle:
            manifest = json.load(handle)
    for key, source in SOURCES.items():
        entry = manifest.get(key, {})
        path = os.path.join(CACHE, source["file"])
        need = args.force or not os.path.exists(path)
        if need:
            print("baixando %s -> %s" % (key, path))
            download(source["url"], path)
        else:
            print("cache presente: %s" % path)
        if not entry.get("sha256"):
            entry = {
                "url": source["url"],
                "file": os.path.relpath(path, ROOT).replace("\\", "/"),
                "bytes": os.path.getsize(path),
                "sha256": sha256_of(path),
                "accessed_at_utc": datetime.now(timezone.utc).replace(
                    microsecond=0).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "dataset": source["dataset"],
                "license": source["license"],
                "citation": source["citation"],
            }
            manifest[key] = entry
            with open(MANIFEST, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n")
        if path.lower().endswith(".zip"):
            extracted = extract_zip(path)
            entry["extracted"] = [os.path.relpath(p, ROOT).replace("\\", "/")
                                  for p in extracted]
            for extracted_path in extracted:
                entry.setdefault("extracted_sha256", {})[
                    os.path.basename(extracted_path)] = sha256_of(extracted_path)
            manifest[key] = entry
            with open(MANIFEST, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n")
    print(json.dumps(manifest, ensure_ascii=False, indent=1)[:3000])
    return 0


if __name__ == "__main__":
    sys.exit(main())
