#!/usr/bin/env python3
"""Gera data/definitions/scenario/state_infrastructure_2026.json.

Infraestrutura inicial por estado (B5): estrada, energia e água, nível 0..3.
Os valores são NACIONAIS (World Bank WDI) propagados a cada estado do país —
granularidade `nation`. Nenhum valor subnacional é obtido nem estimado: onde
não há observação, o campo fica null e `data_status` = no_data.

Fontes (cache bruto em tools/_tmp_state_infra_raw/, só leitura):
  - World Bank WDI  EG.ELC.ACCS.ZS / .UR.ZS / .RU.ZS  (acesso à eletricidade, %)
  - World Bank WDI  SH.H2O.BASW.ZS / .UR.ZS / .RU.ZS  (água básica JMP, %)
  - World Bank WDI  LP.LPI.INFR.XQ                     (índice LPI de infra, 1..5)
Água: espelho WDI da série JMP (CC BY 4.0). O GHO/WHO `WSH_WATER_BASIC` é
CC BY-NC-SA e serve apenas de conferência — não entra no build.
As séries IS.ROD.* (extensão de estrada) estão descontinuadas e são ignoradas.

Uso: python tools/build_state_infrastructure.py
Saída determinística (bytes idênticos entre execuções).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
RAW = ROOT / "tools" / "_tmp_state_infra_raw"
OUT = ROOT / "data" / "definitions" / "scenario" / "state_infrastructure_2026.json"

EDITION = "2026"
RETRIEVED = "2026-11-18"
YEAR_MIN = 2010
YEAR_MAX = 2025
PCT_DIGITS = 2
INDEX_DIGITS = 3

# Tradução id do jogo -> código World Bank. Carregada de
# data/definitions/economy/inequality.json (campo `iso_worldbank`); não se
# inventa alias: um id sem correspondência declarada fica sem observação.
ISO_OVERRIDES: dict[str, str] = {}

# Regra de conversão declarada no JSON e única fonte dos níveis.
CONVERSION_RULE: dict[str, Any] = {
    "infra_max_level": 3,
    "types": ["road", "power", "water"],
    "no_data_level": 0,
    "power": {
        "field": "electricity_access_rural_pct",
        "fallback": "electricity_access_pct",
        "bands": [
            {"min_pct": 0, "level": 0},
            {"min_pct": 50, "level": 1},
            {"min_pct": 75, "level": 2},
            {"min_pct": 95, "level": 3},
        ],
    },
    "water": {
        "field": "water_basic_rural_pct",
        "fallback": "water_basic_pct",
        "bands": [
            {"min_pct": 0, "level": 0},
            {"min_pct": 50, "level": 1},
            {"min_pct": 75, "level": 2},
            {"min_pct": 95, "level": 3},
        ],
    },
    "road": {
        "field": "road_quality_index",
        "bands": [
            {"min_index": 1.0, "level": 0},
            {"min_index": 2.0, "level": 1},
            {"min_index": 3.0, "level": 2},
            {"min_index": 4.0, "level": 3},
        ],
    },
    "rationale": (
        "Rural nas três métricas de acesso: o nível de infraestrutura da região "
        "mede o pior caso — energia e água chegam primeiro à cidade, e o rural é o "
        "gargalo; a média nacional esconderia a falta de acesso. Água usa o "
        "espelho WDI (SH.H2O.BASW.*, CC BY 4.0) da série JMP; o GHO/WHO foi só "
        "conferência e não entra no arquivo. Estrada usa o LPI de qualidade da "
        "infraestrutura de comércio e transporte (1..5) como proxy de qualidade "
        "percebida: as séries WDI de extensão de estrada estão descontinuadas "
        "(última atualização 2013, sem observações) e não há densidade km/km² "
        "disponível, então nenhum número foi derivado delas."
    ),
}

NOTE = (
    "Valores são NACIONAIS (granularidade nation) e propagados a cada estado do "
    "país; nenhum valor subnacional foi obtido. Água vem do espelho WDI "
    "(SH.H2O.BASW.ZS/.UR.ZS/.RU.ZS, CC BY 4.0) da série JMP; o GHO/WHO "
    "WSH_WATER_BASIC (CC BY-NC-SA) foi apenas conferência, com valores "
    "idênticos, e não é fonte do arquivo. Ausência de observação é representada "
    "por null e data_status no_data — não por cobertura zero. Cada dimensão "
    "(total/urbano/rural) guarda seu próprio ano e fonte; estrada usa o índice "
    "LPI de qualidade da infraestrutura (1..5) como proxy, porque as séries WDI "
    "de extensão de estrada estão mortas e não foram usadas."
)

ELECTRICITY_INDICATORS = {
    "electricity_access_pct": "EG.ELC.ACCS.ZS",
    "electricity_access_urban_pct": "EG.ELC.ACCS.UR.ZS",
    "electricity_access_rural_pct": "EG.ELC.ACCS.RU.ZS",
}
ELECTRICITY_SOURCES = {
    "electricity_access_pct": "worldbank_eg_elc_accs_zs",
    "electricity_access_urban_pct": "worldbank_eg_elc_accs_ur_zs",
    "electricity_access_rural_pct": "worldbank_eg_elc_accs_ru_zs",
}
WATER_INDICATORS = {
    "water_basic_pct": "SH.H2O.BASW.ZS",
    "water_basic_urban_pct": "SH.H2O.BASW.UR.ZS",
    "water_basic_rural_pct": "SH.H2O.BASW.RU.ZS",
}
WATER_SOURCES = {
    "water_basic_pct": "worldbank_sh_h2o_basw_zs",
    "water_basic_urban_pct": "worldbank_sh_h2o_basw_ur_zs",
    "water_basic_rural_pct": "worldbank_sh_h2o_basw_ru_zs",
}
GHO_CROSS_CHECK_ID = "who_unicef_jmp_wsh_water_basic"
ROAD_SOURCE_ID = "worldbank_lp_lpi_infr_xq"
ROAD_RAW = "wb_LP.LPI.INFR.XQ.json"

STATE_YEAR_FIELDS = (
    "electricity_year_total",
    "electricity_year_urban",
    "electricity_year_rural",
    "water_year_total",
    "water_year_urban",
    "water_year_rural",
)

FAMILY_FIELDS: dict[str, dict[str, str]] = {
    "electricity": {
        "total": "electricity_access_pct",
        "urban": "electricity_access_urban_pct",
        "rural": "electricity_access_rural_pct",
    },
    "water": {
        "total": "water_basic_pct",
        "urban": "water_basic_urban_pct",
        "rural": "water_basic_rural_pct",
    },
}

NATION_ONLY_REASON = (
    "nação presente apenas em data/definitions/population/state_regions.json; "
    "não existe em data/definitions/scenario/nations.json"
)
NO_OBSERVATION_REASON = (
    "nenhuma observação 2010–2025 nas séries usadas (World Bank "
    "EG.ELC.ACCS.ZS/.UR.ZS/.RU.ZS, SH.H2O.BASW.ZS/.UR.ZS/.RU.ZS, "
    "LP.LPI.INFR.XQ)"
)


# ---------------------------------------------------------------------------
# Leitura dos dados brutos
# ---------------------------------------------------------------------------


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def worldbank_payload(data: Any) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    """Aceita [meta, rows] (API) ou lista plana de observações."""
    if (
        isinstance(data, list)
        and len(data) == 2
        and isinstance(data[0], dict)
        and isinstance(data[1], list)
    ):
        return data[0], data[1]
    return {}, data


def latest_observation(rows: list[tuple[int, float]]) -> tuple[int, float] | None:
    """Ano mais recente (2010–2025) com valor não nulo."""
    best: tuple[int, float] | None = None
    for year, value in rows:
        if value is None:
            continue
        if not (YEAR_MIN <= year <= YEAR_MAX):
            continue
        if best is None or year > best[0]:
            best = (year, value)
    return best


def as_float(value: Any) -> float | None:
    if value is None:
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if number != number:  # NaN
        return None
    return number


def collect_indicators(
    indicators: dict[str, str],
) -> dict[str, dict[str, tuple[int, float]]]:
    """{iso3: {campo: (ano, valor)}} com o ano mais recente por campo."""
    per_nation: dict[str, dict[str, tuple[int, float]]] = {}
    for field, code in indicators.items():
        _, rows = worldbank_payload(load_json(RAW / f"wb_{code}.json"))
        by_country: dict[str, list[tuple[int, float]]] = {}
        for row in rows:
            iso = row.get("countryiso3code")
            if not iso:
                continue
            year = row.get("date")
            if year is None:
                continue
            try:
                year_int = int(str(year)[:4])
            except ValueError:
                continue
            value = as_float(row.get("value"))
            if value is None:
                continue
            by_country.setdefault(iso, []).append((year_int, value))
        for iso, series in by_country.items():
            latest = latest_observation(series)
            if latest is not None:
                per_nation.setdefault(iso, {})[field] = latest
    return per_nation


def collect_road() -> dict[str, tuple[int, float]]:
    _, rows = worldbank_payload(load_json(RAW / ROAD_RAW))
    by_country: dict[str, list[tuple[int, float]]] = {}
    for row in rows:
        iso = row.get("countryiso3code")
        year = row.get("date")
        value = as_float(row.get("value"))
        if not iso or year is None or value is None:
            continue
        try:
            year_int = int(str(year)[:4])
        except ValueError:
            continue
        by_country.setdefault(iso, []).append((year_int, value))
    out: dict[str, tuple[int, float]] = {}
    for iso, series in by_country.items():
        latest = latest_observation(series)
        if latest is not None:
            out[iso] = latest
    return out


# ---------------------------------------------------------------------------
# Regra de conversão (mesma lógica do validador)
# ---------------------------------------------------------------------------

CONVERSION_RULE_NO_DATA_LEVEL = int(CONVERSION_RULE["no_data_level"])


def band_level(value: float, bands: list[dict[str, Any]], key: str) -> int:
    level = CONVERSION_RULE_NO_DATA_LEVEL
    for band in sorted(bands, key=lambda b: b[key]):
        if value >= float(band[key]):
            level = int(band["level"])
    return level


def percent_level(rule_key: str, record: dict[str, Any]) -> int:
    rule = CONVERSION_RULE[rule_key]
    value = record.get(rule["field"])
    if value is None:
        value = record.get(rule["fallback"])
    if value is None:
        return CONVERSION_RULE_NO_DATA_LEVEL
    return band_level(float(value), rule["bands"], "min_pct")


def road_level(record: dict[str, Any]) -> int:
    rule = CONVERSION_RULE["road"]
    value = record.get(rule["field"])
    if value is None:
        return CONVERSION_RULE_NO_DATA_LEVEL
    return band_level(float(value), rule["bands"], "min_index")


def infra_levels(flat: dict[str, Any]) -> dict[str, int]:
    return {
        "road": road_level(flat),
        "power": percent_level("power", flat),
        "water": percent_level("water", flat),
    }


# ---------------------------------------------------------------------------
# Montagem do dataset
# ---------------------------------------------------------------------------


def iso_overrides() -> dict[str, str]:
    inequality = load_json(ROOT / "data" / "definitions" / "economy" / "inequality.json")
    mapping: dict[str, str] = dict(ISO_OVERRIDES)
    for nation_id, entry in inequality.get("nations", {}).items():
        iso = entry.get("iso_worldbank")
        if iso and iso != nation_id:
            mapping[nation_id] = iso
    return mapping


def round_pct(value: float | None) -> float | None:
    return None if value is None else round(value, PCT_DIGITS)


def round_index(value: float | None) -> float | None:
    return None if value is None else round(value, INDEX_DIGITS)


def worldbank_source(code: str, indicator: str, last_updated: str) -> dict[str, Any]:
    return {
        "title": indicator,
        "publisher": "World Bank — World Development Indicators",
        "url": f"https://data.worldbank.org/indicator/{code}",
        "api": f"https://api.worldbank.org/v2/country/{{ISO3}}/indicator/{code}?format=json",
        "license": "CC BY 4.0",
        "last_updated": last_updated,
        "retrieved": RETRIEVED,
    }


def build_sources() -> dict[str, Any]:
    sources: dict[str, Any] = {}
    families = ((ELECTRICITY_INDICATORS, ELECTRICITY_SOURCES), (WATER_INDICATORS, WATER_SOURCES))
    for indicators, source_ids in families:
        for field, code in indicators.items():
            meta, rows = worldbank_payload(load_json(RAW / f"wb_{code}.json"))
            indicator = code
            for row in rows:
                label = (row.get("indicator") or {}).get("value")
                if label:
                    indicator = label
                    break
            sources[source_ids[field]] = worldbank_source(
                code,
                indicator,
                str(meta.get("lastupdated") or "not_provided_by_api"),
            )

    road_meta, road_rows = worldbank_payload(load_json(RAW / ROAD_RAW))
    road_title = "Logistics performance index: Quality of trade and transport-related infrastructure (1=low to 5=high)"
    for row in road_rows:
        label = (row.get("indicator") or {}).get("value")
        if label:
            road_title = label
            break
    sources[ROAD_SOURCE_ID] = worldbank_source(
        "LP.LPI.INFR.XQ",
        road_title,
        str(road_meta.get("lastupdated") or "not_provided_by_api"),
    )

    # Conferência, não fonte: mesma série JMP publicada pelo GHO/WHO.
    sources[GHO_CROSS_CHECK_ID] = {
        "title": "Population using at least basic drinking water services (%) — WHO/UNICEF JMP, GHO indicator WSH_WATER_BASIC",
        "url": "https://ghoapi.azureedge.net/api/WSH_WATER_BASIC",
        "license": "CC BY-NC-SA 3.0 IGO",
        "role": "cross_check_nao_usado_no_build",
        "note": "valores idênticos ao espelho WDI SH.H2O.BASW.* (JMP); não é fonte do arquivo",
    }
    return sources


def dimension_ref(iso: str, name: str, code: str, year: int) -> dict[str, Any]:
    return {
        "title": f"World Bank {code} — {name} ({year})",
        "url": f"https://api.worldbank.org/v2/country/{iso}/indicator/{code}?format=json&date={year}",
        "year": year,
    }


def access_block(
    family: str,
    source_ids: dict[str, str],
    observations: dict[str, tuple[int, float]],
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Bloco por família com percentual, ano e fonte POR DIMENSÃO."""
    block: dict[str, Any] = {}
    flat: dict[str, Any] = {}
    family_fields = FAMILY_FIELDS[family]
    for dim in ("total", "urban", "rural"):
        field = family_fields[dim]
        observation = observations.get(field)
        value = None if observation is None else round_pct(observation[1])
        block[f"{dim}_pct"] = value
        block[f"year_{dim}"] = None if observation is None else observation[0]
        block[f"source_{dim}"] = source_ids[field] if observation is not None else None
        flat[field] = value
        flat[f"{family}_year_{dim}"] = None if observation is None else observation[0]
    return block, flat


