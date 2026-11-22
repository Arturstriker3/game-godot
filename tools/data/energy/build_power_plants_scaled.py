"""Parque inicial ESCALONADO para a matriz Ember 2024-2025 (D-115b).

Entrada:
  * `power_plants_base_2021_gppd.json` — usinas GPPD ~2021 já atribuídas à
    cidade/nação do atlas (regra D-115a);
  * `national_capacity.json` — alvo por nação x fonte (Ember, último ano com as
    9 fontes de capacidade completas — ver `year_reason` no arquivo);
  * `renewable_potential.json` — peso solar/eólico por cidade (GSA/GWA);
  * `hydro_potential.json` — peso hídrico por cidade;
  * `cities.json` — população (peso das fontes sem potencial dedicado).

Regra:
  * para cada nação x fonte com usina no GPPD e alvo Ember > 0, aplica um FATOR
    declarado `alvo / base` (em mili-MW, com o resto de arredondamento na maior
    usina do grupo; a soma fecha exatamente no alvo);
  * fonte SEM usina no GPPD mas com MW na Ember: distribui o alvo entre as
    cidades da nação proporcionalmente ao potencial — solar/eólico pelo
    potencial GSA/GWA da cidade, hídrico pelo potencial hídrico, demais pela
    população (indústria por cidade não existe como dado; declarado). Cada
    cidade recebe uma usina SINTÉTICA declarada (`synthetic: true`);
  * óleo: a Ember não separa (fica em `Other fossil`), então o alvo é null e o
    parque de óleo do GPPD fica NÃO escalonado, com o motivo declarado;
  * nação sem matriz Ember (8 lacunas): parque fica não escalonado e declarado;
  * nenhum MW é inventado sem estar no alvo Ember ou no GPPD.

Saída: `power_plants_2026_scaled.json` (`data_year: 2026`,
`capacity_target_years: [2024, 2025]`, `base_reference_year: 2021`).

Uso:
    python tools/data/energy/build_power_plants_scaled.py
"""

from __future__ import annotations

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import build_energy_common as ec  # noqa: E402

ENERGY = os.path.join(ec.DEFS, "energy")
BASE_PATH = os.path.join(ENERGY, "power_plants_base_2021_gppd.json")
CAPACITY_PATH = os.path.join(ENERGY, "national_capacity.json")
DEMAND_PATH = os.path.join(ENERGY, "national_demand.json")
PARAMS_PATH = os.path.join(ENERGY, "power_params.json")
RENEWABLE_PATH = os.path.join(ENERGY, "renewable_potential.json")
HYDRO_PATH = os.path.join(ENERGY, "hydro_potential.json")
OUT_PATH = os.path.join(ENERGY, "power_plants_2026_scaled.json")

GROUPS = ["hydro", "solar", "wind", "coal", "gas", "oil", "nuclear", "biomass",
          "other"]
# Fontes com alvo DERIVADO da geracao Ember quando a capacidade declarada e
# ausente/zero ou fisicamente insuficiente (D-115): capacidade = geracao /
# (CF do modelo * 8760). O CF e o declarado em power_params.json (espelho de
# sim/power.h), nao um numero inventado.
DERIVED_SOURCES = ("oil", "other", "biomass")
TOLERANCE_MW = 0.01


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def milli(mw):
    return int(round(float(mw) * 1000.0))


def scaled_record(plant, capacity_mw, base_mw, factor, method, note):
    """Registro compacto do parque escalonado (o resto está no arquivo base)."""
    record = {
        "id": plant["id"],
        "name": plant["name"],
        "nation": plant["nation"],
        "city_id": plant.get("city_id"),
        "lat": plant["lat"],
        "lon": plant["lon"],
        "fuel_group": plant["fuel_group"],
        "capacity_mw": round(float(capacity_mw), 3),
        "capacity_mw_gppd": round(float(base_mw), 3),
        "scale_factor": None if factor is None else round(float(factor), 9),
        "scaling_method": method,
        "method": ("gppd_2021_scaled_to_ember" if method == "scaled_to_ember"
                   else "gppd_2021_unadjusted"),
    }
    if note:
        record["scaling_note"] = note
    return record


