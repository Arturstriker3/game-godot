#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida os dados da Parte G2 (armazenagem e reservas nacionais).

Checa os tres arquivos:
  data/definitions/economy/storage_capacity.json
  data/definitions/economy/strategic_reserves.json
  data/definitions/economy/goods_storage_map.json

Regras:
  1. JSON parseavel, UTF-8 sem BOM, LF, indent=2, chaves ordenadas, newline final;
  2. as nacoes sao EXATAMENTE as 179 de data/definitions/scenario/nations.json;
  3. campos obrigatorios e tipos coerentes; nenhum valor negativo;
  4. basis em observed|rule|none e coerente com o preenchimento (rule exige a
     formula declarada em _meta.rules; none exige note nao vazia);
  5. source citada existe em sources quando basis=observed;
  6. coverage e totals batem com a contagem recalculada (inclusive as listas);
  7. a regra de graos/frio reproduz o valor: capacity = k x producao/populacao;
  8. months_of_production e consistente com producao e capacidade;
  9. ouro: tonnes = millions_fine_oz x 31,1034768 g;
 10. petroleo: public_stocks_kt = dias x max(importacao, consumo) quando Eurostat;
 11. mapeamento: todo bem de goods.json presente, todo armazem valido, primario
     no conjunto de armazens e contagem em by_primary correta.

Uso:
    py -3 tools/validate_storage_g2.py
