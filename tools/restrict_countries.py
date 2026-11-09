"""Restringe o mapa do opengs a Inglaterra/França/Alemanha e aplica cores RTS.

Apenas GBR, FRA e DEU mantêm províncias; todo o resto vira NNN (neutro).
As cores seguem a paleta clássica de jogos RTS:
  - Inglaterra (GBR): vermelho
  - França (FRA):     azul
  - Alemanha (DEU):   cinza
  - Neutro (NNN):     cinza escuro
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFS = os.path.join(ROOT, "data", "definitions")
HIST = os.path.join(ROOT, "data", "history")

KEEP = {"GBR", "FRA", "DEU"}

COUNTRIES = {
    "NNN": {"map_color": [0.32, 0.32, 0.35], "base_name": "Neutral", "ideology": "DEMOCRACY"},
    "GBR": {"map_color": [0.82, 0.12, 0.12], "base_name": "England", "ideology": "DEMOCRACY"},
    "FRA": {"map_color": [0.10, 0.30, 0.85], "base_name": "France", "ideology": "DEMOCRACY"},
    "DEU": {"map_color": [0.55, 0.55, 0.60], "base_name": "Germany", "ideology": "DEMOCRACY"},
}


def rewrite_provinces() -> None:
    path = os.path.join(HIST, "provinces", "provinces.json")
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    for entry in data.values():
        owner = entry.get("province_owner", "NNN")
        controller = entry.get("province_controller", "NNN")
        if owner not in KEEP:
            owner = "NNN"
        if controller not in KEEP:
            controller = "NNN"
        entry["province_owner"] = owner
        entry["province_controller"] = controller

    with open(path, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent="\t")
        handle.write("\n")


def rewrite_countries() -> None:
    path = os.path.join(DEFS, "countries.txt")
    with open(path, "w", encoding="utf-8") as handle:
        for tag in ("NNN", "GBR", "FRA", "DEU"):
            handle.write(tag + "\n")


def rewrite_country_history() -> None:
    folder = os.path.join(HIST, "countries")
    for tag, content in COUNTRIES.items():
        path = os.path.join(folder, tag + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(content, handle, indent="\t")
            handle.write("\n")


def main() -> int:
    rewrite_provinces()
    rewrite_countries()
    rewrite_country_history()

    with open(os.path.join(HIST, "provinces", "provinces.json"), "r", encoding="utf-8") as handle:
        data = json.load(handle)

    counts = {}
    for entry in data.values():
        tag = entry.get("province_owner")
        counts[tag] = counts.get(tag, 0) + 1

    print("Províncias por dono:", counts)
    print("Países ativos: NNN, GBR, FRA, DEU")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
