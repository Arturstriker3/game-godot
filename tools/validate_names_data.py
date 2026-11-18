"""Valida os dados de nomes da Parte C4 (bancos) e da B7 (serviços informais).

Uso:
    python tools/validate_names_data.py

Verifica, sem tocar no jogo:
  * JSON em UTF-8, sem BOM, e legível;
  * bancos: uma entrada por nação do jogo (todas as 179 de world/out/nations.csv),
    moeda igual à da nação, banco central por moeda coerente entre as nações da
    mesma moeda, status válidos e fonte declarada quando há nome;
  * serviços informais: ids únicos, nomes nos três idiomas, termos locais com
    ISO3 existente no jogo e fonte declarada;
  * toda `source_key` citada existe no bloco `sources` do próprio arquivo.

Sai com código 1 e lista de erros quando algo falha. Não inventa correção.
"""

from __future__ import annotations

import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "world", "out", "nations.csv")
BANKS_JSON = os.path.join(ROOT, "data", "definitions", "economy", "banks.json")
INFORMAL_JSON = os.path.join(ROOT, "data", "definitions", "economy", "informal_services.json")

STATUS_OK = {"present", "none", "no_data"}
LANGS = ("pt", "en", "es")


def read_json_strict(path: str, errors: list[str]) -> dict | None:
    """Lê JSON exigindo UTF-8 sem BOM."""
    try:
        with open(path, "rb") as fh:
            raw = fh.read()
    except OSError as exc:
        errors.append(f"{os.path.relpath(path, ROOT)}: não foi possível abrir ({exc})")
        return None
    if raw.startswith(b"\xef\xbb\xbf"):
        errors.append(f"{os.path.relpath(path, ROOT)}: BOM UTF-8 no início")
        raw = raw[3:]
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        errors.append(f"{os.path.relpath(path, ROOT)}: não é UTF-8 válido ({exc})")
        return None
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        errors.append(f"{os.path.relpath(path, ROOT)}: JSON inválido ({exc})")
        return None
    if not isinstance(data, dict):
        errors.append(f"{os.path.relpath(path, ROOT)}: raiz não é objeto JSON")
        return None
    return data


def load_nations(errors: list[str]) -> dict[str, dict[str, str]]:
    if not os.path.exists(NATIONS_CSV):
        errors.append(f"{os.path.relpath(NATIONS_CSV, ROOT)}: arquivo ausente")
        return {}
    with open(NATIONS_CSV, encoding="utf-8-sig", newline="") as fh:
        rows = list(csv.DictReader(fh))
    nations = {}
    for row in rows:
        iso3 = (row.get("id") or "").strip()
        if not iso3:
            continue
        nations[iso3] = {
            "name": (row.get("name") or "").strip(),
            "currency": (row.get("currency") or "").strip(),
            "region": (row.get("region") or "").strip(),
        }
    if len(nations) != 179:
        errors.append(f"nations.csv: esperava 179 nações, achei {len(nations)}")
    return nations


def check_sources(block: dict, where: str, sources: dict, errors: list[str]) -> None:
    key = block.get("source_key")
    if not key:
        errors.append(f"{where}: source_key ausente")
    elif key not in sources:
        errors.append(f"{where}: source_key '{key}' não existe em sources")


def valid_year(value) -> bool:
    return isinstance(value, int) and 1900 <= value <= 2100


