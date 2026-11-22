"""Normaliza o proxy hidrico por provincia pela ancora nacional publica.

Entrada:
  * `data/definitions/energy/renewable_potential.json` — proxy
    `raw = precip x area x desnivel x runoff x rio` por provincia (E1 anterior),
    com `national_anchor_missing: true` (so havia ancora GLOBAL);
  * World Bank Data360 `WB_CLEAR_HP_TECHNFEASIBLE_PT` — fracao do potencial
    tecnico hidreletrico ja desenvolvida em 2021 (CC BY 4.0);
  * Ember, `Hydro` de 2021 (CC BY 4.0) — capacidade instalada do denominador.

O potencial tecnico nacional em MW e derivado:

    potencial_mw = hidro_instalada_2021_mw / (developed_pct / 100)

e a divisao interna do pais continua sendo o proxy (precipitacao x desnivel x
rio). A soma das provincias de cada pais e IGUAL ao potencial nacional (o
ultimo resto de arredondamento vai para a provincia de maior raw), entao o teto
de MW do E2 fecha por construcao com a ancora publica.

Paises sem ancora (ou sem hidro em 2021) ficam `technical_potential_mw: null`,
`national_anchor_missing: true` — nenhum numero nacional e inventado.

Saida: `data/definitions/energy/hydro_potential.json`.

Uso:
    python tools/data/energy/build_hydro_national.py
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
WB_PATH = os.path.join(CACHE, "wb_hydro_developed_pct.json")
WB_MANIFEST = os.path.join(CACHE, "wb_hydro_developed_pct.manifest.json")
RENEWABLES_PATH = os.path.join(ec.DEFS, "energy", "renewable_potential.json")
OUT_PATH = os.path.join(ec.DEFS, "energy", "hydro_potential.json")

BASE_YEAR = 2021  # ano do indicador do Banco Mundial (Atlas Hidreletrico 2021)


def ember_hydro_2021() -> dict:
    """{iso3: hydro_mw} em 2021 (denominador do percentual desenvolvido)."""
    out = {}
    with open(EMBER_CSV, encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("Electricity source") != "Hydro":
                continue
            if row.get("Year") != str(BASE_YEAR) or row.get("Area type") == "Region":
                continue
            iso3 = (row.get("ISO 3 code") or "").strip()
            raw = (row.get("Capacity (GW)") or "").strip()
            if not iso3 or raw == "":
                continue
            out[iso3] = float(raw) * 1000.0
    return out


def main() -> int:
    for path in (EMBER_CSV, WB_PATH, RENEWABLES_PATH):
        if not os.path.exists(path):
            raise SystemExit("entrada ausente: %s" % path)
    with open(WB_PATH, encoding="utf-8") as handle:
        wb = json.load(handle)
    with open(WB_MANIFEST, encoding="utf-8") as handle:
        wb_manifest = json.load(handle)
    with open(RENEWABLES_PATH, encoding="utf-8") as handle:
        renewables = json.load(handle)
    hydro_2021 = ember_hydro_2021()
    tags, _ = ec.load_registry()

    # proxy por provincia -> soma bruta por nacao
    provinces = []
    raw_by_nation: dict[str, float] = {}
    for entry in renewables["by_province"]:
        hydro = entry.get("hydro") or {}
        raw = float(hydro.get("raw_index") or 0.0)
        nation = str(entry.get("nation") or "")
        if raw > 0.0:
            raw_by_nation[nation] = raw_by_nation.get(nation, 0.0) + raw
        provinces.append((int(entry["province_id"]), nation, raw,
                          float(hydro.get("twh_per_year") or 0.0),
                          str(hydro.get("confidence") or "")))

    nation_out = {}
    for tag in tags:
        iso_candidates = [ec.nation_code(tag), tag]
        iso3 = next((code for code in iso_candidates
                     if code in wb["countries"]), None)
        developed_pct = None
        if iso3 is not None:
            developed_pct = float(wb["countries"][iso3]["developed_pct"])
        installed = next((hydro_2021[code] for code in iso_candidates
                          if code in hydro_2021), None)
        potential = None
        if developed_pct and developed_pct > 0 and installed and installed > 0:
            potential = installed / (developed_pct / 100.0)
        nation_out[tag] = {
            "nation": tag,
            "country_iso3": iso3,
            "year": BASE_YEAR if iso3 is not None else None,
            "developed_pct": developed_pct,
            "installed_hydro_mw": round(installed, 3) if installed is not None else None,
            "technical_potential_mw": round(potential, 3) if potential is not None else None,
            "proxy_raw_total": round(raw_by_nation.get(tag, 0.0), 6),
            "national_anchor_missing": potential is None,
            "confidence": "low" if potential is not None else "no_data",
            "method": "derived_installed_over_developed_pct",
            "distribution_missing": bool(potential is not None and raw_by_nation.get(tag, 0.0) <= 0.0),
        }

    # normalizacao por nacao: soma das provincias == potencial nacional
    province_out = []
    by_nation_accum: dict[str, float] = {}
    for pid, nation, raw, twh, confidence in provinces:
        anchor = nation_out.get(nation, {}).get("technical_potential_mw")
        total_raw = raw_by_nation.get(nation, 0.0)
        if confidence == "no_data":
            # Sem precipitacao/area/relevo o proxy nao existe: nao vira zero.
            province_out.append({
                "province_id": pid, "nation": nation,
                "raw_index": None, "share_of_nation": None,
                "potential_mw": None, "national_anchor_missing": anchor is None,
                "no_hydro_data": True,
            })
            continue
        if anchor is None or total_raw <= 0.0:
            # Sem ancora nacional OU ancora sem proxy para dividir: nada de inventar
            # distribuicao. As provincias ficam nulas e o motivo fica no agregado.
            province_out.append({
                "province_id": pid, "nation": nation,
                "raw_index": round(raw, 6),
                "share_of_nation": (round(raw / total_raw, 9) if total_raw > 0 else None),
                "potential_mw": None,
                "national_anchor_missing": anchor is None,
                "distribution_missing": anchor is not None,
            })
            continue
        share = raw / total_raw
        value = anchor * share
        by_nation_accum[nation] = by_nation_accum.get(nation, 0.0) + value
        province_out.append({
            "province_id": pid, "nation": nation,
            "raw_index": round(raw, 6),
            "share_of_nation": round(share, 9),
            "potential_mw": round(value, 6),
            "national_anchor_missing": False,
        })

    # resto de arredondamento: a provincia de maior raw de cada nacao fecha a soma
    for nation, anchor_entry in nation_out.items():
        anchor = anchor_entry["technical_potential_mw"]
        if anchor is None:
            continue
        acc = by_nation_accum.get(nation, 0.0)
        residual = round(anchor - acc, 6)
        if abs(residual) < 1e-9:
            continue
        candidates = [p for p in province_out
                      if p["nation"] == nation and p["potential_mw"] is not None]
        if not candidates:
            continue
        biggest = max(candidates, key=lambda p: p["raw_index"])
        biggest["potential_mw"] = round(biggest["potential_mw"] + residual, 6)

    # agregado por cidade: soma ponderada pela fracao da provincia na cidade
    city_provinces = ec.city_provinces()
    province_mw = {p["province_id"]: (p["potential_mw"], p["nation"])
                   for p in province_out}
    city_out = {}
    for city_id, pairs in city_provinces.items():
        total = 0.0
        has_value = False
        nation = ""
        for pid, share in pairs:
            value, province_nation = province_mw.get(pid, (None, ""))
            if nation == "":
                nation = province_nation
            if value is None:
                continue
            total += value * share
            has_value = True
        city_out[city_id] = {
            "city_id": city_id,
            "nation": nation,
            "potential_mw": round(total, 6) if has_value else None,
            "national_anchor_missing": not has_value,
        }

    missing = sorted(tag for tag, entry in nation_out.items()
                     if entry["national_anchor_missing"])
    no_proxy = sorted(tag for tag, entry in nation_out.items()
                      if entry["proxy_raw_total"] <= 0.0)
    provided = len(nation_out) - len(missing)
    payload = {
        "meta": {
            "generated_by": "tools/data/energy/build_hydro_national.py",
            "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
            "method": ("potencial tecnico nacional em MW derivado de "
                       "instalada_2021_mw / (developed_pct/100); divisao interna por "
                       "provincia = proxy precip x desnivel x rio normalizado a soma 1 "
                       "dentro da nacao"),
            "sources": {
                "national_anchor": {
                    "dataset": "World Bank Data360 WB_CLEAR_HP_TECHNFEASIBLE_PT",
                    "indicator": "Technically feasible hydropower potential developed (%)",
                    "year": BASE_YEAR,
                    "url": wb["meta"].get("url"),
                    "metadata_url": wb["meta"].get("metadata_url"),
                    "license": wb["meta"].get("license"),
                    "citation": wb["meta"].get("citation"),
                    "sha256_cache": wb_manifest.get("sha256"),
                    "accessed_at_utc": wb_manifest.get("accessed_at_utc"),
                },
                "installed_denominator": {
                    "dataset": "Ember, Yearly Electricity Data (Hydro, 2021)",
                    "license": {"name": "CC BY 4.0", "commercial_ok": True},
                },
                "province_proxy": {
                    "dataset": "renewable_potential.json (proxy_hydro_free)",
                    "method": "proxy_precip_relief_river_free",
                    "note": ("precipitação CHIRPS v3.0 anual 2025 (domínio público/"
                             "CC BY 4.0), PET anual CGIAR-CSI v3.1 (CC BY 4.0) e "
                             "desnível ETOPO1 (domínio público); NÃO usa WorldClim"),
                },
            },
            "derivation_note": ("o denominador (Ember) nao e a mesma publicacao usada "
                                "pelo Banco Mundial (Hydropower & Dams World Atlas 2021); "
                                "o potencial carrega esse desvio e por isso a confianca "
                                "e 'low', nunca 'high'"),
            "counts": {
                "nations": len(nation_out),
                "nations_with_anchor": provided,
                "nations_without_anchor": len(missing),
                "nations_without_proxy": len(no_proxy),
                "nations_anchor_without_distribution": sorted(
                    tag for tag, entry in nation_out.items()
                    if entry.get("distribution_missing")),
                "provinces": len(province_out),
                "provinces_without_anchor": sum(
                    1 for p in province_out if p["national_anchor_missing"]),
                "provinces_no_hydro_data": sum(
                    1 for p in province_out if p.get("no_hydro_data")),
                "cities": len(city_out),
                "cities_without_anchor": sum(
                    1 for c in city_out.values() if c["national_anchor_missing"]),
                "world_technical_potential_mw": round(
                    sum(e["technical_potential_mw"] for e in nation_out.values()
                        if e["technical_potential_mw"] is not None), 3),
            },
            "gaps": {"nations_without_anchor": missing,
                     "nations_without_proxy": no_proxy},
        },
        "by_nation": dict(sorted(nation_out.items())),
        "by_province": province_out,
        "by_city": dict(sorted(city_out.items())),
    }
    ec.write_json(OUT_PATH, payload)
    print("nações com âncora nacional: %d/%d (sem âncora: %d)"
          % (provided, len(nation_out), len(missing)))
    print("potencial técnico mundial declarado: %.0f GW"
          % (payload["meta"]["counts"]["world_technical_potential_mw"] / 1000.0))
    top = sorted((e for e in nation_out.values()
                  if e["technical_potential_mw"] is not None),
                 key=lambda e: e["technical_potential_mw"], reverse=True)[:8]
    for entry in top:
        print("  %-4s %9.0f MW  (%.1f%% desenvolvido, instalado %.0f MW)"
              % (entry["nation"], entry["technical_potential_mw"],
                 entry["developed_pct"], entry["installed_hydro_mw"]))
    if missing:
        print("sem âncora: %s" % " ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
