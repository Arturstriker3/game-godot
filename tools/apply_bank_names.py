"""Aplica os nomes fictícios de banco em `data/definitions/economy/banks.json`.

Por decisão do dono (direitos/marcas), o dado de bancos não guarda nome real de
banco: guarda só o ESTRUTURAL com fonte (existe banco estatal? qual o país de
origem do capital estrangeiro? qual o ano do ranking?) e nomes FICTÍCIOS gerados
pelo `BusinessNameGenerator` (setor `finance`), como as empresas da F9. Bancos
centrais usam rótulo institucional genérico por país ou por união monetária.

Entradas:
  * `data/definitions/economy/banks.json` (o dado a reescrever);
  * `tools/data/bank_shards/generated_names.json` (saída de `gen_bank_names.gd`);
  * `data/definitions/i18n/i18n.csv` (nomes de país em pt-BR/en/es);
  * `data/definitions/world/out/nations.csv` (nome e moeda de referência).

O título de toda fonte é substituído por um rótulo genérico (o `url` continua
sendo a referência), exceto as publicações/datasets em `KEEP_TITLES`.

Saída: o próprio `banks.json`, UTF-8 sem BOM, indent 2, chaves ordenadas, com
newline final. A execução é idempotente: rodar de novo sobre a saída não muda
nada.

Uso:
    python tools/apply_bank_names.py
"""

from __future__ import annotations

import csv
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BANKS_JSON = os.path.join(ROOT, "data", "definitions", "economy", "banks.json")
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "world", "out", "nations.csv")
I18N_CSV = os.path.join(ROOT, "data", "definitions", "i18n", "i18n.csv")
GENERATED_JSON = os.path.join(ROOT, "tools", "data", "bank_shards", "generated_names.json")

GENERATED_AT = "2026-11-18"
EXPECTED_NATIONS = 179

POLICY = (
    "Nomes de bancos são FICTÍCIOS, gerados pelo BusinessNameGenerator (setor "
    "finance) com semente determinística por nação e papel; o dado real é apenas "
    "estrutural (existe banco estatal? qual o país de origem do capital "
    "estrangeiro?) e vem com fonte. Bancos centrais usam rótulo institucional "
    "genérico, por país ou por união monetária."
)
NOTE = (
    "Bancos por nação (Parte C4). public/private/foreign guardam status estrutural "
    "('present' = existe, 'none' = verificado que não há, 'no_data' = não "
    "verificável) e a origem do capital; nomes são fictícios "
    "(name_basis/branch_name_basis). central_bank é por moeda: 'Banco Central de "
    "<país>' ou o rótulo da união monetária."
)

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

# Publicações, rankings e datasets cujo título é a própria referência e não cita
# banco real: mantêm o título original.
KEEP_TITLES = {
    "banker2024",
    "banker2025",
    "europe_a_banker2024",
    "europe_b_banker2024",
    "me_oceania_banker2024",
    "gfdd",
    "cloessens_vanhoren",
    "bis_cbanks",
    "bis_cbdir",
    "europe_a_bis_cbdir",
    "europe_b_bis_cbdir",
    "annual_reports_2023",
}

TITLE_ANNUAL = [
    "annual report", "financial report", "rapport", "relatório", "relatorio",
    "informe anual", "report",
]
TITLE_ANNUAL_TEXT = "Relatório anual do banco citado (site institucional)"
TITLE_LIST_KEYWORDS = ["wikipedia", "list of banks"]
TITLE_LIST_TEXT = "Lista de bancos do país (verbete enciclopédico)"
TITLE_GENERIC_TEXT = "Fonte citada para o dado estrutural do setor bancário"


class ApplyError(RuntimeError):
    pass


def read_json(path: str):
    with open(path, encoding="utf-8-sig") as fh:
        return json.load(fh)


