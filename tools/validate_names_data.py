"""Valida os dados de nomes da Parte C4 (bancos) e da B7 (serviços informais).

Uso:
    python tools/validate_names_data.py

Verifica, sem tocar no jogo:
  * JSON em UTF-8, sem BOM, e legível;
  * bancos: uma entrada por nação do jogo (todas as 179 de world/out/nations.csv),
    moeda igual à da nação, banco central genérico por país ou por união monetária
    (nome fixo nas três línguas), status válidos e fonte declarada quando há nome;
  * nomes fictícios: todo nome de banco exibido (public/private/foreign) tem
    `name_basis` começando em "fictício:"; `public` sem banco estatal tem
    `proposed_name` com a mesma marcação;
  * unicidade: nomes de public/private não colidem entre si nem com filiais
    estrangeiras (a filial pode repetir entre países do mesmo grupo de origem);
  * marcas: nenhum nome de `tools/real_bank_names_denylist.txt` aparece nos campos
    de nome/nota, nos títulos/notas de fonte (nunca em `url`), no documento
    `docs/data/names_banks_informal.md` nem nos valores `banks.*` do i18n;
  * serviços informais: ids únicos, nomes nos três idiomas, termos locais com
    ISO3 existente no jogo e fonte declarada;
  * toda `source_key` citada existe no bloco `sources` do próprio arquivo;
  * proveniência: fonte sem `url` precisa de `url_note` (≥ 30 caracteres) e
    ausência (`none`/`no_data`) precisa de `source_key` existente ou
    `source_note` (≥ 30 caracteres);
  * denylist: entrada que normaliza para vazio é erro, e a seção `[central]` não
    pode conter o rótulo genérico "Banco Central de <país>" (aceito no dado).

Sai com código 1 e lista de erros quando algo falha. Não inventa correção.
"""

from __future__ import annotations

import csv
import json
import os
import re
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "world", "out", "nations.csv")
BANKS_JSON = os.path.join(ROOT, "data", "definitions", "economy", "banks.json")
INFORMAL_JSON = os.path.join(ROOT, "data", "definitions", "economy", "informal_services.json")
I18N_CSV = os.path.join(ROOT, "data", "definitions", "i18n", "i18n.csv")
I18N_JSON = os.path.join(ROOT, "data", "definitions", "i18n", "i18n.json")
DENYLIST_TXT = os.path.join(ROOT, "tools", "real_bank_names_denylist.txt")
NAMES_DOC = os.path.join(ROOT, "docs", "data", "names_banks_informal.md")

STATUS_OK = {"present", "none", "no_data"}
CENTRAL_KINDS = {"national", "monetary_union", "none"}
LANGS = ("pt", "en", "es")

PUBLIC_KEYS = {
    "status", "owner", "name", "name_basis", "basis", "year", "source_key", "note",
}
PRIVATE_KEYS = set(PUBLIC_KEYS)
FOREIGN_KEYS = {
    "status", "owner", "origin_iso3", "origin_name", "origin_basis",
    "branch_candidate", "branch_status", "branch_name_basis", "source_key", "note",
}
CENTRAL_KEYS = {
    "name", "name_en", "name_es", "kind", "zone", "status", "source_key", "note",
}

EUROZONE = {
    "AUT", "BEL", "CYP", "DEU", "ESP", "EST", "FIN", "FRA", "GRC", "HRV",
    "IRL", "ITA", "LTU", "LUX", "LVA", "MLT", "NED", "PRT", "SVK", "SVN",
}
XOF = {"BEN", "BFA", "CIV", "GNB", "MLI", "NER", "SEN", "TGO"}
XAF = {"CAF", "CMR", "COG", "GAB", "GNQ", "TCD"}
XCD = {"ATG", "DMA", "GRD", "KNA", "LCA", "VCT"}

ZONE_LABELS = {
    "euro": (
        "Banco Central da Zona do Euro",
        "Central Bank of the Euro Area",
        "Banco Central de la Zona del Euro",
    ),
    "uemoa": (
        "Banco Central da União Monetária Oeste-Africana",
        "Central Bank of the West African Monetary Union",
        "Banco Central de la Unión Monetaria de África Occidental",
    ),
    "cemac": (
        "Banco Central da União Monetária da África Central",
        "Central Bank of the Central African Monetary Union",
        "Banco Central de la Unión Monetaria de África Central",
    ),
    "ecs": (
        "Banco Central da União Monetária do Caribe Oriental",
        "Central Bank of the Eastern Caribbean Monetary Union",
        "Banco Central de la Unión Monetaria del Caribe Oriental",
    ),
}

