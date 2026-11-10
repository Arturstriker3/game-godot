"""Anexa os depósitos de urânio (campos principais, coordenadas reais) ao
deposits.json. O urânio entra no mapa e no núcleo, mas não é usado pela cadeia
econômica tão cedo — fica como matéria-prima disponível.

Uso: python tools/add_uranium.py
Depois: python tools/build_resource_map.py
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")

# (nome, lon, lat, quantity_kt, quality, depth_m)
# quantity_kt = metal de urânio recuperável em kt (estimativas reais aproximadas,
# não reserva medida): Olympic Dam ~2,3 Mt, demais 100–300 kt.
URANIUM = [
    ("Arlit", 7.5, 18.7, 300.0, 0.75, 200.0),
    ("Rössing", 15.0, -22.5, 300.0, 0.72, 350.0),
    ("Cigar Lake", -104.5, 58.0, 150.0, 0.90, 450.0),
    ("McArthur River", -105.8, 57.5, 200.0, 0.88, 550.0),
    ("Olympic Dam", 136.9, -30.4, 2300.0, 0.70, 350.0),
    ("Ranger", 133.0, -12.7, 100.0, 0.74, 150.0),
    ("Inkai", 68.0, 44.5, 200.0, 0.76, 300.0),
    ("Krasnokamensk", 118.0, 50.0, 150.0, 0.73, 400.0),
]

RECOVERY = 0.80


def lonlat_to_pixel(lon, lat):
    return (lon + 180.0) / 360.0 * 5000.0, (90.0 - lat) / 180.0 * 2500.0


def main():
    data = json.load(open(DEPOSITS, encoding="utf-8"))
    deposits = data["deposits"]
    existing = {str(d.get("names", [""])[0]).lower() for d in deposits}

    added = 0
    for index, (name, lon, lat, quantity, quality, depth) in enumerate(URANIUM, start=1):
        if name.lower() in existing:
            continue
        px, py = lonlat_to_pixel(lon, lat)
        recoverable = quantity * RECOVERY
        extraction_cost = round(20.0 + depth * 0.02 + (1.0 - quality) * 30.0, 1)
        tech = 1 if depth < 500.0 and quality > 0.5 else 2 if depth < 2000.0 else 3
        deposits.append({
            "id": "DEP-URANIUM-%06d" % index,
            "territory": "",
            "owner": "NNN",
            "resource": "uranium",
            "px": round(px, 1),
            "py": round(py, 1),
            "lon": round(lon, 2),
            "lat": round(lat, 2),
            "quantity_kt": quantity,
            "recoverable_kt": round(recoverable, 1),
            "quality": quality,
            "depth_m": int(depth),
            "extraction_cost": extraction_cost,
            "required_tech": tech,
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
    print("urânio anexado:", added)


if __name__ == "__main__":
    main()