def validate_banks(data: dict, nations: dict[str, dict[str, str]], errors: list[str]) -> dict:
    entries = data.get("nations")
    if not isinstance(entries, dict):
        errors.append("banks.json: 'nations' ausente ou não é objeto")
        return {}
    sources = data.get("sources") or {}
    if not isinstance(sources, dict) or not sources:
        errors.append("banks.json: 'sources' ausente ou vazio")

    missing = sorted(set(nations) - set(entries))
    extra = sorted(set(entries) - set(nations))
    if missing:
        errors.append(f"banks.json: nações sem entrada ({len(missing)}): {', '.join(missing)}")
    if extra:
        errors.append(f"banks.json: ISO3 fora do jogo ({len(extra)}): {', '.join(extra)}")

    central_by_currency: dict[str, tuple[str, str]] = {}
    stats = {"present": 0, "none": 0, "no_data": 0}
    slot_stats: dict[str, dict[str, int]] = {
        "public": {"present": 0, "none": 0, "no_data": 0},
        "private": {"present": 0, "none": 0, "no_data": 0},
        "foreign": {"present": 0, "none": 0, "no_data": 0},
    }

    for iso3 in sorted(entries):
        entry = entries[iso3]
        where = f"banks.{iso3}"
        if not isinstance(entry, dict):
            errors.append(f"{where}: entrada não é objeto")
            continue
        expected = nations.get(iso3)
        if expected is None:
            continue
        if entry.get("name") != expected["name"]:
            errors.append(f"{where}: name '{entry.get('name')}' != nations.csv '{expected['name']}'")
        if entry.get("currency") != expected["currency"]:
            errors.append(
                f"{where}: currency '{entry.get('currency')}' != nations.csv '{expected['currency']}'"
            )

        cb = entry.get("central_bank")
        if not isinstance(cb, dict) or not cb.get("name"):
            if not (isinstance(cb, dict) and cb.get("status") == "none" and cb.get("note")):
                errors.append(f"{where}: central_bank.name ausente sem status none + note")
        else:
            check_sources(cb, f"{where}.central_bank", sources, errors)
            currency = expected["currency"]
            seen = central_by_currency.get(currency)
            if seen is None:
                central_by_currency[currency] = (cb["name"], iso3)
            elif seen[0] != cb["name"]:
                # Moeda compartilhada pode ter autoridade local própria (BCE/BCR
                # em país dolarizado); avisa sem falhar.
                print(
                    f"aviso: {where}: banco central '{cb['name']}' difere de "
                    f"'{seen[0]}' ({seen[1]}) na moeda {currency}"
                )

        for slot in ("public", "private", "foreign"):
            block = entry.get(slot)
            if not isinstance(block, dict):
                errors.append(f"{where}.{slot}: ausente ou não é objeto")
                continue
            status = block.get("status")
            if status not in STATUS_OK:
                errors.append(f"{where}.{slot}: status '{status}' inválido")
                continue
            stats[status] += 1
            slot_stats[slot][status] += 1
            if status == "present":
                if slot != "foreign" and not block.get("name"):
                    errors.append(f"{where}.{slot}: status present sem name")
                if slot != "foreign":
                    if not valid_year(block.get("year")):
                        errors.append(f"{where}.{slot}: status present sem year válido")
                    if not block.get("basis"):
                        errors.append(f"{where}.{slot}: status present sem basis")
                check_sources(block, f"{where}.{slot}", sources, errors)
            else:
                if not block.get("note"):
                    errors.append(f"{where}.{slot}: status {status} sem note explicando")
            if slot == "public" and status == "none":
                if not block.get("proposed_name"):
                    errors.append(f"{where}.public: status none sem proposed_name")
            if slot == "private":
                if block.get("owner") != "national":
                    errors.append(f"{where}.private: owner deve ser 'national'")
            if slot == "public":
                if block.get("owner") != "state":
                    errors.append(f"{where}.public: owner deve ser 'state'")
            if slot == "foreign":
                if block.get("owner") != "foreign":
                    errors.append(f"{where}.foreign: owner deve ser 'foreign'")
                if status == "present":
                    origin = block.get("origin_iso3")
                    if origin not in nations:
                        errors.append(f"{where}.foreign: origin_iso3 '{origin}' fora do jogo")
                    if not block.get("branch_candidate"):
                        errors.append(f"{where}.foreign: sem branch_candidate")

    coverage = data.get("coverage")
    if isinstance(coverage, dict):
        declared_total = coverage.get("nations_total")
        if declared_total is not None and declared_total != len(entries):
            errors.append(
                f"banks.coverage: nations_total {declared_total} != {len(entries)} entradas"
            )
        for slot in ("public", "private", "foreign"):
            declared = coverage.get(slot) or {}
            for status in ("present", "none", "no_data"):
                if declared.get(status) != slot_stats[slot][status]:
                    errors.append(
                        f"banks.coverage.{slot}.{status}: declarado {declared.get(status)} "
                        f"!= contado {slot_stats[slot][status]}"
                    )

    stats["currencies"] = len(central_by_currency)
    stats["nations"] = len(entries)
    stats["by_slot"] = slot_stats
    return stats


