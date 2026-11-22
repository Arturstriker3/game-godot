"""Parque inicial ESCALONADO para a matriz Ember 2024-2025 (D-115b).

Entrada:
  * `power_plants_base_2021_gppd.json` — usinas GPPD ~2021 já atribuídas à
    cidade/nação do atlas (regra D-115a);
  * `national_capacity.json` — alvo por nação x fonte (Ember, último ano não
    vazio entre 2024 e 2025);
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
RENEWABLE_PATH = os.path.join(ENERGY, "renewable_potential.json")
HYDRO_PATH = os.path.join(ENERGY, "hydro_potential.json")
OUT_PATH = os.path.join(ENERGY, "power_plants_2026_scaled.json")

GROUPS = ["hydro", "solar", "wind", "coal", "gas", "oil", "nuclear", "biomass",
          "other"]
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


def main() -> int:
    base = load(BASE_PATH)
    capacity = load(CAPACITY_PATH)
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
    for tag in sorted(set(base_nations) | set(nations)):
        targets = (nations.get(tag) or {}).get("by_source") or {}
        base_by_fuel = (base_nations.get(tag) or {}).get("by_fuel") or {}
        weights_by_source = city_weights(tag, cities, renewable, hydro)
        for source in GROUPS:
            target = targets.get(source)
            group = groups.get(tag, {}).get(source, [])
            base_mw = round(sum(float(p["capacity_mw"]) for p in group), 3)
            key = "%s/%s" % (tag, source)
            if target is None:
                # Óleo (Ember não separa) ou nação sem matriz: não escala.
                note = ("ember_oil_not_separated" if source == "oil"
                        else "national_target_missing")
                for plant in group:
                    scaled_plants.append(scaled_record(
                        plant, plant["capacity_mw"], plant["capacity_mw"],
                        None, "unscaled", note))
                scale_factors[key] = {"base_mw": base_mw, "target_mw": None,
                                      "factor": None, "method": "unscaled",
                                      "note": note}
                nation_source_check[key] = {"target_mw": None,
                                            "scaled_mw": base_mw,
                                            "delta_mw": 0.0,
                                            "ok": True,
                                            "note": note}
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
                nation_source_check[key] = {"target_mw": target,
                                            "scaled_mw": 0.0,
                                            "delta_mw": round(-target, 3),
                                            "ok": abs(0.0 - target) <= TOLERANCE_MW,
                                            "method": "retired_by_ember_target_zero"}
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
                    scaled_plants.append(scaled_record(
                        plant, value / 1000.0, plant["capacity_mw"], factor,
                        "scaled_to_ember",
                        "scaled_to_zero" if value <= 0 else None))
                scale_factors[key] = {"base_mw": base_mw, "target_mw": target,
                                      "factor": round(factor, 9),
                                      "method": "scaled_to_ember",
                                      "note": None}
                scaled_mw = round(sum(value / 1000.0 for value in scaled_milli), 3)
                nation_source_check[key] = {
                    "target_mw": target, "scaled_mw": scaled_mw,
                    "delta_mw": round(scaled_mw - target, 3),
                    "ok": abs(scaled_mw - target) <= TOLERANCE_MW,
                    "method": "scaled_to_ember",
                }
                continue

            # Base zero e alvo > 0: usina sintética por cidade (peso declarado).
            if target_milli := milli(target):
                allocation = allocate(target_milli, weights_by_source[source])
                if not allocation:
                    scale_factors[key] = {"base_mw": 0.0, "target_mw": target,
                                          "factor": None,
                                          "method": "no_city_for_allocation",
                                          "note": "nação sem cidade para ratear"}
                    nation_source_check[key] = {
                        "target_mw": target, "scaled_mw": 0.0,
                        "delta_mw": round(-target, 3), "ok": False,
                        "method": "no_city_for_allocation",
                    }
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
                                      "weight_basis": sorted(set(
                                          row["basis"]
                                          for row in weights_by_source[source].values()))}
                scaled_mw = round(sum(value / 1000.0
                                      for value in allocation.values()), 3)
                nation_source_check[key] = {
                    "target_mw": target, "scaled_mw": scaled_mw,
                    "delta_mw": round(scaled_mw - target, 3),
                    "ok": abs(scaled_mw - target) <= TOLERANCE_MW,
                    "method": "ember_city_allocation",
                }
            else:
                scale_factors[key] = {"base_mw": 0.0, "target_mw": target,
                                      "factor": None, "method": "no_base_no_target",
                                      "note": "alvo arredonda a 0"}
                nation_source_check[key] = {"target_mw": target, "scaled_mw": 0.0,
                                            "delta_mw": round(-target, 3),
                                            "ok": True, "method": "rounds_to_zero"}

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
                   "(maior resto). Nenhum MW fora do alvo Ember ou do GPPD."),
        "target_source": {
            "dataset": "Ember, Yearly Electricity Data (global release)",
            "license": {"name": "CC BY 4.0", "commercial_ok": True},
            "year_rule": "último ano não vazio entre 2024 e 2025 por país",
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
            "oil": ("a Ember não separa óleo (dentro de Other fossil); o parque de "
                    "óleo do GPPD fica NÃO escalonado, com `oil` null em "
                    "national_capacity.json"),
            "nations_without_ember": sorted(set(base_nations) - set(nations)),
            "nation_source_failures": failures,
        },
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
    if failures:
        print("falhas:", " ".join(failures[:20]))
    return 0


def _compact(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


if __name__ == "__main__":
    sys.exit(main())
