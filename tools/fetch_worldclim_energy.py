"""Baixa e extrai os rasters mensais de radiacao solar (srad) e vento (wind)
do WorldClim 2.1 (10 arc-min), no mesmo cache de tools/fetch_worldclim.py:
tools/data/worldclim/ (gitignored).

Fontes:
  wc2.1_10m_srad.zip   radiacao solar (kJ m-2 day-1), 12 tifs mensais
  wc2.1_10m_wind.zip   velocidade do vento (m s-1), 12 tifs mensais

Grade global 2160x1080, mesma convencao do wc2.1_10m_elev.tif:
lon -180..180, lat 90..-90, celula 1/6 grau, linha 0 = topo.

Idempotente: nao rebaixa zip existente e nao reextrai tif ja presente.
Grava tools/data/worldclim/manifest_energy.json com sha256/bytes dos zips,
sha256/shape/dtype dos 24 tifs, licenca e citacao.

Uso: python tools/fetch_worldclim_energy.py
"""

import datetime
import hashlib
import json
import logging
import os
import urllib.request
import zipfile

import numpy as np
import tifffile

# tifffile avisa ao nao conseguir converter a tag GDAL_NODATA "-3.4e+38"
# para o dtype inteiro dos rasters srad; lemos o nodata de outra forma.
try:
    logging.getLogger("tifffile").setLevel(logging.ERROR)
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tools", "data", "worldclim")
MANIFEST = os.path.join(OUT, "manifest_energy.json")

BASE = "https://geodata.ucdavis.edu/climate/worldclim/2_1/base"

# variavel -> (zip, prefixo mensal)
VARIABLES = {
    "srad": ("wc2.1_10m_srad.zip", "wc2.1_10m_srad_"),
    "wind": ("wc2.1_10m_wind.zip", "wc2.1_10m_wind_"),
}
MONTHS = [("%02d" % m) for m in range(1, 13)]
EXPECTED_SHAPE = (1080, 2160)

# WorldClim usa -3.4e38 como nodata (ocorre sobre o oceano).
NODATA_TAG = 42113  # GDAL_NODATA
FALLBACK_NODATA = -1.0e30

LICENSE = (
    "WorldClim 2.1 - ver https://www.worldclim.org/data/worldclim21.html "
    "(nao declarada explicitamente na pagina; uso academico/atribuicao)"
)
CITATION = "WorldClim 2.1, Fick & Hijmans 2017, Int. J. Climatol. 37:4302-4315"


def download(url, dest):
    """Baixa url para dest; retorna numero de bytes gravados."""
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=900) as response:
        data = response.read()
    with open(dest, "wb") as handle:
        handle.write(data)
    return len(data)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def nodata_of(path):
    """Le a tag GDAL_NODATA do GeoTIFF, se existir."""
    try:
        with tifffile.TiffFile(path) as tif:
            tags = tif.pages[0].tags
            if NODATA_TAG in tags:
                raw = str(tags[NODATA_TAG].value).strip().strip("\x00")
                if raw:
                    return float(raw)
    except Exception:
        pass
    return None


def load_tif(path):
    """Retorna (array float, mask de valores validos, dtype original)."""
    data = tifffile.imread(path)
    if data.ndim != 2:
        raise SystemExit("esperava raster 2D em %s, veio shape %s" % (path, data.shape))
    dtype = str(data.dtype)
    nodata = nodata_of(path)
    values = data.astype("float64")
    if nodata is None:
        nodata = FALLBACK_NODATA
    valid = values > nodata * 0.5 if nodata < 0 else values != nodata
    # valores absurdamente negativos tambem sao nodata, mesmo sem tag
    valid &= values > FALLBACK_NODATA * 0.5
    return values, valid, dtype


def fmt_range(values, valid):
    if not valid.any():
        return "sem dado valido"
    return "min=%.4f max=%.4f" % (values[valid].min(), values[valid].max())