def build_nation_record(
    nation_id: str,
    name: str | None,
    iso: str,
    electricity: dict[str, tuple[int, float]],
    water: dict[str, tuple[int, float]],
    road: tuple[int, float] | None,
) -> dict[str, Any]:
    record: dict[str, Any] = {
        "name": name,
        "granularity": "nation",
    }

    flat: dict[str, Any] = {}

    electricity_block, electricity_flat = access_block(
        "electricity", ELECTRICITY_SOURCES, electricity
    )
    record["electricity"] = electricity_block
    flat.update(electricity_flat)

    water_block, water_flat = access_block("water", WATER_SOURCES, water)
    record["water"] = water_block
    flat.update(water_flat)

    has_electricity = electricity_block["total_pct"] is not None or (
        electricity_block["urban_pct"] is not None or electricity_block["rural_pct"] is not None
    )
    has_water = water_block["total_pct"] is not None or (
        water_block["urban_pct"] is not None or water_block["rural_pct"] is not None
    )

    if road is not None:
        index = round_index(road[1])
        record["road"] = {
            "quality_index": index,
            "year": road[0],
            "metric": "lpi_infrastructure_quality",
            "source": ROAD_SOURCE_ID,
        }
        flat["road_quality_index"] = index
        flat["road_year"] = road[0]
    else:
        record["road"] = {
            "quality_index": None,
            "year": None,
            "metric": "lpi_infrastructure_quality",
            "source": None,
        }
        flat["road_quality_index"] = None
        flat["road_year"] = None

    if has_electricity or has_water:
        record["data_status"] = "observed"
    else:
        record["data_status"] = "no_data"
        if name is None:
            record["reason"] = NATION_ONLY_REASON
        else:
            record["reason"] = NO_OBSERVATION_REASON

    record["infra_levels"] = infra_levels(flat)

    # refs citam a série que DECIDE o nível: rural, senão total.
    refs: list[dict[str, Any]] = []
    for family, indicators, block in (
        ("electricity", ELECTRICITY_INDICATORS, electricity_block),
        ("water", WATER_INDICATORS, water_block),
    ):
        if block["rural_pct"] is not None:
            dim = "rural"
        elif block["total_pct"] is not None:
            dim = "total"
        elif block["urban_pct"] is not None:
            dim = "urban"
        else:
            continue
        field = FAMILY_FIELDS[family][dim]
        refs.append(
            dimension_ref(
                iso,
                name or nation_id,
                indicators[field],
                int(block[f"year_{dim}"]),
            )
        )
    if road is not None:
        refs.append(
            dimension_ref(iso, name or nation_id, "LP.LPI.INFR.XQ", int(record["road"]["year"]))
        )
    record["refs"] = refs
    record["_flat"] = flat
    return record


