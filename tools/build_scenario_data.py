"""Gera os JSONs de cenário a partir dos CSVs (fonte de edição).

O jogo lê os JSONs; os CSVs são a fonte de edição amigável (Excel/Sheets).
A produção de cada cidade é a SOMA da saída das construções em buildings.csv.

Uso:
    <python> tools/build_scenario_data.py

Entrada (data/definitions/scenario/):
    cities.csv    id,nation,home_nation,name,population,x,y,landmass
    ports.csv     id,name,nation,x,y,landmass
    buildings.csv city_id,building_id,name,output_tons_per_day

Saída (data/definitions/scenario/):
    cities.json    [ { id, nation, home_nation, name, population, production,
                      x, y, landmass }, ... ]
    ports.json     [ { id, name, nation, x, y, landmass }, ... ]
    buildings.json [ { city_id, id, name, output_tons_per_day }, ... ]
"""

import csv
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIO = os.path.join(HERE, "..", "data", "definitions", "scenario")


def _rows(name: str) -> list[dict]:
    path = os.path.join(SCENARIO, name)
    with open(path, "r", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def main() -> None:
    cities = []
    for row in _rows("cities.csv"):
        cities.append({
            "id": row["id"].strip(),
            "nation": row["nation"].strip(),
            "home_nation": row["home_nation"].strip(),
            "name": row["name"].strip(),
            "population": float(row["population"]),
            "production": 0.0,  # preenchido pelas construções
            "x": float(row["x"]),
            "y": float(row["y"]),
            "landmass": row["landmass"].strip(),
        })

    buildings = []
    for row in _rows("buildings.csv"):
        buildings.append({
            "city_id": row["city_id"].strip(),
            "id": row["building_id"].strip(),
            "name": row["name"].strip(),
            "output_tons_per_day": float(row["output_tons_per_day"]),
        })

    # Produção = soma das construções da cidade.
    by_city = {c["id"]: c for c in cities}
    for b in buildings:
        city = by_city.get(b["city_id"])
        if city is not None:
            city["production"] += b["output_tons_per_day"]

    ports = []
    for row in _rows("ports.csv"):
        ports.append({
            "id": row["id"].strip(),
            "name": row["name"].strip(),
            "nation": row["nation"].strip(),
            "x": float(row["x"]),
            "y": float(row["y"]),
            "landmass": row["landmass"].strip(),
        })

    def _write(name: str, data) -> None:
        path = os.path.join(SCENARIO, name)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, ensure_ascii=False, indent=2)
        print(f"{name}: {len(data)}")

    _write("cities.json", cities)
    _write("ports.json", ports)
    _write("buildings.json", buildings)
    print("OK")


if __name__ == "__main__":
    main()
