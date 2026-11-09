"""Imprime um mapa ASCII de donos das províncias numa janela do raster."""
import json
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROV_TXT = os.path.join(ROOT, "data", "definitions", "provinces.txt")
PROV_PNG = os.path.join(ROOT, "map", "map_data", "provinces.png")
PROV_JSON = os.path.join(ROOT, "data", "history", "provinces", "provinces.json")


def main() -> int:
    x0, y0, x1, y1, step = (int(v) for v in sys.argv[1:6])

    colors = {}
    for line in open(PROV_TXT, encoding="utf-8"):
        parts = line.strip().split(";")
        if len(parts) < 7 or parts[1] not in ("land", "lake"):
            continue
        colors[(int(parts[2]), int(parts[3]), int(parts[4]))] = parts[0]

    owners = {k: v["province_owner"] for k, v in json.load(open(PROV_JSON, encoding="utf-8")).items()}
    img = np.asarray(Image.open(PROV_PNG).convert("RGB"))

    for y in range(y0, y1, step):
        row = []
        for x in range(x0, x1, step):
            key = tuple(int(c) for c in img[y, x])
            pid = colors.get(key)
            if pid is None:
                row.append("~")  # oceano
                continue
            tag = owners.get(pid, "???")
            row.append({"GBR": "E", "FRA": "F", "DEU": "G", "NNN": "."}.get(tag, "?"))
        print(f"{y:5d} " + "".join(row))

    print("      " + "".join(str((x // 100) % 10) for x in range(x0, x1, step)))
    print("E=Inglaterra  F=Franca  G=Alemanha  .=neutro  ~=oceano")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
