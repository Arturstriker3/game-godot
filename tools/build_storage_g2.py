#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Constroi os dados da Parte G2 (armazenagem e reservas nacionais).

Entradas:
  data/definitions/scenario/nations.json        as 179 nacoes do jogo
  data/definitions/agriculture/crop_yields.json producao de graos (regra de capacidade)
  data/definitions/world/nations_source.csv     ISO2/ISO3/populacao (para mapear fontes)
  data/definitions/economy/goods.json           os 113 bens do jogo
  tools/storage_g2_reference.json               valores publicados (curados, com citacao)
  tools/_tmp_storage_raw/                       cache bruto (IMF, Eurostat, EIA)

Saidas (UTF-8 sem BOM, LF, indent=2, chaves ordenadas):
  data/definitions/economy/storage_capacity.json     graos (Mt), frio (m3), gas (TWh)
  data/definitions/economy/strategic_reserves.json   petroleo publico, graos publicos, ouro
  data/definitions/economy/goods_storage_map.json    113 bens -> tipo de armazem

Uso:
    py -3 tools/fetch_storage_g2.py --fetch
    py -3 tools/build_storage_g2.py
"""

from __future__ import annotations

import csv
import html
import io
import json
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "tools", "_tmp_storage_raw")
NATIONS = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
YIELDS = os.path.join(ROOT, "data", "definitions", "agriculture", "crop_yields.json")
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "world", "nations_source.csv")
GOODS = os.path.join(ROOT, "data", "definitions", "economy", "goods.json")
REFERENCE = os.path.join(ROOT, "tools", "storage_g2_reference.json")
OUT_DIR = os.path.join(ROOT, "data", "definitions", "economy")

TROY_OZ_T = 31.1034768 / 1_000_000.0  # t por onca troy fina (31,1034768 g)
BCM_CF = 35.3146667             # bilhoes de pes cubicos por bcm
BCM_TWH = 11.0                  # TWh por bcm (PCI ~ 38,6 MJ/m3), convencao declarada
T_PER_BBL = 0.1364              # t por barril de petroleo cru (7,33 bbl/t), conversao declarada

GRAIN_CROPS = ("wheat", "rice", "maize", "soy")
COLD_UNIT_M3 = ("m3",)

# fontes coletadas por script (o cache bruto fica em tools/_tmp_storage_raw/)
BUILT_SOURCES = {
    "imf_ifs_gold": {
        "title": "International Financial Statistics (IFS) — Official Reserve Assets, Gold, volume in millions of fine troy ounces (RAFAGOLDV_OZT)",
        "publisher": "International Monetary Fund (IMF)",
        "url": "https://data.imf.org/en/Data-Explorer?datasetUrn=IMF.STA%3AIFS%283.0.0%29",
        "year": 2026,
        "license": "© IMF, uso com citação (consulta via api.db.nomics.world, provider IMF/IFS)",
        "query": "INDICATOR=RAFAGOLDV_OZT, FREQ=M, ultima observacao por area",
        "quote": "Monthly – United States – International Reserves, Official Reserve Assets, Gold (Including Gold Deposits and, If Appropriate, Gold Swapped), Volume in Millions of Fine Troy Ounces — 261,49892624 (2025-06); Alemanha 107,714; Italia 78,83; Franca 78,352 (milhoes de oncas troy finas)",
    },
    "eurostat_stk_oem": {
        "title": "Emergency oil stocks in days equivalent — monthly data (nrg_stk_oem), series STK_EUE_DIR, IMP_DNC e IC_DC",
        "publisher": "Eurostat",
        "url": "https://ec.europa.eu/eurostat/databrowser/view/nrg_stk_oem/default/table",
        "year": 2026,
        "license": "© European Union, reutilização com atribuição (CC BY 4.0)",
        "query": "ultimo mes publicado por pais; dias x max(importacao liquida diaria, consumo interno diario)",
        "quote": "Monthly – Emergency Stocks held by the MS in accordance with the EU Directive (in Days Equivalent) – Number – Germany: 95,58 (2025-10); Monthly – Daily net imports for compliance – Thousand tonnes – Germany: 206,987 (2025-10)",
    },
    "eia_spr": {
        "title": "Weekly U.S. Ending Stocks of Crude Oil in SPR (WCSSTUS1), thousand barrels",
        "publisher": "U.S. Energy Information Administration (EIA)",
        "url": "https://www.eia.gov/dnav/pet/hist/LeafHandler.ashx?n=PET&s=WCSSTUS1&f=W",
        "year": 2026,
        "license": "domínio público (EIA)",
        "query": "ultima semana publicada",
        "quote": "Weekly U.S. Ending Stocks of Crude Oil in SPR (Thousand Barrels) — 2026-Nov, semana 11/13: 276.361 (Release Date: 11/18/2026)",
    },
    "eia_us_gas_capacity": {
        "title": "Underground Natural Gas Storage Capacity — U.S., annual (working gas capacity)",
        "publisher": "U.S. Energy Information Administration (EIA)",
        "url": "https://www.eia.gov/dnav/ng/ng_stor_cap_dcu_nus_a.htm",
        "year": 2026,
        "license": "domínio público (EIA)",
        "query": "Total Working Gas Capacity, coluna mais recente (MMcf -> TWh a 11,0 TWh/bcm)",
        "quote": "Total Working Gas Capacity — 4.793.009 (milhoes de pes cubicos), coluna corrente; Total Storage Capacity 9.319.291 (Release Date: 10/30/2026)",
    },
    "faostat_qcl_production": {
        "title": "FAOSTAT — Crops and livestock products (QCL), produção de trigo, arroz, milho e soja (elemento 5510)",
        "publisher": "FAO — Food and Agriculture Organization of the United Nations",
        "url": "https://www.fao.org/faostat/en/#data/QCL",
        "year": 2024,
        "license": "CC BY 4.0",
        "query": "reaproveitado de data/definitions/agriculture/crop_yields.json (base da regra de grãos)",
    },
    "wdi_population": {
        "title": "World Bank WDI SP.POP.TOTL — população, último ano disponível",
        "publisher": "World Bank",
        "url": "https://data.worldbank.org/indicator/SP.POP.TOTL",
        "year": 2025,
        "license": "CC BY 4.0",
        "query": "reaproveitado de data/definitions/world/nations_source.csv (base da regra de frio)",
    },
}


def read_json(path: str):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def utc_today() -> str:
    import datetime
    return datetime.date.today().isoformat()


def write_json(path: str, payload: dict) -> None:
    text = json.dumps(payload, indent=2, ensure_ascii=False, sort_keys=True) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def load_nations() -> list[dict]:
    return read_json(NATIONS)


def load_iso_map(nations: list[dict]) -> dict[str, dict]:
    """ISO3 do jogo -> {iso2, population, name_en}.

    O CSV de mundo usa ISO3 padrao; o jogo usa SOL (Ilhas Salomao) e SSD
    (Sudao do Sul), que nao existem la. O alias e explicito e a populacao cai
    para a de data/definitions/scenario/nations.json.
    """
    aliases = {"SOL": ("SB", "SLB"), "SSD": ("SS", "SSD")}
    out = {}
    with io.open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            out[row["iso3"]] = {
                "iso2": row["iso2"],
                "population": int(float(row["population"])) if row["population"] else None,
                "name_en": row["name_en"],
            }
    for nation in nations:
        iso3 = nation["id"]
        if iso3 in out:
            continue
        iso2, ref = aliases.get(iso3, (None, None))
        if not iso2:
            raise SystemExit("sem ISO2 para %s" % iso3)
        base = out.get(ref, {})
        out[iso3] = {
            "iso2": iso2,
            "population": base.get("population") or nation.get("population"),
            "name_en": base.get("name_en") or nation["name"],
        }
    return out


# ---------------------------------------------------------------- cache

def _cache_json(name: str) -> dict:
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        raise SystemExit("cache ausente: %s (rode tools/fetch_storage_g2.py --fetch)" % path)
    return read_json(path)


def _latest(period: list, value: list):
    for idx in range(len(period) - 1, -1, -1):
        val = value[idx]
        if isinstance(val, (int, float)):
            return period[idx], float(val)
    return None, None


def gold_tonnes(iso2_map: dict[str, str], nations: list[dict]) -> dict[str, dict]:
    payload = _cache_json("imf_ifs_gold.json")
    by_iso2 = {}
    for series in payload["series"]:
        area = series["ref_area"]
        if not re.fullmatch(r"[A-Z]{2}", area):
            continue
        period, value = _latest(series["period"], series["value"])
        if value is None:
            continue
        prev = by_iso2.get(area)
        if prev is None or period > prev[0]:
            by_iso2[area] = (period, value)
    out = {}
    for nation in nations:
        iso2 = iso2_map[nation["id"]]["iso2"]
        if iso2 in by_iso2:
            period, millions_oz = by_iso2[iso2]
            out[nation["id"]] = {
                "period": period,
                "millions_fine_oz": round(millions_oz, 6),
                "tonnes": round(millions_oz * 1_000_000 * TROY_OZ_T, 4),
            }
    return out


def _value_at(item, period: str):
    """Valor da serie no periodo pedido; se nao houver, o mais recente antes dele."""
    best = None
    for idx, per in enumerate(item["period"]):
        if per <= period and isinstance(item["value"][idx], (int, float)):
            best = (per, float(item["value"][idx]))
    return best[1] if best else None


def eurostat_oil(nations: list[dict], iso2_map: dict[str, dict]) -> dict[str, dict]:
    payload = _cache_json("eurostat_oil_stocks.json")
    series = {}
    for item in payload["series"]:
        series[(item["geo"], item["stk_flow"])] = item
    out = {}
    for nation in nations:
        geo = iso2_map[nation["id"]]["iso2"]
        days_item = series.get((geo, "STK_EUE_DIR"))
        if not days_item:
            continue
        period, days = _latest(days_item["period"], days_item["value"])
        if days is None:
            continue
        imports = consumption = None
        if (geo, "IMP_DNC") in series:
            imports = _value_at(series[(geo, "IMP_DNC")], period)
        if (geo, "IC_DC") in series:
            consumption = _value_at(series[(geo, "IC_DC")], period)
        # a diretiva 2009/119/CE mede os dias sobre o MAIOR entre importacao liquida
        # diaria e consumo interno diario; usamos os dois denominadores publicados
        candidates = [v for v in (imports, consumption) if v]
        base = max(candidates) if candidates else None
        entry = {
            "days_net_imports": round(days, 3),
            "period": period,
            "daily_net_imports_kt": round(imports, 6) if imports is not None else None,
            "daily_inland_consumption_kt": round(consumption, 6) if consumption is not None else None,
            "public_stocks_kt": round(days * base, 3) if base else None,
            "stale": period < "2024-01",
        }
        out[nation["id"]] = entry
    return out


def eia_us_spr() -> dict:
    path = os.path.join(CACHE, "eia_spr_weekly.html")
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    rows = text.split("<tr")[1:]
    latest_date, latest_value = None, None
    released = None
    rel = re.search(r"Release Date:\s*([0-9/]+)", text)
    if rel:
        released = rel.group(1)
    for row in rows:
        cells = [
            html.unescape(re.sub(r"<[^>]+>", "", cell)).replace("\xa0", " ").strip()
            for cell in re.findall(r"<td[^>]*>(.*?)</td>", row, re.S)
        ]
        if len(cells) < 3 or not re.fullmatch(r"\d{4}-[A-Z][a-z]{2}", cells[0]):
            continue
        for idx in range(1, len(cells) - 1, 2):
            date, value = cells[idx], cells[idx + 1]
            if re.fullmatch(r"\d{2}/\d{2}", date) and value.replace(",", "").isdigit():
                latest_date, latest_value = date, int(value.replace(",", ""))
    if latest_value is None:
        raise SystemExit("nao consegui ler o SPR no HTML da EIA")
    return {
        "date": latest_date,
        "release_date": released,
        "thousand_barrels": latest_value,
        "kt": round(latest_value * T_PER_BBL, 1),
    }


def eia_us_gas() -> dict:
    path = os.path.join(CACHE, "eia_us_gas_capacity.html")
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    chunks = text.split('<tr class="DataRow">')[1:]
    found = {}
    for chunk in chunks:
        chunk = chunk.split('<tr class="DataRow">')[0]
        label = re.search(r'class="DataStub1"><B>([^<]+)|class="DataStub1">([^<]+)', chunk)
        if not label:
            continue
        name = (label.group(1) or label.group(2)).strip()
        values = [v.strip() for _, v in re.findall(r'class="(DataB|Current2|DataB2)">([^<]+)', chunk)]
        if values:
            found.setdefault(name, values)
    working_mmcf = int(found["Total Working Gas Capacity"][-1].replace(",", ""))
    design_mmcf = int(found["Total Storage Capacity"][-1].replace(",", ""))
    released = re.search(r"Release Date:\s*([0-9/]+)", text)
    return {
        "release_date": released.group(1) if released else None,
        "working_mmcf": working_mmcf,
        "design_mmcf": design_mmcf,
        "working_twh": round(working_mmcf / 1000.0 / BCM_CF * BCM_TWH, 1),
        "design_twh": round(design_mmcf / 1000.0 / BCM_CF * BCM_TWH, 1),
    }


# ------------------------------------------------------------- producao

def grain_production() -> dict[str, dict]:
    payload = read_json(YIELDS)
    out = {}
    for iso3, item in payload["nations"].items():
        total, per_crop, year = 0.0, {}, None
        for crop in GRAIN_CROPS:
            entry = item["crops"].get(crop) or {}
            prod = entry.get("production_t")
            if isinstance(prod, (int, float)):
                total += float(prod)
                per_crop[crop] = float(prod)
                if entry.get("year") and (year is None or entry["year"] > year):
                    year = entry["year"]
        if total > 0:
            out[iso3] = {"total_t": total, "crops_t": per_crop, "year": year}
    return out


# ------------------------------------------------------------- referencia

def load_reference() -> dict:
    if not os.path.exists(REFERENCE):
        raise SystemExit("referencia ausente: %s" % REFERENCE)
    return read_json(REFERENCE)


def reference_index(ref: dict) -> dict[tuple[str, str], dict]:
    out = {}
    for entry in ref.get("entries", []):
        key = (entry["iso3"], entry["metric"])
        # primeira entrada vence; duplicata acontece so se houver mais de uma fonte
        out.setdefault(key, entry)
    return out


# ------------------------------------------------------------ montagem

def build(nations, iso_map, production, ref, gold, oil_eu, spr, us_gas) -> tuple:
    idx = reference_index(ref)
    sources = dict(ref["sources"])
    sources.update({key: value for key, value in BUILT_SOURCES.items() if key not in sources})

    # --- calibracao das regras pela amostra observada
    grain_ratios = []
    cold_per_capita = []
    for nation in nations:
        iso3 = nation["id"]
        g = idx.get((iso3, "grain_storage_capacity_mt"))
        prod = production.get(iso3)
        if g and prod and prod["total_t"] > 0:
            grain_ratios.append((float(g["value"]) * 1e6) / prod["total_t"])
        c = idx.get((iso3, "cold_storage_capacity_m3"))
        pop = iso_map[iso3]["population"]
        if c and pop:
            cold_per_capita.append(float(c["value"]) / pop)
    grain_k = round(statistics.median(grain_ratios), 4) if len(grain_ratios) >= 5 else 0.25
    cold_k = round(statistics.median(cold_per_capita), 4) if len(cold_per_capita) >= 5 else 0.06

    storage = {
        "_meta": {
            "dataset": "storage_capacity",
            "version": "g2",
            "generated_by": "tools/build_storage_g2.py",
            "retrieved": utc_today(),
            "nations": len(nations),
            "units": {
                "grain_capacity_mt": "milhoes de toneladas metricas (1 Mt = 1e6 t)",
                "cold_capacity_m3": "metros cubicos de camara fria",
                "gas_working_capacity_twh": "TWh de gas natural (capacidade de trabalho)",
            },
            "rules": {
                "grain": {
                    "kind": "months_of_production",
                    "formula": "capacity_mt = k_graos * producao_anual_de_graos_t / 1e6",
                    "k_graos": grain_k,
                    "k_basis": "mediana de capacidade observada / producao anual observada nas nacoes com dado publicado (n=%d)" % len(grain_ratios),
                    "production_basis": "FAOSTAT QCL wheat+rice+maize+soy, ultimo ano <=2024 (crop_yields.json)",
                    "when": "somente quando nao existe capacidade publicada para o pais",
                },
                "cold": {
                    "kind": "per_capita",
                    "formula": "capacity_m3 = k_frio * populacao",
                    "k_frio_m3_per_capita": cold_k,
                    "k_basis": "mediana de m3 observados / populacao nas nacoes com dado publicado (n=%d)" % len(cold_per_capita),
                    "when": "somente quando nao existe capacidade publicada para o pais",
                },
                "gas": {
                    "kind": "none",
                    "game_rule": "sem capacid. documentada -> o jogo comeca com 0 TWh e a capacidade e construida (armazem de gas)",
                    "why": "nao existe base global publica; GIE/AGSI+ exige chave de API e so cobre a Europa",
                },
                "oil_public": {
                    "kind": "none",
                    "game_rule": "sem reserva publica documentada -> o jogo comeca com 0 t de reserva estrategica",
                },
            },
            "no_data_rule": "sem valor publicado e sem base de calculo -> null com basis='none' e motivo em note; nunca ha numero inventado",
        },
        "sources": sources,
        "coverage": {},
        "nations": {},
    }

    reserves = {
        "_meta": {
            "dataset": "strategic_reserves",
            "version": "g2",
            "generated_by": "tools/build_storage_g2.py",
            "retrieved": utc_today(),
            "nations": len(nations),
            "units": {
                "oil_public_stocks_kt": "mil toneladas metricas de petroleo cru",
                "oil_public_days_net_imports": "dias de importacao liquida cobertos",
                "grain_public_stocks_t": "toneladas",
                "gold_tonnes": "toneladas de ouro fino",
            },
            "conversions": {
                "gold": "1 onca troy fina = 31,1034768 g; IMF IFS publica milhoes de oncas troy finas",
                "oil_bbl_to_t": "1 barril de cru = %.4f t (7,33 bbl/t), aplicado so onde a fonte publica em barris" % T_PER_BBL,
                "gas_bcm_to_twh": "1 bcm = %.1f TWh (PCI 38,6 MJ/m3), convencao declarada" % BCM_TWH,
            },
            "no_data_rule": "sem valor publicado -> null com basis='none' e motivo; reserva publica ausente vale 0 no jogo (regra declarada), nao no dado",
        },
        "sources": sources,
        "coverage": {},
        "nations": {},
    }

    counts = {
        "grain_observed": 0, "grain_rule": 0, "grain_none": 0,
        "cold_observed": 0, "cold_rule": 0, "cold_none": 0,
        "gas_observed": 0, "gas_none": 0,
        "oil_observed": 0, "oil_none": 0,
        "grain_public_observed": 0,
        "gold_observed": 0, "gold_none": 0,
    }
    lists = {key: [] for key in (
        "grain_rule_ids", "grain_none_ids", "cold_rule_ids", "cold_none_ids",
        "gas_none_ids", "oil_none_ids", "gold_none_ids")}

    for nation in nations:
        iso3 = nation["id"]
        name = nation["name"]
        prod = production.get(iso3)
        pop = iso_map[iso3]["population"]

        # ---------------- graos
        g = idx.get((iso3, "grain_storage_capacity_mt"))
        if g:
            grain = {
                "capacity_mt": round(float(g["value"]), 4),
                "basis": "observed",
                "source": g["source_key"],
                "year": g["year"],
                "estimate": bool(g.get("estimate")),
                "note": g.get("note"),
            }
            counts["grain_observed"] += 1
        elif prod:
            cap = grain_k * prod["total_t"] / 1e6
            grain = {
                "capacity_mt": round(cap, 6),
                "basis": "rule",
                "source": None,
                "year": prod["year"],
                "note": "regra k_graos x producao anual (ver _meta.rules.grain); producao %.0f t em %s" % (
                    prod["total_t"], prod["year"]),
            }
            counts["grain_rule"] += 1
            lists["grain_rule_ids"].append(iso3)
        else:
            grain = {"capacity_mt": None, "basis": "none", "source": None, "year": None,
                     "note": "sem capacidade publicada e sem producao de graos no FAOSTAT"}
            counts["grain_none"] += 1
            lists["grain_none_ids"].append(iso3)
        grain["months_of_production"] = (
            round(grain["capacity_mt"] * 1e6 / (prod["total_t"] / 12.0), 6)
            if grain["capacity_mt"] and prod else None)

        # ---------------- frio
        c = idx.get((iso3, "cold_storage_capacity_m3"))
        if c:
            cold = {"capacity_m3": round(float(c["value"]), 2), "basis": "observed",
                    "source": c["source_key"], "year": c["year"],
                    "estimate": bool(c.get("estimate")), "note": c.get("note")}
            counts["cold_observed"] += 1
        elif pop:
            cold = {"capacity_m3": round(cold_k * pop, 1), "basis": "rule", "source": None,
                    "year": None,
                    "note": "regra k_frio x populacao (ver _meta.rules.cold); populacao %d" % pop}
            counts["cold_rule"] += 1
            lists["cold_rule_ids"].append(iso3)
        else:
            cold = {"capacity_m3": None, "basis": "none", "source": None, "year": None,
                    "note": "sem populacao e sem dado publicado"}
            counts["cold_none"] += 1
            lists["cold_none_ids"].append(iso3)

        # ---------------- gas
        gas_ref = idx.get((iso3, "gas_storage_capacity_twh"))
        if iso3 == "USA":
            gas = {"working_capacity_twh": us_gas["working_twh"], "basis": "observed",
                   "source": "eia_us_gas_capacity", "year": 2024,
                   "release_date": us_gas["release_date"],
                   "note": "EIA working gas capacity %s MMcf (coluna de 2024; release %s) / %.4f bcf-por-bcm x %.1f TWh/bcm" % (
                       format(us_gas["working_mmcf"], ","), us_gas["release_date"], BCM_CF, BCM_TWH)}
            counts["gas_observed"] += 1
        elif gas_ref:
            gas = {"working_capacity_twh": round(float(gas_ref["value"]), 4), "basis": "observed",
                   "source": gas_ref["source_key"], "year": gas_ref["year"],
                   "estimate": bool(gas_ref.get("estimate")), "note": gas_ref.get("note")}
            counts["gas_observed"] += 1
        else:
            gas = {"working_capacity_twh": None, "basis": "none", "source": None, "year": None,
                   "note": "sem capacidade documentada (GIE/AGSI+ so Europa, com chave); jogo comeca com 0"}
            counts["gas_none"] += 1
            lists["gas_none_ids"].append(iso3)

        storage["nations"][iso3] = {"name": name, "grain": grain, "cold": cold, "gas": gas}

        # ---------------- petroleo publico
        oil_ref = idx.get((iso3, "oil_public_stocks_kt"))
        oil_cap = idx.get((iso3, "oil_public_capacity_kt"))
        oil_days = idx.get((iso3, "oil_public_days_net_imports"))
        if iso3 == "USA":
            oil = {"public_stocks_kt": spr["kt"],
                   "capacity_kt": round(float(oil_cap["value"]), 3) if oil_cap else None,
                   "days_net_imports": round(float(oil_days["value"]), 3) if oil_days else None,
                   "basis": "observed", "source": "eia_spr", "year": 2026,
                   "note": "EIA weekly SPR %s: %s mil barris x %.4f t/bbl" % (
                       spr["date"], format(spr["thousand_barrels"], ","), T_PER_BBL)}
            counts["oil_observed"] += 1
        elif iso3 in oil_eu:
            e = oil_eu[iso3]
            oil = {"public_stocks_kt": e["public_stocks_kt"],
                   "capacity_kt": round(float(oil_cap["value"]), 3) if oil_cap else None,
                   "days_net_imports": e["days_net_imports"], "basis": "observed",
                   "source": "eurostat_stk_oem", "year": e["period"],
                   "note": "relacao mensal da diretiva 2009/119/CE (STK_EUE_DIR x max(IMP_DNC, IC_DC))" +
                           ("; ultima observacao publicada, pais sem reporte recente" if e["stale"] else "")}
            if e["public_stocks_kt"] is None:
                oil["note"] += "; sem denominador (importacao/consumo) publicado no mesmo mes"
            counts["oil_observed"] += 1
        elif oil_ref:
            oil = {"public_stocks_kt": round(float(oil_ref["value"]), 3),
                   "capacity_kt": round(float(oil_cap["value"]), 3) if oil_cap else None,
                   "days_net_imports": round(float(oil_days["value"]), 3) if oil_days else None,
                   "basis": "observed", "estimate": bool(oil_ref.get("estimate")),
                   "source": oil_ref["source_key"], "year": oil_ref["year"], "note": oil_ref.get("note")}
            counts["oil_observed"] += 1
        else:
            oil = {"public_stocks_kt": None, "capacity_kt": None, "days_net_imports": None,
                   "basis": "none", "source": None, "year": None,
                   "note": "sem reserva publica documentada nas fontes consultadas; jogo comeca com 0"}
            counts["oil_none"] += 1
            lists["oil_none_ids"].append(iso3)

        # ---------------- graos publicos
        gp = idx.get((iso3, "grain_public_stocks_t"))
        gp_cap = idx.get((iso3, "grain_public_capacity_t"))
        if gp or gp_cap:
            grain_public = {
                "stocks_t": round(float(gp["value"]), 2) if gp else None,
                "capacity_t": round(float(gp_cap["value"]), 2) if gp_cap else None,
                "basis": "observed",
                "source": (gp or gp_cap)["source_key"],
                "year": (gp or gp_cap)["year"],
                "note": (gp or gp_cap).get("note"),
            }
            counts["grain_public_observed"] += 1
        else:
            grain_public = {"stocks_t": None, "capacity_t": None, "basis": "none",
                            "source": None, "year": None,
                            "note": "sem estoque publico de graos publicado"}

        # ---------------- ouro
        gold_item = gold.get(iso3)
        if gold_item:
            gold_entry = {"tonnes": gold_item["tonnes"], "basis": "observed",
                          "source": "imf_ifs_gold", "period": gold_item["period"],
                          "millions_fine_oz": gold_item["millions_fine_oz"],
                          "stale": gold_item["period"] < "2025-01",
                          "note": "IMF IFS RAFAGOLDV_OZT; inclui depositos de ouro e ouro emprestado"}
            counts["gold_observed"] += 1
        else:
            gold_entry = {"tonnes": None, "basis": "none", "source": None, "period": None,
                          "millions_fine_oz": None,
                          "note": "area sem serie RAFAGOLDV_OZT no IFS"}
            counts["gold_none"] += 1
            lists["gold_none_ids"].append(iso3)

        reserves["nations"][iso3] = {"name": name, "oil": oil, "grain_public": grain_public,
                                     "gold": gold_entry}

    # ---------------- cobertura
    storage["coverage"] = {
        "nations_total": len(nations),
        "grain_observed": counts["grain_observed"],
        "grain_rule": counts["grain_rule"],
        "grain_none": counts["grain_none"],
        "grain_rule_ids": sorted(lists["grain_rule_ids"]),
        "grain_none_ids": sorted(lists["grain_none_ids"]),
        "cold_observed": counts["cold_observed"],
        "cold_rule": counts["cold_rule"],
        "cold_none": counts["cold_none"],
        "cold_rule_ids": sorted(lists["cold_rule_ids"]),
        "cold_none_ids": sorted(lists["cold_none_ids"]),
        "gas_observed": counts["gas_observed"],
        "gas_none": counts["gas_none"],
        "gas_none_ids": sorted(lists["gas_none_ids"]),
        "totals": {
            "grain_capacity_mt": round(sum(
                (n["grain"]["capacity_mt"] or 0.0) for n in storage["nations"].values()), 3),
            "cold_capacity_m3": round(sum(
                (n["cold"]["capacity_m3"] or 0.0) for n in storage["nations"].values()), 1),
            "gas_working_capacity_twh": round(sum(
                (n["gas"]["working_capacity_twh"] or 0.0) for n in storage["nations"].values()), 3),
        },
        "observed_totals": {
            "grain_capacity_mt": round(sum(
                (n["grain"]["capacity_mt"] or 0.0) for n in storage["nations"].values()
                if n["grain"]["basis"] == "observed"), 3),
            "cold_capacity_m3": round(sum(
                (n["cold"]["capacity_m3"] or 0.0) for n in storage["nations"].values()
                if n["cold"]["basis"] == "observed"), 1),
            "gas_working_capacity_twh": round(sum(
                (n["gas"]["working_capacity_twh"] or 0.0) for n in storage["nations"].values()
                if n["gas"]["basis"] == "observed"), 3),
        },
    }
    reserves["coverage"] = {
        "nations_total": len(nations),
        "oil_observed": counts["oil_observed"],
        "oil_none": counts["oil_none"],
        "oil_none_ids": sorted(lists["oil_none_ids"]),
        "grain_public_observed": counts["grain_public_observed"],
        "gold_observed": counts["gold_observed"],
        "gold_none": counts["gold_none"],
        "gold_none_ids": sorted(lists["gold_none_ids"]),
        "totals": {
            "oil_public_stocks_kt": round(sum(
                (n["oil"]["public_stocks_kt"] or 0.0) for n in reserves["nations"].values()), 3),
            "gold_tonnes": round(sum(
                (n["gold"]["tonnes"] or 0.0) for n in reserves["nations"].values()), 3),
        },
    }
    meta_extra = {
        "spr": spr,
        "us_gas": us_gas,
        "grain_sample": len(grain_ratios),
        "cold_sample": len(cold_per_capita),
        "grain_k": grain_k,
        "cold_k": cold_k,
    }
    return storage, reserves, meta_extra


# ------------------------------------------------------------- mapeamento

WAREHOUSES = {
    "silo": {
        "name": "Silo graneleiro",
        "where": "regiao + porto",
        "guards": "graos e farinaceos a granel",
    },
    "cold_room": {
        "name": "Camara fria / frigorifico",
        "where": "cidade + porto",
        "guards": "carne, pereciveis e cadeia de frio (exige energia)",
    },
    "fuel_tank": {
        "name": "Tanque de petroleo/combustiveis",
        "where": "regiao (campo/refinaria) + porto",
        "guards": "petroleo, combustiveis, etanol e quimicos liquidos",
    },
    "gas_storage": {
        "name": "Armazenagem de gas",
        "where": "regiao",
        "guards": "gas natural",
    },
    "bulk_yard": {
        "name": "Patio de granéis",
        "where": "regiao (mina) + porto",
        "guards": "carvao, minerios, fosfato, enxofre, madeira, algodao",
    },
    "general_warehouse": {
        "name": "Armazem geral",
        "where": "cidade",
        "guards": "semiprocessados e finais",
    },
    "vault": {
        "name": "Cofre",
        "where": "cidade",
        "guards": "ouro, diamante, uranio, terras raras (liga as reservas do BC)",
    },
    "military_depot": {
        "name": "Paiol militar",
        "where": "economia de guerra",
        "guards": "municao, explosivos e racao de combate",
    },
}

# id -> (armazem primario, armazens secundarios, motivo)
GOODS_MAP = {
    # graos e derivados do silo
    "wheat": ("silo", [], "grao a granel"),
    "rice": ("silo", [], "grao a granel"),
    "maize": ("silo", [], "grao a granel"),
    "soy": ("silo", [], "oleaginosa a granel"),
    "flour": ("silo", ["general_warehouse"], "farinha ensacada circula entre silo e armazem"),
    "bran": ("silo", [], "farelo a granel"),
    "feed": ("silo", [], "racao a granel"),
    "bagged_rice": ("general_warehouse", ["silo"], "arroz ja ensacado; guardado tambem em silo"),
    # camara fria
    "meat": ("cold_room", [], "carne exige cadeia de frio"),
    "potato": ("cold_room", [], "tuberculo perecivel"),
    "vaccines": ("cold_room", ["general_warehouse"], "vacina exige cadeia de frio"),
    "canned_food": ("general_warehouse", [], "conserva estavel em prateleira"),
    # tanque
    "oil": ("fuel_tank", [], "petroleo cru"),
    "fuel": ("fuel_tank", [], "combustivel refinado"),
    "ethanol": ("fuel_tank", [], "etanol liquido"),
    "chemicals": ("fuel_tank", ["general_warehouse"], "quimicos liquidos a granel; parte ensacada"),
    # gas
    "gas": ("gas_storage", [], "gas natural"),
    # patio de granéis
    "coal": ("bulk_yard", [], "carvao mineral a granel"),
    "charcoal": ("bulk_yard", ["general_warehouse"], "carvao vegetal ensacado"),
    "iron": ("bulk_yard", [], "minerio de ferro"),
    "bauxite": ("bulk_yard", [], "minerio de bauxita"),
    "copper": ("bulk_yard", [], "minerio de cobre"),
    "lead": ("bulk_yard", [], "minerio de chumbo"),
    "tungsten": ("bulk_yard", [], "minerio de tungstenio"),
    "phosphate": ("bulk_yard", [], "fosfato a granel"),
    "sulfur": ("bulk_yard", [], "enxofre a granel"),
    "wood": ("bulk_yard", [], "madeira em toras"),
    "cotton": ("bulk_yard", [], "algodao em fardos"),
    "sawdust": ("bulk_yard", ["general_warehouse"], "serragem a granel"),
    "fertilizer": ("bulk_yard", ["general_warehouse"], "fertilizante granulado/ensacado"),
    "organic_fertilizer": ("bulk_yard", ["general_warehouse"], "adubo organico a granel"),
    # cofre
    "gold": ("vault", [], "ouro em barra/lingote bruto"),
    "gold_bar": ("vault", [], "barra de ouro acabada"),
    "diamond": ("vault", [], "pedra preciosa"),
    "uranium": ("vault", [], "material nuclear"),
    "rare_earths": ("vault", [], "terras raras (alto valor)"),
    "processed_rare_earths": ("vault", ["general_warehouse"], "oxidos processados"),
    "jewelry": ("vault", [], "joia de alto valor"),
    # paiol militar
    "gunpowder": ("military_depot", [], "polvora"),
    "explosives": ("military_depot", [], "explosivo"),
    "casing": ("military_depot", ["general_warehouse"], "estojo de municao"),
    "projectile": ("military_depot", [], "projetil"),
    "ammunition": ("military_depot", ["general_warehouse"], "municao leve"),
    "artillery_ammo": ("military_depot", [], "municao de artilharia"),
    "small_arms": ("military_depot", ["general_warehouse"], "arma leve"),
    "artillery": ("military_depot", [], "artilharia"),
    "military_drone": ("military_depot", [], "drone militar"),
    "infantry_gear": ("military_depot", ["general_warehouse"], "equipamento de infantaria"),
    "combat_ration": ("military_depot", ["general_warehouse"], "racao de combate"),
}


def build_goods_map(goods: list[dict]) -> dict:
    entries = {}
    missing = []
    for good in goods:
        gid = good["id"]
        if gid in GOODS_MAP:
            primary, secondary, reason = GOODS_MAP[gid]
        else:
            primary, secondary, reason = "general_warehouse", [], "semiprocessado/final padrao"
        entries[gid] = {
            "name": good["name"],
            "sector": good.get("sector"),
            "kind": good.get("kind"),
            "tier": good.get("tier"),
            "primary": primary,
            "secondary": sorted(secondary),
            "reason": reason,
        }
        if gid not in GOODS_MAP:
            missing.append(gid)
    per_warehouse = {key: 0 for key in WAREHOUSES}
    for item in entries.values():
        per_warehouse[item["primary"]] += 1
    return {
        "_meta": {
            "dataset": "goods_storage_map",
            "version": "g2",
            "generated_by": "tools/build_storage_g2.py",
            "retrieved": utc_today(),
            "goods": len(entries),
            "source": "docs/plans/perf_pop_financas_plano.md secao PARTE G2 (tabela de armazens)",
            "no_data_rule": "todo bem tem armazem primario; bem sem regra propria cai em general_warehouse (declarado)",
        },
        "warehouses": WAREHOUSES,
        "coverage": {
            "goods_total": len(entries),
            "by_primary": per_warehouse,
            "explicit": len(entries) - len(missing),
            "fallback_general_warehouse": sorted(missing),
        },
        "goods": entries,
    }


def main() -> int:
    nations = load_nations()
    iso_map = load_iso_map(nations)
    production = grain_production()
    ref = load_reference()
    gold = gold_tonnes(iso_map, nations)
    oil_eu = eurostat_oil(nations, iso_map)
    spr = eia_us_spr()
    us_gas = eia_us_gas()

    storage, reserves, meta = build(nations, iso_map, production, ref, gold, oil_eu, spr, us_gas)
    storage["_meta"]["data_snapshot"] = meta
    reserves["_meta"]["data_snapshot"] = meta

    goods = read_json(GOODS)
    goods_map = build_goods_map(goods)

    os.makedirs(OUT_DIR, exist_ok=True)
    write_json(os.path.join(OUT_DIR, "storage_capacity.json"), storage)
    write_json(os.path.join(OUT_DIR, "strategic_reserves.json"), reserves)
    write_json(os.path.join(OUT_DIR, "goods_storage_map.json"), goods_map)

    print("storage_capacity.json   graos: %d observado / %d regra / %d sem dado" % (
        storage["coverage"]["grain_observed"], storage["coverage"]["grain_rule"],
        storage["coverage"]["grain_none"]))
    print("                        frio:  %d observado / %d regra / %d sem dado" % (
        storage["coverage"]["cold_observed"], storage["coverage"]["cold_rule"],
        storage["coverage"]["cold_none"]))
    print("                        gas:   %d observado / %d sem dado" % (
        storage["coverage"]["gas_observed"], storage["coverage"]["gas_none"]))
    print("strategic_reserves.json petroleo: %d observado / %d sem dado; ouro: %d / %d" % (
        reserves["coverage"]["oil_observed"], reserves["coverage"]["oil_none"],
        reserves["coverage"]["gold_observed"], reserves["coverage"]["gold_none"]))
    print("goods_storage_map.json  %d bens, %d sem regra explicita" % (
        goods_map["coverage"]["goods_total"], len(goods_map["coverage"]["fallback_general_warehouse"])))
    print("k_graos=%.4f (n=%d)  k_frio=%.4f m3/hab (n=%d)" % (
        meta["grain_k"], meta["grain_sample"], meta["cold_k"], meta["cold_sample"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
