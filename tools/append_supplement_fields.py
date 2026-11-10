"""Anexa os campos de suplemento de carvão (bacias faltantes) e diamante
(campos principais) ao deposits_supplement.json. Números reais de fontes
públicas (ver docs/debug/supplement_research.md — pesquisa do subagente).

Uso: python tools/append_supplement_fields.py
Depois: python tools/apply_deposit_supplement.py
        python tools/build_resource_map.py
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUPPLEMENT = os.path.join(ROOT, "data", "definitions", "natural",
                          "deposits_supplement.json")

COAL = [
    # (nome, lon, lat, quantity_kt, quality, depth_m, status)
    ("Kalimantan", 114.5, -1.0, 25840000.0, 0.70, 150.0, "Producer"),
    ("Ordos", 110.0, 39.0, 56000000.0, 0.74, 300.0, "Producer"),
    ("Jharia", 86.0, 23.7, 19430000.0, 0.66, 400.0, "Producer"),
    ("Karaganda", 73.0, 49.5, 8000000.0, 0.75, 400.0, "Producer"),
    ("Hunter Valley", 151.0, -32.5, 4000000.0, 0.73, 250.0, "Producer"),
    ("Cesar", -73.0, 10.0, 520000.0, 0.72, 200.0, "Producer"),
]

DIAMOND = [
    # quantity_kt = proxy (Mct × 10); quality/depth por tipo de jazida.
    ("Jwaneng", 24.7, -24.5, 880.0, 0.90, 400.0, "Producer"),
    ("Orapa", 25.4, -21.3, 860.0, 0.88, 350.0, "Producer"),
    ("Catoca", 20.3, -9.6, 1200.0, 0.85, 300.0, "Producer"),
    ("Venetia", 29.3, -22.4, 1020.0, 0.87, 450.0, "Producer"),
    ("Mbuji-Mayi", 23.6, -6.1, 350.0, 0.80, 250.0, "Producer"),
    ("Cullinan", 28.5, -25.7, 230.0, 0.82, 500.0, "Producer"),
    ("Orange River", 16.5, -28.5, 800.0, 0.86, 100.0, "Producer"),
    ("Mir", 113.9, 62.5, 380.0, 0.84, 600.0, "Producer"),
    ("Argyle", 128.4, -16.7, 610.0, 0.83, 200.0, "Past Producer"),
    ("Diavik", -110.3, 64.5, 1500.0, 0.89, 300.0, "Past Producer"),
    ("Koidu", -11.0, 8.6, 50.0, 0.78, 150.0, "Producer"),
]


def main():
    data = json.load(open(SUPPLEMENT, encoding="utf-8"))
    fields = data.get("fields", [])
    existing = {str(f.get("name", "")).lower() for f in fields}

    added = 0
    for name, lon, lat, quantity, quality, depth, status in COAL:
        if name.lower() in existing:
            continue
        fields.append({
            "name": name, "resource": "coal", "lon": lon, "lat": lat,
            "quantity_kt": quantity, "quality": quality, "depth_m": depth,
            "status": status,
        })
        existing.add(name.lower())
        added += 1
    for name, lon, lat, quantity, quality, depth, status in DIAMOND:
        if name.lower() in existing:
            continue
        fields.append({
            "name": name, "resource": "diamond", "lon": lon, "lat": lat,
            "quantity_kt": quantity, "quality": quality, "depth_m": depth,
            "status": status,
        })
        existing.add(name.lower())
        added += 1

    data["fields"] = fields
    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(SUPPLEMENT, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("campos anexados:", added, "| total fields:", len(fields))


if __name__ == "__main__":
    main()
