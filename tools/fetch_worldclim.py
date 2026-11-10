"""Baixa e extrai os rasters do WorldClim 2.1 (10 arc-min) usados pelo
apply_real_climate.py. Cache em tools/data/worldclim/ (gitignored).

Fontes:
  wc2.1_10m_elev.zip   altitude (m)
  wc2.1_10m_bio.zip    variáveis bioclimáticas (19 .tif; usamos bio_1 e bio_12)

Uso: python tools/fetch_worldclim.py
"""

import os
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tools", "data", "worldclim")

BASE = "https://geodata.ucdavis.edu/climate/worldclim/2_1/base"
FILES = ["wc2.1_10m_elev.zip", "wc2.1_10m_bio.zip"]


def download(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=360) as response:
        data = response.read()
    with open(dest, "wb") as handle:
        handle.write(data)
    return len(data)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in FILES:
        dest = os.path.join(OUT, name)
        if os.path.exists(dest):
            print("já existe:", name)
        else:
            size = download(BASE + "/" + name, dest)
            print("baixado:", name, "%.1f MB" % (size / 1048576.0))
        with zipfile.ZipFile(dest) as archive:
            archive.extractall(OUT)
            print("extraído:", ", ".join(archive.namelist()))


if __name__ == "__main__":
    main()