def ensure_downloads(manifest):
    """Baixa os zips ausentes e devolve metadados de cada um."""
    downloads = {}
    for name in sorted({entry[0] for entry in VARIABLES.values()}):
        dest = os.path.join(OUT, name)
        url = BASE + "/" + name
        if os.path.exists(dest):
            print("ja existe: %s (%d bytes)" % (name, os.path.getsize(dest)))
        else:
            size = download(url, dest)
            print("baixado: %s %.1f MB" % (name, size / 1048576.0))
        downloads[name] = {
            "url": url,
            "sha256": sha256_of(dest),
            "bytes": os.path.getsize(dest),
        }
        manifest["downloads"][name] = downloads[name]
    return downloads


def ensure_extraction():
    """Extrai cada zip apenas se algum tif mensal estiver faltando."""
    for var, (archive_name, prefix) in VARIABLES.items():
        expected = [os.path.join(OUT, prefix + m + ".tif") for m in MONTHS]
        if all(os.path.exists(path) for path in expected):
            print("ja extraido: %s" % var)
            continue
        archive_path = os.path.join(OUT, archive_name)
        with zipfile.ZipFile(archive_path) as archive:
            archive.extractall(OUT)
            print("extraido: %s (%d arquivos)" % (archive_name, len(archive.namelist())))


def validate_and_collect(manifest):
    """Valida os 24 tifs, acumula min/max por variavel e preenche o manifest."""
    files = []
    summary = {}
    for var, (_archive_name, prefix) in VARIABLES.items():
        global_values = []
        global_valid = []
        per_month = {}
        var_dtype = "?"
        for month in MONTHS:
            name = prefix + month + ".tif"
            path = os.path.join(OUT, name)
            if not os.path.exists(path):
                raise SystemExit("tif ausente: %s" % name)
            values, valid, dtype = load_tif(path)
            var_dtype = dtype
            if values.shape != EXPECTED_SHAPE:
                raise SystemExit("%s: shape %s != %s" % (name, values.shape, EXPECTED_SHAPE))
            if not valid.any():
                raise SystemExit("%s: todos os pixels sao nodata" % name)
            files.append({
                "name": name,
                "sha256": sha256_of(path),
                "shape": [int(values.shape[0]), int(values.shape[1])],
                "dtype": dtype,
            })
            per_month[month] = (values, valid)
            global_values.append(values)
            global_valid.append(valid)
        all_values = np.concatenate([v.ravel() for v in global_values])
        all_valid = np.concatenate([m.ravel() for m in global_valid])
        summary[var] = fmt_range(all_values, all_valid)
        print("[%s] shape=%s dtype=%s | m01 %s | m07 %s | global %s"
              % (var,
                 EXPECTED_SHAPE,
                 var_dtype,
                 fmt_range(*per_month["01"]),
                 fmt_range(*per_month["07"]),
                 summary[var]))
    manifest["files"] = files
    return summary


def main():
    os.makedirs(OUT, exist_ok=True)
    manifest = {
        "source_urls": [BASE + "/" + name for name in sorted({e[0] for e in VARIABLES.values()})],
        "accessed_at_utc": datetime.datetime.now(datetime.timezone.utc)
        .replace(microsecond=0)
        .isoformat()
        .replace("+00:00", "Z"),
        "downloads": {},
        "license": LICENSE,
        "citation": CITATION,
        "grid": {"shape": list(EXPECTED_SHAPE), "resolution_arcmin": 10},
        "files": [],
    }

    ensure_downloads(manifest)
    ensure_extraction()
    summary = validate_and_collect(manifest)

    with open(MANIFEST, "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, ensure_ascii=False)
        handle.write("\n")

    print("manifest: %s (%d tifs)" % (os.path.relpath(MANIFEST, ROOT), len(manifest["files"])))
    probe = [f for f in manifest["files"] if f["name"] == "wc2.1_10m_srad_01.tif"]
    if probe:
        print("sha256 wc2.1_10m_srad_01.tif = %s" % probe[0]["sha256"])
    for name, info in sorted(manifest["downloads"].items()):
        print("zip %s bytes=%d sha256=%s" % (name, info["bytes"], info["sha256"]))
    for var, text in summary.items():
        print("global %s: %s" % (var, text))


if __name__ == "__main__":
    main()
