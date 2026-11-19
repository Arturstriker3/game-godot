#!/usr/bin/env python3
"""Constrói `data/definitions/agriculture/postharvest_losses.json` (Parte G, G1).

Perda pós-colheita/armazenagem (%) por nação do jogo x cultura primária de
comida (wheat, rice, maize, potato, soy), para ligar à infraestrutura B5
(armazém ruim perde mais).

Fontes, em ordem de prioridade:

1. APHLIS (African Postharvest Losses Information System) — tabela pública
   "value chain" em percentagem, por país africano e cultura de grão
   (maize, rice, wheat). `loss_pct` = linha "All steps" do último ano com
   valor; `storage_share_pct` = "Household-level storage" + "Market storage"
   do mesmo ano. `data_status: "observed"`. Cache HTML em
   `tools/_tmp_agri_raw/aphlis/` (NÃO versionado).
2. FAO 2011 "Global food losses and food waste" (Gustavsson et al.), Annex 4,
   p. 26-27: percentagens por REGIÃO e por grupo de commodities na etapa
   "postharvest handling and storage" (o valor entra como `loss_pct`; a fonte
   não separa manuseio de armazenagem, então `storage_share_pct` fica null).
   É taxa REGIONAL aplicada ao país, não medição nacional:
   `data_status: "regional_observed"`.
   O pareamento país -> região FAO vem do Annex 1 (p. 23), transcrito abaixo.
3. Regra declarada (último recurso): mediana dos observados da MESMA região do
   jogo (`region` de `scenario/nations.json`) para a cultura; sem observado na
   região, mediana global da cultura. `data_status: "regional_default"`.

Nada aqui inventa número: onde não há observação nem mediana, `loss_pct` fica
null e `data_status` = `no_data` (não ocorre com as fontes atuais).

Uso:
    python tools/build_agri_losses.py            # usa o cache em tools/_tmp_agri_raw
    python tools/build_agri_losses.py --fetch    # baixa o que faltar no cache

Reprodutibilidade: mesma entrada => mesmos bytes (nações ordenadas por id,
culturas na ordem fixa CROPS, JSON UTF-8 sem BOM, tab, LF).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import html as html_lib
import json
import re
import statistics
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from agri_common import nations, write_json  # noqa: E402

RAW_DIR = ROOT / "tools" / "_tmp_agri_raw"
APHLIS_DIR = RAW_DIR / "aphlis"
OUT_PATH = ROOT / "data" / "definitions" / "agriculture" / "postharvest_losses.json"
NATIONS_SOURCE = ROOT / "data" / "definitions" / "world" / "nations_source.csv"
SCENARIO_NATIONS = ROOT / "data" / "definitions" / "scenario" / "nations.json"

RETRIEVED = "2026-11-18"
CROPS = ("wheat", "rice", "maize", "potato", "soy")

# Culturas de grão que o APHLIS publica (batata e soja não existem lá).
APHLIS_CROPS = ("wheat", "rice", "maize")

# Países do seletor de localidade do APHLIS (https://www.aphlis.net/, lido em
# 2026-11-18): só a África Subsaariana tem estimativas por país. Djibouti,
# Egito, Líbia, Argélia, Marrocos e Tunísia NÃO estão no APHLIS.
APHLIS_COUNTRIES = frozenset(
    "AO BJ BW BF BI CM CF TD CG CI CD GQ ER SZ ET GA GM GH GN GW KE LS LR MG "
    "MW ML MR MZ NA NE NG RW SN SL SO ZA SS SD TZ TG UG ZM ZW".split()
)

APHLIS_URL = "https://www.aphlis.net/en/data/tables/value-chain/{iso2}/{crop}/all-years?metric=prc"
FAO_PDF_URL = "https://www.fao.org/3/i2697e/i2697e.pdf"

# ---------------------------------------------------------------------------
# FAO 2011 — Annex 4 (p. 26-27): % de perda na etapa "postharvest handling
# and storage", por região e grupo de commodities (valor de referência).
#
# Literal de:
#   Gustavsson, J., Cederberg, C., Sonesson, U., van Otterdijk, R. & Meybeck, A.
#   2011. Global food losses and food waste. FAO, Rome. Annex 4.
# ---------------------------------------------------------------------------

FAO_REGIONS = {
    "europe": ("Europa (inclui Rússia)", 26),
    "namerica_oceania": ("América do Norte e Oceania", 26),
    "industrialized_asia": ("Ásia industrializada", 26),
    "ssa": ("África Subsaariana", 27),
    "na_wca": ("Norte da África, Ásia Ocidental e Central", 27),
    "sse_asia": ("Sul e Sudeste Asiático", 27),
    "latam": ("América Latina", 27),
}

# grupo de commodities -> valor da etapa pós-colheita (% do que entra na etapa)
FAO_2011_POSTHARVEST = {
    "europe": {"cereals": 4.0, "roots": 9.0, "oilseeds": 1.0},
    "namerica_oceania": {"cereals": 2.0, "roots": 10.0, "oilseeds": 0.0},
    "industrialized_asia": {"cereals": 10.0, "roots": 7.0, "oilseeds": 3.0},
    "ssa": {"cereals": 8.0, "roots": 18.0, "oilseeds": 8.0},
    "na_wca": {"cereals": 8.0, "roots": 10.0, "oilseeds": 6.0},
    "sse_asia": {"cereals": 7.0, "roots": 19.0, "oilseeds": 12.0},
    "latam": {"cereals": 4.0, "roots": 14.0, "oilseeds": 3.0},
}

FAO_GROUP_BY_CROP = {
    "wheat": "cereals",
    "rice": "cereals",
    "maize": "cereals",
    "potato": "roots",
    "soy": "oilseeds",
}

# Annex 1 (p. 23) — países por região, transcrito na ordem do documento.
FAO_ANNEX1 = {
    "europe": (
        "ALB FRA NED ARM GEO NOR AUT DEU POL AZE GRC PRT BLR HUN ROU BEL ISL "
        "RUS BIH IRL SRB BGR ITA SVK HRV LVA SVN CYP LTU ESP CZE LUX SWE DNK "
        "MKD CHE EST MDA UKR FIN MNE GBR"
    ),
    "namerica_oceania": "AUS CAN NZL USA",
    "industrialized_asia": "JPN CHN KOR",
    "ssa": (
        "AGO LBR BEN MWI BWA MLI BFA MRT BDI MOZ CMR NAM CAF NER TCD NGA COG "
        "RWA COD SEN CIV SLE GNQ SOM ERI ZAF ETH SDN GAB SWZ GMB TZA GHA TGO "
        "GIN UGA GNB ZMB KEN ZWE LSO"
    ),
    "na_wca": (
        "DZA EGY IRQ ISR JOR KAZ KWT KGZ LBN LBY MNG MAR OMN SAU SYR TJK TUN "
        "TUR TKM ARE UZB YEM"
    ),
    "sse_asia": (
        "AFG BGD BTN KHM IND IDN IRN LAO MYS MMR NPL PAK PHL LKA THA VNM"
    ),
    "latam": (
        "ARG BLZ BOL BRA CHL COL CRI CUB DOM ECU SLV GTM GUY HTI HND JAM MEX "
        "NIC PAN PRY PER SUR URY VEN"
    ),
}

# ISO2 ausente no nations_source.csv (Somalilândia não tem ISO2 no APHLIS).
ISO2_OVERRIDES = {"SSD": "SS", "SOL": None}

# Rótulos do APHLIS (tabela "value chain").
APHLIS_TOTAL_LABEL = "All steps"
APHLIS_STORAGE_LABELS = ("Household-level storage", "Market storage")

PAUSE_SECONDS = 0.6
HTTP_TIMEOUT = 60
HTTP_RETRIES = 3
USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0 Safari/537.36"
)

# ---------------------------------------------------------------------------
# Leitura do projeto
# ---------------------------------------------------------------------------


def load_json_file(path: Path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def nation_regions() -> dict[str, str]:
    """Região do jogo (`region`) por id de nação, de scenario/nations.json."""
    rows = load_json_file(SCENARIO_NATIONS)
    return {row["id"]: row.get("region") for row in rows}


def iso2_map(wanted: set[str]) -> dict[str, str]:
    """{iso3: iso2} das nações do jogo, de nations_source.csv + overrides."""
    out: dict[str, str] = {}
    with open(NATIONS_SOURCE, encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            iso3 = row.get("iso3")
            if iso3 in wanted and row.get("iso2"):
                out[iso3] = row["iso2"].upper()
    for iso3, iso2 in ISO2_OVERRIDES.items():
        if iso3 in wanted:
            if iso2:
                out[iso3] = iso2
            else:
                out.pop(iso3, None)
    return out


def fao_region_by_nation() -> dict[str, str]:
    """{iso3: região FAO} a partir do Annex 1 transcrito acima."""
    out: dict[str, str] = {}
    for region, iso3s in FAO_ANNEX1.items():
        for iso3 in iso3s.split():
            if iso3 in out:
                raise SystemExit(f"FAO Annex 1: {iso3} duplicado ({out[iso3]} e {region})")
            out[iso3] = region
    return out


# ---------------------------------------------------------------------------
# Cache bruto (NÃO versionado) + download
# ---------------------------------------------------------------------------


def cache_targets(iso2_by_nation: dict[str, str], game: dict[str, str]) -> list[tuple[str, str]]:
    """Pares (iso2, crop) que devem existir no cache do APHLIS."""
    targets = []
    for iso3 in sorted(game):
        iso2 = iso2_by_nation.get(iso3)
        if iso2 and iso2 in APHLIS_COUNTRIES:
            for crop in APHLIS_CROPS:
                targets.append((iso2, crop))
    return targets


def http_get(url: str) -> bytes:
    last_error: Exception | None = None
    for attempt in range(1, HTTP_RETRIES + 1):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
                return response.read()
        except (urllib.error.URLError, TimeoutError) as error:  # pragma: no cover - rede
            last_error = error
            if attempt < HTTP_RETRIES:
                time.sleep(PAUSE_SECONDS * attempt * 3)
    raise SystemExit(f"falha ao baixar {url}: {last_error}")


def ensure_cache(fetch: bool, targets: list[tuple[str, str]]) -> None:
    """Garante o cache do APHLIS (e o PDF da FAO) — baixa só com --fetch."""
    APHLIS_DIR.mkdir(parents=True, exist_ok=True)
    missing = [
        (iso2, crop)
        for iso2, crop in targets
        if not (APHLIS_DIR / f"{iso2}_{crop}.html").exists()
    ]
    missing_pdf = not (RAW_DIR / "FAO_2011_i2697e.pdf").exists()

    if not fetch:
        if missing:
            raise SystemExit(
                f"cache incompleto em tools/_tmp_agri_raw/aphlis: faltam {len(missing)} "
                f"de {len(targets)} páginas (ex.: {missing[0][0]} {missing[0][1]}) — "
                "rode com --fetch para baixar"
            )
        return

    print(f"[fetch] APHLIS: {len(missing)} páginas a baixar (de {len(targets)})")
    for index, (iso2, crop) in enumerate(missing, 1):
        url = APHLIS_URL.format(iso2=iso2, crop=crop)
        payload = http_get(url)
        (APHLIS_DIR / f"{iso2}_{crop}.html").write_bytes(payload)
        print(f"[fetch] {index:>3}/{len(missing)} {iso2} {crop} <- {len(payload)} bytes")
        time.sleep(PAUSE_SECONDS)

    if missing_pdf:
        print(f"[fetch] FAO 2011 <- {FAO_PDF_URL}")
        (RAW_DIR / "FAO_2011_i2697e.pdf").write_bytes(http_get(FAO_PDF_URL))


# ---------------------------------------------------------------------------
# APHLIS — parsing da tabela "value chain" (%)
# ---------------------------------------------------------------------------

CELL_RE = re.compile(r"<t[hd][^>]*>(.*?)</t[hd]>", re.S)
TAG_RE = re.compile(r"<[^>]+>")
YEAR_RE = re.compile(r"^(?:19|20)\d{2}$")
NUMBER_RE = re.compile(r"^-?\d+(?:\.\d+)?$")


def cell_text(cell: str) -> str:
    return re.sub(r"\s+", " ", html_lib.unescape(TAG_RE.sub("", cell))).strip()


def parse_value_chain(source: str) -> dict[str, dict[int, float | None]]:
    """{rótulo da etapa: {ano: valor}} da tabela APHLIS."""
    head_start = source.find("<thead>")
    head_end = source.find("</thead>")
    body_start = source.find("<tbody>")
    body_end = source.find("</tbody>")
    if min(head_start, head_end, body_start, body_end) < 0:
        raise SystemExit("HTML do APHLIS sem <thead>/<tbody>: cache corrompido, rode --fetch")

    header_rows = re.findall(r"<tr.*?</tr>", source[head_start:head_end], re.S)
    years: list[int] = []
    for row in reversed(header_rows):
        cells = [cell_text(c) for c in CELL_RE.findall(row)]
        years = [int(c) for c in cells if YEAR_RE.match(c)]
        if years:
            break
    if not years:
        raise SystemExit("HTML do APHLIS sem anos no cabeçalho: cache corrompido, rode --fetch")

    table: dict[str, dict[int, float | None]] = {}
    for row in re.findall(r"<tr.*?</tr>", source[body_start:body_end], re.S):
        cells = CELL_RE.findall(row)
        if not cells:
            continue
        label = cell_text(cells[0])
        values = cells[2 : 2 + len(years)]
        if len(values) != len(years):
            continue
        parsed: dict[int, float | None] = {}
        for year, raw in zip(years, values):
            text = cell_text(raw)
            parsed[year] = float(text) if NUMBER_RE.match(text) else None
        table[label] = parsed
    return table


def aphlis_observation(source: str) -> dict | None:
    """Último ano com perda total > 0 e a armazenagem do mesmo ano."""
    table = parse_value_chain(source)
    totals = table.get(APHLIS_TOTAL_LABEL)
    if not totals:
        return None
    for year in sorted(totals, reverse=True):
        total = totals[year]
        if total is None or total <= 0:
            continue
        parts = [
            table.get(label, {}).get(year)
            for label in APHLIS_STORAGE_LABELS
        ]
        numeric = [value for value in parts if value is not None]
        storage = round(sum(numeric), 1) if numeric else None
        return {"loss_pct": round(total, 1), "storage_share_pct": storage, "year": year}
    return None


# ---------------------------------------------------------------------------
# FAO 2011 — verificação dos valores transcritos contra o PDF em cache
# ---------------------------------------------------------------------------

FAO_SECTION_MARKERS = {
    "europe": "for Europe incl. Russia",
    "namerica_oceania": "for North America and Oceania",
    "industrialized_asia": "for Industrialized Asia",
    "ssa": "for sub-Saharan Africa",
    "na_wca": "for North Africa, West and Central Asia",
    "sse_asia": "for South and Southeast Asia",
    "latam": "for Latin America",
}

FAO_LINE_PREFIX = {
    "cereals": r"cereals",
    "roots": r"roots\s+and\s+tubers",
    # No PDF da FAO a linha da África Subsaariana traz "Oilseeds an pulses"
    # (sic); o padrão aceita a falta do "d".
    "oilseeds": r"oilseeds\s+an(?:d)?\s+pulses",
}


def pdf_text() -> str | None:
    """Texto do PDF da FAO em cache, ou None se indisponível."""
    path = RAW_DIR / "FAO_2011_i2697e.pdf"
    if not path.exists():
        return None
    try:
        from pypdf import PdfReader  # import local: dependência só desta checagem
    except ImportError:
        return None
    try:
        reader = PdfReader(str(path))
    except Exception:  # pragma: no cover - PDF corrompido
        return None
    return "\n".join((page.extract_text() or "") for page in reader.pages)


def verify_fao_constants() -> tuple[int, list[str]]:
    """Compara FAO_2011_POSTHARVEST com o Annex 4 do PDF em cache.

    Retorna (quantos valores foram conferidos, erros).
    """
    text = pdf_text()
    if text is None:
        print("[fao] verificação do PDF não executada (cache ausente ou pypdf indisponível)")
        return 0, []

    # O texto do PDF quebra os títulos das seções no meio; normaliza tudo para
    # uma linha só e recorta cada seção entre o seu título e o próximo.
    flat = re.sub(r"\s+", " ", text)
    prefix = "Estimated/assumed waste percentages for each commodity group in each step of the FSC "
    marks = []
    for region, marker in FAO_SECTION_MARKERS.items():
        position = flat.find(prefix + marker)
        if position >= 0:
            marks.append((position, region))
    marks.sort()
    if len(marks) != len(FAO_SECTION_MARKERS):
        found = {region for _, region in marks}
        missing = sorted(set(FAO_SECTION_MARKERS) - found)
        return 0, [f"FAO PDF: seção(ões) não encontrada(s): {', '.join(missing)}"]

    sections: dict[str, str] = {}
    for index, (position, region) in enumerate(marks):
        end = marks[index + 1][0] if index + 1 < len(marks) else position + 1500
        sections[region] = flat[position:end]

    checked = 0
    errors: list[str] = []
    for region, groups in FAO_2011_POSTHARVEST.items():
        block = sections[region]
        for group, expected in groups.items():
            label = FAO_LINE_PREFIX[group]
            match = re.search(
                label + r"\s+([0-9][0-9.,%\s]*)",
                block,
                re.IGNORECASE,
            )
            if match is None:
                errors.append(f"FAO PDF: linha '{label}' ausente na seção {region}")
                continue
            numbers = [float(v) for v in re.findall(r"(\d+(?:\.\d+)?)%", match.group(1))]
            if len(numbers) < 2:
                errors.append(f"FAO PDF: linha '{label}' sem 2 percentagens ({region})")
                continue
            checked += 1
            if abs(numbers[1] - expected) > 1e-9:
                errors.append(
                    f"FAO PDF: {region}/{group} = {numbers[1]}% no PDF, "
                    f"{expected}% na constante"
                )
    return checked, errors


# ---------------------------------------------------------------------------
# Montagem do JSON
# ---------------------------------------------------------------------------


def aphlis_entry(iso3: str, crop: str, iso2: str, observation: dict) -> dict:
    url = APHLIS_URL.format(iso2=iso2, crop=crop)
    return {
        "loss_pct": observation["loss_pct"],
        "storage_share_pct": observation["storage_share_pct"],
        "source": "APHLIS",
        "year": observation["year"],
        "data_status": "observed",
        "method": None,
        "refs": [{"title": f"APHLIS {iso3} {crop} — value chain, % de perda", "url": url}],
    }


def fao_entry(crop: str, fao_region: str) -> dict:
    group = FAO_GROUP_BY_CROP[crop]
    label, page = FAO_REGIONS[fao_region]
    value = FAO_2011_POSTHARVEST[fao_region][group]
    group_pt = {"cereals": "cereais", "roots": "raízes e tubérculos", "oilseeds": "oleaginosas e leguminosas"}[group]
    return {
        "loss_pct": round(value, 1),
        "storage_share_pct": None,
        "source": "FAO_2011",
        "year": 2011,
        "data_status": "regional_observed",
        "method": (
            f"FAO 2011 Annex 4, etapa 'postharvest handling and storage', grupo {group_pt}, "
            f"região '{label}' (taxa REGIONAL do grupo de culturas, não medição do país; "
            "% do que entra na etapa; a fonte não separa manuseio de armazenagem)"
        ),
        "refs": [
            {
                "title": f"FAO 2011 (Gustavsson et al.), Annex 4, p. {page} — {label}",
                "url": FAO_PDF_URL,
            }
        ],
    }


def rule_entry(crop: str, region: str, observed: dict[tuple[str, str], dict]) -> dict:
    regional = [
        row["loss_pct"]
        for (iso3, obs_crop), row in observed.items()
        if obs_crop == crop and REGIONS[iso3] == region
    ]
    if regional:
        values = regional
        basis = f"mediana de {len(values)} observações da região '{region}' para {crop}"
    else:
        values = [row["loss_pct"] for (_, obs_crop), row in observed.items() if obs_crop == crop]
        if not values:
            return {
                "loss_pct": None,
                "storage_share_pct": None,
                "source": "NONE",
                "year": None,
                "data_status": "no_data",
                "method": f"sem observação em '{region}' e sem mediana global para {crop}",
                "refs": [],
            }
        basis = f"mediana global de {len(values)} observações para {crop} (região '{region}' sem observação)"

    return {
        "loss_pct": round(statistics.median(values), 1),
        "storage_share_pct": None,
        "source": "RULE_REGIONAL_MEDIAN",
        "year": None,
        "data_status": "regional_default",
        "method": f"Regra declarada: {basis}; os observados vêm de APHLIS e FAO 2011",
        "refs": [],
    }


REGIONS: dict[str, str] = {}


def build(game: dict[str, str], iso2_by_nation: dict[str, str], fao_map: dict[str, str]):
    observed: dict[tuple[str, str], dict] = {}
    aphlis_rows: dict[tuple[str, str], dict] = {}
    fao_rows: dict[tuple[str, str], dict] = {}

    for iso3 in sorted(game):
        iso2 = iso2_by_nation.get(iso3)
        if not iso2 or iso2 not in APHLIS_COUNTRIES:
            continue
        for crop in APHLIS_CROPS:
            path = APHLIS_DIR / f"{iso2}_{crop}.html"
            observation = aphlis_observation(path.read_text(encoding="utf-8", errors="replace"))
            if observation is None:
                continue
            row = aphlis_entry(iso3, crop, iso2, observation)
            aphlis_rows[(iso3, crop)] = row
            observed[(iso3, crop)] = row

    for iso3 in sorted(game):
        fao_region = fao_map.get(iso3)
        if not fao_region:
            continue
        for crop in CROPS:
            if (iso3, crop) in observed:
                continue
            row = fao_entry(crop, fao_region)
            fao_rows[(iso3, crop)] = row
            observed[(iso3, crop)] = row

    nations_block: dict[str, dict] = {}
    for iso3 in sorted(game):
        crops_block: dict[str, dict] = {}
        for crop in CROPS:
            row = observed.get((iso3, crop))
            if row is None:
                row = rule_entry(crop, REGIONS[iso3], observed)
            crops_block[crop] = row
        nations_block[iso3] = {
            "name": game[iso3],
            "region": REGIONS[iso3],
            "crops": crops_block,
        }

    payload = {
        "_meta": {
            "dataset": "postharvest_losses",
            "version": "g1",
            "generated_by": "tools/build_agri_losses.py",
            "retrieved": RETRIEVED,
            "nations": len(nations_block),
            "crops": list(CROPS),
            "unit": (
                "% da produção que se perde entre colheita e consumo "
                "(manuseio+secagem+armazenagem+transporte)"
            ),
            "rule": (
                "país sem observação herda a mediana dos observados da mesma região do jogo; "
                "sem observado na região, mediana global"
            ),
            "data_status": {
                "observed": (
                    "medição/estimativa do próprio país e cultura (APHLIS)"
                ),
                "regional_observed": (
                    "taxa publicada para a região e o grupo de culturas, aplicada ao país "
                    "(FAO 2011); não é medição nacional"
                ),
                "regional_default": (
                    "país sem fonte: mediana dos observados da mesma região do jogo "
                    "(ou global) — regra declarada"
                ),
                "no_data": (
                    "sem observação e sem mediana (não ocorre nesta versão)"
                ),
            },
        },
        "sources": {
            "APHLIS": {
                "title": "APHLIS postharvest loss estimates",
                "publisher": "APHLIS",
                "url": "https://www.aphlis.net/",
                "license": "CC BY (conferir na fonte e declarar)",
                "accessed": RETRIEVED,
            },
            "FAO_2011": {
                "title": "Global food losses and food waste",
                "publisher": "FAO",
                "year": 2011,
                "url": FAO_PDF_URL,
            },
            "RULE_REGIONAL_MEDIAN": {
                "title": "Regra declarada: mediana regional",
                "publisher": "OpenGS project",
                "year": 2026,
            },
        },
        "nations": nations_block,
    }
    return payload, aphlis_rows, fao_rows


# ---------------------------------------------------------------------------
# Autoverificação / relatórios
# ---------------------------------------------------------------------------


def entry_rows(payload: dict):
    for iso3, nation in payload["nations"].items():
        for crop, row in nation["crops"].items():
            yield iso3, crop, row


def autocheck(payload: dict, aphlis_rows: dict, fao_rows: dict) -> list[str]:
    errors: list[str] = []
    nations_block = payload["nations"]

    if len(nations_block) != 179:
        errors.append(f"{len(nations_block)} nações (esperado 179)")

    total_entries = sum(len(n["crops"]) for n in nations_block.values())
    if total_entries != 179 * len(CROPS):
        errors.append(f"{total_entries} entradas (esperado {179 * len(CROPS)})")

    valid_status = {"observed", "regional_observed", "regional_default", "no_data"}
    sourced = {"observed", "regional_observed"}
    for iso3, nation in nations_block.items():
        if list(nation["crops"]) != list(CROPS):
            errors.append(f"{iso3}: culturas fora da ordem/lista fixa")
        if not nation.get("region"):
            errors.append(f"{iso3}: sem região do jogo")
        for crop, row in nation["crops"].items():
            if row["data_status"] not in valid_status:
                errors.append(f"{iso3}/{crop}: data_status inválido {row['data_status']}")
            if not row.get("source"):
                errors.append(f"{iso3}/{crop}: sem source")
            if row["data_status"] in sourced and not row.get("refs"):
                errors.append(f"{iso3}/{crop}: {row['data_status']} sem refs/URL")
            if row["data_status"] not in sourced and row.get("refs"):
                errors.append(f"{iso3}/{crop}: {row['data_status']} com refs")
            if row["data_status"] == "observed" and row["source"] != "APHLIS":
                errors.append(f"{iso3}/{crop}: observed de fonte {row['source']}")
            if row["data_status"] == "regional_observed" and row["source"] != "FAO_2011":
                errors.append(f"{iso3}/{crop}: regional_observed de fonte {row['source']}")
            if row["data_status"] == "regional_default" and row["source"] != "RULE_REGIONAL_MEDIAN":
                errors.append(f"{iso3}/{crop}: regional_default de fonte {row['source']}")
            for field in ("loss_pct", "storage_share_pct"):
                value = row.get(field)
                if value is None:
                    continue
                if not (0.0 <= float(value) <= 100.0):
                    errors.append(f"{iso3}/{crop}: {field}={value} fora de 0..100")
            if row["loss_pct"] is not None and round(float(row["loss_pct"]), 1) != float(row["loss_pct"]):
                errors.append(f"{iso3}/{crop}: loss_pct sem arredondamento a 1 casa")
        if len(nation["crops"]) != len(CROPS):
            errors.append(f"{iso3}: {len(nation['crops'])} culturas (esperado {len(CROPS)})")

    for key, row in aphlis_rows.items():
        if row["source"] != "APHLIS" or row["data_status"] != "observed":
            errors.append(f"APHLIS {key}: status/source incorretos")
    for key, row in fao_rows.items():
        if row["source"] != "FAO_2011" or row["data_status"] != "regional_observed":
            errors.append(f"FAO {key}: status/source incorretos")

    return errors


def print_stats(payload: dict) -> None:
    status_count: dict[str, int] = {}
    source_count: dict[str, int] = {}
    for _, _, row in entry_rows(payload):
        status_count[row["data_status"]] = status_count.get(row["data_status"], 0) + 1
        source_count[row["source"]] = source_count.get(row["source"], 0) + 1

    print("\n[autocheck] contagens")
    print(f"  nações: {len(payload['nations'])}")
    print(f"  entradas: {sum(len(n['crops']) for n in payload['nations'].values())} (179 x 5)")
    print("  por data_status:")
    for status in ("observed", "regional_observed", "regional_default", "no_data"):
        print(f"    {status:<17} {status_count.get(status, 0):>4}")
    print("  por fonte:       " + ", ".join(f"{k}={v}" for k, v in sorted(source_count.items())))

    print("\n[loss_pct por cultura] min / mediana / max (observado APHLIS / regional FAO)")
    for crop in CROPS:
        values = [
            row["loss_pct"]
            for _, row_crop, row in entry_rows(payload)
            if row_crop == crop and row["loss_pct"] is not None
        ]
        observed = sum(
            1
            for _, row_crop, row in entry_rows(payload)
            if row_crop == crop and row["data_status"] == "observed"
        )
        regional = sum(
            1
            for _, row_crop, row in entry_rows(payload)
            if row_crop == crop and row["data_status"] == "regional_observed"
        )
        if not values:
            print(f"  {crop:<7} sem valor")
            continue
        print(
            f"  {crop:<7} {min(values):>5.1f} / {statistics.median(values):>5.1f} / "
            f"{max(values):>5.1f}  ({observed:>3} / {regional:>3})"
        )

    outside = sum(
        1
        for _, _, row in entry_rows(payload)
        if row["loss_pct"] is not None and not (0.0 <= float(row["loss_pct"]) <= 100.0)
    )
    print(f"  valores fora de 0..100: {outside}")


def print_coverage(payload: dict, aphlis_rows: dict, fao_rows: dict, fao_map: dict[str, str]) -> None:
    print("\n[cobertura] observado por fonte")
    for crop in CROPS:
        ap = sum(1 for (_, c) in aphlis_rows if c == crop)
        fa = sum(1 for (_, c) in fao_rows if c == crop)
        rule = sum(
            1
            for _, row_crop, row in entry_rows(payload)
            if row_crop == crop and row["data_status"] == "regional_default"
        )
        nodata = sum(
            1
            for _, row_crop, row in entry_rows(payload)
            if row_crop == crop and row["data_status"] == "no_data"
        )
        print(f"  {crop:<7} APHLIS={ap:>3}  FAO_2011={fa:>3}  regra={rule:>3}  no_data={nodata:>3}")

    print("\n[cobertura] países com observação APHLIS (por país, culturas)")
    per_nation: dict[str, list[str]] = {}
    for (iso3, crop) in sorted(aphlis_rows):
        per_nation.setdefault(iso3, []).append(crop)
    for iso3, crops in per_nation.items():
        print(f"  {iso3}: {', '.join(crops)}")
    print(f"  total: {len(per_nation)} países, {len(aphlis_rows)} pares país/cultura")

    by_region = {
        region: sum(1 for iso3 in iso3s.split() if iso3 in fao_map)
        for region, iso3s in FAO_ANNEX1.items()
    }
    print("\n[cobertura] países mapeados no Annex 1 da FAO por região")
    for region in FAO_REGIONS:
        label, _ = FAO_REGIONS[region]
        print(f"  {region:<20} {by_region.get(region, 0):>3}  ({label})")


def print_rule_gaps(payload: dict) -> None:
    print("\n[cobertura] regra regional (por região do jogo)")
    regions: dict[str, int] = {}
    for iso3, nation in payload["nations"].items():
        for row in nation["crops"].values():
            if row["data_status"] == "regional_default":
                regions[nation["region"]] = regions.get(nation["region"], 0) + 1
    for region, count in sorted(regions.items()):
        print(f"  {region:<15} {count:>3} entradas na regra")


def print_sample(payload: dict) -> None:
    print("\n[amostra]")
    samples = (
        ("NGA", "maize"),
        ("BRA", "soy"),
        ("IND", "rice"),
        ("USA", "maize"),
        ("UKR", "wheat"),
        ("CHN", "rice"),
        ("EGY", "wheat"),
    )
    for iso3, crop in samples:
        row = payload["nations"][iso3]["crops"][crop]
        print(
            f"  {iso3} {crop:<6} loss={row['loss_pct']}% "
            f"storage={row['storage_share_pct']} "
            f"fonte={row['source']} ano={row['year']} status={row['data_status']}"
        )


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    global REGIONS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="baixa o que faltar no cache")
    args = parser.parse_args(argv)

    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    game = nations()
    if len(game) != 179:
        print(f"aviso: nations() devolveu {len(game)} nações (esperado 179)")

    REGIONS = nation_regions()
    iso2_by_nation = iso2_map(set(game))
    fao_map = fao_region_by_nation()

    ensure_cache(args.fetch, cache_targets(iso2_by_nation, game))

    checked, fao_errors = verify_fao_constants()
    if checked:
        print(f"[fao] {checked} valores do Annex 4 conferidos no PDF em cache")

    payload, aphlis_rows, fao_rows = build(game, iso2_by_nation, fao_map)
    write_json(OUT_PATH, payload)

    errors = autocheck(payload, aphlis_rows, fao_rows) + fao_errors
    print("\n[autocheck]")
    if errors:
        for error in errors:
            print(f"  FALHA: {error}")
    else:
        print(
            "  OK: 179 nações, 895 entradas, status/source válidos, 0..100, "
            "refs em todo observed/regional_observed"
        )

    print_stats(payload)
    print_coverage(payload, aphlis_rows, fao_rows, fao_map)
    print_rule_gaps(payload)
    print_sample(payload)
    print(f"\n[postharvest_losses] sha256 {sha256(OUT_PATH)}")
    print(f"[postharvest_losses] arquivo {OUT_PATH.relative_to(ROOT)}")

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