STATE_FIELDS = (
    "electricity_access_pct",
    "electricity_access_urban_pct",
    "electricity_access_rural_pct",
    "water_basic_pct",
    "water_basic_urban_pct",
    "water_basic_rural_pct",
    *STATE_YEAR_FIELDS,
    "road_quality_index",
    "road_year",
)


def main() -> int:
    state_regions = load_json(ROOT / "data" / "definitions" / "population" / "state_regions.json")
    states_in = state_regions["states"]
    nations_list = load_json(ROOT / "data" / "definitions" / "scenario" / "nations.json")
    nations_by_id = {nation["id"]: nation for nation in nations_list}

    overrides = iso_overrides()
    electricity = collect_indicators(ELECTRICITY_INDICATORS)
    water = collect_indicators(WATER_INDICATORS)
    road = collect_road()

    nation_ids = sorted({entry["nation"] for entry in states_in.values()})
    nations_out: dict[str, Any] = {}
    for nation_id in nation_ids:
        iso = overrides.get(nation_id, nation_id)
        record = build_nation_record(
            nation_id,
            nations_by_id.get(nation_id, {}).get("name"),
            iso,
            electricity.get(iso, {}),
            water.get(iso, {}),
            road.get(iso),
        )
        nations_out[nation_id] = record

    states_out: dict[str, Any] = {}
    for state_id in sorted(states_in):
        entry = states_in[state_id]
        nation_id = entry["nation"]
        nation = nations_out[nation_id]
        flat = nation["_flat"]
        has_data = nation["data_status"] == "observed"
        state_record: dict[str, Any] = {
            "nation": nation_id,
            "granularity": "nation",
            "data_status": "observed_national" if has_data else "no_data",
        }
        if not has_data:
            # Mesmo texto da nação: a ausência do estado é a ausência do país.
            state_record["reason"] = nation["reason"]
        for key in STATE_FIELDS:
            state_record[key] = flat.get(key)
        state_record["infra_levels"] = dict(nation["infra_levels"])
        states_out[state_id] = state_record

    level_counts: dict[str, dict[str, int]] = {}
    for infra_type in CONVERSION_RULE["types"]:
        counts = {str(level): 0 for level in range(int(CONVERSION_RULE["infra_max_level"]) + 1)}
        for state_record in states_out.values():
            counts[str(state_record["infra_levels"][infra_type])] += 1
        level_counts[infra_type] = counts

    def has_metric(record: dict[str, Any], family: str) -> bool:
        block = record[family]
        if family == "road":
            return block["quality_index"] is not None
        return any(block[field] is not None for field in ("total_pct", "urban_pct", "rural_pct"))

    coverage = {
        "states_total": len(states_out),
        "nations_total": len(nations_list),
        "nations_in_states": len(nations_out),
        "nations_with_electricity": sum(1 for r in nations_out.values() if has_metric(r, "electricity")),
        "nations_with_water": sum(1 for r in nations_out.values() if has_metric(r, "water")),
        "nations_with_road": sum(1 for r in nations_out.values() if has_metric(r, "road")),
        "nations_no_data": sum(1 for r in nations_out.values() if r["data_status"] == "no_data"),
        "states_observed": sum(1 for r in states_out.values() if r["data_status"] == "observed_national"),
        "states_no_data": sum(1 for r in states_out.values() if r["data_status"] == "no_data"),
        "level_counts": level_counts,
    }

    for record in nations_out.values():
        record.pop("_flat", None)

    dataset = {
        "version": 1,
        "dataset": "state_infrastructure",
        "edition": EDITION,
        "retrieved": RETRIEVED,
        "note": NOTE,
        "units": {
            "pct": "percent of population, 0..100",
            "road_quality_index": "LPI 1..5 (1=low, 5=high)",
        },
        "conversion_rule": CONVERSION_RULE,
        "metric_sources": {
            "electricity_access_pct": "worldbank_eg_elc_accs_zs",
            "electricity_access_urban_pct": "worldbank_eg_elc_accs_ur_zs",
            "electricity_access_rural_pct": "worldbank_eg_elc_accs_ru_zs",
            "water_basic_pct": "worldbank_sh_h2o_basw_zs",
            "water_basic_urban_pct": "worldbank_sh_h2o_basw_ur_zs",
            "water_basic_rural_pct": "worldbank_sh_h2o_basw_ru_zs",
            "road_quality_index": ROAD_SOURCE_ID,
        },
        "sources": build_sources(),
        "coverage": coverage,
        "nations": nations_out,
        "states": states_out,
    }

    text = json.dumps(dataset, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)

    print(f"[build] wrote {OUT.relative_to(ROOT)} ({len(text.encode('utf-8'))} bytes)")
    print(f"[build] coverage: {json.dumps(coverage, ensure_ascii=False, sort_keys=True)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
