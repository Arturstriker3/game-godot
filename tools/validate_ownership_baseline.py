#!/usr/bin/env python
"""Valida data/definitions/economy/ownership_baseline.json.

Confere o contrato do arquivo contra data/definitions/scenario/nations.csv:
  * 179/179 nações, chave = id de nations.csv;
  * toda nação com valor numérico tem refs (title/url/year) e fonte declarada;
  * SOE em [0,100]; estoques de IDE >= 0; anos inteiros em [1990,2026];
  * soe_* nulo <=> state_sectors nulo;
  * o bloco coverage bate com a contagem recalculada.

Sai com código 0 quando tudo passa; 1 quando há erro.
"""

from __future__ import annotations

import csv
import json
import sys
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
NATIONS_CSV = ROOT / "data" / "definitions" / "scenario" / "nations.csv"
BASELINE = ROOT / "data" / "definitions" / "economy" / "ownership_baseline.json"

YEAR_MIN, YEAR_MAX = 1990, 2026
FDI_FIELDS = ("fdi_inward_stock_usd", "fdi_outward_stock_usd")
PCT_FIELDS = ("fdi_inward_pct_gdp", "fdi_outward_pct_gdp")
SOE_FIELDS = ("soe_share_gdp", "soe_share_employment")
SOURCE_KEYS = ("imf_dip", "worldbank_gdp", "oecd_soe", "imf_fiscal_monitor_soe", "national_study")

errors: list[str] = []
warnings: list[str] = []


def err(msg: str) -> None:
    errors.append(msg)


