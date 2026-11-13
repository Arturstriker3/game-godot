#!/usr/bin/env python3
"""Valida os dados de cultura do OpenGS Game.

Verifica ``data/definitions/culture/``:

* ``cultures.csv``/``cultures.json`` — ids únicos, nomes i18n, família, idioma,
  religião e cor legível; ΔE*ab >= 25 entre culturas da mesma família;
* ``nation_cultures.csv`` — toda nação do cenário coberta, cultura principal na
  composição, aceitas existentes e composição somando 100;
* ``city_cultures.csv`` — todas as 834 cidades cobertas, soma 100, no máximo 5
  culturas nomeadas + ``outras``, culturas existentes e nação correta.

Uso::

    python tools/validate_cultures.py
    python tools/validate_cultures.py --root .

Códigos de saída: ``0`` ok, ``1`` erros, ``2`` arquivo ausente/ilegível.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CULTURE_DIR = Path("data/definitions/culture")
SCENARIO_DIR = Path("data/definitions/scenario")
OTHER = "outras"
MIN_DELTA_E = 25.0
MAX_NAMED_CITIES = 5
HEX = re.compile(r"^#[0-9a-fA-F]{6}$")


def read_csv(path: Path) -> list[dict]:
    with path.open("r", encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


def srgb_to_lab(hex_value: str) -> tuple[float, float, float]:
    rgb = [int(hex_value[i : i + 2], 16) / 255.0 for i in (1, 3, 5)]

    def linear(x: float) -> float:
        return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4

    r, g, b = (linear(v) for v in rgb)
    x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047
    y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b)
    z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883

    def f(t: float) -> float:
        return t ** (1 / 3) if t > 0.008856 else (7.787 * t + 16 / 116)

    fx, fy, fz = f(x), f(y), f(z)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


def parse_mix(text: str) -> list[tuple[str, float]]:
    entries = []
    for raw in (text or "").split(";"):
        raw = raw.strip()
        if not raw:
            continue
        if ":" not in raw:
            raise ValueError(f"entrada inválida {raw!r}")
        cid, share = raw.split(":", 1)
        entries.append((cid.strip(), float(share)))
    return entries


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Valida os dados de cultura.")
    parser.add_argument("--root", default=str(ROOT))
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    culture_dir = root / CULTURE_DIR
    scenario_dir = root / SCENARIO_DIR

    errors: list[str] = []
    warnings: list[str] = []

    paths = {
        name: culture_dir / name
        for name in ("cultures.csv", "cultures.json", "nation_cultures.csv", "city_cultures.csv")
    }
    for name, path in paths.items():
        if not path.exists():
            print(f"ERRO: arquivo ausente: {CULTURE_DIR / name}")
            return 2

    try:
        cultures = read_csv(paths["cultures.csv"])
        cultures_json = json.loads(paths["cultures.json"].read_text(encoding="utf-8"))
        nations = read_csv(paths["nation_cultures.csv"])
        cities = read_csv(paths["city_cultures.csv"])
        scenario_nations = read_csv(scenario_dir / "nations.csv")
        scenario_cities = read_csv(scenario_dir / "cities.csv")
    except (OSError, UnicodeDecodeError, csv.Error, json.JSONDecodeError) as exc:
        print(f"ERRO: falha ao ler dados: {exc}")
        return 2

    # --- catálogo ---------------------------------------------------------
    required = ("id", "name_pt", "name_en", "name_es", "family", "language", "religion", "color")
    ids: set[str] = set()
    lab: dict[str, tuple[float, float, float]] = {}
    families: dict[str, list[str]] = {}
    for index, row in enumerate(cultures, start=1):
        label = f"cultures.csv:{index}"
        for field in required:
            if not (row.get(field) or "").strip():
                errors.append(f"{label}: campo vazio {field!r}")
        cid = (row.get("id") or "").strip()
        if not cid:
            continue
        if cid in ids:
            errors.append(f"{label}: id duplicado {cid!r}")
        if cid == OTHER:
            errors.append(f"{label}: 'outras' é reservado")
        ids.add(cid)
        color = (row.get("color") or "").strip()
        if color and not HEX.match(color):
            errors.append(f"{label}: cor inválida {color!r}")
        elif color:
            lab[cid] = srgb_to_lab(color)
        families.setdefault((row.get("family") or "").strip(), []).append(cid)

    for family, members in sorted(families.items()):
        for i, left in enumerate(members):
            for right in members[i + 1 :]:
                if left in lab and right in lab:
                    delta = math.dist(lab[left], lab[right])
                    if delta < MIN_DELTA_E:
                        errors.append(
                            f"cores muito próximas em {family!r}: {left}/{right} "
                            f"ΔE*ab {delta:.1f} < {MIN_DELTA_E:g}"
                        )

    json_ids = [c.get("id") for c in cultures_json]
    if json_ids != [c.get("id") for c in cultures]:
        errors.append("cultures.json não corresponde a cultures.csv (ids/ordem)")
    for row in cultures_json:
        if row.get("color") != next(
            (c.get("color") for c in cultures if c.get("id") == row.get("id")), None
        ):
            errors.append(f"cultures.json: cor divergente para {row.get('id')!r}")

    # --- nações -----------------------------------------------------------
    scenario_nation_ids = {r.get("id") for r in scenario_nations}
    nation_ids = {r.get("nation") for r in nations}
    for missing in sorted(scenario_nation_ids - nation_ids):
        errors.append(f"nation_cultures.csv: nação ausente {missing}")
    for extra in sorted(nation_ids - scenario_nation_ids):
        errors.append(f"nation_cultures.csv: nação desconhecida {extra}")

    for index, row in enumerate(nations, start=2):
        nation = row.get("nation") or f"linha {index}"
        label = f"nation_cultures.csv:{nation}"
        try:
            mix = parse_mix(row.get("composition", ""))
        except ValueError as exc:
            errors.append(f"{label}: {exc}")
            continue
        total = sum(share for _, share in mix)
        if abs(total - 100.0) > 0.05:
            errors.append(f"{label}: composição soma {total:g}")
        used = {cid for cid, _ in mix}
        primary = (row.get("primary_culture") or "").strip()
        if primary not in ids:
            errors.append(f"{label}: cultura principal desconhecida {primary!r}")
        if primary not in used:
            errors.append(f"{label}: cultura principal fora da composição")
        for cid in sorted(used - ids - {OTHER}):
            errors.append(f"{label}: cultura desconhecida {cid!r}")
        for accepted in filter(None, (row.get("accepted_cultures") or "").split(";")):
            accepted = accepted.strip()
            if accepted not in ids:
                errors.append(f"{label}: cultura aceita desconhecida {accepted!r}")
            if accepted == primary:
                errors.append(f"{label}: cultura principal listada como aceita")
        if not (row.get("source") or "").strip():
            errors.append(f"{label}: fonte ausente")
        if not (row.get("year") or "").strip():
            warnings.append(f"{label}: ano da fonte ausente")

    # --- cidades ----------------------------------------------------------
    scenario_city = {r.get("id"): r for r in scenario_cities}
    city_ids = [r.get("city_id") for r in cities]
    for missing in sorted(set(scenario_city) - set(city_ids)):
        errors.append(f"city_cultures.csv: cidade ausente {missing}")
    for extra in sorted(set(city_ids) - set(scenario_city)):
        errors.append(f"city_cultures.csv: cidade desconhecida {extra}")
    if len(city_ids) != len(set(city_ids)):
        errors.append("city_cultures.csv: city_id duplicado")

    regional = 0
    fallback = 0
    for index, row in enumerate(cities, start=2):
        city_id = row.get("city_id") or f"linha {index}"
        label = f"city_cultures.csv:{city_id}"
        try:
            mix = parse_mix(row.get("cultures", ""))
        except ValueError as exc:
            errors.append(f"{label}: {exc}")
            continue
        total = sum(share for _, share in mix)
        if abs(total - 100.0) > 0.05:
            errors.append(f"{label}: mistura soma {total:g}")
        named = [cid for cid, _ in mix if cid != OTHER]
        if len(named) > MAX_NAMED_CITIES:
            errors.append(f"{label}: {len(named)} culturas nomeadas (máx. {MAX_NAMED_CITIES})")
        if len(named) != len(set(named)):
            errors.append(f"{label}: cultura repetida")
        for cid, share in mix:
            if share <= 0:
                errors.append(f"{label}: fração não positiva {cid}:{share:g}")
            if cid != OTHER and cid not in ids:
                errors.append(f"{label}: cultura desconhecida {cid!r}")
        city = scenario_city.get(city_id)
        if city is not None and city.get("nation") != row.get("nation"):
            errors.append(
                f"{label}: nação {row.get('nation')!r} != cenário {city.get('nation')!r}"
            )
        source = (row.get("source") or "").strip()
        if not source:
            errors.append(f"{label}: fonte ausente")
        elif source == "national_fallback":
            fallback += 1
        else:
            regional += 1

    print(f"culturas: {len(cultures)}")
    print(f"famílias: {len(families)}")
    print(f"nações: {len(nations)}")
    print(f"cidades: {len(cities)} (regional {regional}, fallback {fallback})")
    for message in warnings:
        print(f"AVISO: {message}")
    for message in errors:
        print(f"ERRO: {message}")
    print("OK" if not errors else f"{len(errors)} erro(s)")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
