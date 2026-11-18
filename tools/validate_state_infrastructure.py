#!/usr/bin/env python3
"""Valida data/definitions/scenario/state_infrastructure_2026.json.

Confere integridade estrutural e coerência com as fontes do repositório:
  1. conjunto de estados == state_regions.json (ids e nação)
  2. faixas de percentual (0..100) e do índice LPI (1..5)
  3. infra_levels 0..3 e recalculados pela regra declarada em conversion_rule
  4. kInfraMaxLevel de gdextension/src/sim/sim_constants.h == infra_max_level
  5. valores do estado == valores da nação (sem drift)
  6. coverage reconferida e data_status coerente (no_data com reason, observado sem)
  7. refs presentes para toda nação com algum valor

Uso: python tools/validate_state_infrastructure.py
Exit 0 (imprime OK) ou 1 (lista de problemas). Apenas stdlib.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DATASET = ROOT / "data" / "definitions" / "scenario" / "state_infrastructure_2026.json"
STATE_REGIONS = ROOT / "data" / "definitions" / "population" / "state_regions.json"
NATIONS = ROOT / "data" / "definitions" / "scenario" / "nations.json"
SIM_CONSTANTS = ROOT / "gdextension" / "src" / "sim" / "sim_constants.h"

PERCENT_FIELDS = (
    "electricity_access_pct",
    "electricity_access_urban_pct",
    "electricity_access_rural_pct",
    "water_basic_pct",
    "water_basic_urban_pct",
    "water_basic_rural_pct",
)
STATE_YEAR_FIELDS = (
    "electricity_year_total",
    "electricity_year_urban",
    "electricity_year_rural",
    "water_year_total",
    "water_year_urban",
    "water_year_rural",
)
DIMENSIONS = ("total", "urban", "rural")
INFRA_TYPES = ("road", "power", "water")

problems: list[str] = []


def fail(message: str) -> None:
    problems.append(message)


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def nation_flat(record: dict[str, Any]) -> dict[str, Any]:
    """Achata o bloco da nação para o mesmo formato dos estados."""
    electricity = record.get("electricity") or {}
    water = record.get("water") or {}
    road = record.get("road") or {}
    flat = {
        "electricity_access_pct": electricity.get("total_pct"),
        "electricity_access_urban_pct": electricity.get("urban_pct"),
        "electricity_access_rural_pct": electricity.get("rural_pct"),
        "water_basic_pct": water.get("total_pct"),
        "water_basic_urban_pct": water.get("urban_pct"),
        "water_basic_rural_pct": water.get("rural_pct"),
        "road_quality_index": road.get("quality_index"),
        "road_year": road.get("year"),
    }
    for family, block in (("electricity", electricity), ("water", water)):
        for dim in DIMENSIONS:
            flat[f"{family}_year_{dim}"] = block.get(f"year_{dim}")
    return flat


def band_level(value: float, bands: list[dict[str, Any]], key: str, no_data_level: int) -> int:
    level = no_data_level
    for band in sorted(bands, key=lambda item: item[key]):
        if value >= float(band[key]):
            level = int(band["level"])
    return level


def levels_from_values(flat: dict[str, Any], rule: dict[str, Any]) -> dict[str, int]:
    no_data_level = int(rule.get("no_data_level", 0))

    def percent_level(rule_key: str) -> int:
        spec = rule[rule_key]
        value = flat.get(spec["field"])
        if value is None:
            value = flat.get(spec["fallback"])
        if value is None:
            return no_data_level
        return band_level(float(value), spec["bands"], "min_pct", no_data_level)

    road_spec = rule["road"]
    road_value = flat.get(road_spec["field"])
    road = (
        no_data_level
        if road_value is None
        else band_level(float(road_value), road_spec["bands"], "min_index", no_data_level)
    )
    return {"road": road, "power": percent_level("power"), "water": percent_level("water")}


def check_states(dataset: dict[str, Any], states_in: dict[str, Any]) -> None:
    states_out = dataset.get("states") or {}
    if not isinstance(states_out, dict):
        fail("states: não é um objeto")
        return
    missing = sorted(set(states_in) - set(states_out))
    extra = sorted(set(states_out) - set(states_in))
    if missing:
        fail(f"states: {len(missing)} ids de state_regions.json ausentes (ex.: {missing[:5]})")
    if extra:
        fail(f"states: {len(extra)} ids fora de state_regions.json (ex.: {extra[:5]})")
    if len(states_out) != len(states_in):
        fail(f"states: {len(states_out)} estados, esperado {len(states_in)}")
    for state_id, state in states_out.items():
        expected_nation = (states_in.get(state_id) or {}).get("nation")
        if expected_nation is not None and state.get("nation") != expected_nation:
            fail(
                f"states[{state_id}]: nation {state.get('nation')!r} != {expected_nation!r} "
                "de state_regions.json"
            )


def check_ranges(dataset: dict[str, Any]) -> None:
    for state_id, state in (dataset.get("states") or {}).items():
        for field in PERCENT_FIELDS:
            value = state.get(field)
            if value is not None and not (0 <= float(value) <= 100):
                fail(f"states[{state_id}].{field}: {value} fora de [0,100]")
        road = state.get("road_quality_index")
        if road is not None and not (1 <= float(road) <= 5):
            fail(f"states[{state_id}].road_quality_index: {road} fora de [1,5]")
    for nation_id, nation in (dataset.get("nations") or {}).items():
        flat = nation_flat(nation)
        for field in PERCENT_FIELDS:
            value = flat.get(field)
            if value is not None and not (0 <= float(value) <= 100):
                fail(f"nations[{nation_id}].{field}: {value} fora de [0,100]")
        road = flat.get("road_quality_index")
        if road is not None and not (1 <= float(road) <= 5):
            fail(f"nations[{nation_id}].road_quality_index: {road} fora de [1,5]")


def check_levels(dataset: dict[str, Any], rule: dict[str, Any]) -> None:
    max_level = int(rule.get("infra_max_level", 0))
    for state_id, state in (dataset.get("states") or {}).items():
        levels = state.get("infra_levels") or {}
        for infra_type in INFRA_TYPES:
            level = levels.get(infra_type)
            if level is None:
                fail(f"states[{state_id}].infra_levels.{infra_type}: ausente")
                continue
            if not (0 <= int(level) <= max_level):
                fail(f"states[{state_id}].infra_levels.{infra_type}: {level} fora de 0..{max_level}")
        expected = levels_from_values(state, rule)
        for infra_type, expected_level in expected.items():
            actual = levels.get(infra_type)
            if actual is not None and int(actual) != expected_level:
                fail(
                    f"states[{state_id}].infra_levels.{infra_type}: {actual} gravado, "
                    f"{expected_level} recalculado pela conversion_rule"
                )
    for nation_id, nation in (dataset.get("nations") or {}).items():
        levels = nation.get("infra_levels") or {}
        flat = nation_flat(nation)
        for infra_type in INFRA_TYPES:
            level = levels.get(infra_type)
            if level is None:
                fail(f"nations[{nation_id}].infra_levels.{infra_type}: ausente")
                continue
            if not (0 <= int(level) <= max_level):
                fail(f"nations[{nation_id}].infra_levels.{infra_type}: {level} fora de 0..{max_level}")
        expected = levels_from_values(flat, rule)
        for infra_type, expected_level in expected.items():
            actual = levels.get(infra_type)
            if actual is not None and int(actual) != expected_level:
                fail(
                    f"nations[{nation_id}].infra_levels.{infra_type}: {actual} gravado, "
                    f"{expected_level} recalculado pela conversion_rule"
                )


def check_sim_constant(dataset: dict[str, Any]) -> None:
    rule = dataset.get("conversion_rule") or {}
    declared = rule.get("infra_max_level")
    if declared is None:
        fail("conversion_rule.infra_max_level: ausente")
        return
    try:
        source = SIM_CONSTANTS.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        fail(f"{SIM_CONSTANTS.relative_to(ROOT)}: não foi possível ler ({error})")
        return
    match = re.search(r"constexpr\s+int\s+kInfraMaxLevel\s*=\s*(\d+)\s*;", source)
    if not match:
        fail(f"{SIM_CONSTANTS.relative_to(ROOT)}: kInfraMaxLevel não encontrado")
        return
    cpp_value = int(match.group(1))
    if cpp_value != int(declared):
        fail(
            "kInfraMaxLevel divergente: sim_constants.h = "
            f"{cpp_value}, conversion_rule.infra_max_level = {declared}"
        )


def is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def state_pct_field(family: str, dim: str) -> str:
    if family == "electricity":
        return "electricity_access_pct" if dim == "total" else f"electricity_access_{dim}_pct"
    return "water_basic_pct" if dim == "total" else f"water_basic_{dim}_pct"


def check_dimension_metadata(dataset: dict[str, Any]) -> None:
    """Ano e fonte POR DIMENSÃO: fonte existe em sources; ano inteiro sse há valor."""
    sources = dataset.get("sources") or {}
    for nation_id, nation in (dataset.get("nations") or {}).items():
        for family in ("electricity", "water"):
            block = nation.get(family) or {}
            for dim in DIMENSIONS:
                pct = block.get(f"{dim}_pct")
                year = block.get(f"year_{dim}")
                source = block.get(f"source_{dim}")
                where = f"nations[{nation_id}].{family}"
                if pct is None:
                    if year is not None:
                        fail(f"{where}.year_{dim}: {year!r} sem percentual")
                    if source is not None:
                        fail(f"{where}.source_{dim}: {source!r} sem percentual")
                    continue
                if not is_int(year):
                    fail(f"{where}.year_{dim}: {year!r} não é inteiro com percentual presente")
                if source is None:
                    fail(f"{where}.source_{dim}: ausente com percentual presente")
                elif source not in sources:
                    fail(f"{where}.source_{dim}: {source!r} não existe em sources")
    for state_id, state in (dataset.get("states") or {}).items():
        for family in ("electricity", "water"):
            for dim in DIMENSIONS:
                pct = state.get(state_pct_field(family, dim))
                year = state.get(f"{family}_year_{dim}")
                where = f"states[{state_id}]"
                if pct is None and year is not None:
                    fail(f"{where}.{family}_year_{dim}: {year!r} sem percentual")
                if pct is not None and not is_int(year):
                    fail(f"{where}.{family}_year_{dim}: {year!r} não é inteiro com percentual presente")


def check_state_matches_nation(dataset: dict[str, Any]) -> None:
    nations = dataset.get("nations") or {}
    for state_id, state in (dataset.get("states") or {}).items():
        nation = nations.get(state.get("nation"))
        if nation is None:
            fail(f"states[{state_id}]: nação {state.get('nation')!r} ausente em nations")
            continue
        flat = nation_flat(nation)
        for field in PERCENT_FIELDS + STATE_YEAR_FIELDS + ("road_year",):
            if state.get(field) != flat.get(field):
                fail(
                    f"states[{state_id}].{field}: {state.get(field)!r} != "
                    f"nations[{state.get('nation')}].{field} {flat.get(field)!r}"
                )
        if (state.get("infra_levels") or {}) != (nation.get("infra_levels") or {}):
            fail(f"states[{state_id}].infra_levels divergente da nação")


def has_any_value(flat: dict[str, Any]) -> bool:    return any(flat.get(field) is not None for field in PERCENT_FIELDS) or (
        flat.get("road_quality_index") is not None
    )


def check_status_and_coverage(dataset: dict[str, Any], nations_total: int) -> None:
    nations = dataset.get("nations") or {}
    states = dataset.get("states") or {}
    coverage = dataset.get("coverage") or {}

    no_data_nations = {nation_id for nation_id, record in nations.items() if record.get("data_status") == "no_data"}
    for nation_id, record in nations.items():
        status = record.get("data_status")
        if status not in ("observed", "no_data"):
            fail(f"nations[{nation_id}].data_status inválido: {status!r}")
            continue
        flat = nation_flat(record)
        if status == "observed":
            if flat["electricity_access_pct"] is None and flat["electricity_access_urban_pct"] is None \
                    and flat["electricity_access_rural_pct"] is None \
                    and flat["water_basic_pct"] is None and flat["water_basic_urban_pct"] is None \
                    and flat["water_basic_rural_pct"] is None:
                fail(f"nations[{nation_id}]: data_status observed sem elétrica nem água")
        else:
            if not record.get("reason"):
                fail(f"nations[{nation_id}]: data_status no_data sem reason")
            if has_any_value(flat):
                fail(f"nations[{nation_id}]: data_status no_data mas tem valor em alguma métrica")
            if any(int(value) != 0 for value in (record.get("infra_levels") or {}).values()):
                fail(f"nations[{nation_id}]: data_status no_data com nível != 0")

    level_counts = {infra_type: {str(level): 0 for level in range(4)} for infra_type in INFRA_TYPES}
    states_observed = 0
    states_no_data = 0
    for state_id, state in states.items():
        nation_id = state.get("nation")
        nation = nations.get(nation_id)
        if nation is None:
            continue
        nation_status = nation.get("data_status")
        state_status = state.get("data_status")
        if nation_status == "observed":
            if state_status != "observed_national":
                fail(f"states[{state_id}].data_status {state_status!r} com nação observed")
            if state.get("reason"):
                fail(f"states[{state_id}]: data_status observed_national com reason")
            states_observed += 1
        else:
            if state_status != "no_data":
                fail(f"states[{state_id}].data_status {state_status!r} com nação no_data")
            if not state.get("reason"):
                fail(f"states[{state_id}]: data_status no_data sem reason")
            states_no_data += 1
            for field in PERCENT_FIELDS:
                if state.get(field) is not None:
                    fail(f"states[{state_id}].{field}: nação no_data exige null")
            if any(int(value) != 0 for value in (state.get("infra_levels") or {}).values()):
                fail(f"states[{state_id}]: nação no_data exige níveis 0")
        for infra_type in INFRA_TYPES:
            level = str((state.get("infra_levels") or {}).get(infra_type))
            if level in level_counts[infra_type]:
                level_counts[infra_type][level] += 1

    recounted = {
        "states_total": len(states),
        "nations_total": nations_total,
        "nations_in_states": len(nations),
        "nations_with_electricity": sum(
            1
            for record in nations.values()
            if any(
                (record.get("electricity") or {}).get(field) is not None
                for field in ("total_pct", "urban_pct", "rural_pct")
            )
        ),
        "nations_with_water": sum(
            1
            for record in nations.values()
            if any(
                (record.get("water") or {}).get(field) is not None
                for field in ("total_pct", "urban_pct", "rural_pct")
            )
        ),
        "nations_with_road": sum(
            1 for record in nations.values() if (record.get("road") or {}).get("quality_index") is not None
        ),
        "nations_no_data": len(no_data_nations),
        "states_observed": states_observed,
        "states_no_data": states_no_data,
        "level_counts": level_counts,
    }
    for key, value in recounted.items():
        if coverage.get(key) != value:
            fail(f"coverage.{key}: {coverage.get(key)!r} != recontagem {value!r}")


def check_refs(dataset: dict[str, Any]) -> None:
    for nation_id, record in (dataset.get("nations") or {}).items():
        flat = nation_flat(record)
        if not has_any_value(flat):
            continue
        refs = record.get("refs") or []
        if not refs:
            fail(f"nations[{nation_id}]: tem valor mas refs vazio")
            continue
        for index, ref in enumerate(refs):
            for key in ("title", "url", "year"):
                if ref.get(key) in (None, ""):
                    fail(f"nations[{nation_id}].refs[{index}].{key}: ausente")


def main() -> int:
    try:
        dataset = load_json(DATASET)
        state_regions = load_json(STATE_REGIONS)["states"]
        nations_total = len(load_json(NATIONS))
    except (OSError, KeyError, ValueError) as error:
        print(f"FALHA ao ler entradas: {error}")
        return 1

    rule = dataset.get("conversion_rule") or {}

    check_states(dataset, state_regions)
    check_ranges(dataset)
    check_levels(dataset, rule)
    check_sim_constant(dataset)
    check_state_matches_nation(dataset)
    check_dimension_metadata(dataset)
    check_status_and_coverage(dataset, nations_total)
    check_refs(dataset)

    if problems:
        print(f"FALHA: {len(problems)} problema(s)")
        for problem in problems:
            print(f"  - {problem}")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
