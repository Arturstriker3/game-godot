#!/usr/bin/env python3
"""Utilitários comuns dos construtores da Parte G (agricultura sazonal).

Somente leitura dos arquivos do projeto + escrita determinística de JSON.
Nada aqui inventa número: só resolve ids, geografia derivada e formato.

- `nations()`      -> {id: nome} das 179 nações de `scenario/nations.json`
- `game_food_crops()` -> ids de primário de comida em `economy/goods.json`
- `centroids()`    -> {id: (lat, lon)} derivado das cidades (x/y do mapa
  equirretangular 5000x2500, que é a projeção declarada em
  `natural/_sources.json`), ponderado pela população de cenário.
- `climate_band()` -> faixa de latitude usada na REGRA declarada de calendário
  quando o país não tem calendário observado.
- `write_json()`   -> UTF-8 sem BOM, tab, newline LF, ordem de inserção.
"""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEF = ROOT / "data" / "definitions"
OUT_DIR = DEF / "agriculture"

# Faixas de latitude da regra declarada (não são classificação climática
# observada; servem só para agrupar países sem calendário publicado).
BANDS = (
    (23.5, "tropical"),
    (35.0, "subtropical"),
    (55.0, "temperate"),
    (90.0, "cold"),
)


def load_json(path: Path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def nations() -> dict[str, str]:
    rows = load_json(DEF / "scenario" / "nations.json")
    return {row["id"]: row["name"] for row in rows}


def game_food_crops() -> list[str]:
    """Primários de comida do jogo (kind == raw e food == true)."""
    goods = load_json(DEF / "economy" / "goods.json")
    return [g["id"] for g in goods if g.get("kind") == "raw" and g.get("food")]


def _city_xy_to_latlon(x: float, y: float) -> tuple[float, float]:
    # Projeção equirretangular 5000x2500 declarada em natural/_sources.json.
    lon = (x / 5000.0) * 360.0 - 180.0
    lat = 90.0 - (y / 2500.0) * 180.0
    return lat, lon


def centroids() -> dict[str, tuple[float, float]]:
    """Centroide do país = média das cidades ponderada pela população."""
    cities = load_json(DEF / "scenario" / "cities.json")
    acc: dict[str, list[float]] = {}
    for city in cities:
        nation = city.get("nation") or city.get("home_nation")
        if not nation:
            continue
        lat, lon = _city_xy_to_latlon(float(city["x"]), float(city["y"]))
        weight = float(city.get("population") or city.get("sim_population") or 1.0)
        slot = acc.setdefault(nation, [0.0, 0.0, 0.0])
        slot[0] += lat * weight
        slot[1] += lon * weight
        slot[2] += weight
    return {k: (v[0] / v[2], v[1] / v[2]) for k, v in acc.items() if v[2] > 0}


def hemisphere(lat: float) -> str:
    return "south" if lat < 0 else "north"


def climate_band(lat: float) -> str:
    absolute = abs(lat)
    for limit, name in BANDS:
        if absolute < limit:
            return name
    return "cold"


def write_json(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, ensure_ascii=False, indent="\t")
    path.write_text(text + "\n", encoding="utf-8", newline="\n")


def season_months(start: int, end: int) -> list[int]:
    """Meses cobertos por uma janela que pode cruzar o ano."""
    if start <= end:
        return list(range(start, end + 1))
    return list(range(start, 13)) + list(range(1, end + 1))


def dedupe_seasons(seasons: list[dict]) -> list[dict]:
    """Remove safras repetidas preservando a ordem e a primeira ocorrência."""
    seen: set[str] = set()
    out: list[dict] = []
    for season in seasons:
        key = json.dumps(season, ensure_ascii=False, sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        out.append(season)
    return out
