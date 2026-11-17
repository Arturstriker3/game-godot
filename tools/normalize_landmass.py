"""Normaliza o rótulo `landmass` das cidades e portos do cenário.

Problema corrigido (economy_live_path_test): a Europa continental aparecia com
DOIS rótulos — `continent` (BE/DE/FR/NL/VAT e os portos legados) e `europe`
(demais países). Como a posse mineral usa a massa de terra da cidade mais
próxima do depósito, o minério da Lorena (mais perto de Luxemburgo, `europe`)
era entregue às cidades francesas também rotuladas `europe`: Fort-de-France,
Mamoudzou e Saint-Denis — 771 mil kt de ferro atravessando o Atlântico.

Regra (idempotente):

1. A massa de terra da Europa continental é `continent` (o rótulo já usado pelos
   portos legados em `ports_source.csv`); `europe` passa a `continent`.
2. Ilhas que herdaram o rótulo do país recebem o nome da própria ilha, para não
   disputar depósitos com o continente.

Edita a fonte (`data/definitions/world/*_source.csv`) e o cenário gerado
(`data/definitions/scenario/cities.csv`, `ports.csv`, `cities.json`,
`ports.json`), sempre preservando formatação e fim de linha. Depois rode
`tools/build_scenario_data.py` se o pipeline completo for refeito.

Uso: python tools/normalize_landmass.py [--check]
"""

from __future__ import annotations

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORLD = os.path.join(ROOT, "data", "definitions", "world")
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# r10 (recal3): portos legados com rótulo de landmass trocado (o `nearest_port`
# do sim filtra por nation+landmass, então um porto com o rótulo errado nunca é
# escolhido). A tabela e o motivo de cada linha vivem na curadoria.
from world_city_curation import PORT_LANDMASS_FIX  # noqa: E402

CANONICAL = {"europe": "continent"}

# Ilhas/não-continentais que herdaram o rótulo do país de origem.
ISLAND_BY_ID = {
    "FRA-FOR": "martinique",   # Fort-de-France
    "FRA-MAM": "mayotte",      # Mamoudzou
    "FRA-SAI": "reunion",      # Saint-Denis (Reunião)
    "PRT-ANG": "azores",       # Angra do Heroísmo
    "NOR-LON": "svalbard",     # Longyearbyen
}

CSV_FILES = [
    (os.path.join(WORLD, "cities_source.csv"), "id", "landmass"),
    (os.path.join(WORLD, "ports_source.csv"), "id", "landmass"),
    (os.path.join(SCENARIO, "cities.csv"), "id", "landmass"),
    (os.path.join(SCENARIO, "ports.csv"), "id", "landmass"),
]
JSON_FILES = [
    os.path.join(SCENARIO, "cities.json"),
    os.path.join(SCENARIO, "ports.json"),
]


def _target(row_id: str, landmass: str, table: dict = None) -> str:
    if table is not None:
        # Tabela por id: só os ids listados mudam, e para o valor tabelado.
        fix = table.get(row_id)
        return fix[0] if fix else landmass
    if row_id in ISLAND_BY_ID:
        return ISLAND_BY_ID[row_id]
    return CANONICAL.get(landmass, landmass)


def fix_csv(path: str, id_field: str, landmass_field: str, check: bool,
            table: dict = None) -> int:
    with open(path, encoding="utf-8", newline="") as fh:
        text = fh.read()
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    header = lines[0].split(",")
    id_idx = header.index(id_field)
    lm_idx = header.index(landmass_field)
    changes = 0
    for i in range(1, len(lines)):
        if not lines[i]:
            continue
        fields = lines[i].split(",")
        if len(fields) <= max(id_idx, lm_idx):
            continue
        target = _target(fields[id_idx], fields[lm_idx], table)
        if target != fields[lm_idx]:
            fields[lm_idx] = target
            lines[i] = ",".join(fields)
            changes += 1
    if changes and not check:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(newline.join(lines))
    return changes


def fix_json(path: str, check: bool, table: dict = None) -> int:
    """Substitui só as linhas `"landmass": ...` do bloco da cidade/porto."""
    with open(path, encoding="utf-8", newline="") as fh:
        text = fh.read()
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    changes = 0
    current_id = ""
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith('"id": "'):
            current_id = stripped.split('"')[3]
            continue
        if stripped.startswith('"landmass": "'):
            value = stripped.split('"')[3]
            target = _target(current_id, value, table)
            if target != value:
                lines[i] = line.replace(f'"{value}"', f'"{target}"', 1)
                changes += 1
    if changes and not check:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(newline.join(lines))
    return changes


def main() -> int:
    check = "--check" in sys.argv[1:]
    total = 0
    for path, id_field, lm_field in CSV_FILES:
        changes = fix_csv(path, id_field, lm_field, check)
        total += changes
        print(f"{os.path.relpath(path, ROOT)}: {changes} linha(s)")
    for path in JSON_FILES:
        changes = fix_json(path, check)
        total += changes
        print(f"{os.path.relpath(path, ROOT)}: {changes} linha(s)")
    # r10: passada por id (PORT_LANDMASS_FIX) nos arquivos de PORTO, depois da
    # genérica (o rótulo tabelado vence o canônico).
    for path, id_field, lm_field in (
            (os.path.join(WORLD, "ports_source.csv"), "id", "landmass"),
            (os.path.join(SCENARIO, "ports.csv"), "id", "landmass")):
        changes = fix_csv(path, id_field, lm_field, check, PORT_LANDMASS_FIX)
        total += changes
        print(f"{os.path.relpath(path, ROOT)}: {changes} linha(s) [r10]")
    changes = fix_json(os.path.join(SCENARIO, "ports.json"), check,
                       PORT_LANDMASS_FIX)
    total += changes
    print(f"data/definitions/scenario/ports.json: {changes} linha(s) [r10]")
    # Sanidade: o JSON tem de continuar válido.
    for path in JSON_FILES:
        with open(path, encoding="utf-8") as fh:
            json.load(fh)
    if check:
        print(f"CHECK: {total} linha(s) fora do rótulo canônico")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
