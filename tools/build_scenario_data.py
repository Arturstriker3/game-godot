"""Gera os JSONs de cenário a partir dos CSVs (fonte de edição).

O jogo lê os JSONs; os CSVs são a fonte de edição amigável (Excel/Sheets).
A produção de cada cidade é a SOMA da saída das construções em buildings.csv.

Uso:
    <python> tools/build_scenario_data.py

Entrada (data/definitions/scenario/):
    cities.csv    id,nation,home_nation,name,population,x,y,landmass
    ports.csv     id,name,nation,x,y,landmass
    buildings.csv city_id,building_id,name,output_tons_per_day
    nations.csv   id,name,flag,color,capital,currency,government,ai_profile,region,
                  population,gdp_ppp_bn

Saída (data/definitions/scenario/):
    cities.json    [ { id, nation, home_nation, name, population, production,
                      x, y, landmass }, ... ]
    ports.json     [ { id, name, nation, x, y, landmass }, ... ]
    buildings.json [ { city_id, id, name, output_tons_per_day }, ... ]
    nations.json   [ { id, name, flag, + campos opcionais do schema }, ... ]
"""

import csv
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIO = os.path.join(HERE, "..", "data", "definitions", "scenario")
I18N_DIR = os.path.join(HERE, "..", "data", "definitions", "i18n")
LOCALES = ["pt_BR", "en", "es"]


def merge_i18n(csv_rows, existing):
    """Mescla as linhas do `i18n.csv` sobre o JSON existente (por locale).

    Regras (recal3, item 4):
      * o CSV MANDA nas chaves que ele tem (nome de cidade/porto/nação);
      * chave que só existe no JSON é PRESERVADA (não nasce de CSV);
      * valor vazio no CSV não apaga o valor que já estava no JSON.

    Devolve ``({locale: {key: value}}, stats)``; `stats` só serve ao relatório
    do build (quantas vieram do CSV, quantas foram preservadas/sobrescritas).
    """
    i18n = {loc: {k: v for k, v in existing.get(loc, {}).items()}
            for loc in LOCALES}
    csv_keys = set()
    stats = {"from_csv": 0, "overridden": 0, "preserved": 0}
    for row in csv_rows:
        key = (row.get("key") or "").strip()
        if not key:
            continue
        csv_keys.add(key)
        overrode = False
        for loc in LOCALES:
            val = (row.get(loc) or "").strip()
            if not val:
                continue
            if (loc in existing and key in existing[loc]
                    and existing[loc][key] != val):
                overrode = True
            i18n[loc][key] = val
        if overrode:
            stats["overridden"] += 1
    stats["from_csv"] = len(csv_keys)
    stats["preserved"] = sum(1 for k in existing.get("pt_BR", {})
                             if k not in csv_keys)
    return i18n, stats


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
            # Unidade de simulação: aglomeração da fonte. O núcleo C++ consome
            # este campo; `population` é a população nacional real rateada.
            "sim_population": float(row.get("sim_population") or row["population"]),
            "agglomeration_population": (int(float(row["agglomeration_population"])) if (row.get("agglomeration_population") or "").strip() else None),
            # População URBANA do nó e a fonte da regra (gravadas por
            # tools/build_world_cities.py; fonte única: cities.csv).
            "urban_population": int(float(row["urban_population"])),
            "urban_population_source": row["urban_population_source"].strip(),
            "production": 0.0,  # preenchido pelas construções
            "x": float(row["x"]),
            "y": float(row["y"]),
            "landmass": row["landmass"].strip(),
            "capital": row.get("capital", "0").strip().lower() in ("1", "true", "yes"),
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
            # Cidade de origem do porto: o mapa usa para saber de quem ele é
            # (badge quando o ponto de costa encosta no tecido urbano). Vazio
            # quando a fonte não tem cidade (o mapa trata como porto isolado).
            "port_of": row.get("port_of", "").strip(),
        })

    nations = []
    for row in _rows("nations.csv"):
        nation = {
            "id": row["id"].strip(),
            "name": row["name"].strip(),
            "flag": row["flag"].strip(),
        }
        # Campos opcionais do schema do plano (docs/plans/world_nations_plan.md
        # §5). Ausentes/vazios permanecem ausentes: o leitor do jogo trata todos
        # como opcionais e a UI declara a falta em vez de inventar valor.
        for key in ("color", "capital", "currency", "government", "ai_profile",
                    "region"):
            value = row.get(key, "").strip()
            if value:
                nation[key] = value
        for key in ("population", "gdp_ppp_bn"):
            value = row.get(key, "").strip()
            if not value:
                continue
            number = float(value)
            nation[key] = int(number) if key == "population" else number
        nations.append(nation)

    def _write(name: str, data) -> None:
        path = os.path.join(SCENARIO, name)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(data, fh, ensure_ascii=False, indent=2)
        print(f"{name}: {len(data)}")

    _write("cities.json", cities)
    _write("ports.json", ports)
    _write("buildings.json", buildings)
    _write("nations.json", nations)

    # i18n: key,pt_BR,en,es -> { locale: { key: value } }
    # MESCLA com o JSON existente (recal3, item 4): o CSV é a fonte das chaves
    # de cidade/porto/nação, mas o JSON versionado carrega chaves que não nascem
    # de CSV (`map.region.*`, `map.legend.routes.*`, `city.services.*`,
    # `dock.services`, `map.icon.industry`). Regenerar do zero APAGAVA essas
    # chaves e a UI caía para a chave crua. Agora: o CSV vence no conflito e o
    # que só existe no JSON é preservado.
    with open(os.path.join(I18N_DIR, "i18n.csv"), encoding="utf-8") as fh:
        csv_rows = list(csv.DictReader(fh))
    path_json = os.path.join(I18N_DIR, "i18n.json")
    existing = {loc: {} for loc in LOCALES}
    if os.path.exists(path_json):
        with open(path_json, encoding="utf-8") as fh:
            data = json.load(fh)
        for loc in LOCALES:
            if isinstance(data.get(loc), dict):
                existing[loc] = {str(k): str(v) for k, v in data[loc].items()}
    i18n, stats = merge_i18n(csv_rows, existing)
    with open(path_json, "w", encoding="utf-8") as fh:
        json.dump(i18n, fh, ensure_ascii=False, indent=1)
    print(f"i18n.json: {len(i18n['pt_BR'])} chaves "
          f"(CSV {stats['from_csv']}, preservadas do JSON "
          f"{stats['preserved']}, sobrescritas pelo CSV {stats['overridden']})")

    print("OK")


if __name__ == "__main__":
    main()
