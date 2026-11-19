#!/usr/bin/env python3
"""Constrói os dois JSONs de pecuária/perecibilidade da Parte G (G1).

Saídas:

1. `data/definitions/agriculture/livestock_cycle.json` — ciclo de pecuária e
   leite do jogo. Produção CONTÍNUA (não é safra): `seasonality: "continuous"`,
   `monthly_factor: null`. Números de produção por cabeça vêm do FAOSTAT QCL
   (último ano <= 2024); parâmetros de ciclo vêm de texto publicado da FAO.
2. `data/definitions/agriculture/product_shelf_life.json` — dias de prateleira
   por produto, na condição declarada, para o núcleo comparar primário x
   processado.

Fontes (todas com URL; cache bruto NÃO versionado em `tools/_tmp_agri_raw/`):

- FAOSTAT QCL — `Production_Crops_Livestock_E_All_Data_(Normalized).zip`
  (já usado pelos outros construtores da Parte G). Peso de carcaça:
  item 867 "Meat of cattle with the bone, fresh or chilled", elemento 5417
  "Yield/Carcass Weight", kg/An. Leite por animal: item 882
  "Raw milk of cattle", elemento 5417, kg/An.
- FAO `x6512e19` (World Animal Review) — "Age at slaughter (400 kg): 17
  months". Só o que está escrito na página.
- FAO `u9550T0s` ("Milk yield and lactation length in tropical cattle") —
  "The standard period of 305 days has been chosen because this is
  approximately the normal lactation length of cows calving at intervals of
  12 months."
- USDA FSIS FoodKeeper (dataset publicado em data.gov, espelho arquivado) —
  dias de prateleira por produto/condição (DOP_Pantry / DOP_Refrigerate).
- FAO INPhO "Post-harvest compendium" de trigo, arroz, milho e soja — duração de
  armazenagem do grão a granel, transcrita da seção de armazenagem (o de soja só
  é citado como motivo de ausência de valor).

Regra de conversão: quando a fonte publica uma FAIXA, usa-se o limite
superior; meses/anos viram dias com 30,4 d/mês e 365 d/ano, arredondando ao
dia inteiro. Essas duas decisões são declaração do projeto, não número de
fonte, e estão registradas em `_meta`.

Nada aqui inventa número: onde não há fonte acessível para o produto,
`storage_days` (ou o parâmetro de ciclo) fica null com `data_status`
`no_data` e `reason`.

Uso:
    python tools/build_agri_livestock.py            # usa o cache
    python tools/build_agri_livestock.py --fetch    # baixa o que faltar

Reprodutibilidade: mesma entrada => mesmos bytes (nações ordenadas por id,
produtos em ordem fixa, JSON UTF-8 sem BOM, tab, LF).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import re
import sys
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from agri_common import nations, write_json  # noqa: E402

RAW_DIR = ROOT / "tools" / "_tmp_agri_raw"
LIT_DIR = RAW_DIR / "lit"
DEF = ROOT / "data" / "definitions"
OUT_LIVESTOCK = DEF / "agriculture" / "livestock_cycle.json"
OUT_SHELF = DEF / "agriculture" / "product_shelf_life.json"
NATIONS_SOURCE = DEF / "world" / "nations_source.csv"
GOODS = DEF / "economy" / "goods.json"

RETRIEVED = "2026-11-18"
MAX_YEAR = 2024
EXPECTED_NATIONS = 179

FAO_QCL_ZIP = "FAO_QCL.zip"
FAO_QCL_URL = (
    "https://bulks-faostat.fao.org/production/"
    "Production_Crops_Livestock_E_All_Data_(Normalized).zip"
)
FAO_CSV_IN_QCL = "Production_Crops_Livestock_E_All_Data_(Normalized).csv"

# Itens/elementos CONFIRMADOS no arquivo QCL em cache (não no briefing):
# o elemento 5419 citado no briefing não existe em E_Elements.csv; o leite por
# animal é publicado no elemento 5417, em kg/An, para o item 882.
FAO_ITEMS = {
    "cattle_carcass": {"item": "867", "element": "5417", "unit": "kg/cabeça"},
    "milk_cow": {"item": "882", "element": "5417", "unit": "kg/cabeça/ano"},
}

FOODKEEPER_URL = "https://www.fsis.usda.gov/shared/data/EN/foodkeeper.json"
FOODKEEPER_ARCHIVE_URL = (
    "https://web.archive.org/web/20250702182320id_/"
    "https://www.fsis.usda.gov/shared/data/EN/foodkeeper.json"
)
FOODKEEPER_PATH = RAW_DIR / "foodkeeper.json"

INPHO = {
    "WHEAT": "https://www.fao.org/fileadmin/user_upload/inpho/docs/Post_Harvest_Compendium_-_WHEAT.pdf",
    "RICE": "https://www.fao.org/fileadmin/user_upload/inpho/docs/Post_Harvest_Compendium_-_RICE.pdf",
    "MAIZE": "https://www.fao.org/fileadmin/user_upload/inpho/docs/Post_Harvest_Compendium_-_MAIZE.pdf",
    "SOYBEAN": "https://www.fao.org/fileadmin/user_upload/inpho/docs/Post_Harvest_Compendium_-_SOYBEAN.pdf",
}

LIT = {
    "slaughter_age": ("https://www.fao.org/4/x6512e/X6512E19.htm", "wraw_unit.html"),
    "extensive_age": ("https://www.fao.org/4/x6512e/X6512E25.htm", "wraw_systems.html"),
    "lactation": ("https://www.fao.org/4/u9550t/u9550T0s.htm", "milk_tropical.html"),
}

# ---------------------------------------------------------------------------
# Mapa FAOSTAT: id do jogo -> nome da área no FAOSTAT
# ---------------------------------------------------------------------------
#
# Mesmo mapa usado por `build_agri_yields.py` (base: nome inglês de
# `nations_source.csv` casado literalmente com E_AreaCodes.csv; só os nomes
# que divergem estão listados). O que não casar entra em `no_data` com motivo.

FAOSTAT_OVERRIDES = {
    "BHS": "Bahamas",
    "BOL": "Bolivia (Plurinational State of)",
    "BRN": "Brunei Darussalam",
    # QCL publica 867/882 só sob a área 'China' (351, M49 159); 'China; mainland'
    # (41) não tem nenhuma linha desses itens no arquivo em cache. Hong Kong,
    # Macao e Taiwan vêm em áreas próprias (96, 128, 214).
    "CHN": "China",
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

FAOSTAT_EXCLUDED = {
    "SAH": "FAOSTAT QCL não publica área para o Saara Ocidental",
    "SOL": "FAOSTAT não tem entrada para Somalilândia (o dado de 'Somalia' é outro país)",
    "TWN": "FAOSTAT só publica Taiwan como agregado 'China; Taiwan Province of', excluído por regra",
    "XKX": "FAOSTAT não tem entrada para Kosovo",
}

MONTH_DAYS = 30.4
YEAR_DAYS = 365
DAYS_PER = {"Days": 1.0, "Weeks": 7.0, "Months": MONTH_DAYS, "Years": float(YEAR_DAYS)}


def months_to_days(months: int) -> int:
    return int(round(months * MONTH_DAYS))


# ---------------------------------------------------------------------------
# Leitura do projeto
# ---------------------------------------------------------------------------


def load_json_file(path: Path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def game_ids() -> list[str]:
    return [g["id"] for g in load_json_file(GOODS)]


def nation_english_names(wanted: set[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    with open(NATIONS_SOURCE, encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            iso3 = row.get("iso3")
            if iso3 in wanted:
                out[iso3] = row["name_en"]
    return out


# ---------------------------------------------------------------------------
# Cache bruto + download
# ---------------------------------------------------------------------------


def http_get(url: str, timeout: int = 600) -> bytes:
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": (
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                "(KHTML, like Gecko) Chrome/126.0 Safari/537.36"
            )
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def ensure_cache(fetch: bool) -> None:
    """Garante cache do QCL, FoodKeeper, PDFs INPhO e páginas de literatura."""
    wanted = [RAW_DIR / FAO_QCL_ZIP, FOODKEEPER_PATH]
    wanted += [RAW_DIR / f"inpho_{name}.pdf" for name in INPHO]
    wanted += [LIT_DIR / filename for _, filename in LIT.values()]

    if not fetch:
        missing = [p for p in wanted if not p.exists()]
        if missing:
            names = ", ".join(str(p.relative_to(ROOT)) for p in missing)
            raise SystemExit(f"cache incompleto: {names} — rode com --fetch")
        return

    RAW_DIR.mkdir(parents=True, exist_ok=True)
    LIT_DIR.mkdir(parents=True, exist_ok=True)

    if not (RAW_DIR / FAO_QCL_ZIP).exists():
        print(f"[fetch] {FAO_QCL_ZIP} <- {FAO_QCL_URL}")
        (RAW_DIR / FAO_QCL_ZIP).write_bytes(http_get(FAO_QCL_URL))

    if not FOODKEEPER_PATH.exists():
        print(f"[fetch] foodkeeper.json <- {FOODKEEPER_URL}")
        try:
            payload = http_get(FOODKEEPER_URL, timeout=120)
        except (urllib.error.URLError, urllib.error.HTTPError) as error:
            print(f"[fetch] fonte oficial bloqueou ({error}); usando espelho arquivado")
            payload = http_get(FOODKEEPER_ARCHIVE_URL, timeout=120)
        if payload[:2] == b"\x1f\x8b":  # o servidor às vezes serve gzip cru
            import gzip

            payload = gzip.decompress(payload)
        FOODKEEPER_PATH.write_bytes(payload)

    for name, url in INPHO.items():
        path = RAW_DIR / f"inpho_{name}.pdf"
        if not path.exists():
            print(f"[fetch] inpho_{name}.pdf <- {url}")
            path.write_bytes(http_get(url))

    for key, (url, filename) in LIT.items():
        path = LIT_DIR / filename
        if not path.exists():
            print(f"[fetch] {filename} <- {url}")
            path.write_bytes(http_get(url))


# ---------------------------------------------------------------------------
# FAOSTAT QCL
# ---------------------------------------------------------------------------


def open_zip_csv(zip_name: str, member: str):
    zf = zipfile.ZipFile(RAW_DIR / zip_name)
    return io.TextIOWrapper(zf.open(member), encoding="utf-8-sig", newline="")


def faostat_area_index() -> set[str]:
    with open_zip_csv(FAO_QCL_ZIP, "Production_Crops_Livestock_E_AreaCodes.csv") as fh:
        return {row["Area"] for row in csv.DictReader(fh)}


def build_faostat_map(game: dict[str, str]) -> tuple[dict[str, str], list[dict]]:
    areas = faostat_area_index()
    names = nation_english_names(set(game))
    mapping: dict[str, str] = {}
    unmatched: list[dict] = []
    used: dict[str, str] = {}

    for iso in sorted(game):
        if iso in FAOSTAT_EXCLUDED:
            unmatched.append({"nation": iso, "name": game[iso], "reason": FAOSTAT_EXCLUDED[iso]})
            continue
        area = FAOSTAT_OVERRIDES.get(iso)
        if area is None:
            candidate = names.get(iso)
            if candidate and candidate in areas:
                area = candidate
        if area is None or area not in areas:
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
    return mapping, unmatched


def load_livestock(fao_map: dict[str, str]) -> tuple[dict[str, dict], dict[str, str]]:
    """{id: {'cattle': {...}, 'milk': {...}}} e nomes reais de item/elemento.

    Cada série guarda o último ano <= MAX_YEAR com valor > 0.
    """
    area_to_nation = {area: iso for iso, area in fao_map.items()}
    item_to_key = {spec["item"]: key for key, spec in FAO_ITEMS.items()}
    element_of = {spec["element"] for spec in FAO_ITEMS.values()}
    names: dict[str, str] = {}

    out: dict[str, dict] = {}
    with open_zip_csv(FAO_QCL_ZIP, FAO_CSV_IN_QCL) as fh:
        reader = csv.reader(fh)
        header = next(reader)
        i_area, i_item, i_item_name = header.index("Area"), header.index("Item Code"), header.index("Item")
        i_elem, i_elem_name = header.index("Element Code"), header.index("Element")
        i_year, i_unit, i_value = header.index("Year"), header.index("Unit"), header.index("Value")
        i_flag = header.index("Flag")

        for row in reader:
            item = row[i_item]
            key = item_to_key.get(item)
            if key is None or row[i_elem] not in element_of:
                continue
            nation = area_to_nation.get(row[i_area])
            if nation is None:
                continue
            try:
                year = int(row[i_year])
            except ValueError:
                continue
            if year > MAX_YEAR:
                continue
            raw = row[i_value].strip()
            if not raw:
                continue
            try:
                value = float(raw)
            except ValueError:
                continue
            if value <= 0:
                continue

            names[item] = row[i_item_name]
            names[f"{item}/{row[i_elem]}"] = row[i_elem_name]
            names[f"{item}/{row[i_elem]}/unit"] = row[i_unit]

            slot = out.setdefault(nation, {})
            current = slot.get(key)
            if current is None or year > current["year"]:
                slot[key] = {
                    "value": value,
                    "year": year,
                    "unit": row[i_unit],
                    "flag": row[i_flag].strip() or None,
                }
    return out, names


# ---------------------------------------------------------------------------
# Literatura do ciclo (texto publicado, verificado no cache)
# ---------------------------------------------------------------------------

# Transcrito de FAO World Animal Review (x6512e19): "Age at slaughter (400 kg) 17 months"
# na unidade de produção mista modelada. A mesma revista (x6512e25) documenta o
# extremo oposto ("age at slaughter: 6-7 years" no sistema extensivo primitivo).
SLAUGHTER_AGE_MONTHS = 17
SLAUGHTER_AGE_QUOTE = "Age at slaughter (400 kg) 17 months"
EXTENSIVE_AGE_QUOTE = "age at slaughter: 6-7 years"

# Transcrito de FAO (u9550T0s): "The standard period of 305 days has been chosen
# because this is approximately the normal lactation length of cows calving at
# intervals of 12 months."
LACTATION_DAYS = 305
CALVING_INTERVAL_MONTHS = 12
LACTATION_QUOTE = (
    "The standard period of 305 days has been chosen because this is approximately "
    "the normal lactation length of cows calving at intervals of 12 months"
)


def html_flat(path: Path) -> str:
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = re.sub(r"<script.*?</script>|<style.*?</style>", " ", raw, flags=re.S)
    text = re.sub(r"<[^>]+>", " ", text)
    text = text.replace("&ndash;", "-").replace("&nbsp;", " ")
    text = re.sub(r"&#(\d+);", lambda m: chr(int(m.group(1))), text)
    text = re.sub(r"&amp;", "&", text)
    return re.sub(r"\s+", " ", text)


def verify_literature() -> list[str]:
    """Confere as frases transcritas contra as páginas em cache."""
    errors: list[str] = []
    checks = (
        ("slaughter_age", SLAUGHTER_AGE_QUOTE),
        ("extensive_age", EXTENSIVE_AGE_QUOTE),
        ("lactation", LACTATION_QUOTE),
    )
    for key, quote in checks:
        filename = LIT[key][1]
        path = LIT_DIR / filename
        if not path.exists():
            print(f"[lit] {filename} ausente no cache; verificação pulada")
            continue
        if quote not in html_flat(path):
            errors.append(f"literatura: frase não encontrada em {filename}: {quote!r}")
    return errors


# ---------------------------------------------------------------------------
# Shelf life — USDA FoodKeeper
# ---------------------------------------------------------------------------


def load_foodkeeper() -> dict[int, dict]:
    payload = load_json_file(FOODKEEPER_PATH)
    sheet = next(s for s in payload["sheets"] if s["name"] == "Product")
    products: dict[int, dict] = {}
    for row in sheet["data"]:
        record: dict = {}
        for cell in row:
            record.update(cell)
        products[int(record["ID"])] = record
    return products


def foodkeeper_days(row: dict, prefix: str) -> tuple[int, str, str]:
    """Dias pelo limite superior da faixa publicada em DOP_<prefix>_*."""
    minimum = row.get(f"{prefix}_Min")
    maximum = row.get(f"{prefix}_Max")
    metric = row.get(f"{prefix}_Metric")
    if metric not in DAYS_PER:
        raise SystemExit(f"FoodKeeper: métrica inesperada {metric!r} em {row.get('Name')!r}")
    value = maximum if maximum is not None else minimum
    if value is None:
        raise SystemExit(f"FoodKeeper: produto sem valor em {prefix}: {row.get('Name')!r}")
    days = int(round(float(value) * DAYS_PER[metric]))
    if minimum is not None and maximum is not None and minimum == maximum:
        span = f"{_num(minimum)} {metric}"
    else:
        span = f"{_num(minimum)}-{_num(maximum)} {metric}"
    return days, span, metric


def _num(value) -> str:
    if value is None:
        return "?"
    return str(int(value)) if float(value).is_integer() else str(value)


FOODKEEPER_SPECS = {
    # id do jogo -> (id FoodKeeper, substring do nome, prefixo DOP)
    "potato": (297, "Potatoes", "DOP_Pantry"),
    "flour": (222, "Flour", "DOP_Pantry"),
    "bread": (195, "Commercial bread products", "DOP_Pantry"),
    "bagged_rice": (338, "Rice", "DOP_Pantry"),
    "meat": (40, "Beef", "DOP_Refrigerate"),
    "canned_food": (372, "Canned goods", "DOP_Pantry"),
}

# ---------------------------------------------------------------------------
# Shelf life — FAO INPhO (grão a granel)
# ---------------------------------------------------------------------------

INPHO_QUOTES = {
    "wheat": (
        "WHEAT",
        "For longer periods of storage from 4-12 months, the moisture content must be reduced to 13 percentage or below",
        12,
        "limite superior (12 meses) da faixa publicada para grão a <= 13% de umidade; "
        "a mesma página diz que a 14% o grão se conserva 2-3 meses",
    ),
    "rice": (
        "RICE",
        "Paddy is stored until the next harvest season or for 6-12 months",
        12,
        "limite superior (12 meses) da faixa publicada para arroz em casca",
    ),
    "maize": (
        "MAIZE",
        "season- long storage (6-9 months)",
        9,
        "limite superior (9 meses) da faixa publicada para armazenagem de safra",
    ),
}


INPHO_EXTRA_QUOTES = {
    # frases citadas nas notes/textos, verificadas no mesmo PDF (por nome do arquivo)
    "WHEAT": ("At 14 percentage moisture grain can be safely stored for 2 to 3 months",),
    "SOYBEAN": (
        "The storage period was 8 months, the normal storage period from soybean "
        "harvesting to the next planting date in Costa Rica",
    ),
}


def pdf_flat(name: str) -> str | None:
    path = RAW_DIR / f"inpho_{name}.pdf"
    if not path.exists():
        return None
    try:
        from pypdf import PdfReader
    except ImportError:
        return None
    reader = PdfReader(str(path))
    text = "\n".join((page.extract_text() or "") for page in reader.pages)
    return re.sub(r"\s+", " ", text)


def verify_inpho() -> list[str]:
    errors: list[str] = []
    cache: dict[str, str | None] = {}
    checks = [(INPHO_QUOTES[game_id][0], INPHO_QUOTES[game_id][1]) for game_id in INPHO_QUOTES]
    for name, quotes in INPHO_EXTRA_QUOTES.items():
        checks += [(name, quote) for quote in quotes]
    for name, quote in checks:
        if name not in cache:
            cache[name] = pdf_flat(name)
        flat = cache[name]
        if flat is None:
            print(f"[inpho] {name}: PDF ausente ou pypdf indisponível; verificação pulada")
            continue
        if quote not in flat:
            errors.append(f"INPhO {name}: frase não encontrada no PDF: {quote!r}")
    return errors


# ---------------------------------------------------------------------------
# Montagem — livros de prateleira
# ---------------------------------------------------------------------------

SHELF_PRODUCTS = (
    "wheat",
    "rice",
    "maize",
    "potato",
    "soy",
    "flour",
    "bran",
    "bread",
    "bagged_rice",
    "feed",
    "meat",
    "canned_food",
)

# tier/condição declarada por produto; a condição é a da fonte citada.
SHELF_META = {
    "wheat": ("raw", "ambient_dry"),
    "rice": ("raw", "ambient_dry"),
    "maize": ("raw", "ambient_dry"),
    "potato": ("raw", "ambient"),
    "soy": ("raw", "ambient_dry"),
    "flour": ("semi", "ambient_dry"),
    "bran": ("semi", "ambient_dry"),
    "bread": ("final", "ambient"),
    "bagged_rice": ("final", "ambient_dry"),
    "feed": ("semi", "ambient_dry"),
    "meat": ("final", "refrigerated"),
    "canned_food": ("final", "ambient"),
}

SHELF_NO_DATA = {
    "soy": (
        "Nenhuma fonte acessível publica vida de prateleira em dias para soja a granel. "
        "O compêndio FAO de soja (INPhO, ver sources.FAO_INPHO_SOYBEAN) relata apenas "
        "uma DURAÇÃO de armazenagem praticada (8 meses da colheita ao plantio seguinte, "
        "Costa Rica), que não é vida útil medida; por isso o campo fica null."
    ),
    "bran": (
        "O FoodKeeper (USDA FSIS) não tem entrada para farelo e os manuais FAO "
        "consultados não publicam vida de prateleira em dias para farelo. Campo null."
    ),
    "feed": (
        "Ração composta não está no FoodKeeper nem nos compêndios FAO usados; "
        "sem fonte acessível para um número de dias. Campo null."
    ),
}


def build_shelf_life() -> dict:
    keeper = load_foodkeeper()
    products: dict[str, dict] = {}

    for game_id in SHELF_PRODUCTS:
        tier, condition = SHELF_META[game_id]
        row: dict = {
            "in_game": True,
            "tier": tier,
            "storage_days": None,
            "ambient_or_cold": condition,
            "data_status": "no_data",
            "source": None,
            "url": None,
            "notes": None,
        }

        if game_id in FOODKEEPER_SPECS:
            fk_id, name_needle, prefix = FOODKEEPER_SPECS[game_id]
            record = keeper.get(fk_id)
            if record is None:
                raise SystemExit(f"FoodKeeper: id {fk_id} ausente no cache")
            name = record.get("Name") or ""
            if name_needle.lower() not in name.lower():
                raise SystemExit(
                    f"FoodKeeper: id {fk_id} agora é {name!r} (esperado conter {name_needle!r})"
                )
            days, span, metric = foodkeeper_days(record, prefix)
            subtitle = record.get("Name_subtitle") or ""
            row.update(
                {
                    "storage_days": days,
                    "data_status": "observed",
                    "source": "FOODKEEPER",
                    "url": FOODKEEPER_URL,
                    "notes": (
                        f"FoodKeeper id {fk_id} '{name}"
                        f"{' — ' + subtitle if subtitle else ''}': "
                        f"{prefix} {span}; storage_days = limite superior em dias"
                    ),
                }
            )
        elif game_id in INPHO_QUOTES:
            name, quote, months, note = INPHO_QUOTES[game_id]
            row.update(
                {
                    "storage_days": months_to_days(months),
                    "data_status": "observed",
                    "source": f"FAO_INPHO_{name}",
                    "url": INPHO[name],
                    "notes": f"'{quote}' — {note}; {months} meses -> {months_to_days(months)} dias",
                }
            )
        else:
            row.update({"notes": SHELF_NO_DATA[game_id], "reason": SHELF_NO_DATA[game_id]})

        products[game_id] = row

    pairs = {}
    for raw_id, processed_id in (
        ("wheat", "flour"),
        ("wheat", "bread"),
        ("rice", "bagged_rice"),
        ("maize", "flour"),
        ("meat", "canned_food"),
    ):
        raw_days = products[raw_id]["storage_days"]
        processed_days = products[processed_id]["storage_days"]
        if raw_days and processed_days:
            pairs[f"{raw_id}->{processed_id}"] = {
                "raw_days": raw_days,
                "processed_days": processed_days,
                "ratio": round(processed_days / raw_days, 4),
            }

    return {
        "_meta": {
            "dataset": "product_shelf_life",
            "version": "g1",
            "generated_by": "tools/build_agri_livestock.py",
            "retrieved": RETRIEVED,
            "products": len(products),
            "condition": "declarada por produto em `ambient_or_cold` (é a condição da fonte citada)",
            "range_rule": (
                "quando a fonte publica faixa, usa-se o limite superior; "
                "meses/anos viram dias com 30,4 d/mês e 365 d/ano, arredondado ao dia"
            ),
            "grain_condition": (
                "grãos e farinhas: ambiente seco (umidade <= 13%), que é a condição "
                "descrita nas fontes; carne: refrigerada"
            ),
        },
        "sources": {
            "FOODKEEPER": {
                "title": "FSIS - FoodKeeper Data (FoodKeeper-Data / foodkeeper.json)",
                "publisher": "USDA Food Safety and Inspection Service",
                "url": FOODKEEPER_URL,
                "alt_url": FOODKEEPER_ARCHIVE_URL,
                "license": "CC0 1.0 (data.gov)",
                "accessed": RETRIEVED,
                "note": "dataset publicado em data.gov; o host da FSIS responde 403 sem navegador",
            },
            "FAO_INPHO_WHEAT": {
                "title": "Wheat post-harvest operations (INPhO compendium)",
                "publisher": "FAO",
                "url": INPHO["WHEAT"],
                "section": "capítulo de armazenagem (Storage)",
                "accessed": RETRIEVED,
                "note": "PDF; página não numerada na extração — citação pela seção e pela frase transcrita",
            },
            "FAO_INPHO_RICE": {
                "title": "Rice post-harvest operations (INPhO compendium)",
                "publisher": "FAO",
                "url": INPHO["RICE"],
                "section": "capítulo de armazenagem (Storage)",
                "accessed": RETRIEVED,
                "note": "PDF; página não numerada na extração — citação pela seção e pela frase transcrita",
            },
            "FAO_INPHO_MAIZE": {
                "title": "Maize post-harvest operations (INPhO compendium)",
                "publisher": "FAO",
                "url": INPHO["MAIZE"],
                "section": "capítulo de armazenagem (Storage)",
                "accessed": RETRIEVED,
                "note": "PDF; página não numerada na extração — citação pela seção e pela frase transcrita",
            },
            "FAO_INPHO_SOYBEAN": {
                "title": "Soybean post-harvest operations (INPhO compendium)",
                "publisher": "FAO",
                "url": INPHO["SOYBEAN"],
                "section": "capítulo de armazenagem (Storage)",
                "accessed": RETRIEVED,
                "note": "citado só em soy.reason: a fonte não publica vida de prateleira em dias",
            },
        },
        "products": products,
        "chain_effect": {
            "note": (
                "primário -> processado: a razão vem dos dias medidas acima "
                "(pairs abaixo), não de constante inventada; onde falta valor a "
                "comparação não é publicada"
            ),
            "pairs": pairs,
        },
    }


# ---------------------------------------------------------------------------
# Montagem — ciclo de pecuária
# ---------------------------------------------------------------------------


def build_livestock(
    game: dict[str, str],
    fao_map: dict[str, str],
    unmatched: list[dict],
    livestock: dict[str, dict],
    fao_names: dict[str, str],
) -> dict:
    reason_by_nation = {row["nation"]: row["reason"] for row in unmatched}

    cattle_spec = FAO_ITEMS["cattle_carcass"]
    milk_spec = FAO_ITEMS["milk_cow"]

    nations_block: dict[str, dict] = {}
    for iso in sorted(game):
        series = livestock.get(iso, {})
        cattle = series.get("cattle_carcass")
        milk = series.get("milk_cow")

        if cattle is None:
            cattle_row = {
                "carcass_kg_per_head": None,
                "year": None,
                "source": None,
                "data_status": "no_data",
                "reason": reason_by_nation.get(
                    iso,
                    "FAOSTAT QCL não traz peso de carcaça bovina (> 0) para esta área até 2024",
                ),
            }
        else:
            cattle_row = {
                "carcass_kg_per_head": round(cattle["value"], 3),
                "year": cattle["year"],
                "source": "FAOSTAT_QCL",
                "data_status": "observed",
                "flag": cattle["flag"],
            }

        if milk is None:
            milk_row = {
                "kg_per_head_year": None,
                "year": None,
                "source": None,
                "data_status": "no_data",
                "reason": reason_by_nation.get(
                    iso,
                    "FAOSTAT QCL não traz leite por animal (> 0) para esta área até 2024",
                ),
            }
        else:
            milk_row = {
                "kg_per_head_year": round(milk["value"], 3),
                "year": milk["year"],
                "source": "FAOSTAT_QCL",
                "data_status": "observed",
                "flag": milk["flag"],
            }

        nations_block[iso] = {
            "name": game[iso],
            "faostat_area": fao_map.get(iso),
            "cattle": cattle_row,
            "milk": milk_row,
        }

    observed_cattle = sum(1 for n in nations_block.values() if n["cattle"]["data_status"] == "observed")
    observed_milk = sum(1 for n in nations_block.values() if n["milk"]["data_status"] == "observed")
    stale_cattle = sorted(
        iso
        for iso, n in nations_block.items()
        if n["cattle"]["data_status"] == "observed" and n["cattle"]["year"] < 2020
    )
    stale_milk = sorted(
        iso
        for iso, n in nations_block.items()
        if n["milk"]["data_status"] == "observed" and n["milk"]["year"] < 2020
    )
    missing_cattle = sorted(
        iso for iso, n in nations_block.items() if n["cattle"]["data_status"] == "no_data"
    )
    missing_milk = sorted(
        iso for iso, n in nations_block.items() if n["milk"]["data_status"] == "no_data"
    )

    return {
        "_meta": {
            "dataset": "livestock_cycle",
            "version": "g1",
            "generated_by": "tools/build_agri_livestock.py",
            "retrieved": RETRIEVED,
            "nations": len(nations_block),
            "max_year": MAX_YEAR,
            "game_chain": "feed -> meat (recipe 'meat'); milk não é bem do jogo",
            "faostat_items": {
                "cattle_carcass": {
                    "item": int(cattle_spec["item"]),
                    "item_name": fao_names.get(cattle_spec["item"]),
                    "element": int(cattle_spec["element"]),
                    "element_name": fao_names.get(f"{cattle_spec['item']}/{cattle_spec['element']}"),
                    "unit": fao_names.get(f"{cattle_spec['item']}/{cattle_spec['element']}/unit"),
                },
                "milk_cow": {
                    "item": int(milk_spec["item"]),
                    "item_name": fao_names.get(milk_spec["item"]),
                    "element": int(milk_spec["element"]),
                    "element_name": fao_names.get(f"{milk_spec['item']}/{milk_spec['element']}"),
                    "unit": fao_names.get(f"{milk_spec['item']}/{milk_spec['element']}/unit"),
                },
                "note": (
                    "o briefing citava o elemento 5419 ('Yield', hg/An) para o leite; "
                    "esse elemento não existe no QCL em cache (E_Elements.csv) e o leite por "
                    "animal do item 882 é publicado no elemento 5417, em kg/An, já em kg "
                    "(sem divisão por 10). O item 882 chama-se 'Raw milk of cattle' no arquivo."
                ),
            },
            "coverage": {
                "cattle_carcass_observed": observed_cattle,
                "cattle_carcass_no_data": len(nations_block) - observed_cattle,
                "milk_observed": observed_milk,
                "milk_no_data": len(nations_block) - observed_milk,
                "cattle_carcass_stale_before_2020": stale_cattle,
                "milk_stale_before_2020": stale_milk,
                "cattle_carcass_no_data_nations": missing_cattle,
                "milk_no_data_nations": missing_milk,
            },
            "faostat_area_note": (
                "CHN usa a área 'China' (351, M49 159): no QCL em cache os itens 867 e 882 "
                "só são publicados sob ela; 'China; mainland' (41) não tem linhas desses "
                "itens. Hong Kong (96), Macao (128) e Taiwan (214) são áreas separadas."
            ),
            "seasonality_rule": (
                "produção contínua: o jogo trata como fluxo diário plano; "
                "não há série mensal publicada usada aqui"
            ),
        },
        "sources": {
            "FAOSTAT_QCL": {
                "title": "FAOSTAT - Crops and livestock products (QCL), Production",
                "publisher": "FAO",
                "url": FAO_QCL_URL,
                "license": "CC BY 4.0",
                "accessed": RETRIEVED,
                "year_max": MAX_YEAR,
            },
            "CYCLE_LITERATURE": {
                "title": "FAO World Animal Review — unidade de produção mista (x6512e19) e Primitive extensive beef production (x6512e25)",
                "publisher": "FAO",
                "url": LIT["slaughter_age"][0],
                "url_extensive": LIT["extensive_age"][0],
                "accessed": RETRIEVED,
                "note": "páginas HTML sem paginação; a citação é a seção indicada em cada produto",
            },
            "MILK_LITERATURE": {
                "title": "Milk yield and lactation length in tropical cattle",
                "publisher": "FAO",
                "url": LIT["lactation"][0],
                "accessed": RETRIEVED,
                "note": "página HTML sem paginação; citação pela seção",
            },
        },
        "products": {
            "meat": {
                "in_game": True,
                "kind": "final",
                "flow": "continuous",
                "seasonality": "continuous",
                "monthly_factor": None,
                "slaughter_age_months": SLAUGHTER_AGE_MONTHS,
                "carcass_kg_per_head": None,
                "source": "CYCLE_LITERATURE",
                "data_status": "observed",
                "notes": (
                    "pecuária de corte: fluxo diário plano (sem safra). "
                    f"'{SLAUGHTER_AGE_QUOTE}' (FAO x6512e19). "
                    f"A mesma revista registra, no extremo oposto, '{EXTENSIVE_AGE_QUOTE}' "
                    "no sistema extensivo primitivo (FAO x6512e25): a idade de abate é "
                    "propriedade do sistema, e o jogo usa o valor da unidade modelada. "
                    "Peso de carcaça por cabeça é por nação em `nations` (FAOSTAT item 867)."
                ),
            },
            "milk": {
                "in_game": False,
                "kind": "reference",
                "flow": "continuous",
                "seasonality": "continuous",
                "monthly_factor": None,
                "lactation_days": LACTATION_DAYS,
                "calving_interval_months": CALVING_INTERVAL_MONTHS,
                "source": "MILK_LITERATURE",
                "data_status": "observed",
                "notes": (
                    "sem bem 'milk' no jogo: entra só como referência declarada. "
                    f"'{LACTATION_QUOTE}' (FAO u9550T0s). "
                    "O jogo deve tratar a produção como fluxo diário plano; "
                    "leite por animal/ano por nação em `nations` (FAOSTAT item 882), "
                    "não usado para consumo até existir o bem."
                ),
            },
        },
        "nations": nations_block,
    }


# ---------------------------------------------------------------------------
# Autoverificação
# ---------------------------------------------------------------------------


def autocheck_livestock(payload: dict) -> list[str]:
    errors: list[str] = []
    block = payload["nations"]
    if len(block) != EXPECTED_NATIONS:
        errors.append(f"{len(block)} nações (esperado {EXPECTED_NATIONS})")

    for iso, nation in block.items():
        for field, sub in (("cattle", "carcass_kg_per_head"), ("milk", "kg_per_head_year")):
            row = nation[field]
            if row["data_status"] == "observed":
                value = row[sub]
                if value is None or value <= 0:
                    errors.append(f"{iso}/{field}: observed com valor {value!r}")
                if row.get("year") is None:
                    errors.append(f"{iso}/{field}: observed sem ano")
                if row.get("source") != "FAOSTAT_QCL":
                    errors.append(f"{iso}/{field}: observed sem source FAOSTAT_QCL")
                if row.get("reason"):
                    errors.append(f"{iso}/{field}: observed não deve ter reason")
            elif row["data_status"] == "no_data":
                if row[sub] is not None:
                    errors.append(f"{iso}/{field}: no_data com valor")
                if not row.get("reason"):
                    errors.append(f"{iso}/{field}: no_data sem reason")
            else:
                errors.append(f"{iso}/{field}: data_status inválido {row['data_status']}")

    if payload["_meta"]["nations"] != len(block):
        errors.append("_meta.nations diverge do bloco nations")

    for name, product in payload["products"].items():
        if product["seasonality"] != "continuous":
            errors.append(f"produto {name}: seasonality != continuous")
        if product["monthly_factor"] is not None:
            errors.append(f"produto {name}: monthly_factor preenchido sem série mensal")
        source_key = product.get("source")
        if source_key not in payload["sources"]:
            errors.append(f"produto {name}: source {source_key!r} fora de sources")
        if not payload["sources"].get(source_key, {}).get("url"):
            errors.append(f"produto {name}: source sem URL")
    return errors


def autocheck_shelf(payload: dict, game: list[str]) -> list[str]:
    errors: list[str] = []
    products = payload["products"]

    if list(products) != list(SHELF_PRODUCTS):
        errors.append("lista/ordem de produtos diverge de SHELF_PRODUCTS")
    for game_id in SHELF_PRODUCTS:
        if game_id not in game:
            errors.append(f"{game_id} não existe em economy/goods.json")
    if len(products) != 12:
        errors.append(f"{len(products)} produtos (esperado 12)")

    for game_id, row in products.items():
        if row["storage_days"] is None:
            if row["data_status"] != "no_data":
                errors.append(f"{game_id}: sem dias mas data_status {row['data_status']}")
            if not row.get("reason"):
                errors.append(f"{game_id}: sem dias e sem reason")
            if row.get("source") or row.get("url"):
                errors.append(f"{game_id}: no_data não deve citar fonte")
        else:
            if row["storage_days"] <= 0:
                errors.append(f"{game_id}: storage_days <= 0")
            if not row.get("source") or not row.get("url"):
                errors.append(f"{game_id}: valor sem source+url")
            if row.get("source") not in payload["sources"]:
                errors.append(f"{game_id}: source fora de sources")
            if row["data_status"] != "observed":
                errors.append(f"{game_id}: valor com data_status {row['data_status']}")

    pairs = payload["chain_effect"]["pairs"]
    for key, pair in pairs.items():
        expected = round(pair["processed_days"] / pair["raw_days"], 4)
        if pair["ratio"] != expected:
            errors.append(f"chain_effect {key}: razão incoerente com os dias")
    return errors


# ---------------------------------------------------------------------------
# Relatórios
# ---------------------------------------------------------------------------


def print_livestock(payload: dict) -> None:
    meta = payload["_meta"]
    print("\n[autocheck] livestock_cycle")
    print(f"  nações: {len(payload['nations'])}")
    print(
        "  carcaça bovina: observed={cattle_carcass_observed} "
        "no_data={cattle_carcass_no_data}".format(**meta["coverage"])
    )
    print(
        "  leite por animal: observed={milk_observed} no_data={milk_no_data}".format(
            **meta["coverage"]
        )
    )
    print(f"  item carcaça: {meta['faostat_items']['cattle_carcass']}")
    print(f"  item leite:   {meta['faostat_items']['milk_cow']}")
    coverage = meta["coverage"]
    for field in ("cattle_carcass", "milk"):
        stale = coverage[f"{field}_stale_before_2020"]
        missing = coverage[f"{field}_no_data_nations"]
        print(
            f"  {field}: ano < 2020 em {len(stale)} nações"
            f"{': ' + ', '.join(stale) if stale else ''}; sem dado: {', '.join(missing) or '—'}"
        )


def print_shelf(payload: dict) -> None:
    print("\n[autocheck] product_shelf_life")
    observed = sum(1 for p in payload["products"].values() if p["storage_days"] is not None)
    print(f"  produtos: {len(payload['products'])} (observed={observed}, no_data={12 - observed})")
    for key, pair in payload["chain_effect"]["pairs"].items():
        print(
            f"  cadeia {key:<22} {pair['raw_days']:>5} -> {pair['processed_days']:>5} d "
            f"(x{pair['ratio']})"
        )


def print_sample(payload: dict) -> None:
    print("\n[amostra] livestock_cycle — carcaça e leite por país")
    print(f"  {'país':<5} {'carcaça kg/cab':>15} {'ano':>6} {'leite kg/cab/ano':>18} {'ano':>6}")
    for iso in ("BRA", "USA", "IND", "CHN", "NGA", "UKR", "EGY"):
        row = payload["nations"][iso]
        cattle = row["cattle"]
        milk = row["milk"]
        print(
            f"  {iso:<5} {str(cattle['carcass_kg_per_head']):>15} {str(cattle['year']):>6} "
            f"{str(milk['kg_per_head_year']):>18} {str(milk['year']):>6}"
        )


def print_shelf_table(payload: dict) -> None:
    print("\n[amostra] product_shelf_life — tabela completa")
    print(f"  {'produto':<12} {'tier':<6} {'condição':<12} {'dias':>6}  fonte")
    for game_id, row in payload["products"].items():
        days = row["storage_days"]
        print(
            f"  {game_id:<12} {row['tier']:<6} {row['ambient_or_cold']:<12} "
            f"{(str(days) if days is not None else 'null'):>6}  "
            f"{row['source'] or '—'}"
        )
    print("\n[amostra] product_shelf_life — sem valor (null) e motivo")
    for game_id, row in payload["products"].items():
        if row["storage_days"] is None:
            print(f"  {game_id}: {row['reason']}")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="baixa o que faltar no cache")
    args = parser.parse_args(argv)

    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    ensure_cache(args.fetch)

    game = nations()
    if len(game) != EXPECTED_NATIONS:
        print(f"aviso: nations() devolveu {len(game)} nações (esperado {EXPECTED_NATIONS})")
    goods = game_ids()

    fao_map, unmatched = build_faostat_map(game)
    livestock, fao_names = load_livestock(fao_map)

    errors = verify_literature() + verify_inpho()

    payload_livestock = build_livestock(game, fao_map, unmatched, livestock, fao_names)
    write_json(OUT_LIVESTOCK, payload_livestock)

    payload_shelf = build_shelf_life()
    write_json(OUT_SHELF, payload_shelf)

    errors += autocheck_livestock(payload_livestock)
    errors += autocheck_shelf(payload_shelf, goods)

    print("[autocheck]")
    if errors:
        for error in errors:
            print(f"  FALHA: {error}")
    else:
        print(
            "  OK: 179 nações, valores > 0 quando não nulos, 12 produtos de prateleira, "
            "todo valor com source+url, toda ausência com reason"
        )

    print_livestock(payload_livestock)
    print_shelf(payload_shelf)
    print_sample(payload_livestock)
    print_shelf_table(payload_shelf)

    if unmatched:
        print("\n[cobertura] nações sem área FAOSTAT")
        for row in unmatched:
            print(f"  {row['nation']} ({row['name']}): {row['reason']}")

    print(f"\n[livestock_cycle] sha256 {sha256(OUT_LIVESTOCK)}")
    print(f"[livestock_cycle] arquivo {OUT_LIVESTOCK.relative_to(ROOT)}")
    print(f"[product_shelf_life] sha256 {sha256(OUT_SHELF)}")
    print(f"[product_shelf_life] arquivo {OUT_SHELF.relative_to(ROOT)}")

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