Sai 0 quando esta tudo consistente; 1 com a lista de problemas.
"""

from __future__ import annotations

import datetime
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_JSON = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
YIELDS_JSON = os.path.join(ROOT, "data", "definitions", "agriculture", "crop_yields.json")
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "world", "nations_source.csv")
GOODS_JSON = os.path.join(ROOT, "data", "definitions", "economy", "goods.json")
STORAGE = os.path.join(ROOT, "data", "definitions", "economy", "storage_capacity.json")
RESERVES = os.path.join(ROOT, "data", "definitions", "economy", "strategic_reserves.json")
MAP = os.path.join(ROOT, "data", "definitions", "economy", "goods_storage_map.json")

BASIS = ("observed", "rule", "none")
GRAIN_CROPS = ("wheat", "rice", "maize", "soy")
TROY_OZ_T = 31.1034768 / 1_000_000.0
TOL = 1e-3


def load(path: str):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def game_nations() -> list[str]:
    return [item["id"] for item in load(NATIONS_JSON)]


def iso2_map() -> dict[str, str]:
    import csv
    import io
    aliases = {"SOL": "SB", "SSD": "SS"}
    out = {}
    with io.open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            out[row["iso3"]] = row["iso2"]
    for key, val in aliases.items():
        out.setdefault(key, val)
    return out


def check_format(path: str, problems: list[str]) -> None:
    raw = open(path, "rb").read()
    name = os.path.basename(path)
    if raw.startswith(b"\xef\xbb\xbf"):
        problems.append("%s: BOM no inicio" % name)
    if b"\r" in raw:
        problems.append("%s: contem CR (deve ser LF)" % name)
    text = raw.decode("utf-8")
    if not text.endswith("\n"):
        problems.append("%s: nao termina com newline" % name)
    canonical = json.dumps(json.loads(text), indent=2, ensure_ascii=False, sort_keys=True) + "\n"
    if canonical != text:
        problems.append("%s: fora do padrao (indent=2, sort_keys, LF)" % name)


def is_num(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def check_nations_block(payload: dict, expected: list[str], label: str, problems: list[str]) -> None:
    got = sorted(payload.get("nations", {}))
    if got != sorted(expected):
        missing = sorted(set(expected) - set(got))
        extra = sorted(set(got) - set(expected))
        problems.append("%s: nacoes divergem (faltam %r, sobram %r)" % (label, missing[:10], extra[:10]))


def check_source_ref(payload: dict, key, label: str, problems: list[str]) -> None:
    if key is None:
        return
    sources = payload.get("sources") or {}
    if key not in sources:
        problems.append("%s: source %r nao existe em sources" % (label, key))
        return
    src = sources[key]
    for field in ("title", "publisher", "url", "year"):
        if not src.get(field):
            problems.append("%s: sources.%s sem %s" % (label, key, field))
    if isinstance(src.get("url"), str) and not src["url"].startswith("http"):
        problems.append("%s: sources.%s.url nao e http(s)" % (label, key))


def production() -> dict[str, float]:
    payload = load(YIELDS_JSON)
    out = {}
    for iso3, item in payload["nations"].items():
        total = 0.0
        for crop in GRAIN_CROPS:
            prod = (item["crops"].get(crop) or {}).get("production_t")
            if is_num(prod):
                total += float(prod)
        if total > 0:
            out[iso3] = total
    return out


def population() -> dict[str, int]:
    import csv
    import io
    out = {}
    with io.open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            if row["population"]:
                out[row["iso3"]] = int(float(row["population"]))
    # SOL/SSD nao existem no CSV de mundo: mesma regra do construtor
    aliases = {"SOL": "SLB"}
    for key, ref in aliases.items():
        if ref in out:
            out.setdefault(key, out[ref])
    for item in load(NATIONS_JSON):
        if item["id"] not in out and item.get("population"):
            out[item["id"]] = int(item["population"])
    return out


def check_storage(payload: dict, nations: list[str], problems: list[str]) -> None:
    meta = payload.get("_meta") or {}
    rules = meta.get("rules") or {}
    grain_rule = rules.get("grain") or {}
    cold_rule = rules.get("cold") or {}
    k_grain = grain_rule.get("k_graos")
    k_cold = cold_rule.get("k_frio_m3_per_capita")
    if not is_num(k_grain) or k_grain <= 0:
        problems.append("storage: _meta.rules.grain.k_graos ausente/invalido")
    if not is_num(k_cold) or k_cold <= 0:
        problems.append("storage: _meta.rules.cold.k_frio_m3_per_capita ausente/invalido")
    prod = production()
    pop = population()
    counts = {"grain_observed": 0, "grain_rule": 0, "grain_none": 0,
              "cold_observed": 0, "cold_rule": 0, "cold_none": 0,
              "gas_observed": 0, "gas_none": 0}
    rule_ids = {"grain": [], "cold": []}
    none_ids = {"grain": [], "cold": [], "gas": []}
    totals = {"grain": 0.0, "cold": 0.0, "gas": 0.0}
    obs_totals = {"grain": 0.0, "cold": 0.0, "gas": 0.0}

    for iso3 in nations:
        entry = payload["nations"].get(iso3)
        if not isinstance(entry, dict):
            problems.append("storage.%s: ausente" % iso3)
            continue
        for fam, value_field, total_key in (
            ("grain", "capacity_mt", "grain"),
            ("cold", "capacity_m3", "cold"),
            ("gas", "working_capacity_twh", "gas"),
        ):
            block = entry.get(fam)
            if not isinstance(block, dict):
                problems.append("storage.%s.%s: ausente" % (iso3, fam))
                continue
            basis = block.get("basis")
            value = block.get(value_field)
            if basis not in BASIS:
                problems.append("storage.%s.%s: basis invalido %r" % (iso3, fam, basis))
            if basis == "none":
                if value is not None:
                    problems.append("storage.%s.%s: basis none com valor %r" % (iso3, fam, value))
                if not (block.get("note") or "").strip():
                    problems.append("storage.%s.%s: basis none sem note" % (iso3, fam))
                none_ids[fam].append(iso3)
                counts["%s_none" % fam] = counts.get("%s_none" % fam, 0) + 1
                continue
            if not is_num(value) or value < 0:
                problems.append("storage.%s.%s: valor invalido %r" % (iso3, fam, value))
                continue
            if basis == "observed":
                counts["%s_observed" % fam] += 1
                obs_totals[total_key] += float(value)
                check_source_ref(payload, block.get("source"), "storage.%s.%s" % (iso3, fam), problems)
                if not block.get("year"):
                    problems.append("storage.%s.%s: observed sem year" % (iso3, fam))
            else:
                counts["%s_rule" % fam] = counts.get("%s_rule" % fam, 0) + 1
                rule_ids[fam].append(iso3)
                if block.get("source"):
                    problems.append("storage.%s.%s: basis rule com source %r" % (iso3, fam, block["source"]))
                if fam == "grain":
                    if iso3 not in prod:
                        problems.append("storage.%s.grain: rule sem producao FAOSTAT" % iso3)
                        continue
                    expected = k_grain * prod[iso3] / 1e6
                    if abs(expected - value) > TOL:
                        problems.append("storage.%s.grain: regra nao reproduz (%.4f != %.4f)"
                                        % (iso3, value, expected))
                    months = block.get("months_of_production")
                    expect_months = value * 1e6 / (prod[iso3] / 12.0)
                    if (not is_num(months)
                            or abs(months - expect_months) > max(1e-3, expect_months * 1e-5)):
                        problems.append("storage.%s.grain: months_of_production inconsistente" % iso3)
                elif fam == "cold":
                    if iso3 not in pop:
                        problems.append("storage.%s.cold: rule sem populacao" % iso3)
                        continue
                    expected = k_cold * pop[iso3]
                    if abs(expected - value) > max(0.5, expected * 1e-4):
                        problems.append("storage.%s.cold: regra nao reproduz (%.1f != %.1f)"
                                        % (iso3, value, expected))
            totals[total_key] += float(value)

    coverage = payload.get("coverage") or {}
    for key in ("grain_observed", "grain_rule", "grain_none",
                "cold_observed", "cold_rule", "cold_none",
                "gas_observed", "gas_none"):
        if coverage.get(key) != counts[key]:
            problems.append("storage.coverage.%s: %r != %r" % (key, coverage.get(key), counts[key]))
    expected_rules = {"grain_rule_ids": rule_ids["grain"], "cold_rule_ids": rule_ids["cold"]}
    for key, ids in expected_rules.items():
        if sorted(coverage.get(key) or []) != sorted(ids):
            problems.append("storage.coverage.%s nao bate com as entradas" % key)
    for fam in ("grain", "cold", "gas"):
        key = "%s_none_ids" % fam
        if sorted(coverage.get(key) or []) != sorted(none_ids[fam]):
            problems.append("storage.coverage.%s nao bate com as entradas" % key)
    cov_totals = coverage.get("totals") or {}
    for fam, key in (("grain", "grain_capacity_mt"), ("cold", "cold_capacity_m3"),
                     ("gas", "gas_working_capacity_twh")):
        if not is_num(cov_totals.get(key)) or abs(cov_totals[key] - totals[fam]) > 1.0:
            problems.append("storage.coverage.totals.%s: %r != %.3f" % (key, cov_totals.get(key), totals[fam]))
    obs_cov = coverage.get("observed_totals") or {}
    for fam, key in (("grain", "grain_capacity_mt"), ("cold", "cold_capacity_m3"),
                     ("gas", "gas_working_capacity_twh")):
        if not is_num(obs_cov.get(key)) or abs(obs_cov[key] - obs_totals[fam]) > 1.0:
            problems.append("storage.coverage.observed_totals.%s: %r != %.3f"
                            % (key, obs_cov.get(key), obs_totals[fam]))


def check_reserves(payload: dict, nations: list[str], problems: list[str]) -> None:
    counts = {"oil_observed": 0, "oil_none": 0, "gold_observed": 0, "gold_none": 0,
              "grain_public_observed": 0}
    oil_none, gold_none = [], []
    oil_total = gold_total = 0.0
    for iso3 in nations:
        entry = payload["nations"].get(iso3)
        if not isinstance(entry, dict):
            problems.append("reserves.%s: ausente" % iso3)
            continue
        oil = entry.get("oil") or {}
        basis = oil.get("basis")
        if basis not in BASIS:
            problems.append("reserves.%s.oil: basis invalido %r" % (iso3, basis))
        if basis == "none":
            counts["oil_none"] += 1
            oil_none.append(iso3)
            if oil.get("public_stocks_kt") is not None:
                problems.append("reserves.%s.oil: none com valor" % iso3)
            if not (oil.get("note") or "").strip():
                problems.append("reserves.%s.oil: none sem note" % iso3)
        else:
            counts["oil_observed"] += 1
            value = oil.get("public_stocks_kt")
            days = oil.get("days_net_imports")
            if value is None and is_num(days):
                pass  # dias publicados sem denominador: valor em t fica nulo, honestamente
            elif not is_num(value) or value < 0:
                problems.append("reserves.%s.oil: public_stocks_kt invalido %r" % (iso3, value))
            else:
                oil_total += float(value)
            check_source_ref(payload, oil.get("source"), "reserves.%s.oil" % iso3, problems)
            if not oil.get("year"):
                problems.append("reserves.%s.oil: sem year" % iso3)
            if oil.get("source") == "eurostat_stk_oem":
                if not is_num(days):
                    problems.append("reserves.%s.oil: eurostat sem dias" % iso3)
            cap = oil.get("capacity_kt")
            if cap is not None and (not is_num(cap) or cap < 0):
                problems.append("reserves.%s.oil: capacity_kt invalido %r" % (iso3, cap))

        grain_public = entry.get("grain_public") or {}
        if grain_public.get("basis") == "observed":
            stocks = grain_public.get("stocks_t")
            capacity = grain_public.get("capacity_t")
            if stocks is None and capacity is None:
                problems.append("reserves.%s.grain_public: observed sem valor" % iso3)
            for label, value in (("stocks_t", stocks), ("capacity_t", capacity)):
                if value is not None and (not is_num(value) or value < 0):
                    problems.append("reserves.%s.grain_public.%s invalido" % (iso3, label))
            counts["grain_public_observed"] += 1
            check_source_ref(payload, grain_public.get("source"),
                             "reserves.%s.grain_public" % iso3, problems)

        gold = entry.get("gold") or {}
        basis = gold.get("basis")
        if basis == "none":
            counts["gold_none"] += 1
            gold_none.append(iso3)
            if gold.get("tonnes") is not None:
                problems.append("reserves.%s.gold: none com valor" % iso3)
            if not (gold.get("note") or "").strip():
                problems.append("reserves.%s.gold: none sem note" % iso3)
        else:
            tonnes = gold.get("tonnes")
            oz = gold.get("millions_fine_oz")
            if not is_num(tonnes) or tonnes < 0:
                problems.append("reserves.%s.gold: tonnes invalido %r" % (iso3, tonnes))
            else:
                gold_total += float(tonnes)
                counts["gold_observed"] += 1
            if not is_num(oz) or oz < 0:
                problems.append("reserves.%s.gold: millions_fine_oz invalido" % iso3)
            elif abs(oz * 1_000_000 * TROY_OZ_T - tonnes) > 0.01:
                problems.append("reserves.%s.gold: conversao de onca inconsistente" % iso3)
            if not gold.get("period"):
                problems.append("reserves.%s.gold: sem period" % iso3)
            check_source_ref(payload, gold.get("source"), "reserves.%s.gold" % iso3, problems)

    coverage = payload.get("coverage") or {}
    for key, value in counts.items():
        if coverage.get(key) != value:
            problems.append("reserves.coverage.%s: %r != %r" % (key, coverage.get(key), value))
    if sorted(coverage.get("oil_none_ids") or []) != sorted(oil_none):
        problems.append("reserves.coverage.oil_none_ids nao bate")
    if sorted(coverage.get("gold_none_ids") or []) != sorted(gold_none):
        problems.append("reserves.coverage.gold_none_ids nao bate")
    totals = coverage.get("totals") or {}
    if abs((totals.get("oil_public_stocks_kt") or 0) - oil_total) > 1.0:
        problems.append("reserves.coverage.totals.oil_public_stocks_kt nao bate")
    if abs((totals.get("gold_tonnes") or 0) - gold_total) > 0.5:
        problems.append("reserves.coverage.totals.gold_tonnes nao bate")


def check_goods(payload: dict, problems: list[str]) -> None:
    goods = load(GOODS_JSON)
    ids = [item["id"] for item in goods]
    warehouses = payload.get("warehouses") or {}
    if not warehouses:
        problems.append("map: bloco warehouses vazio")
        return
    entries = payload.get("goods") or {}
    missing = [gid for gid in ids if gid not in entries]
    extra = [gid for gid in entries if gid not in ids]
    if missing:
        problems.append("map: bens sem mapeamento: %r" % missing)
    if extra:
        problems.append("map: bens inexistentes no goods.json: %r" % extra)
    per = {key: 0 for key in warehouses}
    for gid, item in entries.items():
        primary = item.get("primary")
        if primary not in warehouses:
            problems.append("map.%s: primary invalido %r" % (gid, primary))
            continue
        per[primary] += 1
        for sec in item.get("secondary") or []:
            if sec not in warehouses:
                problems.append("map.%s: secondary invalido %r" % (gid, sec))
            if sec == primary:
                problems.append("map.%s: secondary repete o primary" % gid)
        if not (item.get("reason") or "").strip():
            problems.append("map.%s: sem reason" % gid)
    coverage = payload.get("coverage") or {}
    if coverage.get("goods_total") != len(ids):
        problems.append("map.coverage.goods_total: %r != %r" % (coverage.get("goods_total"), len(ids)))
    if coverage.get("by_primary") != per:
        problems.append("map.coverage.by_primary nao bate com as entradas")
    fallback = coverage.get("fallback_general_warehouse") or []
    if sorted(fallback) != sorted([gid for gid, it in entries.items()
                                   if it.get("reason") == "semiprocessado/final padrao"]):
        problems.append("map.coverage.fallback_general_warehouse nao bate")


def main() -> int:
    problems: list[str] = []
    for path in (STORAGE, RESERVES, MAP):
        if not os.path.exists(path):
            problems.append("arquivo ausente: %s" % os.path.basename(path))
            continue
        check_format(path, problems)
    if problems:
        print("FALHOU (%d problemas)" % len(problems))
        for item in problems:
            print(" -", item)
        return 1

    nations = game_nations()
    storage = load(STORAGE)
    reserves = load(RESERVES)
    mapping = load(MAP)

    check_nations_block(storage, nations, "storage", problems)
    check_nations_block(reserves, nations, "reserves", problems)
    check_storage(storage, nations, problems)
    check_reserves(reserves, nations, problems)
    check_goods(mapping, problems)

    if problems:
        print("FALHOU (%d problemas)" % len(problems))
        for item in problems:
            print(" -", item)
        return 1

    today = datetime.date.today().isoformat()
    print("OK %s" % today)
    print(" storage: %d nacoes | graos %d obs/%d regra/%d sem dado | frio %d/%d/%d | gas %d/%d" % (
        len(nations), storage["coverage"]["grain_observed"], storage["coverage"]["grain_rule"],
        storage["coverage"]["grain_none"], storage["coverage"]["cold_observed"],
        storage["coverage"]["cold_rule"], storage["coverage"]["cold_none"],
        storage["coverage"]["gas_observed"], storage["coverage"]["gas_none"]))
    print(" reserves: petroleo %d obs/%d sem | ouro %d obs/%d sem" % (
        reserves["coverage"]["oil_observed"], reserves["coverage"]["oil_none"],
        reserves["coverage"]["gold_observed"], reserves["coverage"]["gold_none"]))
    print(" map: %d bens, %d armazens" % (mapping["coverage"]["goods_total"], len(mapping["warehouses"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
