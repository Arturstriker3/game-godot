#!/usr/bin/env python3
"""Constrói os dados de rendimento e estoque de culturas do jogo (Parte G, G1).

Gera dois arquivos determinísticos em `data/definitions/agriculture/`:

- `crop_yields.json`  — rendimento por nação/cultura derivado do FAOSTAT QCL
  (área colhida 5312 e produção 5510 do último ano <= 2024 com área > 0).
- `crop_stocks.json`  — estoque inicial de fim de safra para 2027-01-01, do
  USDA PSD (`Ending Stocks`, atributo 176) quando existe; caso contrário a
  REGRA DECLARADA de 60 dias de consumo.

Nada aqui inventa número: onde a fonte não tem o dado, o campo fica `null`,
`data_status` fica `no_data` e `reason`/`note` explicam em pt-BR.

Uso:
    python tools/build_agri_yields.py            # usa o cache em tools/_tmp_agri_raw
    python tools/build_agri_yields.py --fetch    # baixa as fontes que faltarem

Reprodutibilidade: mesma entrada => mesmos bytes (nações ordenadas por id,
culturas na ordem fixa CROPS, JSON UTF-8 sem BOM, tab, LF).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from agri_common import nations, write_json  # noqa: E402

RAW_DIR = ROOT / "tools" / "_tmp_agri_raw"
OUT_DIR = ROOT / "data" / "definitions" / "agriculture"
NATIONS_SOURCE = ROOT / "data" / "definitions" / "world" / "nations_source.csv"
GOODS_PATH = ROOT / "data" / "definitions" / "economy" / "goods.json"

RETRIEVED = "2026-11-18"
MAX_YEAR = 2024
RULE_DAYS = 60

CROPS = ("wheat", "rice", "maize", "potato", "soy")

# Culturas que existem no FAOSTAT mas NÃO são bens do jogo. Vão só em
# `non_game_reference`, para conferência; não alimentam o núcleo.
NON_GAME_ITEMS = {"cassava": "125"}  # Cassava, fresh
NON_GAME_REFERENCE = (
    ("NGA", "cassava", "mandioca"),
)

# ---------------------------------------------------------------------------
# Fontes (cache bruto + URL de reprodução)
# ---------------------------------------------------------------------------

SOURCES = {
    "FAO_QCL.zip": {
        "title": "FAOSTAT - Crops and livestock products (QCL)",
        "publisher": "FAO",
        "url": "https://bulks-faostat.fao.org/production/Production_Crops_Livestock_E_All_Data_(Normalized).zip",
        "year": MAX_YEAR,
        "license": "CC BY 4.0",
    },
    "psd_grains.zip": {
        "title": "USDA PSD - Grains and pulses",
        "publisher": "USDA Foreign Agricultural Service",
        "url": "https://apps.fas.usda.gov/psdonline/downloads/psd_grains_pulses_csv.zip",
        "year": MAX_YEAR,
        "license": "Public domain (USDA FAS)",
    },
    "psd_oilseeds.zip": {
        "title": "USDA PSD - Oilseeds",
        "publisher": "USDA Foreign Agricultural Service",
        "url": "https://apps.fas.usda.gov/psdonline/downloads/psd_oilseeds_csv.zip",
        "year": MAX_YEAR,
        "license": "Public domain (USDA FAS)",
    },
}

FAO_CSV_IN_QCL = "Production_Crops_Livestock_E_All_Data_(Normalized).csv"

# Elementos do FAOSTAT QCL usados (confirmados em E_Elements.csv).
FAO_ELEMENT_AREA = "5312"   # Area harvested (ha)
FAO_ELEMENT_PRODUCTION = "5510"  # Production (t)

# Itens do FAOSTAT QCL usados (confirmados em E_ItemCodes.csv).
FAO_ITEMS = {
    "wheat": ("15", "Wheat"),
    "rice": ("27", "Rice"),
    "maize": ("56", "Maize (corn)"),
    "potato": ("116", "Potatoes"),
    "soy": ("236", "Soya beans"),
}

# Códigos do USDA PSD (confirmados lendo os CSV de grãos e oleaginosas).
PSD_COMMODITY_CODES = {
    "wheat": "0410000",   # Wheat
    "rice": "0422110",    # Rice, Milled
    "maize": "0440000",   # Corn
    "soy": "2222000",     # Oilseed, Soybean
}
PSD_ATTR_ENDING = "176"       # Ending Stocks
PSD_ATTR_CONSUMPTION = "125"  # Domestic Consumption
PSD_UNIT_MT = "(1000 MT)"

# ---------------------------------------------------------------------------
# Mapa explícito FAOSTAT: id do jogo -> nome da área no FAOSTAT
# ---------------------------------------------------------------------------
#
# A base é o nome inglês de `nations_source.csv`, casado LITERALMENTE com a
# lista de áreas do FAOSTAT (E_AreaCodes.csv). Só os nomes que divergem estão
# listados aqui — nenhum alias silencioso: o que não está mapeado é registrado
# em `_meta.unmatched_nations`.

FAOSTAT_OVERRIDES = {
    "BHS": "Bahamas",
    "BOL": "Bolivia (Plurinational State of)",
    "BRN": "Brunei Darussalam",
    "CHN": "China, mainland",  # 41; 'China' (351) é agregado e não é usado
    "CIV": "Côte d'Ivoire",
    "COG": "Congo",
    "CZE": "Czechia",
    "GBR": "United Kingdom of Great Britain and Northern Ireland",
    "GMB": "Gambia",
    "GNB": "Guinea-Bissau",
    "IRN": "Iran (Islamic Republic of)",
    "KOR": "Republic of Korea",
    "LAO": "Lao People's Democratic Republic",
    "MDA": "Republic of Moldova",
    "NED": "Netherlands (Kingdom of the)",
    "PRK": "Democratic People's Republic of Korea",
    "RUS": "Russian Federation",
    "SSD": "South Sudan",
    "SYR": "Syrian Arab Republic",
    "TLS": "Timor-Leste",
    "TUR": "Türkiye",
    "TZA": "United Republic of Tanzania",
    "VEN": "Venezuela (Bolivarian Republic of)",
    "VNM": "Viet Nam",
}

# Países cujo dado do FAOSTAT não é usado, com o motivo declarado.
FAOSTAT_EXCLUDED = {
    "SAH": "FAOSTAT QCL não publica área para o Saara Ocidental",
    "SOL": "FAOSTAT não tem entrada para Somalilândia (o dado de 'Somalia' é outro país)",
    "TWN": "FAOSTAT só publica Taiwan como agregado 'China; Taiwan Province of', excluído por regra",
    "XKX": "FAOSTAT não tem entrada para Kosovo",
}

# ---------------------------------------------------------------------------
# Mapa explícito USDA PSD: id do jogo -> Country_Name do PSD
# ---------------------------------------------------------------------------
#
# Só entram entidades que existem como país no PSD. Agregados do PSD
# ('World', 'European Union', 'Belgium-Luxembourg', 'Former Yugoslavia', …)
# nunca são usados.

PSD_NAMES = {
    "AFG": "Afghanistan",
    "ALB": "Albania",
    "DZA": "Algeria",
    "AGO": "Angola",
    "ARG": "Argentina",
    "ARM": "Armenia",
    "AUS": "Australia",
    "AUT": "Austria",
    "AZE": "Azerbaijan",
    "BHR": "Bahrain",
    "BGD": "Bangladesh",
    "BRB": "Barbados",
    "BLR": "Belarus",
    "BEN": "Benin",
    "BTN": "Bhutan",
    "BOL": "Bolivia",
    "BIH": "Bosnia and Herzegovina",
    "BWA": "Botswana",
    "BRA": "Brazil",
    "BRN": "Brunei",
    "BGR": "Bulgaria",
    "BFA": "Burkina Faso",
    "BDI": "Burundi",
    "KHM": "Cambodia",
    "CMR": "Cameroon",
    "CAN": "Canada",
    "CAF": "Central African Republic",
    "TCD": "Chad",
    "CHL": "Chile",
    "CHN": "China",
    "COL": "Colombia",
    "COG": "Congo (Brazzaville)",
    "COD": "Congo (Kinshasa)",
    "CRI": "Costa Rica",
    "CIV": "Cote d'Ivoire",
    "HRV": "Croatia",
    "CUB": "Cuba",
    "CYP": "Cyprus",
    "CZE": "Czech Republic",
    "DNK": "Denmark",
    "DJI": "Djibouti",
    "DOM": "Dominican Republic",
    "ECU": "Ecuador",
    "EGY": "Egypt",
    "SLV": "El Salvador",
    "ERI": "Eritrea",
    "EST": "Estonia",
    "SWZ": "Eswatini",
    "ETH": "Ethiopia",
    "FJI": "Fiji",
    "FIN": "Finland",
    "FRA": "France",
    "GAB": "Gabon",
    "GMB": "Gambia, The",
    "GEO": "Georgia",
    "DEU": "Germany",
    "GHA": "Ghana",
    "GRC": "Greece",
    "GTM": "Guatemala",
    "GIN": "Guinea",
    "GNB": "Guinea-Bissau",
    "GUY": "Guyana",
    "HTI": "Haiti",
    "HND": "Honduras",
    "HUN": "Hungary",
    "ISL": "Iceland",
    "IND": "India",
    "IDN": "Indonesia",
    "IRN": "Iran",
    "IRQ": "Iraq",
    "IRL": "Ireland",
    "ISR": "Israel",
    "ITA": "Italy",
    "JAM": "Jamaica",
    "JPN": "Japan",
    "JOR": "Jordan",
    "KAZ": "Kazakhstan",
    "KEN": "Kenya",
    "PRK": "Korea, North",
    "KOR": "Korea, South",
    "KWT": "Kuwait",
    "KGZ": "Kyrgyzstan",
    "LAO": "Laos",
    "LVA": "Latvia",
    "LBN": "Lebanon",
    "LSO": "Lesotho",
    "LBR": "Liberia",
    "LBY": "Libya",
    "LTU": "Lithuania",
    "MDG": "Madagascar",
    "MWI": "Malawi",
    "MYS": "Malaysia",
    "MLI": "Mali",
    "MLT": "Malta",
    "MRT": "Mauritania",
    "MUS": "Mauritius",
    "MEX": "Mexico",
    "MDA": "Moldova",
    "MNG": "Mongolia",
    "MAR": "Morocco",
    "MOZ": "Mozambique",
    "MMR": "Burma",  # o PSD mantém o nome antigo para Mianmar
    "NAM": "Namibia",
    "NPL": "Nepal",
    "NED": "Netherlands",
    "NZL": "New Zealand",
    "NIC": "Nicaragua",
    "NER": "Niger",
    "NGA": "Nigeria",
    "MKD": "North Macedonia",
    "NOR": "Norway",
    "OMN": "Oman",
    "PAK": "Pakistan",
    "PAN": "Panama",
    "PNG": "Papua New Guinea",
    "PRY": "Paraguay",
    "PER": "Peru",
    "PHL": "Philippines",
    "POL": "Poland",
    "PRT": "Portugal",
    "QAT": "Qatar",
    "ROU": "Romania",
    "RUS": "Russia",
    "RWA": "Rwanda",
    "SAU": "Saudi Arabia",
    "SEN": "Senegal",
    "SRB": "Serbia",
    "SLE": "Sierra Leone",
    "SGP": "Singapore",
    "SVK": "Slovakia",
    "SVN": "Slovenia",
    "SLB": "Solomon Islands",
    "SOM": "Somalia",
    "ZAF": "South Africa",
    "SSD": "South Sudan",
    "ESP": "Spain",
    "LKA": "Sri Lanka",
    "SDN": "Sudan",
    "SUR": "Suriname",
    "SWE": "Sweden",
    "CHE": "Switzerland",
    "SYR": "Syria",
    "TWN": "Taiwan",
    "TJK": "Tajikistan",
    "TZA": "Tanzania",
    "THA": "Thailand",
    "TGO": "Togo",
    "TTO": "Trinidad and Tobago",
    "TUN": "Tunisia",
    "TUR": "Turkey",
    "TKM": "Turkmenistan",
    "UGA": "Uganda",
    "UKR": "Ukraine",
    "ARE": "United Arab Emirates",
    "GBR": "United Kingdom",
    "USA": "United States",
    "URY": "Uruguay",
    "UZB": "Uzbekistan",
    "VEN": "Venezuela",
    "VNM": "Vietnam",
    "YEM": "Yemen",
    "ZMB": "Zambia",
    "ZWE": "Zimbabwe",
}


# ---------------------------------------------------------------------------
# Cache bruto
# ---------------------------------------------------------------------------


def ensure_sources(fetch: bool) -> None:
    """Baixa as fontes que faltam no cache (só com --fetch)."""
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    missing = [name for name in SOURCES if not (RAW_DIR / name).exists()]
    if not missing:
        return
    if not fetch:
        raise SystemExit(
            "cache incompleto em tools/_tmp_agri_raw: "
            + ", ".join(missing)
            + " — rode com --fetch para baixar"
        )

    import urllib.request

    for name in missing:
        url = SOURCES[name]["url"]
        print(f"[fetch] {name} <- {url}")
        with urllib.request.urlopen(url, timeout=600) as response:
            (RAW_DIR / name).write_bytes(response.read())


def open_zip_csv(zip_name: str, member: str):
    """Abre um CSV dentro de um zip como text stream UTF-8."""
    zf = zipfile.ZipFile(RAW_DIR / zip_name)
    return io.TextIOWrapper(zf.open(member), encoding="utf-8", newline="")


# ---------------------------------------------------------------------------
# Mapa FAOSTAT
# ---------------------------------------------------------------------------


def nation_english_names(wanted: set[str]) -> dict[str, str]:
    """{id: name_en} das nações do jogo, lido de nations_source.csv."""
    out: dict[str, str] = {}
    with open(NATIONS_SOURCE, encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            iso3 = row.get("iso3")
            if iso3 in wanted:
                out[iso3] = row["name_en"]
    return out


def scan_faostat() -> tuple[set[str], dict[str, dict[str, dict[int, dict]]], dict[str, str]]:
    """Uma passada no QCL.

    Devolve (áreas realmente presentes no CSV de dados, {area: {crop: {year: {...}}}},
    {código do item: nome do item no CSV de dados}).
    A lista de áreas vem do PRÓPRIO CSV de dados: o `E_AreaCodes.csv` grafa
    'China; mainland' com ponto e vírgula, enquanto as linhas de dado usam
    'China, mainland' — indexar pelo arquivo de códigos causa falha silenciosa.
    """
    item_to_crop = {code: crop for crop, (code, _) in FAO_ITEMS.items()}
    item_to_crop.update({code: crop for crop, code in NON_GAME_ITEMS.items()})
    elements = {FAO_ELEMENT_AREA, FAO_ELEMENT_PRODUCTION}

    areas: set[str] = set()
    item_names: dict[str, str] = {}
    data: dict[str, dict[str, dict[int, dict]]] = {}
    with open_zip_csv("FAO_QCL.zip", FAO_CSV_IN_QCL) as fh:
        reader = csv.reader(fh)
        next(reader)  # cabeçalho
        for row in reader:
            # colunas: Area Code, M49, Area, Item Code, CPC, Item,
            #          Element Code, Element, Year Code, Year, Unit, Value, Flag
            area = row[2]
            areas.add(area)
            if row[6] not in elements or row[3] not in item_to_crop:
                continue
            item_names.setdefault(row[3], row[5])
            try:
                year = int(row[9])
            except ValueError:
                continue
            if year > MAX_YEAR:
                continue
            raw = row[11].strip()
            if raw == "":
                continue
            try:
                value = float(raw)
            except ValueError:
                continue
            if value < 0:
                continue
            slot = data.setdefault(area, {}).setdefault(item_to_crop[row[3]], {})
            bucket = slot.setdefault(year, {"area": None, "production": None, "flag": None})
            if row[6] == FAO_ELEMENT_AREA:
                bucket["area"] = value
            else:
                bucket["production"] = value
                bucket["flag"] = row[12].strip() or None
    return areas, data, item_names


def build_faostat_map(
    game: dict[str, str], areas: set[str]
) -> tuple[dict[str, str], list[dict], list[dict]]:
    """Resolve id do jogo -> área FAOSTAT.

    Retorna (mapa, nações sem correspondência, overrides inválidos). Um override
    que aponte para nome inexistente no CSV de dados é ERRO (build falha).
    """
    names = nation_english_names(set(game))
    mapping: dict[str, str] = {}
    unmatched: list[dict] = []
    invalid: list[dict] = []
    used: dict[str, str] = {}

    for iso in sorted(game):
        if iso in FAOSTAT_EXCLUDED:
            unmatched.append({"nation": iso, "name": game[iso], "reason": FAOSTAT_EXCLUDED[iso]})
            continue
        area = FAOSTAT_OVERRIDES.get(iso)
        if area is not None:
            if area not in areas:
                invalid.append(
                    {
                        "nation": iso,
                        "name": game[iso],
                        "faostat_area": area,
                        "problem": "área inexistente no CSV de dados do FAOSTAT",
                    }
                )
                continue
        else:
            candidate = names.get(iso)
            if candidate and candidate in areas:
                area = candidate
        if area is None:
            unmatched.append(
                {
                    "nation": iso,
                    "name": game[iso],
                    "reason": "nome inglês não corresponde a nenhuma área do FAOSTAT",
                }
            )
            continue
        if area in used:
            unmatched.append(
                {
                    "nation": iso,
                    "name": game[iso],
                    "reason": f"área FAOSTAT '{area}' já usada por {used[area]}",
                }
            )
            continue
        used[area] = iso
        mapping[iso] = area

    # Auditoria de TODOS os overrides, inclusive os de ids que não existem no jogo.
    for iso in sorted(FAOSTAT_OVERRIDES):
        area = FAOSTAT_OVERRIDES[iso]
        if iso not in game:
            invalid.append(
                {
                    "nation": iso,
                    "name": None,
                    "faostat_area": area,
                    "problem": "id de nação que não existe no jogo",
                }
            )
        elif area not in areas and not any(item["nation"] == iso for item in invalid):
            invalid.append(
                {
                    "nation": iso,
                    "name": game[iso],
                    "faostat_area": area,
                    "problem": "área inexistente no CSV de dados do FAOSTAT",
                }
            )
    return mapping, unmatched, invalid


def faostat_by_nation(
    raw: dict[str, dict[str, dict[int, dict]]], fao_map: dict[str, str]
) -> dict[str, dict[str, dict[int, dict]]]:
    """Reindexa {area: ...} para {id do jogo: ...}."""
    out: dict[str, dict[str, dict[int, dict]]] = {}
    for iso, area in fao_map.items():
        if area in raw:
            out[iso] = raw[area]
    return out


# ---------------------------------------------------------------------------
# FAOSTAT QCL -> rendimento
# ---------------------------------------------------------------------------


def load_faostat(fao_map: dict[str, str]) -> dict[str, dict[str, dict[int, dict]]]:
    """{id: {crop: {year: {area, production, flag}}}} do FAOSTAT QCL."""
    area_to_nation = {area: iso for iso, area in fao_map.items()}
    item_to_crop = {code: crop for crop, (code, _) in FAO_ITEMS.items()}
    elements = {FAO_ELEMENT_AREA, FAO_ELEMENT_PRODUCTION}

    data: dict[str, dict[str, dict[int, dict]]] = {}
    with open_zip_csv("FAO_QCL.zip", FAO_CSV_IN_QCL) as fh:
        reader = csv.reader(fh)
        next(reader)  # cabeçalho
        for row in reader:
            # colunas: Area Code, M49, Area, Item Code, CPC, Item,
            #          Element Code, Element, Year Code, Year, Unit, Value, Flag
            if row[6] not in elements or row[3] not in item_to_crop:
                continue
            nation = area_to_nation.get(row[2])
            if nation is None:
                continue
            try:
                year = int(row[9])
            except ValueError:
                continue
            if year > MAX_YEAR:
                continue
            raw = row[11].strip()
            if raw == "":
                continue
            try:
                value = float(raw)
            except ValueError:
                continue
            if value < 0:
                continue
            slot = data.setdefault(nation, {}).setdefault(item_to_crop[row[3]], {})
            bucket = slot.setdefault(year, {"area": None, "production": None, "flag": None})
            if row[6] == FAO_ELEMENT_AREA:
                bucket["area"] = value
            else:
                bucket["production"] = value
                bucket["flag"] = row[12].strip() or None
    return data


def pick_year(series: dict[int, dict]) -> tuple[int, dict] | None:
    """Último ano <= MAX_YEAR com área colhida > 0 e produção presente."""
    for year in sorted(series, reverse=True):
        row = series[year]
        if row["area"] and row["area"] > 0 and row["production"] is not None:
            return year, row
    return None


def last_production_year(series: dict[int, dict]) -> tuple[int, float] | None:
    for year in sorted(series, reverse=True):
        row = series[year]
        if row["production"] and row["production"] > 0:
            return year, row["production"]
    return None


def game_good_ids() -> set[str]:
    """Ids de bens do jogo (`economy/goods.json`)."""
    with open(GOODS_PATH, encoding="utf-8") as fh:
        return {row["id"] for row in json.load(fh)}


def build_non_game_reference(
    game: dict[str, str],
    fao: dict[str, dict[str, dict[int, dict]]],
    item_names: dict[str, str],
    goods: set[str],
) -> dict[str, dict]:
    """Bloco de conferência: culturas do FAOSTAT que não são bens do jogo.

    Fica fora de `nations` e não alimenta o núcleo (`in_game: false`).
    """
    block: dict[str, dict] = {}
    for iso, crop, label_pt in NON_GAME_REFERENCE:
        if crop in goods:
            raise ValueError(
                f"'{crop}' existe em economy/goods.json; não pode ir em non_game_reference"
            )
        code = NON_GAME_ITEMS[crop]
        series = fao.get(iso, {}).get(crop, {})
        picked = pick_year(series)
        entry = {
            "in_game": False,
            "faostat_item": item_names.get(code),
            "faostat_item_code": int(code),
            "yield_t_ha": None,
            "area_harvested_ha": None,
            "production_t": None,
            "year": None,
            "source": "FAOSTAT_QCL",
            "note": (
                f"{label_pt} não é bem do jogo (goods.json sem '{crop}'); "
                "número só de referência"
            ),
        }
        if picked is not None:
            year, row = picked
            area = float(row["area"])
            production = float(row["production"])
            # Bloco de referência: área e produção ficam como estão no CSV
            # (sem arredondar); só o rendimento é derivado, com 3 casas.
            entry["yield_t_ha"] = round(production / area, 3)
            entry["area_harvested_ha"] = area
            entry["production_t"] = production
            entry["year"] = year
        block[iso] = {"name": game.get(iso, iso), "crops": {crop: entry}}
    return block


def build_yields(game: dict[str, str], fao_map: dict[str, str], unmatched: list[dict], fao,
                 item_names: dict[str, str]):
    no_map_reason = "FAOSTAT sem correspondência de país (ver _meta.unmatched_nations)"

    nations_block: dict[str, dict] = {}
    for iso in sorted(game):
        crops_block: dict[str, dict] = {}
        series_by_crop = fao.get(iso, {})
        for crop in CROPS:
            series = series_by_crop.get(crop, {})
            picked = pick_year(series)
            if iso not in fao_map:
                crops_block[crop] = {
                    "yield_t_ha": None,
                    "area_harvested_ha": None,
                    "production_t": None,
                    "year": None,
                    "source": "FAOSTAT_QCL",
                    "faostat_area": None,
                    "flag": None,
                    "data_status": "no_data",
                    "reason": no_map_reason,
                }
                continue
            if picked is None:
                has_area = any((r["area"] or 0) > 0 for r in series.values())
                reason = (
                    "FAOSTAT sem produção no ano com área colhida"
                    if has_area
                    else "FAOSTAT sem área colhida para a cultura"
                )
                crops_block[crop] = {
                    "yield_t_ha": None,
                    "area_harvested_ha": None,
                    "production_t": None,
                    "year": None,
                    "source": "FAOSTAT_QCL",
                    "faostat_area": fao_map[iso],
                    "flag": None,
                    "data_status": "no_data",
                    "reason": reason,
                }
                continue
            year, row = picked
            area = float(row["area"])
            production = float(row["production"])
            crops_block[crop] = {
                "yield_t_ha": round(production / area, 3),
                "area_harvested_ha": round(area, 1),
                "production_t": round(production, 1),
                "year": year,
                "source": "FAOSTAT_QCL",
                "faostat_area": fao_map[iso],
                "flag": row["flag"],
                "data_status": "observed",
                "reason": None,
            }
        nations_block[iso] = {"name": game[iso], "crops": crops_block}

    payload = {
        "_meta": {
            "dataset": "crop_yields",
            "version": "g1",
            "generated_by": "tools/build_agri_yields.py",
            "retrieved": RETRIEVED,
            "nations": len(nations_block),
            "crops": list(CROPS),
            "method": (
                "FAOSTAT QCL: área colhida (elemento 5312) e produção (5510) do último ano "
                "<=2024 com área>0; rendimento = produção/área (derivado)"
            ),
            "faostat_elements": {"area_harvested": FAO_ELEMENT_AREA, "production": FAO_ELEMENT_PRODUCTION},
            "faostat_items": {crop: {"code": code, "name": name} for crop, (code, name) in FAO_ITEMS.items()},
            "faostat_area_index": (
                "nomes de 'Area' lidos do próprio CSV de dados do QCL; o E_AreaCodes.csv grafa "
                "nomes compostos com ';' (ex.: 'China; mainland') e não é usado"
            ),
            "unmatched_nations": unmatched,
            "non_game_note": (
                "culturas fora do jogo ficam em non_game_reference, com in_game:false; "
                "não alimentam o núcleo"
            ),
        },
        "sources": {
            "FAOSTAT_QCL": {
                "title": SOURCES["FAO_QCL.zip"]["title"],
                "publisher": SOURCES["FAO_QCL.zip"]["publisher"],
                "url": SOURCES["FAO_QCL.zip"]["url"],
                "year": SOURCES["FAO_QCL.zip"]["year"],
                "license": SOURCES["FAO_QCL.zip"]["license"],
                "accessed": RETRIEVED,
            }
        },
        "nations": nations_block,
        "non_game_reference": build_non_game_reference(
            game, fao, item_names, game_good_ids()
        ),
    }
    return payload


# ---------------------------------------------------------------------------
# USDA PSD -> estoque
# ---------------------------------------------------------------------------


def load_psd() -> dict[str, dict[str, dict[int, dict]]]:
    """{id: {crop: {year: {ending, consumption}}}} do USDA PSD (1000 MT)."""
    name_to_nation = {name: iso for iso, name in PSD_NAMES.items()}
    commodity_to_crop = {code: crop for crop, code in PSD_COMMODITY_CODES.items()}
    attrs = {PSD_ATTR_ENDING, PSD_ATTR_CONSUMPTION}

    data: dict[str, dict[str, dict[int, dict]]] = {}
    for zip_name, member in (
        ("psd_grains.zip", "psd_grains_pulses.csv"),
        ("psd_oilseeds.zip", "psd_oilseeds.csv"),
    ):
        with open_zip_csv(zip_name, member) as fh:
            for row in csv.DictReader(fh):
                if row["Attribute_ID"] not in attrs:
                    continue
                if row["Commodity_Code"] not in commodity_to_crop:
                    continue
                nation = name_to_nation.get(row["Country_Name"])
                if nation is None:
                    continue
                if row["Unit_Description"] != PSD_UNIT_MT:
                    continue
                try:
                    year = int(row["Market_Year"])
                except ValueError:
                    continue
                if year > MAX_YEAR:
                    continue
                raw = row["Value"].strip()
                if raw == "":
                    continue
                try:
                    value = float(raw)
                except ValueError:
                    continue
                if value < 0:
                    continue
                crop = commodity_to_crop[row["Commodity_Code"]]
                bucket = data.setdefault(nation, {}).setdefault(crop, {}).setdefault(
                    year, {"ending": None, "consumption": None}
                )
                if row["Attribute_ID"] == PSD_ATTR_ENDING:
                    bucket["ending"] = value
                else:
                    bucket["consumption"] = value
    return data


def build_stocks(game: dict[str, str], fao: dict[str, dict[str, dict[int, dict]]]):
    psd = load_psd()

    nations_block: dict[str, dict] = {}
    psd_without_map = sorted(iso for iso in game if iso not in PSD_NAMES)

    for iso in sorted(game):
        crops_block: dict[str, dict] = {}
        for crop in CROPS:
            series = psd.get(iso, {}).get(crop, {})
            # Último marketing year publicado para o país/cultura (<= 2024).
            latest_year = max(series) if series else None
            latest = series.get(latest_year) if latest_year is not None else None
            has_stock = bool(latest and (latest["ending"] or 0) > 0)

            if has_stock:
                year = latest_year
                ending_t = latest["ending"] * 1000.0
                consumption = latest["consumption"]
                consumption_t = None if consumption is None else consumption * 1000.0
                days = (
                    round(ending_t / consumption_t * 365.0, 1)
                    if consumption_t and consumption_t > 0
                    else None
                )
                crops_block[crop] = {
                    "ending_stocks_t": round(ending_t, 1),
                    "consumption_t": None if consumption_t is None else round(consumption_t, 1),
                    "days_of_consumption": days,
                    "marketing_year": year,
                    "method": "USDA_PSD",
                    "source": "USDA_PSD",
                    "data_status": "observed",
                    "consumption_basis": "USDA PSD Domestic Consumption" if consumption_t else None,
                    "note": None
                    if consumption_t
                    else "PSD tem estoque mas não tem consumo doméstico para a cultura",
                }
                continue

            # Regra declarada: 60 dias de consumo.
            basis = None
            consumption_psd = None
            basis_year = None
            for candidate_year in sorted(series, reverse=True):
                value = series[candidate_year]["consumption"]
                if value and value > 0:
                    consumption_psd = value * 1000.0
                    basis_year = candidate_year
                    basis = "USDA PSD Domestic Consumption"
                    break

            fao_series = fao.get(iso, {}).get(crop, {})
            if consumption_psd is None:
                production = last_production_year(fao_series)
                if production is not None:
                    basis_year, production_t = production
                    consumption_psd = production_t
                    basis = "FAOSTAT production as proxy for consumption"

            if consumption_psd is None or consumption_psd <= 0:
                crops_block[crop] = {
                    "ending_stocks_t": None,
                    "consumption_t": None,
                    "days_of_consumption": None,
                    "marketing_year": None,
                    "method": "rule_days_consumption",
                    "source": "RULE_60D",
                    "data_status": "no_data",
                    "consumption_basis": None,
                    "reason": "sem consumo PSD e sem produção FAOSTAT para estimar estoque",
                    "note": None,
                }
                continue

            note = (
                f"regra de {RULE_DAYS} dias sobre consumo PSD de {basis_year}"
                if basis == "USDA PSD Domestic Consumption"
                else f"regra de {RULE_DAYS} dias sobre produção FAOSTAT de {basis_year}"
            )
            if latest_year is not None:
                note += f"; PSD sem estoque > 0 em {latest_year}"

            crops_block[crop] = {
                "ending_stocks_t": round(consumption_psd * RULE_DAYS / 365.0, 1),
                "consumption_t": round(consumption_psd, 1),
                "days_of_consumption": float(RULE_DAYS),
                "marketing_year": MAX_YEAR,
                "method": "rule_days_consumption",
                "source": "RULE_60D",
                "data_status": "rule",
                "consumption_basis": basis,
                "reason": None,
                "note": note,
            }
        nations_block[iso] = {"name": game[iso], "crops": crops_block}

    payload = {
        "_meta": {
            "dataset": "crop_stocks",
            "version": "g1",
            "generated_by": "tools/build_agri_yields.py",
            "retrieved": RETRIEVED,
            "nations": len(nations_block),
            "crops": list(CROPS),
            "rule_days_default": RULE_DAYS,
            "start_date": "2027-01-01",
            "method": (
                "USDA PSD 'Ending Stocks' (atributo 176, 1000 MT -> t) do último marketing year "
                "publicado para o país/cultura (<=2024); se esse ano não tiver estoque > 0, aplica-se "
                "a regra declarada de 60 dias de consumo"
            ),
            "psd_commodity_codes": dict(PSD_COMMODITY_CODES),
            "psd_attribute_codes": {
                "ending_stocks": PSD_ATTR_ENDING,
                "domestic_consumption": PSD_ATTR_CONSUMPTION,
            },
            "psd_unit": f"{PSD_UNIT_MT} x 1000 = t",
            "psd_notes": {
                "potato": "batata não existe no PSD; sempre regra de 60 dias",
                "rice": "PSD usa arroz beneficiado (Rice, Milled); FAOSTAT produz arroz em casca",
            },
            "psd_unmatched_nations": psd_without_map,
        },
        "sources": {
            "USDA_PSD": {
                "title": "USDA PSD - Production, Supply and Distribution (grains, pulses, oilseeds)",
                "publisher": "USDA Foreign Agricultural Service",
                "url": SOURCES["psd_grains.zip"]["url"],
                "year": MAX_YEAR,
                "license": SOURCES["psd_grains.zip"]["license"],
                "accessed": RETRIEVED,
            },
            "RULE_60D": {
                "title": "Regra declarada: estoque inicial = 60 dias de consumo",
                "publisher": "OpenGS project",
                "year": 2026,
            },
        },
        "nations": nations_block,
    }
    return payload


# ---------------------------------------------------------------------------
# Autoverificação
# ---------------------------------------------------------------------------

YIELD_NUMERIC = ("yield_t_ha", "area_harvested_ha", "production_t")
STOCK_NUMERIC = ("ending_stocks_t", "consumption_t", "days_of_consumption")

# Nações grandes o bastante para que qualquer cultura sem dado mereça ALERTA.
MAJOR_NATIONS = (
    "CHN", "IND", "USA", "BRA", "RUS", "FRA", "ARG", "CAN",
    "AUS", "UKR", "PAK", "NGA", "IDN", "MEX", "TUR",
)


def autocheck(yields: dict, stocks: dict) -> list[str]:
    errors: list[str] = []
    n_nations = len(yields["nations"])

    if n_nations != 179:
        errors.append(f"crop_yields: {n_nations} nações (esperado 179)")
    if len(stocks["nations"]) != 179:
        errors.append(f"crop_stocks: {len(stocks['nations'])} nações (esperado 179)")

    if sorted(yields["nations"]) != sorted(stocks["nations"]):
        errors.append("conjuntos de nações divergem entre os dois arquivos")

    for iso, entry in yields["nations"].items():
        if list(entry["crops"]) != list(CROPS):
            errors.append(f"crop_yields {iso}: culturas fora da ordem/lista")
        for crop, row in entry["crops"].items():
            if crop not in CROPS:
                errors.append(f"crop_yields {iso}: cultura inválida {crop}")
            if not row.get("source"):
                errors.append(f"crop_yields {iso}/{crop}: sem source")
            has_year = row.get("year") is not None
            if not has_year and row.get("data_status") not in ("no_data", "rule"):
                errors.append(f"crop_yields {iso}/{crop}: sem ano e sem status especial")
            if row.get("data_status") not in ("observed", "no_data", "rule"):
                errors.append(f"crop_yields {iso}/{crop}: data_status inválido")
            for field in YIELD_NUMERIC:
                value = row.get(field)
                if value is not None and value < 0:
                    errors.append(f"crop_yields {iso}/{crop}: {field} negativo")

    for iso, entry in stocks["nations"].items():
        if list(entry["crops"]) != list(CROPS):
            errors.append(f"crop_stocks {iso}: culturas fora da ordem/lista")
        for crop, row in entry["crops"].items():
            if crop not in CROPS:
                errors.append(f"crop_stocks {iso}: cultura inválida {crop}")
            if not row.get("source"):
                errors.append(f"crop_stocks {iso}/{crop}: sem source")
            has_year = row.get("marketing_year") is not None
            if not has_year and row.get("data_status") not in ("no_data", "rule"):
                errors.append(f"crop_stocks {iso}/{crop}: sem marketing_year e sem status especial")
            if row.get("data_status") not in ("observed", "rule", "no_data"):
                errors.append(f"crop_stocks {iso}/{crop}: data_status inválido")
            for field in STOCK_NUMERIC:
                value = row.get(field)
                if value is not None and value < 0:
                    errors.append(f"crop_stocks {iso}/{crop}: {field} negativo")

    # Cobertura: nações sem NENHUMA das 5 culturas medidas.
    all_missing = [
        iso
        for iso, entry in yields["nations"].items()
        if all(row["data_status"] == "no_data" for row in entry["crops"].values())
    ]
    print(f"  nações com as 5 culturas em no_data: {len(all_missing)}")
    if all_missing:
        print("    " + ", ".join(all_missing))

    # ALERTA (não falha): culturas sem dado em países grandes.
    for iso in MAJOR_NATIONS:
        if iso not in yields["nations"]:
            print(f"  !! ALERTA: {iso} não está no arquivo")
            continue
        gaps = [
            crop
            for crop, row in yields["nations"][iso]["crops"].items()
            if row["data_status"] == "no_data"
        ]
        if gaps:
            print(f"  !! ALERTA: {iso} sem dado em {', '.join(gaps)}")
    return errors


def print_gaps(yields: dict, stocks: dict) -> None:
    print("\n[lacunas] crop_yields: observed / no_data por cultura")
    for crop in CROPS:
        rows = [entry["crops"][crop] for entry in yields["nations"].values()]
        observed = sum(1 for row in rows if row["data_status"] == "observed")
        missing = sum(1 for row in rows if row["data_status"] == "no_data")
        print(f"  {crop:<7} observed: {observed:>3} | no_data: {missing:>3} / 179")

    print("\n[lacunas] crop_stocks: método por cultura")
    for crop in CROPS:
        observed = sum(
            1 for entry in stocks["nations"].values() if entry["crops"][crop]["method"] == "USDA_PSD"
        )
        rule = sum(
            1
            for entry in stocks["nations"].values()
            if entry["crops"][crop]["method"] == "rule_days_consumption"
            and entry["crops"][crop]["data_status"] == "rule"
        )
        nodata = sum(
            1
            for entry in stocks["nations"].values()
            if entry["crops"][crop]["data_status"] == "no_data"
        )
        print(f"  {crop:<7} PSD: {observed:>3} | regra: {rule:>3} | sem dado: {nodata:>3}")

    total_psd = sum(
        1
        for entry in stocks["nations"].values()
        for row in entry["crops"].values()
        if row["method"] == "USDA_PSD"
    )
    total_rule = sum(
        1
        for entry in stocks["nations"].values()
        for row in entry["crops"].values()
        if row["method"] == "rule_days_consumption" and row["data_status"] == "rule"
    )
    print(f"  total   PSD: {total_psd} | regra: {total_rule} | entradas: {179 * len(CROPS)}")


def print_sample(yields: dict, stocks: dict) -> None:
    print("\n[amostra] rendimento (t/ha), área, ano | estoque")
    rows = (
        ("BRA", "soy"),
        ("USA", "maize"),
        ("IND", "rice"),
        ("IND", "wheat"),
        ("CHN", "rice"),
        ("UKR", "wheat"),
        ("EGY", "wheat"),
    )
    for iso, crop in rows:
        y = yields["nations"][iso]["crops"][crop]
        s = stocks["nations"][iso]["crops"][crop]
        print(
            f"  {iso} {crop}: rend={y['yield_t_ha']} t/ha area={y['area_harvested_ha']} ha "
            f"ano={y['year']} | estoque={s['ending_stocks_t']} t "
            f"({s['method']}, my={s['marketing_year']}, dias={s['days_of_consumption']})"
        )

    print("  CHN, as 5 culturas:")
    for crop in CROPS:
        y = yields["nations"]["CHN"]["crops"][crop]
        print(
            f"    CHN {crop:<7} rend={y['yield_t_ha']} t/ha area={y['area_harvested_ha']} ha "
            f"ano={y['year']} = {y['data_status']} (area FAOSTAT: {y['faostat_area']})"
        )


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def print_override_audit(
    game: dict[str, str], fao_map: dict[str, str], areas: set[str], invalid: list[dict]
) -> None:
    """Audita cada override contra os nomes realmente presentes no CSV de dados."""
    print("[auditoria] overrides do mapa FAOSTAT (nome do jogo -> área no CSV de dados)")
    print(f"  {'id':<4} {'jogo':<26} {'FAOSTAT':<62} existe")
    for iso in sorted(FAOSTAT_OVERRIDES):
        area = FAOSTAT_OVERRIDES[iso]
        exists = "sim" if area in areas else "NAO"
        print(f"  {iso:<4} {game.get(iso, '(fora do jogo)'):<26} {area:<62} {exists}")
    exact = sum(1 for iso in fao_map if iso not in FAOSTAT_OVERRIDES)
    print(
        f"  {len(FAOSTAT_OVERRIDES)} overrides auditados; {len(invalid)} inválido(s); "
        f"{exact} casamentos por nome exato também validados contra o CSV"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="baixa as fontes ausentes no cache")
    args = parser.parse_args(argv)

    # Console em UTF-8 para os textos pt-BR saírem legíveis também no Windows.
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    ensure_sources(args.fetch)

    game = nations()
    if len(game) != 179:
        print(f"aviso: nations() devolveu {len(game)} nações (esperado 179)")

    areas, raw, item_names = scan_faostat()
    fao_map, unmatched, invalid = build_faostat_map(game, areas)

    print_override_audit(game, fao_map, areas, invalid)
    if invalid:
        for item in invalid:
            print(
                f"  FALHA: override {item['nation']} -> '{item['faostat_area']}': {item['problem']}"
            )
        return 1

    fao = faostat_by_nation(raw, fao_map)
    print(f"FAOSTAT: {len(fao_map)} de {len(game)} nações mapeadas; {len(unmatched)} sem correspondência")

    yields = build_yields(game, fao_map, unmatched, fao, item_names)
    stocks = build_stocks(game, fao)

    yield_path = OUT_DIR / "crop_yields.json"
    stock_path = OUT_DIR / "crop_stocks.json"
    write_json(yield_path, yields)
    write_json(stock_path, stocks)

    errors = autocheck(yields, stocks)
    print("\n[autocheck]")
    if errors:
        for error in errors:
            print(f"  FALHA: {error}")
    else:
        print("  OK: 179 nações em cada arquivo; source + ano/status em toda entrada;")
        print("      nenhum valor negativo; nenhuma cultura fora de wheat/rice/maize/potato/soy")

    print_gaps(yields, stocks)
    print_sample(yields, stocks)
    print(f"\n[yields] sha256 {sha256(yield_path)}")
    print(f"[stocks] sha256 {sha256(stock_path)}")

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
