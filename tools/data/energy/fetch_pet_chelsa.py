"""PET anual do proxy hídrico — CHELSA v2.1 (CC0 1.0, ERA5 + Penman-Monteith).

Substitui a PET do CGIAR-CSI v3.1, que é **derivada do WorldClim 2.1** e
"provided for non-commercial use" (csidotinfo.wordpress.com/data/
global-aridity-and-pet-database/), incompatível com uso comercial.

Fonte:
  * CHELSA-climatologies v2.1, variável `pet` (Monthly Potential
    Evapotranspiration, kg m-2 month-1, Penman-Monteith), climatologia
    1981-2010, 30 arc-sec, COG. Forcing declarado no próprio GeoTIFF: ERA5
    (Copernicus Climate Change Service/ECMWF). **Não é produto do WorldClim.**
    Licença na página oficial do dataset: "Creative Commons Zero - No Rights
    Reserved (CC0 1.0)" — domínio público, uso comercial livre.
    https://chelsa-climate.org/datasets/chelsa_climatologies/

Como é baixado (sem baixar os 12 COGs de ~400 MB):
  * cada COG do CHELSA tem pirâmide interna (overviews). A leitura remota usa
    HTTP Range nos *tiles* do nível 1/8 (0,0667° ≈ 7,4 km), que é suficiente
    para o proxy hídrico provincial (CHIRPS é 0,05°), e monta a PET anual
    somando os 12 meses.

Saída (cache gitignored `tools/data/energy/cache/hydro/`):
  * `chelsa_pet_annual_1981-2010_1of8.tif` — PET anual em mm/ano, float32
    (nodata -9999), georreferenciada (WGS84);
  * entrada `chelsa_pet_v21` no `manifest.json` do mesmo cache com URL,
    tamanho, ETag, nível usado, escala, SHA-256 do derivado e data de acesso.

Uso:
    python tools/data/energy/fetch_pet_chelsa.py [--force]
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone

import numpy as np
import requests
import tifffile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
CACHE = os.path.join(ROOT, "tools", "data", "energy", "cache", "hydro")
MANIFEST = os.path.join(CACHE, "manifest.json")
OUT = os.path.join(CACHE, "chelsa_pet_annual_1981-2010_1of8.tif")

BASE_URL = ("https://os.unil.cloud.switch.ch/chelsa02/chelsa/global/"
            "climatologies/pet/1981-2010/")
FILE_PATTERN = "CHELSA_pet_%02d_1981-2010_V.2.1.tif"
MONTHS = tuple(range(1, 13))
LEVEL_RATIO = 8          # 0.0083333° * 8 = 0.0667° (~7,4 km no equador)
WORKERS = 12
NODATA = 65535

SOURCE = {
    "dataset": ("CHELSA-climatologies v2.1 — Monthly Potential "
                "Evapotranspiration (pet), climatologia 1981-2010"),
    "publisher": "WSL / CHELSA (Karger et al.)",
    "landing_url": "https://chelsa-climate.org/datasets/chelsa_climatologies/",
    "bucket_url": BASE_URL,
    "reference_period": "1981-2010 (climatologia)",
    "forcing": "ERA5 (Copernicus Climate Change Service/ECMWF), declarado no GeoTIFF",
    "license": {
        "name": "CC0 1.0 (domínio público; uso comercial livre)",
        "url": "https://creativecommons.org/publicdomain/zero/1.0/",
        "commercial_ok": True,
        "verified_on": "2026-11-22",
        "quote": ("License: Creative Commons Zero - No Rights Reserved "
                  "(CC0 1.0)"),
        "evidence": ("página oficial do dataset CHELSA-climatologies declara "
                     "CC0 1.0; variável 'pet' calculada por Penman-Monteith "
                     "com forçante ERA5 (não é produto do WorldClim)"),
    },
    "citation": ("Karger, D. N. et al. (2021), Climatologies at high "
                 "resolution for the earth's land surface areas, Version 2.1. "
                 "EnviDat. https://doi.org/10.16904/envidat.228.v2.1"),
}


class HttpRangeFile:
    """Arquivo remoto somente-leitura por HTTP Range, com prefetch paralelo."""

    def __init__(self, url: str, size: int):
        self.url = url
        self.name = url
        self.size = size
        self.pos = 0
        self.session = requests.Session()
        self.session.headers.update(
            {"User-Agent": "opengs-game energy data collection"})
        self.blocks: dict[tuple[int, int], bytes] = {}
        self.lock = threading.Lock()

    def _fetch(self, offset: int, count: int) -> bytes:
        for attempt in range(4):
            try:
                response = self.session.get(
                    self.url,
                    headers={"Range": "bytes=%d-%d" % (offset, offset + count - 1)},
                    timeout=180)
                response.raise_for_status()
                data = response.content
                if len(data) != count:
                    raise IOError("range %d+%d devolveu %d bytes"
                                  % (offset, count, len(data)))
                return data
            except Exception:  # noqa: BLE001 - tenta de novo; último levanta
                if attempt == 3:
                    raise
        raise IOError("range inalcançável")

    def prefetch(self, ranges: list[tuple[int, int]]) -> None:
        unique = sorted({(int(off), int(cnt)) for off, cnt in ranges})
        def load(item):
            offset, count = item
            data = self._fetch(offset, count)
            with self.lock:
                self.blocks[item] = data
        with ThreadPoolExecutor(WORKERS) as pool:
            list(pool.map(load, unique))

    def read(self, n: int = -1) -> bytes:
        if n is None or n < 0:
            n = self.size - self.pos
        if n == 0:
            return b""
        key = (self.pos, n)
        data = self.blocks.get(key)
        if data is None:
            data = self._fetch(*key)
            with self.lock:
                self.blocks[key] = data
        self.pos += len(data)
        return data

    def readinto(self, buffer) -> int:
        data = self.read(len(buffer))
        buffer[:len(data)] = data
        return len(data)

    def seek(self, offset: int, whence: int = 0) -> int:
        if whence == 0:
            self.pos = offset
        elif whence == 1:
            self.pos += offset
        elif whence == 2:
            self.pos = self.size + offset
        else:
            raise ValueError("whence inválido: %d" % whence)
        return self.pos

    def tell(self) -> int:
        return self.pos

    def close(self) -> None:
        self.session.close()


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def head_size(session: requests.Session, url: str) -> int:
    response = session.head(url, timeout=60, allow_redirects=True)
    response.raise_for_status()
    return int(response.headers["Content-Length"])


def pick_level(pages, ratio: int):
    """Página de overview com fator `ratio` sobre a resolução nativa."""
    native = pages[0].shape
    rows, cols = native
    for page in pages:
        if page.shape == (rows // ratio, cols // ratio):
            return page
    # Sem o nível exato: escolhe o mais próximo do fator pedido.
    best = None
    for page in pages:
        factor = native[0] / float(page.shape[0])
        if abs(factor - ratio) < (abs(native[0] / float(best.shape[0]) - ratio)
                                  if best is not None else 99.0):
            best = page
    if best is None:
        raise SystemExit("COG sem overviews: %s" % pages[0])
    return best


def read_level(url: str, ratio: int) -> tuple[np.ndarray, tuple, dict]:
    session = requests.Session()
    try:
        size = head_size(session, url)
    finally:
        session.close()
    handle = HttpRangeFile(url, size)
    try:
        with tifffile.TiffFile(handle) as tif:
            page = pick_level(tif.pages, ratio)
            offsets = page.tags["TileOffsets"].value
            counts = page.tags["TileByteCounts"].value
            geo_page = tif.pages[0]
            scale = geo_page.tags["ModelPixelScaleTag"].value
            tie = geo_page.tags["ModelTiepointTag"].value
            metadata = ""
            for candidate in (page, geo_page):
                if "GDAL_METADATA" in candidate.tags:
                    metadata = candidate.tags["GDAL_METADATA"].value
                    break
            level_scale = None
            match = re.search(r'<Item name="SCALE"[^>]*>([0-9.eE+-]+)</Item>',
                              metadata)
            if match:
                level_scale = float(match.group(1))
            stats = {
                "level_shape": list(page.shape),
                "tile_shape": [int(page.tags["TileLength"].value),
                               int(page.tags["TileWidth"].value)],
                "tiles": len(offsets),
                "remote_bytes": size,
                "pixel_scale_deg": round(
                    float(scale[0]) * (geo_page.shape[0] / float(page.shape[0])), 10),
                "geotransform": {"lon0": float(tie[3]), "lat0": float(tie[4])},
                "scale_factor": 1.0 if level_scale is None else level_scale,
            }
            handle.prefetch(list(zip(offsets, counts)))
            array = page.asarray()
    finally:
        handle.close()
    stats["unit"] = "kg m-2 month-1"
    return array, (scale, tie, page.shape[0], geo_page.shape[0]), stats


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    manifest = {}
    if os.path.exists(MANIFEST):
        with open(MANIFEST, encoding="utf-8") as handle:
            manifest = json.load(handle)
    entry = manifest.get("chelsa_pet_v21") or {}
    if os.path.exists(OUT) and not args.force and entry.get("sha256"):
        print("derivado presente: %s" % OUT)
        print("sha256 %s" % entry["sha256"])
        return 0

    os.makedirs(CACHE, exist_ok=True)
    total = None
    months_valid = None
    geometry = None
    files = []
    for month in MONTHS:
        name = FILE_PATTERN % month
        url = BASE_URL + name
        print("mês %02d: %s" % (month, name))
        array, geo, stats = read_level(url, LEVEL_RATIO)
        scale = stats["scale_factor"]
        values = array.astype(np.float32)
        values[array == NODATA] = np.nan
        values = np.where(np.isfinite(values), values * scale, np.nan)
        if total is None:
            total = np.zeros_like(values)
            months_valid = np.zeros(values.shape, dtype=np.int16)
            geometry = geo
        if values.shape != total.shape:
            raise SystemExit("shape do mês %d difere: %s != %s"
                             % (month, values.shape, total.shape))
        valid = np.isfinite(values)
        total[valid] += values[valid]
        months_valid[valid] += 1
        files.append({
            "month": month,
            "url": url,
            "remote_bytes": stats["remote_bytes"],
            "level_shape": stats["level_shape"],
            "tiles": stats["tiles"],
            "unit": stats["unit"],
            "scale_factor": scale,
            "pixel_scale_deg": stats["pixel_scale_deg"],
        })

    annual = np.where(months_valid == len(MONTHS), total, np.nan)
    annual = np.where(np.isfinite(annual), annual, -9999.0).astype(np.float32)
    valid_values = annual[annual > -9000.0]
    mean_value = float(valid_values.mean()) if valid_values.size else 0.0
    print("PET anual: %s | média %.1f mm/ano | min %.1f | max %.1f"
          % (annual.shape, mean_value, float(valid_values.min()),
             float(valid_values.max())))
    if not 100.0 < mean_value < 2500.0:
        raise SystemExit("PET anual fora da faixa física: média %.1f mm/ano"
                         % mean_value)

    scale, tie, level_rows, native_rows = geometry
    i, j, _k, lon, lat, _e = tie
    pixel = float(scale[0]) * native_rows / float(level_rows)
    tifffile.imwrite(
        OUT, annual, compression="zlib", tile=(256, 256),
        extratags=[
            (33550, "d", 3, (pixel, pixel, 0.0), False),
            (33922, "d", 6, (0.0, 0.0, 0.0, float(lon) - i * float(scale[0]),
                             float(lat) + j * float(scale[1]), 0.0), False),
            (42113, "s", 6, "-9999", False),
        ])

    entry = {
        "url": BASE_URL + FILE_PATTERN.replace("%02d", "01..12"),
        "files": files,
        "derived_file": os.path.relpath(OUT, ROOT).replace("\\", "/"),
        "bytes": os.path.getsize(OUT),
        "sha256": sha256_of(OUT),
        "accessed_at_utc": datetime.now(timezone.utc).replace(
            microsecond=0).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "dataset": SOURCE["dataset"],
        "publisher": SOURCE["publisher"],
        "landing_url": SOURCE["landing_url"],
        "reference_period": SOURCE["reference_period"],
        "forcing": SOURCE["forcing"],
        "license": SOURCE["license"],
        "citation": SOURCE["citation"],
        "transform": ("12 COGs mensais lidos no overview 1/8 por HTTP Range; "
                      "PET anual = Σ 12 meses (escala 0,01 do GeoTIFF) em "
                      "mm/ano; meses sem pixel válido = nodata, sem inventar"),
    }
    manifest["chelsa_pet_v21"] = entry
    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n")
    print("gravado %s (%d bytes) sha256 %s"
          % (entry["derived_file"], entry["bytes"], entry["sha256"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