def write_json(path: str, data) -> None:
    text = json.dumps(data, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def load_nations() -> dict[str, dict[str, str]]:
    with open(NATIONS_CSV, encoding="utf-8-sig", newline="") as fh:
        rows = list(csv.DictReader(fh))
    out: dict[str, dict[str, str]] = {}
    for row in rows:
        iso3 = (row.get("id") or "").strip()
        if iso3:
            out[iso3] = {
                "name": (row.get("name") or "").strip(),
                "currency": (row.get("currency") or "").strip(),
            }
    return out


def load_i18n() -> dict[str, dict[str, str]]:
    with open(I18N_CSV, encoding="utf-8-sig", newline="") as fh:
        rows = list(csv.DictReader(fh))
    out: dict[str, dict[str, str]] = {}
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


def seed_basis(iso3: str, slot: str) -> str:
    return (
        "fictício: BusinessNameGenerator/finance; semente "
        f'absi(hash("{iso3}:{slot}")) % 100000'
    )


def generated_name(generated: dict, iso3: str, slot: str) -> str:
    block = (generated.get("nations") or {}).get(iso3) or {}
    name = block.get(slot)
    if not isinstance(name, str) or not name.strip():
        raise ApplyError(f"generated_names.json: sem nome para {iso3}:{slot}")
    return name.strip()


def origin_name(generated: dict, iso3: str, origin: str) -> str:
    origins = generated.get("origins") or {}
    name = origins.get(origin)
    if not isinstance(name, str) or not name.strip():
        name = ((generated.get("nations") or {}).get(iso3) or {}).get("foreign")
    if not isinstance(name, str) or not name.strip():
        raise ApplyError(f"generated_names.json: sem nome para origem {origin} ({iso3})")
    return name.strip()


def build_public(old: dict, iso3: str, generated: dict) -> dict:
    status = old.get("status")
    basis = seed_basis(iso3, "public")
    block: dict = {
        "status": status,
        "owner": old.get("owner") or "state",
        "source_key": old.get("source_key"),
    }
    if status == "present":
        name = generated_name(generated, iso3, "public")
        year = old.get("year")
        block.update({
            "name": name,
            "name_basis": basis,
            "basis": old.get("basis"),
            "year": year,
            "note": (
                "Existe instituição bancária estatal; era a maior de capital "
                f"estatal por ativos no ranking de {year}. Nome fictício por "
                "política de marcas."
            ),
        })
    elif status == "none":
        block.update({
            "name": None,
            "name_basis": None,
            "basis": None,
            "year": None,
            "proposed_name": generated_name(generated, iso3, "public"),
            "proposed_name_basis": basis,
            "note": "Verificado: não há banco estatal no ranking consultado.",
        })
    elif status == "no_data":
        block.update({
            "name": None,
            "name_basis": None,
            "basis": None,
            "year": None,
            "note": (
                "Não verificável: ranking de ativos não publicado ou titularidade "
                "estatal não confirmada."
            ),
        })
    else:
        raise ApplyError(f"banks.{iso3}.public: status '{status}' inválido")
    return block


def build_private(old: dict, iso3: str, generated: dict) -> dict:
    status = old.get("status")
    block: dict = {
        "status": status,
        "owner": old.get("owner") or "national",
        "source_key": old.get("source_key"),
    }
    if status == "present":
        year = old.get("year")
        block.update({
            "name": generated_name(generated, iso3, "private"),
            "name_basis": seed_basis(iso3, "private"),
            "basis": old.get("basis"),
            "year": year,
            "note": (
                "Maior banco privado doméstico por ativos no ranking de "
                f"{year}. Nome fictício por política de marcas."
            ),
        })
    elif status == "none":
        block.update({
            "name": None,
            "name_basis": None,
            "basis": None,
            "year": None,
            "note": "Verificado: não há banco privado doméstico no ranking consultado.",
        })
    elif status == "no_data":
        block.update({
            "name": None,
            "name_basis": None,
            "basis": None,
            "year": None,
            "note": (
                "Não verificável: setor dominado por filiais estrangeiras ou "
                "titularidade doméstica não confirmada."
            ),
        })
    else:
        raise ApplyError(f"banks.{iso3}.private: status '{status}' inválido")
    return block


def build_foreign(old: dict, iso3: str, generated: dict, i18n: dict) -> dict:
    status = old.get("status")
    origin = old.get("origin_iso3")
    block: dict = {
        "status": status,
        "owner": "foreign",
        "origin_iso3": origin,
        "origin_name": old.get("origin_name"),
        "origin_basis": old.get("origin_basis"),
        "branch_candidate": None,
        "branch_status": None,
        "branch_name_basis": None,
        "source_key": old.get("source_key"),
    }
    if status == "present":
        if not origin:
            raise ApplyError(f"banks.{iso3}.foreign: present sem origin_iso3")
        country = i18n.get(origin)
        if not country or not country.get("pt"):
            raise ApplyError(f"i18n.csv: sem nation.{origin} para a origem de {iso3}")
        block.update({
            "branch_candidate": origin_name(generated, iso3, origin),
            "branch_status": "derived_from_origin_largest",
            "branch_name_basis": seed_basis(origin, "foreign"),
            "note": (
                "Origem predominante do capital estrangeiro: "
                f"{country['pt']} (por ativos). A filial é derivada do maior banco "
                "do país de origem, com nome fictício."
            ),
        })
    elif status == "none":
        block["note"] = "Verificado: presença estrangeira irrelevante no sistema."
    elif status == "no_data":
        block["note"] = "Não verificável: sem país de origem predominante na fonte consultada."
    else:
        raise ApplyError(f"banks.{iso3}.foreign: status '{status}' inválido")
    return block


def build_central_bank(old: dict, iso3: str, currency: str, i18n: dict) -> dict:
    base = {"source_key": old.get("source_key")}
    if iso3 == "PAN":
        base.update({
            "name": None,
            "name_en": None,
            "name_es": None,
            "kind": "none",
            "zone": None,
            "status": "none",
            "note": (
                "Não há banco central próprio: a economia usa moeda estrangeira e "
                "a supervisão bancária é de autoridade nacional."
            ),
        })
        return base
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
        base.update({
            "name": pt,
            "name_en": en,
            "name_es": es,
            "kind": "monetary_union",
            "zone": zone,
            "status": "present",
            "note": f"União monetária ({zone}): o banco central é comum às nações da zona.",
        })
        return base
    country = i18n.get(iso3)
    if not country or not country.get("pt") or not country.get("en") or not country.get("es"):
        raise ApplyError(f"i18n.csv: sem nation.{iso3} para o banco central")
    base.update({
        "name": "Banco Central de " + country["pt"],
        "name_en": "Central Bank of " + country["en"],
        "name_es": "Banco Central de " + country["es"],
        "kind": "national",
        "zone": None,
        "status": "present",
        "note": f"Banco central nacional de {country['pt']}; nome institucional genérico.",
    })
    return base


def rewrite_source(key: str, source: dict) -> dict:
    """Substitui o título por rótulo genérico, salvo publicações em KEEP_TITLES."""
    out = dict(source)
    if key in KEEP_TITLES:
        return out
    title = source.get("title") or ""
    if title in (TITLE_ANNUAL_TEXT, TITLE_LIST_TEXT, TITLE_GENERIC_TEXT):
        # Já é rótulo genérico: mantém (idempotência).
        return out
    low = title.lower()
    if any(word in low for word in TITLE_ANNUAL):
        out["title"] = TITLE_ANNUAL_TEXT
    elif any(word in low for word in TITLE_LIST_KEYWORDS):
        out["title"] = TITLE_LIST_TEXT
    else:
        out["title"] = TITLE_GENERIC_TEXT
    return out


def count_status(blocks: dict[str, dict]) -> dict[str, int]:
    counts = {"present": 0, "none": 0, "no_data": 0}
    for block in blocks.values():
        status = block.get("status")
        if status in counts:
            counts[status] += 1
    return counts


def main() -> int:
    try:
        data = read_json(BANKS_JSON)
        nations = load_nations()
        i18n = load_i18n()
        generated = read_json(GENERATED_JSON)
    except (OSError, json.JSONDecodeError, ApplyError) as exc:
        print(f"FALHOU — {exc}")
        return 1

    entries = data.get("nations") or {}
    if len(entries) != EXPECTED_NATIONS:
        print(f"FALHOU — esperava {EXPECTED_NATIONS} nações, achei {len(entries)}")
        return 1

    try:
        new_nations: dict[str, dict] = {}
        for iso3 in sorted(entries):
            old = entries[iso3]
            ref = nations.get(iso3)
            if ref is None:
                raise ApplyError(f"banks.{iso3}: nação fora de nations.csv")
            if old.get("name") != ref["name"]:
                raise ApplyError(
                    f"banks.{iso3}: name '{old.get('name')}' != nations.csv '{ref['name']}'"
                )
            if old.get("currency") != ref["currency"]:
                raise ApplyError(
                    f"banks.{iso3}: currency '{old.get('currency')}' != nations.csv "
                    f"'{ref['currency']}'"
                )
            new_nations[iso3] = {
                "name": ref["name"],
                "currency": ref["currency"],
                "central_bank": build_central_bank(
                    old.get("central_bank") or {}, iso3, ref["currency"], i18n
                ),
                "public": build_public(old.get("public") or {}, iso3, generated),
                "private": build_private(old.get("private") or {}, iso3, generated),
                "foreign": build_foreign(old.get("foreign") or {}, iso3, generated, i18n),
            }
    except ApplyError as exc:
        print(f"FALHOU — {exc}")
        return 1

    sources = {
        key: rewrite_source(key, source)
        for key, source in (data.get("sources") or {}).items()
    }

    public_stats = count_status({k: v["public"] for k, v in new_nations.items()})
    private_stats = count_status({k: v["private"] for k, v in new_nations.items()})
    foreign_stats = count_status({k: v["foreign"] for k, v in new_nations.items()})
    proposed = sum(1 for v in new_nations.values() if v["public"].get("proposed_name"))
    fictional = sum(
        1
        for v in new_nations.values()
        for name in (
            v["public"].get("name"),
            v["private"].get("name"),
            v["foreign"].get("branch_candidate"),
        )
        if name
    )
    central_named = len({
        v["currency"]
        for v in new_nations.values()
        if (v["central_bank"].get("name") or "").strip()
    })

    old_coverage = data.get("coverage") or {}
    coverage = {
        "nations_total": len(new_nations),
        "nations_missing": sorted(set(nations) - set(new_nations)),
        "currencies": len({v["currency"] for v in new_nations.values()}),
        "proposed_public_names": proposed,
        "fictional_names": fictional,
        "central_bank_named": central_named,
        "public": public_stats,
        "private": private_stats,
        "foreign": foreign_stats,
    }
    if "currencies" in old_coverage and old_coverage["currencies"] != coverage["currencies"]:
        print(
            "FALHOU — coverage.currencies anterior "
            f"{old_coverage['currencies']} != contado {coverage['currencies']}"
        )
        return 1

    out = {
        "version": 2,
        "generated_at": GENERATED_AT,
        "owners_note": data.get("owners_note"),
        "policy": POLICY,
        "note": NOTE,
        "coverage": coverage,
        "nations": new_nations,
        "sources": sources,
    }

    try:
        write_json(BANKS_JSON, out)
    except OSError as exc:
        print(f"FALHOU — não consegui escrever banks.json ({exc})")
        return 1

    print(
        "aplicado: nações={n} moedas={c} | public {p} | private {pv} | foreign {f}".format(
            n=len(new_nations), c=coverage["currencies"],
            p=_fmt(public_stats), pv=_fmt(private_stats), f=_fmt(foreign_stats),
        )
    )
    print(
        f"coverage: proposed_public_names={proposed} "
        f"fictional_names={fictional} central_bank_named={central_named}"
    )
    return 0


def _fmt(counts: dict[str, int]) -> str:
    return "{present}/{none}/{no_data}".format(
        present=counts["present"], none=counts["none"], no_data=counts["no_data"]
    )


if __name__ == "__main__":
    sys.exit(main())