def city_weights(nation, cities, renewable, hydro):
    """Peso de cada cidade da nação por fonte (declarado)."""
    city_rows = [city for city in cities if city["tag"] == nation]
    out = {}
    for source in GROUPS:
        weights = {}
        for city in city_rows:
            cid = city["id"]
            weight = 0.0
            if source == "solar":
                solar = ((renewable.get("by_city") or {}).get(cid) or {}).get("solar") or {}
                weight = float(solar.get("kwh_m2_year") or solar.get("kwh_m2_day_mean") or 0.0)
                basis = "gsa_ghi_kwh_m2_year"
            elif source == "wind":
                wind = ((renewable.get("by_city") or {}).get(cid) or {}).get("wind") or {}
                weight = float(wind.get("m_s_mean") or wind.get("power_density_w_m2") or 0.0)
                basis = "gwa_wind_100m"
            elif source == "hydro":
                entry = ((hydro.get("by_city") or {}).get(cid) or {})
                weight = float(entry.get("potential_mw") or 0.0)
                basis = "hydro_potential_mw"
            else:
                weight = float(city.get("population") or 0.0)
                basis = "city_population"
            weights[cid] = {"weight": weight, "basis": basis}
        out[source] = weights
    return out


def allocate(target_milli, weights):
    """Maior resto determinístico: {city_id: milli} somando exatamente o alvo."""
    positives = [(cid, row["weight"]) for cid, row in weights.items()
                 if row["weight"] > 0.0]
    if not positives:
        # Sem peso: rateio igual entre as cidades (declarado no meta).
        positives = [(cid, 1.0) for cid in sorted(weights)]
    if not positives:
        return {}
    total_weight = sum(weight for _cid, weight in positives)
    raw = {cid: target_milli * weight / total_weight for cid, weight in positives}
    floor = {cid: int(value) for cid, value in raw.items()}
    remainder = target_milli - sum(floor.values())
    order = sorted(raw, key=lambda cid: (-(raw[cid] - floor[cid]), cid))
    for cid in order[:remainder]:
        floor[cid] += 1
    return {cid: value for cid, value in floor.items() if value > 0}


def ember_sub_target(entry, source, targets):
    """Capacidade Ember da SUBCATEGORIA certa para oil/other (D-115)."""
    if entry and source in ("oil", "other"):
        breakdown = entry.get("other_breakdown") or {}
        key = ("other_fossil_mw" if source == "oil"
               else "other_renewables_mw")
        if key in breakdown and breakdown.get(key) is not None:
            return float(breakdown[key])
    return targets.get(source)


def derived_capacity(tag, source, demand_nations, cf_model):
    """Geracao Ember / (CF declarado * 8760) para uma fonte, em MW."""
    cf = float(cf_model.get(source) or 0.0)
    if cf <= 0.0:
        return None
    row = (demand_nations.get(tag) or {})
    generation = (row.get("generation_twh") or {}).get(source)
    if generation is None:
        return None
    generation = float(generation)
    if generation <= 0.0:
        return None
    mw = generation * 1e6 / (cf * 8760.0)
    return {
        "generation_twh": round(generation, 4),
        "cf": cf,
        "capacity_mw": round(mw, 3),
        "formula": ("generation_twh * 1e6 / (cf * 8760); cf = power_params.json "
                    "cf_%s (modelo declarado, espelho de sim/power.h)" % source),
        "cf_source": ("data/definitions/energy/power_params.json "
                      "(cf_%s; modelo declarado, nao dado observado)" % source),
        "generation_source": ("Ember, Yearly Electricity Data; bucket %s do "
                              "national_demand.json (Other fossil->oil, "
                              "Other renewables->other, Bioenergy->biomass)"
                              % source),
    }


