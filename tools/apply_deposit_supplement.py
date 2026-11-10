"""Aplica o complemento de depósitos reais ao deposits.json gerado.

O extrato MRDS não traz o pré-sal brasileiro, os campos noruegueses do Mar do
Norte nem parte do gás do Oriente Médio. Este script clona um registro-modelo
do mesmo recurso (para manter a distribuição de atributos) e troca posição,
nome e reservas. Localização real; atributos estimados, como nos demais.

Pode ser rodado de novo (idempotente por nome). Se o deposits.json for
regenerado por tools/build_natural_geography.py, rode este script depois.

Uso: python tools/apply_deposit_supplement.py
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")
SUPPLEMENT = os.path.join(ROOT, "data", "definitions", "natural",
                          "deposits_supplement.json")
TEMPLATES = {"oil": "Brent", "gas": "Troll"}


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def find_template(deposits, name):
    for deposit in deposits:
        if name in deposit.get("names", []):
            return deposit
    raise SystemExit("template não encontrado: " + name)


def to_pixels(lon, lat):
    return ((lon + 180.0) / 360.0 * 5000.0, (90.0 - lat) / 180.0 * 2500.0)


def main():
    data = read_json(DEPOSITS)
    deposits = data["deposits"]
    existing = {str(d.get("names", [""])[0]).lower() for d in deposits}
    templates = {key: find_template(deposits, name) for key, name in TEMPLATES.items()}

    added = 0
    next_id = 900000
    for field in read_json(SUPPLEMENT)["fields"]:
        name = field["name"]
        if name.lower() in existing:
            continue
        resource = field["resource"]
        template = templates[resource]
        px, py = to_pixels(field["lon"], field["lat"])
        ratio = float(field["quantity_kt"]) / float(template["quantity_kt"])
        record = dict(template)
        record["id"] = "DEP-%s-%06d" % (resource.upper(), next_id)
        next_id += 1
        record["territory"] = ""
        record["owner"] = "NNN"
        record["px"] = round(px, 1)
        record["py"] = round(py, 1)
        record["lon"] = field["lon"]
        record["lat"] = field["lat"]
        record["quantity_kt"] = float(field["quantity_kt"])
        record["recoverable_kt"] = round(float(template["recoverable_kt"]) * ratio, 1)
        record["quality"] = float(field["quality"])
        record["depth_m"] = int(field["depth_m"])
        record["max_rate_kt_day"] = round(float(template["max_rate_kt_day"]) * ratio, 1)
        record["status"] = field.get("status", "Producer")
        record["names"] = [name]
        record["sites"] = 1
        record["data_quality"] = "real_location_estimated_attributes"
        deposits.append(record)
        existing.add(name.lower())
        added += 1

    if added:
        with open(DEPOSITS, "w", encoding="utf-8") as handle:
            json.dump(data, handle, ensure_ascii=False, indent=1)
    print("depósitos adicionados:", added, "de", len(read_json(SUPPLEMENT)["fields"]))


main()
