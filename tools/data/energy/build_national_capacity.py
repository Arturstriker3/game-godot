"""Monta a capacidade instalada nacional por fonte (2024-2025) e a matriz por pais.

Entrada: cache da Ember (`fetch_ember_capacity.py`) e o parque atribuido a cidade
(`data/definitions/energy/power_plants_2026.json`, GPPD/WRI CC BY 4.0).
Saida: `data/definitions/energy/national_capacity.json`.

Regras de dado (E1, aceite do plano):
  * capacidade e matriz por nao na ORDEM DO DADO REAL: usa o ultimo ano nao vazio
    entre 2024 e 2025 por pais (Ember), sem interpolar, sem extrapolar;
  * a matriz usa as 9 fontes do jogo: hydro, solar, wind, coal, gas, oil,
    nuclear, biomass, other. A Ember separa coal/gas/hydro/nuclear/solar/wind/
    bioenergy/other fossil/other renewables; OLEO nao e separado (vem dentro de
    "other fossil") e fica `oil: null` com o motivo declarado — nada e inventado;
  * `other` = other fossil + other renewables, com a decomposicao explicita;
  * a soma das fontes e conferida contra o total da Ember e a diferenca fica
    registrada (`check`);
  * cada pais com usina tem a comparacao "derivado das usinas" x "agregado
    nacional" com o desvio percentual por fonte.

Uso:
    python tools/data/energy/build_national_capacity.py
"""

from __future__ import annotations

import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import build_energy_common as ec  # noqa: E402

CACHE = os.path.join(ROOT, "tools", "data", "energy", "cache")
EMBER_CSV = os.path.join(CACHE, "ember_yearly_global.csv")
EMBER_MANIFEST = os.path.join(CACHE, "ember_yearly_global.manifest.json")
PLANTS_PATH = os.path.join(ec.DEFS, "energy", "power_plants_2026.json")
OUT_PATH = os.path.join(ec.DEFS, "energy", "national_capacity.json")

YEARS = (2024, 2025)
TOLERANCE_MW = 0.5

# Fonte da Ember -> grupo do jogo. `other fossil` e `other renewables` caem em
# "other"; o oleo NAO existe separado na Ember (fica dentro de other fossil).
EMBER_TO_GAME = {
    "Hydro": "hydro",
    "Solar": "solar",
    "Wind": "wind",
    "Coal": "coal",
    "Gas": "gas",
    "Nuclear": "nuclear",
    "Bioenergy": "biomass",
    "Other fossil": "other",
    "Other renewables": "other",
}
GAME_SOURCES = ("hydro", "solar", "wind", "coal", "gas", "oil", "nuclear",
                "biomass", "other")


