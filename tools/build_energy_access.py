"""Acesso à eletricidade, perdas de rede e água potável por nação (E5/E7).

Fonte: World Bank, World Development Indicators (WDI), consultados pela API
Data360 (cache em tools/data/wdi_energy.json, gerado por
tools/fetch_wdi_energy.py). Os indicadores de água vêm do JMP WHO/UNICEF
publicado pelo WDI (código SH.H2O.SMDW.*).

Regra do dado: ano MAIS RECENTE não vazio por país/indicador; ausência fica
ausente (sem interpolação, sem extrapolação). Cada registro carrega o ano, o
código do indicador e a confiança.

Uso: python tools/build_energy_access.py
Saída: data/definitions/energy/access_losses.json
"""

from __future__ import annotations

import os
import sys

import build_energy_common as ec

ROOT = ec.ROOT
WDI = os.path.join(ROOT, "tools", "data", "wdi_energy.json")
WDI_MANIFEST = os.path.join(ROOT, "tools", "data", "wdi_energy_manifest.json")
OUT = os.path.join(ROOT, "data", "definitions", "energy", "access_losses.json")

FIELDS = [
    ("electricity_access_pct", "EG.ELC.ACCS.ZS",
     "acesso à eletricidade (% da população)", "high"),
    ("electricity_access_urban_pct", "EG.ELC.ACCS.UR.ZS",
     "acesso à eletricidade urbano (% da população urbana)", "high"),
    ("electricity_access_rural_pct", "EG.ELC.ACCS.RU.ZS",
     "acesso à eletricidade rural (% da população rural)", "high"),
    ("tnd_losses_pct", "EG.ELC.LOSS.ZS",
     "perdas de transmissão e distribuição (% da produção elétrica)", "high"),
    ("water_safely_managed_pct", "SH.H2O.SMDW.ZS",
     "água potável gerida com segurança (% da população; base JMP)", "high"),
    ("water_safely_managed_urban_pct", "SH.H2O.SMDW.UR.ZS",
     "água potável gerida com segurança urbana (% da população urbana)", "medium"),
    ("water_safely_managed_rural_pct", "SH.H2O.SMDW.RU.ZS",
     "água potável gerida com segurança rural (% da população rural)", "medium"),
]


def main():
    if not os.path.exists(WDI):
        raise SystemExit("cache ausente: %s (rode tools/fetch_wdi_energy.py)" % WDI)
    data = ec.read_json(WDI)
    manifest = ec.read_json(WDI_MANIFEST) if os.path.exists(WDI_MANIFEST) else {}
    tags, _ = ec.load_registry()
    tag_by_code = {ec.nation_code(tag): tag for tag in tags}
    rows = data.get("indicators", {})

    nations = {}
    for iso3, values in sorted(rows.items()):
        tag = tag_by_code.get(iso3)
        entry = {
            "country_iso3": iso3,
            "nation": tag or None,
            "playable": bool(tag),
            "source": "World Bank WDI (Data360 API)",
            "method": "latest_non_empty_year_per_indicator",
        }
        found = 0
        for key, code, label, confidence in FIELDS:
            item = values.get(key)
            if not item:
                entry[key] = None
                continue
            value = float(item["value"])
            entry[key] = {
                "value": round(value, 3),
                "year": int(item["year"]),
                "indicator": code,
                "unit": "%",
                "description": label,
                "source": "World Bank WDI / " + ("JMP WHO-UNICEF" if key.startswith("water") else "WDI"),
                "method": "latest_non_empty_year",
                "confidence": confidence,
            }
            found += 1
        entry["indicators_found"] = found
        nations[iso3] = entry

    coverage = {key: sum(1 for e in nations.values() if e.get(key)) for key, _, _, _ in FIELDS}
    playable = {e["nation"]: iso3 for iso3, e in nations.items() if e["nation"]}
    playable_missing = sorted(set(tags) - set(playable))

    payload = {
        "meta": {
            "generated_by": "tools/build_energy_access.py",
            "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E5 e E7",
            "source": "World Bank, World Development Indicators (WDI), via Data360 API",
            "underlying_sources": {
                "electricity_access": "World Bank / SE4ALL Global Tracking Framework (WDI EG.ELC.ACCS.*)",
                "tnd_losses": "World Bank / IEA (WDI EG.ELC.LOSS.ZS)",
                "water_safely_managed": "WHO/UNICEF JMP, published through WDI (SH.H2O.SMDW.*)",
            },
            "license": manifest.get("license", "CC BY 4.0 (World Bank WDI)"),
            "citation": manifest.get("citation", "World Bank WDI via Data360 API"),
            "fetched_at_utc": manifest.get("fetched_at_utc"),
            "method": ("ano mais recente não vazio por país e indicador; nenhuma "
                       "interpolação/extrapolação; ausente = null"),
            "fields": [{"key": k, "indicator": c, "description": d, "confidence": conf}
                       for k, c, d, conf in FIELDS],
            "usage": {
                "E5": "tnd_losses_pct = perda inicial da rede nacional; "
                      "electricity_access_pct(urbano/rural) = cobertura inicial da conexão",
                "E7": "water_safely_managed_pct(urbano/rural) = cobertura inicial da distribuidora de água",
            },
            "counts": {"countries": len(nations), "playable": len(playable),
                       "playable_without_data": len(playable_missing),
                       "coverage": coverage},
            "playable_without_data": playable_missing,
        },
        "by_country": nations,
    }
    ec.write_json(OUT, payload)
    print("países: %d | jogáveis com dado: %d | jogáveis sem registro WDI: %d"
          % (len(nations), len(playable), len(playable_missing)))
    for key, count in coverage.items():
        print("  %-32s %3d países" % (key, count))
    print("jogáveis sem registro:", ", ".join(playable_missing) or "nenhum")
    return 0


if __name__ == "__main__":
    sys.exit(main())