def is_number(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def check_ref(ref: object, where: str) -> None:
    if not isinstance(ref, dict):
        err(f"{where}: ref não é objeto")
        return
    title = ref.get("title")
    url = ref.get("url")
    year = ref.get("year")
    if not isinstance(title, str) or not title.strip():
        err(f"{where}: ref sem title")
    if not isinstance(url, str) or not url.strip():
        err(f"{where}: ref sem url")
    else:
        parsed = urlparse(url)
        if parsed.scheme not in ("http", "https") or not parsed.netloc:
            err(f"{where}: url inválida ({url})")
    if not isinstance(year, int) or isinstance(year, bool) or not (YEAR_MIN <= year <= YEAR_MAX):
        err(f"{where}: ref com year inválido ({year!r})")


def main() -> int:
    if not BASELINE.exists():
        print(f"ERRO: {BASELINE.relative_to(ROOT)} não existe")
        return 1
    if not NATIONS_CSV.exists():
        print(f"ERRO: {NATIONS_CSV.relative_to(ROOT)} não existe")
        return 1

    with NATIONS_CSV.open(encoding="utf-8-sig", newline="") as fh:
        nations = list(csv.DictReader(fh))
    expected_ids = [n["id"] for n in nations]
    expected_names = {n["id"]: n["name"] for n in nations}

    try:
        payload = json.loads(BASELINE.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        print(f"ERRO: JSON inválido: {exc}")
        return 1

    # ---------------------------------------------------------------- topo
    if payload.get("version") != 1:
        err(f"version deve ser 1 (encontrado {payload.get('version')!r})")
    if not isinstance(payload.get("note"), str) or len(payload.get("note", "").strip()) < 40:
        err("note ausente ou curta demais")

    sources = payload.get("sources")
    if not isinstance(sources, dict):
        err("sources ausente")
        sources = {}
    for key in SOURCE_KEYS:
        entry = sources.get(key)
        if not isinstance(entry, dict):
            err(f"sources.{key} ausente")
            continue
        if not isinstance(entry.get("title"), str) or not entry["title"].strip():
            err(f"sources.{key}.title ausente")
        # national_study não tem URL única: as refs por nação trazem a URL do estudo.
        if key != "national_study" and (not isinstance(entry.get("url"), str) or not entry["url"].strip()):
            err(f"sources.{key}.url ausente")

    nations_block = payload.get("nations")
    if not isinstance(nations_block, dict):
        print("ERRO: bloco nations ausente")
        return 1

    # ------------------------------------------------------------- nações
    got = set(nations_block)
    want = set(expected_ids)
    for missing in sorted(want - got):
        err(f"nação ausente: {missing}")
    for extra in sorted(got - want):
        err(f"nação não existe em nations.csv: {extra}")

    with_fdi = 0
    with_soe = 0
    with_sectors = 0
    no_data = 0

    for iso in expected_ids:
        entry = nations_block.get(iso)
        if entry is None:
            continue
        where = f"{iso}"

        if entry.get("name") != expected_names[iso]:
            err(f"{where}: name diverge de nations.csv ({entry.get('name')!r})")

        inward = entry.get("fdi_inward_stock_usd")
        outward = entry.get("fdi_outward_stock_usd")
        has_fdi = inward is not None or outward is not None
        with_fdi += 1 if has_fdi else 0

        for field in FDI_FIELDS:
            value = entry.get(field)
            if value is None:
                continue
            if not is_number(value):
                err(f"{where}.{field}: não numérico ({value!r})")
                continue
            if value < 0:
                err(f"{where}.{field}: estoque negativo ({value})")

        for field in PCT_FIELDS:
            value = entry.get(field)
            if value is None:
                continue
            if not is_number(value):
                err(f"{where}.{field}: não numérico ({value!r})")

        for field in ("fdi_year", "gdp_year"):
            value = entry.get(field)
            if value is None:
                continue
            if not isinstance(value, int) or isinstance(value, bool):
                err(f"{where}.{field}: deve ser inteiro ({value!r})")
            elif not (YEAR_MIN <= value <= YEAR_MAX):
                err(f"{where}.{field}: fora de [{YEAR_MIN},{YEAR_MAX}] ({value})")

        for field in SOE_FIELDS:
            value = entry.get(field)
            if value is None:
                continue
            with_soe += 1 if field == "soe_share_gdp" else 0
            if not is_number(value):
                err(f"{where}.{field}: não numérico ({value!r})")
            elif not (0.0 <= value <= 100.0):
                err(f"{where}.{field}: fora de [0,100] ({value})")

        soe_any = any(entry.get(f) is not None for f in SOE_FIELDS)
        sectors = entry.get("state_sectors")
        sectors_ref = entry.get("state_sectors_ref")
        with_sectors += 1 if sectors else 0
        if soe_any and not entry.get("soe_source"):
            err(f"{where}: soe_* preenchido sem soe_source")
        if not soe_any:
            if entry.get("soe_no_data") is not True:
                err(f"{where}: sem SOE e sem soe_no_data=true")
            if not isinstance(entry.get("soe_reason"), str) or not entry["soe_reason"].strip():
                err(f"{where}: sem SOE e sem soe_reason")
        # state_sectors é qualitativo e independe do peso numérico; mas exige fonte.
        if sectors is None:
            if sectors_ref is not None:
                err(f"{where}: state_sectors_ref presente sem state_sectors")
        else:
            if not isinstance(sectors, list) or not sectors:
                err(f"{where}.state_sectors deve ser lista não vazia quando presente")
            elif not all(isinstance(s, str) and s.strip() for s in sectors):
                err(f"{where}.state_sectors com item inválido")
            if sectors_ref is None:
                err(f"{where}: state_sectors sem state_sectors_ref")
            else:
                check_ref(sectors_ref, f"{where}.state_sectors_ref")

        investors = entry.get("main_investors")
        if not isinstance(investors, list):
            err(f"{where}.main_investors deve ser lista")
            investors = []
        for idx, item in enumerate(investors):
            spot = f"{where}.main_investors[{idx}]"
            if not isinstance(item, dict):
                err(f"{spot}: não é objeto")
                continue
            code = item.get("country")
            if not isinstance(code, str) or len(code) != 3 or not code.isalpha():
                err(f"{spot}.country inválido ({code!r})")
            if not isinstance(item.get("name"), str) or not item["name"].strip():
                err(f"{spot}.name ausente")
            pos = item.get("position_usd")
            if not is_number(pos) or pos < 0:
                err(f"{spot}.position_usd inválido ({pos!r})")
            share = item.get("share")
            if not is_number(share) or not (0.0 <= share <= 1.0):
                err(f"{spot}.share fora de [0,1] ({share!r})")

        # refs / fonte para qualquer valor numérico
        refs = entry.get("refs")
        if not isinstance(refs, list):
            err(f"{where}.refs deve ser lista")
            refs = []
        numeric_any = has_fdi or any(entry.get(f) is not None for f in PCT_FIELDS) or soe_any
        if numeric_any:
            if not refs:
                err(f"{where}: valor numérico sem refs")
            for idx, ref in enumerate(refs):
                check_ref(ref, f"{where}.refs[{idx}]")
            if has_fdi and not entry.get("fdi_source"):
                err(f"{where}: estoque de IDE sem fdi_source")
            if entry.get("fdi_source") and entry["fdi_source"] not in sources:
                err(f"{where}: fdi_source desconhecido ({entry['fdi_source']})")
            if entry.get("soe_source") and entry["soe_source"] not in sources:
                err(f"{where}: soe_source desconhecido ({entry['soe_source']})")

        # nação sem dado precisa explicitar
        if not has_fdi:
            if entry.get("no_data") is not True:
                err(f"{where}: sem IDE mas no_data != true")
            if not isinstance(entry.get("reason"), str) or not entry["reason"].strip():
                err(f"{where}: sem IDE e sem reason")
        no_data += 1 if entry.get("no_data") is True else 0

        confidence = entry.get("confidence")
        if confidence not in ("complete", "partial", "estimated"):
            err(f"{where}.confidence inválida ({confidence!r})")
        if confidence == "complete" and (not has_fdi or entry.get("fdi_inward_pct_gdp") is None):
            err(f"{where}: confidence=complete sem IDE inward + percentual do PIB")

    # ----------------------------------------------------------- coverage
    coverage = payload.get("coverage")
    if not isinstance(coverage, dict):
        err("coverage ausente")
    else:
        if coverage.get("nations_total") != len(expected_ids):
            err(
                f"coverage.nations_total={coverage.get('nations_total')} != {len(expected_ids)} nações em nations.csv"
            )
        if coverage.get("nations_with_fdi") != with_fdi:
            err(f"coverage.nations_with_fdi={coverage.get('nations_with_fdi')} != recalculado {with_fdi}")
        if coverage.get("nations_with_soe") != with_soe:
            err(f"coverage.nations_with_soe={coverage.get('nations_with_soe')} != recalculado {with_soe}")
        if "nations_with_state_sectors" not in coverage:
            err("coverage.nations_with_state_sectors ausente")
        elif coverage["nations_with_state_sectors"] != with_sectors:
            err(
                f"coverage.nations_with_state_sectors={coverage['nations_with_state_sectors']} "
                f"!= recalculado {with_sectors}"
            )
        if with_sectors == 0:
            err("nenhuma nação com state_sectors documentado (mínimo esperado: 10)")
        if "nations_no_data" in coverage and coverage["nations_no_data"] != no_data:
            err(f"coverage.nations_no_data={coverage['nations_no_data']} != recalculado {no_data}")

    # ------------------------------------------------------------- saída
    for warning in warnings:
        print(f"AVISO: {warning}")
    if errors:
        print(f"FALHOU: {len(errors)} problema(s)")
        for message in errors[:40]:
            print("  -", message)
        if len(errors) > 40:
            print(f"  ... e mais {len(errors) - 40}")
        return 1

    print(
        "OK: "
        f"{len(expected_ids)} nações · IDE em {with_fdi} · SOE numérico em {with_soe} · "
        f"setores estatais documentados em {with_sectors} · sem dado em {no_data} · {len(errors)} erros"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
