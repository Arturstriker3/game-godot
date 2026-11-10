"""Anexa os campos principais de terras raras (REE) ao deposits.json. O MRDS é
americano e quase não tem a China (Bayan Obo ~57% do mundo), então este suplemento
completa com os distritos reais.

Uso: python tools/add_rare_earths.py
Depois: python tools/build_resource_map.py
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")

# (nome, lon, lat, quantity_kt, quality, depth_m)
REE = [
    ("Bayan Obo", 109.9, 41.8, 48000.0, 0.70, 300.0),
    ("Sichuan", 102.0, 28.5, 10000.0, 0.68, 250.0),
    ("Jiangxi", 115.0, 25.5, 5000.0, 0.66, 100.0),
    ("Mountain Pass", -115.5, 35.5, 10000.0, 0.72, 250.0),
    ("Mount Weld", 122.5, -28.8, 15000.0, 0.69, 200.0),
    ("Nolans Bore", 133.0, -22.3, 2000.0, 0.65, 150.0),
]

RECOVERY = 0.65


def lonlat_to_pixel(lon, lat):
    return (lon + 180.0) / 360.0 * 5000.0, (90.0 - lat) / 180.0 * 2500.0


def main():
    data = json.load(open(DEPOSITS, encoding="utf-8"))
    deposits = data["deposits"]
    existing = {str(d.get("names", [""])[0]).lower() for d in deposits}

    added = 0
    for index, (name, lon, lat, quantity, quality, depth) in enumerate(REE, start=1):
        if name.lower() in existing:
            continue
        px, py = lonlat_to_pixel(lon, lat)
        recoverable = quantity * RECOVERY
        deposits.append({
            "id": "DEP-REE-%06d" % index,
            "territory": "",
            "owner": "NNN",
            "resource": "rare_earths",
            "px": round(px, 1),
            "py": round(py, 1),
            "lon": round(lon, 2),
            "lat": round(lat, 2),
            "quantity_kt": quantity,
            "recoverable_kt": round(recoverable, 1),
            "quality": quality,
            "depth_m": int(depth),
            "extraction_cost": round(20.0 + depth * 0.02 + (1.0 - quality) * 30.0, 1),
            "required_tech": 1 if depth < 500.0 and quality > 0.5 else 2,
            "max_rate_kt_day": max(1.0, round(recoverable / (25.0 * 365.0), 1)),
            "discovery_difficulty": 0.15,
            "sites": 1,
            "status": "Producer",
            "names": [name],
            "data_quality": "curated",
        })
        existing.add(name.lower())
        added += 1

    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(DEPOSITS, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("terras raras anexadas:", added)


if __name__ == "__main__":
    main()