FICTICIO_PREFIX = "fictício:"
DENYLIST_TEXT_LIMIT = 10
# Rótulo genérico sancionado do banco central: "Banco Central de <país>" /
# "Central Bank of <país>" e variantes ("da", "del", "de la", "dos", "das"),
# seguidas de 1 a 4 palavras. Não é validado contra a lista de países: o prefixo
# basta, porque a denylist não deve conter o rótulo genérico de nação nenhuma.
GENERIC_CENTRAL_RE = re.compile(
    r"^(?:banco central (?:de la|del|dos|das|de|da)|central bank of)"
    r"\s+\w+(?:\s+\w+){0,3}$"
)


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


def normalize(text: str) -> str:
    """Minúsculas, sem acento, pontuação vira espaço."""
    text = unicodedata.normalize("NFKD", text)
    text = "".join(ch for ch in text if not unicodedata.combining(ch))
    text = text.lower()
    text = re.sub(r"[\W_]+", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def load_denylist(errors: list[str]) -> list[str]:
    """Lê as duas seções da denylist (só as entradas, não os cabeçalhos)."""
    entries: list[str] = []
    sections: dict[str, int] = {}
    current = ""
    rel = os.path.relpath(DENYLIST_TXT, ROOT)
    try:
        with open(DENYLIST_TXT, encoding="utf-8-sig") as fh:
            for number, line in enumerate(fh, 1):
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if line.startswith("[") and line.endswith("]"):
                    current = line[1:-1].strip().lower()
                    sections.setdefault(current, 0)
                    continue
                norm = normalize(line)
                if not norm:
                    # Entrada que vira string vazia não protege nada: é erro, não
                    # ruído ignorável (só pontuação, só acento, só símbolo).
                    errors.append(
                        f"{rel}:{number}: entrada sem letras/dígitos ({line!r}); remova-a"
                    )
                    continue
                if current == "central" and GENERIC_CENTRAL_RE.match(norm):
                    # O rótulo genérico "Banco Central de <país>" / "Central Bank
                    # of <país>" pode ser o próprio nome da autoridade monetária,
                    # então é aceito e não pode ser exigido pela denylist.
                    errors.append(
                        f"{rel}:{number}: '[central]' com o rótulo genérico {line!r}; "
                        "remova (o rótulo genérico é aceito por política)"
                    )
                entries.append(norm)
                sections[current] = sections.get(current, 0) + 1
    except OSError as exc:
        errors.append(f"{rel}: não foi possível abrir ({exc})")
        return []
    for wanted in ("commercial", "central"):
        if not sections.get(wanted):
            errors.append(f"{os.path.relpath(DENYLIST_TXT, ROOT)}: seção [{wanted}] vazia")
    return entries


_PATTERNS: dict[str, "re.Pattern[str]"] = {}


def _pattern(entry: str) -> "re.Pattern[str]":
    """Padrão de fronteira de palavra compilado uma vez por entrada."""
    pat = _PATTERNS.get(entry)
    if pat is None:
        pat = re.compile(r"(?<!\w)" + re.escape(entry) + r"(?!\w)")
        _PATTERNS[entry] = pat
    return pat


def find_brand(text, entries: list[str]) -> str:
    """Devolve a primeira marca da denylist presente no texto ("" se limpa)."""
    if not isinstance(text, str) or not text.strip():
        return ""
    norm = normalize(text)
    for entry in entries:
        if _pattern(entry).search(norm):
            return entry
    return ""


def check_no_brand(where: str, text, entries: list[str], errors: list[str]) -> None:
    hit = find_brand(text, entries)
    if hit:
        errors.append(f"{where}: marca real na denylist ('{hit}') em {text!r}")


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


def load_i18n_nations(errors: list[str]) -> dict[str, dict[str, str]]:
    """Nomes de país em pt-BR/en/es (`nation.<ISO3>`) do i18n.csv."""
    out: dict[str, dict[str, str]] = {}
    if not os.path.exists(I18N_CSV):
        errors.append(f"{os.path.relpath(I18N_CSV, ROOT)}: arquivo ausente")
        return out
    with open(I18N_CSV, encoding="utf-8-sig", newline="") as fh:
        rows = list(csv.DictReader(fh))
    for row in rows:
        key = (row.get("key") or "").strip()
        if not key.startswith("nation."):
            continue
        iso3 = key[len("nation."):]
        if not iso3 or "." in iso3:
            continue
        out[iso3] = {
            "pt": (row.get("pt_BR") or "").strip(),
            "en": (row.get("en") or "").strip(),
            "es": (row.get("es") or "").strip(),
        }
    return out


def check_keys(where: str, block: dict, required: set[str], errors: list[str]) -> None:
    missing = sorted(required - set(block))
    if missing:
        errors.append(f"{where}: chaves ausentes {', '.join(missing)}")


def check_sources(block: dict, where: str, sources: dict, errors: list[str]) -> None:
    key = block.get("source_key")
    if not key:
        errors.append(f"{where}: source_key ausente")
    elif key not in sources:
        errors.append(f"{where}: source_key '{key}' não existe em sources")


def valid_year(value) -> bool:
    return isinstance(value, int) and 1900 <= value <= 2100


def expected_central(iso3: str, currency: str, i18n: dict[str, dict[str, str]]):
    """(status, kind, zone, nome_pt, nome_en, nome_es) esperado do banco central."""
    if iso3 == "PAN":
        return ("none", "none", None, None, None, None)
    zone = None
    if currency == "EUR" and iso3 in EUROZONE:
        zone = "euro"
    elif currency == "XOF" and iso3 in XOF:
        zone = "uemoa"
    elif currency == "XAF" and iso3 in XAF:
        zone = "cemac"
    elif currency == "XCD" and iso3 in XCD:
        zone = "ecs"
    if zone is not None:
        pt, en, es = ZONE_LABELS[zone]
        return ("present", "monetary_union", zone, pt, en, es)
    country = i18n.get(iso3) or {}
    return (
        "present",
        "national",
        None,
        "Banco Central de " + country.get("pt", ""),
        "Central Bank of " + country.get("en", ""),
        "Banco Central de " + country.get("es", ""),
    )


def validate_central_bank(where: str, block: dict, iso3: str, currency: str,
                          i18n: dict[str, dict[str, str]], denylist: list[str],
                          errors: list[str]) -> None:
    if not isinstance(block, dict):
        errors.append(f"{where}: ausente ou não é objeto")
        return
    check_keys(where, block, CENTRAL_KEYS, errors)
    status, kind, zone, pt, en, es = expected_central(iso3, currency, i18n)
    if block.get("status") != status:
        errors.append(f"{where}.status: '{block.get('status')}' != esperado '{status}'")
    if block.get("kind") != kind:
        errors.append(f"{where}.kind: '{block.get('kind')}' != esperado '{kind}'")
    if block.get("zone") != zone:
        errors.append(f"{where}.zone: '{block.get('zone')}' != esperado '{zone}'")
    for lang, want in (("name", pt), ("name_en", en), ("name_es", es)):
        got = block.get(lang)
        if got != want:
            errors.append(f"{where}.{lang}: '{got}' != esperado '{want}'")
        if isinstance(got, str) and "(" in got:
            errors.append(f"{where}.{lang}: parênteses com sigla em {got!r}")
        # Exempt deliberado: o rótulo genérico sancionado ("Banco Central de
        # <país>" / união monetária) pode coincidir com o NOME PRÓPRIO da
        # autoridade monetária real (ex.: "Central Bank of Nigeria" é o nome
        # oficial e o rótulo genérico). Nesse caso o texto já foi igualado ao
        # esperado acima e a denylist não pode ser exigida; qualquer outro texto
        # no campo continua sendo varrido.
        if got != want:
            check_no_brand(f"{where}.{lang}", got, denylist, errors)
    if not block.get("note"):
        errors.append(f"{where}: note ausente")
    check_no_brand(f"{where}.note", block.get("note"), denylist, errors)


def validate_banks(data: dict, nations: dict[str, dict[str, str]],
                   i18n: dict[str, dict[str, str]], denylist: list[str],
                   errors: list[str]) -> dict:
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

    seen_names: dict[str, tuple[str, str]] = {}
    branch_names: dict[str, tuple[str, str]] = {}
    central_none: list[str] = []
    stats = {"present": 0, "none": 0, "no_data": 0}
    slot_stats: dict[str, dict[str, int]] = {
        "public": {"present": 0, "none": 0, "no_data": 0},
        "private": {"present": 0, "none": 0, "no_data": 0},
        "foreign": {"present": 0, "none": 0, "no_data": 0},
    }
    proposed_names = 0

    def register_name(where: str, name) -> None:
        if not isinstance(name, str) or not name.strip():
            return
        key = name.strip().lower()
        other = seen_names.get(key)
        if other is not None:
            errors.append(f"{where}: nome '{name}' duplicado (já em {other[1]})")
        else:
            seen_names[key] = (name, where)

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
        if "legal_name" in entry:
            errors.append(f"{where}: 'legal_name' não deve existir (nome real de banco)")

        cb = entry.get("central_bank")
        validate_central_bank(
            f"{where}.central_bank", cb if isinstance(cb, dict) else {},
            iso3, expected["currency"], i18n, denylist, errors,
        )
        if isinstance(cb, dict):
            check_sources(cb, f"{where}.central_bank", sources, errors)
            if cb.get("status") == "none":
                central_none.append(iso3)

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
            required = PUBLIC_KEYS if slot == "public" else (
                PRIVATE_KEYS if slot == "private" else FOREIGN_KEYS
            )
            check_keys(f"{where}.{slot}", block, required, errors)

            if status in ("none", "no_data"):
                # Ausência verificada precisa de proveniência: chave de fonte
                # existente ou a lacuna declarada em `source_note`.
                source_key = block.get("source_key")
                source_note = block.get("source_note")
                if not source_key:
                    if not isinstance(source_note, str) or len(source_note.strip()) < 30:
                        errors.append(
                            f"{where}.{slot}: status {status} sem 'source_key' nem "
                            "'source_note' (≥ 30 caracteres)"
                        )
                elif source_key not in sources:
                    errors.append(
                        f"{where}.{slot}: source_key '{source_key}' não existe em sources"
                    )

            if slot in ("public", "private"):
                name = block.get("name")
                if status == "present":
                    if not name:
                        errors.append(f"{where}.{slot}: status present sem name")
                    if not valid_year(block.get("year")):
                        errors.append(f"{where}.{slot}: status present sem year válido")
                    if not block.get("basis"):
                        errors.append(f"{where}.{slot}: status present sem basis")
                    check_sources(block, f"{where}.{slot}", sources, errors)
                elif status == "none":
                    if name is not None:
                        errors.append(f"{where}.{slot}: status none com name")
                    if not block.get("note"):
                        errors.append(f"{where}.{slot}: status none sem note explicando")
                else:
                    if name is not None:
                        errors.append(f"{where}.{slot}: status no_data com name")
                    if not block.get("note"):
                        errors.append(f"{where}.{slot}: status {status} sem note explicando")
                if name is not None and not str(block.get("name_basis", "")).startswith(FICTICIO_PREFIX):
                    errors.append(f"{where}.{slot}: name sem name_basis 'fictício:'")
                register_name(f"{where}.{slot}.name", name)
                if slot == "public" and status == "none":
                    proposed = block.get("proposed_name")
                    if not proposed:
                        errors.append(f"{where}.public: status none sem proposed_name")
                    else:
                        proposed_names += 1
                    if not str(block.get("proposed_name_basis", "")).startswith(FICTICIO_PREFIX):
                        errors.append(f"{where}.public: proposed_name sem basis 'fictício:'")
                elif slot == "public" and "proposed_name" in block:
                    errors.append(f"{where}.public: proposed_name só existe em status none")
                check_no_brand(f"{where}.{slot}.name", name, denylist, errors)
                check_no_brand(f"{where}.{slot}.note", block.get("note"), denylist, errors)
            if slot == "public":
                if block.get("owner") != "state":
                    errors.append(f"{where}.public: owner deve ser 'state'")
            if slot == "private":
                if block.get("owner") != "national":
                    errors.append(f"{where}.private: owner deve ser 'national'")
            if slot == "foreign":
                if block.get("owner") != "foreign":
                    errors.append(f"{where}.foreign: owner deve ser 'foreign'")
                branch = block.get("branch_candidate")
                if status == "present":
                    origin = block.get("origin_iso3")
                    if origin not in nations:
                        errors.append(f"{where}.foreign: origin_iso3 '{origin}' fora do jogo")
                    if not branch:
                        errors.append(f"{where}.foreign: sem branch_candidate")
                    if block.get("branch_status") != "derived_from_origin_largest":
                        errors.append(f"{where}.foreign: branch_status inválido")
                    basis = str(block.get("branch_name_basis", ""))
                    if not basis.startswith(FICTICIO_PREFIX):
                        errors.append(f"{where}.foreign: branch sem basis 'fictício:'")
                    elif origin and f'hash("{origin}:foreign")' not in basis:
                        errors.append(
                            f"{where}.foreign: branch_name_basis não cita a origem {origin}"
                        )
                    check_sources(block, f"{where}.foreign", sources, errors)
                else:
                    for field in ("branch_candidate", "branch_status", "branch_name_basis"):
                        if block.get(field) is not None:
                            errors.append(f"{where}.foreign: {field} deveria ser null em {status}")
                    if not block.get("note"):
                        errors.append(f"{where}.foreign: status {status} sem note explicando")
                if isinstance(branch, str) and branch.strip():
                    branch_names[branch.strip().lower()] = (
                        branch, f"{where}.foreign.branch_candidate"
                    )
                check_no_brand(f"{where}.foreign.branch_candidate", branch, denylist, errors)
                check_no_brand(f"{where}.foreign.origin_name", block.get("origin_name"), denylist, errors)
                check_no_brand(f"{where}.foreign.note", block.get("note"), denylist, errors)

    # A filial pode repetir entre países do mesmo grupo de origem, mas não pode
    # colidir com nome de banco público/privado (em nenhuma ordem de leitura).
    for key, (name, where) in seen_names.items():
        hit = branch_names.get(key)
        if hit is not None:
            errors.append(f"{where}: nome '{name}' colide com a filial {hit[1]} ('{hit[0]}')")

    if central_none != ["PAN"]:
        errors.append(
            "banks: só PAN pode não ter banco central próprio; achei "
            f"{central_none or 'nenhuma'}"
        )

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
        fictional = sum(
            1
            for entry in entries.values()
            if isinstance(entry, dict)
            for name in (
                (entry.get("public") or {}).get("name"),
                (entry.get("private") or {}).get("name"),
                (entry.get("foreign") or {}).get("branch_candidate"),
            )
            if name
        )
        named_currencies = {
            entry.get("currency")
            for entry in entries.values()
            if isinstance(entry, dict)
            and isinstance(entry.get("central_bank"), dict)
            and entry["central_bank"].get("name")
        }
        if coverage.get("fictional_names") != fictional:
            errors.append(
                f"banks.coverage.fictional_names: declarado {coverage.get('fictional_names')} "
                f"!= contado {fictional}"
            )
        if coverage.get("central_bank_named") != len(named_currencies):
            errors.append(
                f"banks.coverage.central_bank_named: declarado "
                f"{coverage.get('central_bank_named')} != contado {len(named_currencies)}"
            )
        if coverage.get("proposed_public_names") != proposed_names:
            errors.append(
                f"banks.coverage.proposed_public_names: declarado "
                f"{coverage.get('proposed_public_names')} != contado {proposed_names}"
            )
    else:
        errors.append("banks.json: 'coverage' ausente")

    for key, source in sources.items():
        if not isinstance(source, dict):
            errors.append(f"banks.sources.{key}: não é objeto")
            continue
        url = source.get("url")
        url_note = source.get("url_note")
        if not url:
            # Fonte sem URL só é aceitável com a lacuna declarada por escrito.
            if not isinstance(url_note, str) or len(url_note.strip()) < 30:
                errors.append(
                    f"banks.sources.{key}: sem 'url' e sem 'url_note' (≥ 30 caracteres) "
                    "explicando a proveniência"
                )
        check_no_brand(f"banks.sources.{key}.title", source.get("title"), denylist, errors)
        check_no_brand(f"banks.sources.{key}.note", source.get("note"), denylist, errors)

    stats["currencies"] = len({e["currency"] for e in entries.values() if isinstance(e, dict)})
    stats["nations"] = len(entries)
    stats["by_slot"] = slot_stats
    stats["fictional"] = coverage.get("fictional_names") if isinstance(coverage, dict) else None
    stats["central_bank_named"] = (
        coverage.get("central_bank_named") if isinstance(coverage, dict) else None
    )
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
            # Línguas usadas pelos termos de `informal_services.json`. `ru` e `hi`
            # já constavam do dado e faltavam nesta lista (correção do validador).
            if lang not in {*LANGS, "ht", "id", "vi", "zh", "th", "tr", "it", "fr",
                            "ko", "ja", "sw", "sn", "rw", "ar", "bn", "ur", "ne",
                            "km", "lo", "tet", "en", "my", "si", "bs", "mk", "sq",
                            "he", "ka", "hy", "kk", "ky", "uz", "ro", "sr", "uk",
                            "pl", "ms", "pt", "ru", "hi"}:
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


def validate_i18n_banks(denylist: list[str], errors: list[str]) -> None:
    """Os valores `banks.*` do i18n são rótulos genéricos: nenhuma marca real."""
    if os.path.exists(I18N_CSV):
        with open(I18N_CSV, encoding="utf-8-sig", newline="") as fh:
            for row in csv.DictReader(fh):
                key = (row.get("key") or "").strip()
                if not key.startswith("banks."):
                    continue
                for column in ("pt_BR", "en", "es"):
                    value = (row.get(column) or "").strip()
                    hit = find_brand(value, denylist)
                    if hit:
                        errors.append(f"i18n.csv[{key}].{column}: marca real ('{hit}') em {value!r}")
    if os.path.exists(I18N_JSON):
        with open(I18N_JSON, encoding="utf-8-sig") as fh:
            data = json.load(fh)
        if isinstance(data, dict):
            for lang, values in data.items():
                if not isinstance(values, dict):
                    continue
                for key, value in values.items():
                    if not str(key).startswith("banks."):
                        continue
                    hit = find_brand(value, denylist)
                    if hit:
                        errors.append(f"i18n.json[{lang}][{key}]: marca real ('{hit}') em {value!r}")


def validate_names_doc(denylist: list[str], errors: list[str]) -> None:
    """O documento da Parte C4 também não pode exibir nome real de banco."""
    if not os.path.exists(NAMES_DOC):
        return
    with open(NAMES_DOC, encoding="utf-8-sig") as fh:
        text = fh.read()
    norm = normalize(text)
    hits: list[str] = []
    for entry in denylist:
        if entry not in hits and _pattern(entry).search(norm):
            hits.append(entry)
    if hits:
        shown = ", ".join(hits[:DENYLIST_TEXT_LIMIT])
        more = "" if len(hits) <= DENYLIST_TEXT_LIMIT else f" (+{len(hits) - DENYLIST_TEXT_LIMIT})"
        errors.append(
            f"{os.path.relpath(NAMES_DOC, ROOT)}: marcas reais na denylist ({shown}{more})"
        )


def main() -> int:
    errors: list[str] = []
    nations = load_nations(errors)
    i18n = load_i18n_nations(errors)
    denylist = load_denylist(errors)

    banks = read_json_strict(BANKS_JSON, errors)
    informal = read_json_strict(INFORMAL_JSON, errors)

    banks_stats: dict = {}
    informal_stats: dict = {}
    if banks is not None:
        banks_stats = validate_banks(banks, nations, i18n, denylist, errors)
    if informal is not None:
        informal_stats = validate_informal(informal, nations, errors)
    validate_i18n_banks(denylist, errors)
    validate_names_doc(denylist, errors)

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
        "  bancos: {fictional} nomes fictícios, {central} bancos centrais nomeados".format(
            fictional=banks_stats.get("fictional"),
            central=banks_stats.get("central_bank_named"),
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