def load_ember() -> dict:
    """{iso3: {year: {source: capacity_gw}}} e nomes de area."""
    series: dict[str, dict[int, dict[str, float]]] = {}
    names: dict[str, str] = {}
    with open(EMBER_CSV, encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            iso3 = (row.get("ISO 3 code") or "").strip()
            if not iso3 or row.get("Area type") == "Region":
                continue
            try:
                year = int(row["Year"])
            except (TypeError, ValueError):
                continue
            if year not in YEARS:
                continue
            raw = (row.get("Capacity (GW)") or "").strip()
            if raw == "":
                continue
            try:
                value = float(raw) * 1000.0  # GW -> MW
            except ValueError:
                continue
            source = row["Electricity source"]
            series.setdefault(iso3, {}).setdefault(year, {})[source] = value
            names[iso3] = row.get("Area", iso3)
    return series, names


def choose_year(by_year: dict[int, dict[str, float]]) -> int | None:
    """Ultimo ano com capacidade POSITIVA (nao interpola: se falta, assume o mais novo).

    Ano com so zeros (ex.: KGZ 2025 trazia apenas `Other renewables 0.0`) nao conta
    como dado: fica valendo o ano anterior com capacidade de verdade.
    """
    for year in sorted(by_year, reverse=True):
        raw = by_year[year]
        total = raw.get("Total generation")
        if total is not None and total > 0.0:
            return year
    for year in sorted(by_year, reverse=True):
        if any(value > 0.0 for value in by_year[year].values()):
            return year
    return None


def build_nation(tag: str, iso3: str, by_year: dict[int, dict[str, float]],
                 area_name: str) -> dict:
    year = choose_year(by_year)
    raw = by_year[year]
    by_source = {source: None for source in GAME_SOURCES}
    for ember_source, value in raw.items():
        group = EMBER_TO_GAME.get(ember_source)
        if group is None:
            continue
        current = by_source[group]
        by_source[group] = round((current or 0.0) + value, 3)
    total = raw.get("Total generation")
    sum_sources = round(sum(v for v in by_source.values() if v is not None), 3)
    if total is None:
        total = sum_sources
        total_source = "sum_sources"
    else:
        total_source = "ember_total"
    total = round(total, 3)
    delta = round(sum_sources - total, 3)
    return {
        "nation": tag,
        "country_iso3": iso3,
        "area_name": area_name,
        "year": year,
        "total_mw": total,
        "total_source": total_source,
        "by_source": by_source,
        "other_breakdown": {
            "other_fossil_mw": round(raw.get("Other fossil") or 0.0, 3),
            "other_renewables_mw": round(raw.get("Other renewables") or 0.0, 3),
        },
        "ember_raw_mw": {name: round(value, 3) for name, value in sorted(raw.items())},
        "check": {
            "sum_sources_mw": sum_sources,
            "delta_mw": delta,
            "sum_matches_total": abs(delta) <= TOLERANCE_MW,
        },
        "confidence": "high",
        "method": "ember_capacity_latest_year_2024_2026",
    }


def load_plant_totals() -> dict:
    if not os.path.exists(PLANTS_PATH):
        return {}
    with open(PLANTS_PATH, encoding="utf-8") as handle:
        payload = json.load(handle)
    return payload.get("by_nation", {})


def plant_comparison(nations: dict, plants: dict) -> dict:
    out = {}
    for tag, entry in nations.items():
        park = plants.get(tag)
        if not park:
            continue
        by_source = {}
        for source in ("hydro", "solar", "wind", "coal", "gas", "nuclear", "biomass", "other"):
            plant_mw = round(float((park.get("by_fuel") or {}).get(source) or 0.0), 3)
            national_mw = entry["by_source"].get(source)
            if national_mw is None:
                by_source[source] = {"plant_mw": plant_mw, "national_mw": None,
                                     "deviation_pct": None,
                                     "note": "fonte ausente no agregado nacional"}
                continue
            deviation = None
            if national_mw > 0:
                deviation = round((plant_mw / national_mw - 1.0) * 100.0, 1)
            by_source[source] = {"plant_mw": plant_mw,
                                 "national_mw": round(national_mw, 3),
                                 "deviation_pct": deviation}
        plant_total = round(float(park.get("total_mw") or 0.0), 3)
        national_total = entry["total_mw"]
        out[tag] = {
            "nation": tag,
            "year": {"plants": "gppd_2021", "national": entry["year"]},
            "plant_total_mw": plant_total,
            "national_total_mw": national_total,
            "deviation_pct": (round((plant_total / national_total - 1.0) * 100.0, 1)
                              if national_total > 0 else None),
            "by_source": by_source,
        }
    return out


def main() -> int:
    if not os.path.exists(EMBER_CSV):
        raise SystemExit("cache da Ember ausente: rode tools/data/energy/fetch_ember_capacity.py")
    series, names = load_ember()
    tags, _ = ec.load_registry()
    with open(EMBER_MANIFEST, encoding="utf-8") as handle:
        ember_manifest = json.load(handle)

    nations = {}
    missing = []
    for tag in tags:
        candidates = [ec.nation_code(tag), tag]
        found = next((iso for iso in candidates if iso in series), None)
        if found is None:
            missing.append(tag)
            continue
        by_year = series[found]
        year = choose_year(by_year)
        if year is None:
            missing.append(tag)
            continue
        nations[tag] = build_nation(tag, found, by_year, names.get(found, found))

    plants = load_plant_totals()
    comparison = plant_comparison(nations, plants)

    unstable = sorted(tag for tag, entry in nations.items()
                      if not entry["check"]["sum_matches_total"])
    no_2025 = sorted(tag for tag, entry in nations.items() if entry["year"] == 2024)
    top = sorted(nations.values(), key=lambda entry: entry["total_mw"], reverse=True)

    payload = {
        "meta": {
            "generated_by": "tools/data/energy/build_national_capacity.py",
            "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
            "sources": {
                "capacity": {
                    "dataset": "Ember, Yearly Electricity Data (global release)",
                    "url": ember_manifest.get("url"),
                    "landing_url": ember_manifest.get("landing_url"),
                    "sha256": ember_manifest.get("sha256"),
                    "accessed_at_utc": ember_manifest.get("accessed_at_utc"),
                    "license": ember_manifest.get("license"),
                    "citation": ember_manifest.get("citation"),
                },
                "plants": {
                    "dataset": "Global Power Plant Database v1.3.0 (WRI)",
                    "license": {"name": "CC BY 4.0", "commercial_ok": True},
                    "note": "comparacao derivada das usinas; referencia ~2021, NAO 2024-2025",
                },
            },
            "method": ("capacidade (GW -> MW) do ultimo ano nao vazio entre 2024 e 2025 "
                       "por pais; nenhuma interpolacao nem extrapolacao; paises sem 2025 "
                       "ficam com o ano 2024 declarado em `year`"),
            "source_map": {
                "hydro": "Hydro", "solar": "Solar", "wind": "Wind", "coal": "Coal",
                "gas": "Gas", "nuclear": "Nuclear", "biomass": "Bioenergy",
                "other": "Other fossil + Other renewables",
                "oil": None,
            },
            "gaps": {
                "oil": ("a Ember nao separa oleo: ele esta dentro de 'Other fossil', que "
                        "entra em `other` (other_breakdown.other_fossil_mw). `oil` fica "
                        "null — a IRENA separa, mas os termos dela restringem uso "
                        "comercial, entao nao foi usada."),
                "other_renewables": ("geotermia, mare, etc. nao existem como coluna propria "
                                     "na Ember; entram em 'Other renewables' e daí em `other`."),
                "nations_without_ember": missing,
            },
            "counts": {
                "game_nations": len(tags),
                "nations_with_national_capacity": len(nations),
                "nations_missing": len(missing),
                "nations_year_2024": len(no_2025),
                "nations_with_plant_comparison": len(comparison),
                "nations_sum_mismatch": len(unstable),
                "world_total_mw": round(sum(e["total_mw"] for e in nations.values()), 3),
            },
            "plant_comparison_note": ("GPPD e ~2021 e cobre usinas catalogadas; a capacidade "
                                      "nacional e 2024-2025. O desvio negativo esperado "
                                      "mede base desatualizada + cobertura, nao erro de soma."),
        },
        "nations": dict(sorted(nations.items())),
        "plant_comparison": dict(sorted(comparison.items())),
        "top_by_total_mw": [
            {"nation": entry["nation"], "year": entry["year"],
             "total_mw": entry["total_mw"]} for entry in top[:10]
        ],
    }
    ec.write_json(OUT_PATH, payload)
    print("nações com capacidade nacional: %d/%d (faltam %d)"
          % (len(nations), len(tags), len(missing)))
    print("sem 2025 (ano 2024): %d | soma != total: %d"
          % (len(no_2025), len(unstable)))
    print("comparação com o parque: %d nações" % len(comparison))
    for entry in top[:10]:
        print("  %-4s %6d %10.1f MW" % (entry["nation"], entry["year"], entry["total_mw"]))
    if missing:
        print("faltam (sem Ember): %s" % " ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