def main() -> int:
    base = load(BASE_PATH)
    capacity = load(CAPACITY_PATH)
    demand = load(DEMAND_PATH) if os.path.exists(DEMAND_PATH) else {}
    params = load(PARAMS_PATH).get("params", {}) if os.path.exists(PARAMS_PATH) else {}
    demand_nations = demand.get("by_nation") or {}
    cf_model = {source: float(params.get("cf_" + source) or 0.0)
                for source in DERIVED_SOURCES}
    renewable = load(RENEWABLE_PATH) if os.path.exists(RENEWABLE_PATH) else {}
    hydro = load(HYDRO_PATH) if os.path.exists(HYDRO_PATH) else {}
    cities = ec.load_cities()
    city_by_id = {city["id"]: city for city in cities}

    nations = capacity.get("nations") or {}
    base_nations = base.get("by_nation") or {}
    plants = base.get("plants") or []

    # Agrupa as usinas reais por nação x fonte.
    groups = {}
    for plant in plants:
        tag = plant.get("nation")
        if not tag:
            continue
        groups.setdefault(tag, {}).setdefault(plant["fuel_group"], []).append(plant)

    scaled_plants = []
    scale_factors = {}
    nation_source_check = {}
    synthetic_count = 0
    no_target = []
    derived_targets = {}
    for tag in sorted(set(base_nations) | set(nations) | set(demand_nations)):
        entry = nations.get(tag) or {}
        targets = entry.get("by_source") or {}
        base_by_fuel = (base_nations.get(tag) or {}).get("by_fuel") or {}
        weights_by_source = city_weights(tag, cities, renewable, hydro)
        for source in GROUPS:
            ember_target = ember_sub_target(entry, source, targets)
            derived = (derived_capacity(tag, source, demand_nations, cf_model)
                       if source in DERIVED_SOURCES else None)
            target_source = None
            if derived is not None:
                derived_mw = float(derived["capacity_mw"])
                derived_targets["%s/%s" % (tag, source)] = derived
                if ember_target is None:
                    target = derived_mw
                    target_source = "derived_generation_over_cf"
                elif derived_mw > float(ember_target) + TOLERANCE_MW:
                    target = derived_mw
                    target_source = "max_ember_capacity_derived_generation"
                else:
                    target = float(ember_target)
                    target_source = "ember_capacity"
            elif ember_target is not None:
                target = float(ember_target)
                target_source = "ember_capacity"
            else:
                target = None
            group = groups.get(tag, {}).get(source, [])
            base_mw = round(sum(float(p["capacity_mw"]) for p in group), 3)
            key = "%s/%s" % (tag, source)

            def derived_fields(row):
                row["target_source"] = target_source
                row["ember_target_mw"] = (None if ember_target is None
                                          else round(float(ember_target), 3))
                if derived is not None:
                    row["derived_target_mw"] = derived["capacity_mw"]
                    row["derivation"] = derived
                return row

            if target is None:
                # Geracao nao publicada ou nao ha alvo: nao escala.
                note = ("ember_oil_generation_missing" if source == "oil"
                        else "national_target_missing")
                for plant in group:
                    scaled_plants.append(scaled_record(
                        plant, plant["capacity_mw"], plant["capacity_mw"],
                        None, "unscaled", note))
                scale_factors[key] = {"base_mw": base_mw, "target_mw": None,
                                      "factor": None, "method": "unscaled",
                                      "note": note}
                nation_source_check[key] = derived_fields({
                    "target_mw": None, "scaled_mw": base_mw,
                    "delta_mw": 0.0, "ok": True, "note": note})
                continue

            target = float(target)
            if target <= 0.0 and base_mw > 0.0:
                # Alvo zero na Ember (fonte desativada): fator 0 declarado.
                for plant in group:
                    scaled_plants.append(scaled_record(
                        plant, 0.0, plant["capacity_mw"], 0.0,
                        "scaled_to_ember", "retired_by_ember_target_zero"))
                scale_factors[key] = {"base_mw": base_mw, "target_mw": target,
                                      "factor": 0.0,
                                      "method": "scaled_to_ember",
                                      "note": "retired_by_ember_target_zero"}
                nation_source_check[key] = derived_fields({
                    "target_mw": target, "scaled_mw": 0.0,
                    "delta_mw": round(-target, 3),
                    "ok": abs(0.0 - target) <= TOLERANCE_MW,
                    "method": "retired_by_ember_target_zero"})
                continue

            if base_mw > 0.0:
                base_milli = milli(base_mw)
                target_milli = milli(target)
                factor = target_milli / base_milli if base_milli else 1.0
                scaled_milli = [int(round(milli(p["capacity_mw"]) * factor))
                                for p in group]
                residual = target_milli - sum(scaled_milli)
                if residual:
                    biggest = max(range(len(group)),
                                  key=lambda i: milli(group[i]["capacity_mw"]))
                    scaled_milli[biggest] += residual
                for plant, value in zip(group, scaled_milli):
                    if value <= 0:
                        note = "scaled_to_zero"
                    elif target_source == "ember_capacity":
                        note = None
                    else:
                        note = "derived_target_generation_over_cf"
                    scaled_plants.append(scaled_record(
                        plant, value / 1000.0, plant["capacity_mw"], factor,
                        "scaled_to_ember", note))
                scale_factors[key] = {"base_mw": base_mw, "target_mw": target,
                                      "factor": round(factor, 9),
                                      "method": "scaled_to_ember",
                                      "target_source": target_source,
                                      "note": ("derived_target_generation_over_cf"
                                               if target_source != "ember_capacity"
                                               else None)}
                scaled_mw = round(sum(value / 1000.0 for value in scaled_milli), 3)
                nation_source_check[key] = derived_fields({
                    "target_mw": target, "scaled_mw": scaled_mw,
                    "delta_mw": round(scaled_mw - target, 3),
                    "ok": abs(scaled_mw - target) <= TOLERANCE_MW,
                    "method": "scaled_to_ember"})
                continue

            # Base zero e alvo > 0: usina sintética por cidade (peso declarado).
            if target_milli := milli(target):
                allocation = allocate(target_milli, weights_by_source[source])
                if not allocation:
                    scale_factors[key] = {"base_mw": 0.0, "target_mw": target,
                                          "factor": None,
                                          "method": "no_city_for_allocation",
                                          "note": "nação sem cidade para ratear"}
                    nation_source_check[key] = derived_fields({
                        "target_mw": target, "scaled_mw": 0.0,
                        "delta_mw": round(-target, 3), "ok": False,
                        "method": "no_city_for_allocation"})
                    continue
                for cid, value in sorted(allocation.items()):
                    city = city_by_id[cid]
                    lon, lat = ec.map_to_lonlat(city["x"], city["y"])
                    basis = weights_by_source[source][cid]["basis"]
                    scaled_plants.append({
                        "id": "SYN-%s-%s-%s" % (tag, source, cid),
                        "name": "Ember 2024-2025 %s allocation (%s)" % (
                            source, city["name"]),
                        "gppd_country": None,
                        "gppd_country_game": tag,
                        "gppd_country_alias": None,
                        "province_nation": tag,
                        "nation": tag,
                        "fuel_source": source,
                        "fuel_group": source,
                        "renewable": source in ("hydro", "solar", "wind", "biomass"),
                        "capacity_mw": round(value / 1000.0, 3),
                        "capacity_mw_gppd": 0.0,
                        "scale_factor": None,
                        "scaling_method": "ember_city_allocation",
                        "scaling_note": basis,
                        "lat": round(lat, 6),
                        "lon": round(lon, 6),
                        "province_id": None,
                        "city_id": cid,
                        "city_share": None,
                        "commissioning_year": None,
                        "owner": None,
                        "source": "Ember 2024-2025 (declared city allocation)",
                        "method": "ember_2024_2025_city_allocation",
                        "confidence": "declared_allocation",
                        "synthetic": True,
                    })
                    synthetic_count += 1
                scale_factors[key] = {"base_mw": 0.0, "target_mw": target,
                                      "factor": None,
                                      "method": "ember_city_allocation",
                                      "target_source": target_source,
                                      "weight_basis": sorted(set(
                                          row["basis"]
                                          for row in weights_by_source[source].values()))}
                scaled_mw = round(sum(value / 1000.0
                                      for value in allocation.values()), 3)
                nation_source_check[key] = derived_fields({
                    "target_mw": target, "scaled_mw": scaled_mw,
                    "delta_mw": round(scaled_mw - target, 3),
                    "ok": abs(scaled_mw - target) <= TOLERANCE_MW,
                    "method": "ember_city_allocation"})
            else:
                scale_factors[key] = {"base_mw": 0.0, "target_mw": target,
                                      "factor": None, "method": "no_base_no_target",
                                      "target_source": target_source,
                                      "note": "alvo arredonda a 0"}
                nation_source_check[key] = derived_fields({
                    "target_mw": target, "scaled_mw": 0.0,
                    "delta_mw": round(-target, 3),
                    "ok": True, "method": "rounds_to_zero"})

    # Usinas sem nação/cidade (ex.: Antártida) ficam fora do agregado por nação,
    # mas continuam no arquivo e no total, declaradas em mw_without_city.
    for plant in plants:
        if not plant.get("nation"):
            scaled_plants.append(scaled_record(
                plant, plant["capacity_mw"], plant["capacity_mw"],
                None, "unscaled", "no_nation_no_target"))

    # Recalcula by_city / by_nation a partir do parque escalonado.
    by_city = {}
    for plant in scaled_plants:
        if not plant.get("city_id"):
            continue
        cid = plant["city_id"]
        entry = by_city.setdefault(cid, {
            "city_id": cid,
            "nation": city_by_id[cid]["tag"],
            "total_mw": 0.0,
            "plants": 0,
            "by_fuel": {},
        })
        entry["total_mw"] += plant["capacity_mw"]
        entry["plants"] += 1
        entry["by_fuel"][plant["fuel_group"]] = round(
            entry["by_fuel"].get(plant["fuel_group"], 0.0) + plant["capacity_mw"], 3)
    for entry in by_city.values():
        entry["total_mw"] = round(entry["total_mw"], 3)

    by_nation = {}
    for entry in by_city.values():
        tag = entry["nation"]
        nation_entry = by_nation.setdefault(tag, {
            "nation": tag, "total_mw": 0.0, "plants": 0, "by_fuel": {},
        })
        nation_entry["total_mw"] += entry["total_mw"]
        nation_entry["plants"] += entry["plants"]
        for fuel, value in entry["by_fuel"].items():
            nation_entry["by_fuel"][fuel] = round(
                nation_entry["by_fuel"].get(fuel, 0.0) + value, 3)
    for entry in by_nation.values():
        entry["total_mw"] = round(entry["total_mw"], 3)

    total_mw = round(sum(p["capacity_mw"] for p in scaled_plants), 3)
    by_city_mw = round(sum(e["total_mw"] for e in by_city.values()), 3)
    by_nation_mw = round(sum(e["total_mw"] for e in by_nation.values()), 3)
    without_city = [p for p in scaled_plants if not p.get("city_id")]
    mw_without_city = round(sum(p["capacity_mw"] for p in without_city), 3)

    failures = [key for key, row in nation_source_check.items()
                if not row["ok"]]
    target_missing = [plant for plant in scaled_plants
                      if plant.get("scaling_note") == "national_target_missing"]
    target_missing_by_nation = {}
    for plant in target_missing:
        tag = plant["nation"]
        row = target_missing_by_nation.setdefault(
            tag, {"nation": tag, "plants": 0, "mw": 0.0, "sources": set()})
        row["plants"] += 1
        row["mw"] = round(row["mw"] + float(plant["capacity_mw"]), 3)
        row["sources"].add(plant["fuel_group"])
    for row in target_missing_by_nation.values():
        row["sources"] = sorted(row["sources"])
    used_derived = sorted(
        key for key, row in nation_source_check.items()
        if row.get("target_source") in ("derived_generation_over_cf",
                                        "max_ember_capacity_derived_generation"))
    meta = {
        "generated_by": "tools/data/energy/build_power_plants_scaled.py",
        "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
        "data_year": 2026,
        "capacity_target_years": [2024, 2025],
        "base_reference_year": 2021,
        "owner_decision": ("D-115(b): GPPD ~2021 como base, escalonado por nação x "
                           "fonte até a matriz Ember 2024-2025; fonte sem usina no "
                           "GPPD distribuída pelas cidades por potencial/população"),
        "method": ("fator declarado alvo/base em mili-MW; resto de arredondamento na "
                   "maior usina do grupo; fonte sem base = usina sintética por cidade "
                   "(maior resto). Alvo Ember por subcategoria (D-115): "
                   "Other fossil->oil e Other renewables->other; quando a capacidade "
                   "Ember é ausente/zero ou fisicamente insuficiente para a geração "
                   "publicada, o alvo é derivado por geração / (CF do modelo * 8760), "
                   "com CF declarado em power_params.json. Nenhum MW fora do alvo "
                   "Ember, da derivação declarada ou do GPPD."),
        "target_source": {
            "dataset": "Ember, Yearly Electricity Data (global release)",
            "license": {"name": "CC BY 4.0", "commercial_ok": True},
            "year_rule": ("último ano com as 9 fontes de capacidade completas por "
                          "país (2024 ou 2025), mesmo sem `Total generation`; "
                          "`year_reason` em national_capacity.json declara a escolha"),
        },
        "base_source": {
            "dataset": "Global Power Plant Database v1.3.0 (WRI)",
            "license": {"name": "CC BY 4.0", "commercial_ok": True},
            "data_year": 2021,
        },
        "weights": {
            "solar": "GSA GHI anual por cidade (renewable_potential.json)",
            "wind": "GWA vento 100 m por cidade (renewable_potential.json)",
            "hydro": "potencial hídrico por cidade (hydro_potential.json)",
            "other": "população da cidade; indústria por cidade não existe como dado",
            "fallback": "sem peso: rateio igual entre as cidades da nação",
        },
        "gaps": {
            "oil": ("correção D-115: 'Other fossil' ancora o grupo oil e "
                    "'Other renewables' ancora other. A capacidade Ember da "
                    "subcategoria vem de `other_breakdown`; quando é "
                    "ausente/zero ou insuficiente para a geração publicada, o "
                    "alvo é derivado por geração / (cf_oil * 8760). `oil` "
                    "continua null em national_capacity.json (a Ember não "
                    "publica 'Oil' separado)."),
            "nations_without_ember": sorted(
                (set(base_nations) | set(demand_nations)) - set(nations)),
            "national_target_missing": {
                "rule": ("usina real cuja nação×fonte não tem alvo Ember (nação sem "
                         "matriz 2024-2025); o MW do GPPD fica NÃO escalonado e cada "
                         "usina leva `scaling_note: national_target_missing`"),
                "plants": len(target_missing),
                "mw": round(sum(float(p["capacity_mw"]) for p in target_missing), 3),
                "by_nation": dict(sorted(target_missing_by_nation.items())),
            },
            "nation_source_failures": failures,
        },
        "derived_targets": dict(sorted(derived_targets.items())),
        "derived_targets_used": used_derived,
        "scale_factors": dict(sorted(scale_factors.items())),
        "nation_source_check": dict(sorted(nation_source_check.items())),
        "counts": {
            "plants": len(scaled_plants),
            "real_plants": len(scaled_plants) - synthetic_count,
            "synthetic_plants": synthetic_count,
            "cities_with_plants": len(by_city),
            "nations_with_plants": len(by_nation),
            "total_mw": total_mw,
            "mw_without_city": mw_without_city,
            "plants_without_city": len(without_city),
            "by_city_mw": by_city_mw,
            "by_nation_mw": by_nation_mw,
            "nation_source_checks": len(nation_source_check),
            "nation_source_failures": len(failures),
            "derived_targets": len(derived_targets),
            "derived_targets_used": len(used_derived),
            "derived_target_mw": round(sum(
                float(row["capacity_mw"])
                for row in derived_targets.values()), 3),
            "national_target_missing_plants": len(target_missing),
            "national_target_missing_mw": round(
                sum(float(p["capacity_mw"]) for p in target_missing), 3),
        },
        "invariant": {
            "rule": "sum(by_city[].total_mw) == sum(by_nation[].total_mw) (<= 0.01 MW)",
            "by_city_mw": by_city_mw,
            "by_nation_mw": by_nation_mw,
            "difference_mw": round(by_city_mw - by_nation_mw, 6),
            "ok": abs(by_city_mw - by_nation_mw) <= TOLERANCE_MW,
            "total_mw": total_mw,
            "mw_without_city": mw_without_city,
            "total_matches": abs(total_mw - by_nation_mw - mw_without_city) <= 0.05,
            "total_note": ("o total das usinas e a soma dos agregados podem diferir "
                           "em até 0,05 MW pelo arredondamento de 3 casas por cidade; "
                           "a invariante QA é entre by_city e by_nation"),
        },
    }

    payload = {"meta": meta, "by_nation": by_nation, "by_city": by_city,
               "plants": scaled_plants}
    text = ['{', ' "meta": %s,' % _compact(meta),
            ' "by_nation": %s,' % _compact(by_nation),
            ' "by_city": %s,' % _compact(by_city),
            ' "plants": [']
    lines = ["  " + _compact(plant) for plant in scaled_plants]
    text.append(",\n".join(lines))
    text.append(' ]')
    text.append('}')
    ec.write_text(OUT_PATH, "\n".join(text) + "\n")

    print("usinas: %d (reais %d + sintéticas %d) | cidades %d | nações %d"
          % (len(scaled_plants), len(scaled_plants) - synthetic_count,
             synthetic_count, len(by_city), len(by_nation)))
    print("total MW: %.1f | sem cidade: %.1f MW" % (total_mw, mw_without_city))
    print("invariante: by_city %.3f == by_nation %.3f (dif %.6f)"
          % (by_city_mw, by_nation_mw, by_city_mw - by_nation_mw))
    print("checks nação x fonte: %d | falhas: %d"
          % (len(nation_source_check), len(failures)))
    print("sem alvo nacional (national_target_missing): %d usinas / %.1f MW"
          % (len(target_missing),
             sum(float(p["capacity_mw"]) for p in target_missing)))
    if failures:
        print("falhas:", " ".join(failures[:20]))
    return 0


def _compact(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


if __name__ == "__main__":
    sys.exit(main())