def validate_informal(data: dict, nations: dict[str, dict[str, str]], errors: list[str]) -> dict:
    sources = data.get("sources") or {}
    if not isinstance(sources, dict) or not sources:
        errors.append("informal_services.json: 'sources' ausente ou vazio")
    categories = data.get("categories")
    if not isinstance(categories, list) or not categories:
        errors.append("informal_services.json: 'categories' ausente ou vazio")
        return {}

    seen_ids: set[str] = set()
    seen_terms: set[tuple[str, str, str]] = set()
    countries: set[str] = set()
    term_count = 0

    for index, cat in enumerate(categories):
        where = f"informal.categories[{index}]"
        if not isinstance(cat, dict):
            errors.append(f"{where}: não é objeto")
            continue
        cat_id = cat.get("id")
        if not cat_id:
            errors.append(f"{where}: id ausente")
        elif cat_id in seen_ids:
            errors.append(f"{where}: id duplicado '{cat_id}'")
        else:
            seen_ids.add(cat_id)
            where = f"informal.{cat_id}"
        for field in ("name_pt", "name_en", "name_es"):
            if not cat.get(field):
                errors.append(f"{where}: {field} ausente")
        if "description_pt" not in cat:
            errors.append(f"{where}: description_pt ausente")
        terms = cat.get("local_terms")
        if not isinstance(terms, list) or not terms:
            errors.append(f"{where}: local_terms ausente ou vazio")
            continue
        for term in terms:
            if not isinstance(term, dict):
                errors.append(f"{where}: termo não é objeto")
                continue
            iso3 = term.get("iso3")
            text = term.get("term")
            lang = term.get("lang")
            if iso3 not in nations:
                errors.append(f"{where}: ISO3 '{iso3}' não existe no jogo")
            if not text:
                errors.append(f"{where}: termo vazio em {iso3}")
            if lang not in {*LANGS, "ht", "id", "vi", "zh", "th", "tr", "it", "fr",
                            "ko", "ja", "sw", "sn", "rw", "ar", "bn", "ur", "ne",
                            "km", "lo", "tet", "en", "my", "si", "bs", "mk", "sq",
                            "he", "ka", "hy", "kk", "ky", "uz", "ro", "sr", "uk",
                            "pl", "ms", "pt"}:
                errors.append(f"{where}: língua '{lang}' fora da lista aceita")
            key = (iso3 or "", (text or "").lower(), cat_id or "")
            if key in seen_terms:
                errors.append(f"{where}: termo duplicado {key}")
            seen_terms.add(key)
            if iso3:
                countries.add(iso3)
            term_count += 1
            check_sources(term, f"{where}.{iso3}", sources, errors)

    return {
        "categories": len(seen_ids),
        "terms": term_count,
        "countries": len(countries),
    }


def main() -> int:
    errors: list[str] = []
    nations = load_nations(errors)

    banks = read_json_strict(BANKS_JSON, errors)
    informal = read_json_strict(INFORMAL_JSON, errors)

    banks_stats: dict = {}
    informal_stats: dict = {}
    if banks is not None:
        banks_stats = validate_banks(banks, nations, errors)
    if informal is not None:
        informal_stats = validate_informal(informal, nations, errors)

    if errors:
        print(f"FALHOU — {len(errors)} problema(s):")
        for line in errors:
            print(f"  - {line}")
        return 1

    print("OK — dados de nomes válidos")
    slots = banks_stats.get("by_slot", {})
    for slot in ("public", "private", "foreign"):
        counts = slots.get(slot, {})
        print(
            "  bancos.{slot}: {present} present / {none} none / {no_data} no_data".format(
                slot=slot,
                present=counts.get("present", 0),
                none=counts.get("none", 0),
                no_data=counts.get("no_data", 0),
            )
        )
    print(
        "  bancos: {nations} nações, {currencies} moedas".format(
            nations=banks_stats.get("nations", 0),
            currencies=banks_stats.get("currencies", 0),
        )
    )
    print(
        "  informais: {categories} categorias, {terms} termos locais em {countries} países".format(
            categories=informal_stats.get("categories", 0),
            terms=informal_stats.get("terms", 0),
            countries=informal_stats.get("countries", 0),
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
