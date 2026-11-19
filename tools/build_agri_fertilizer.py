#!/usr/bin/env python3
"""Constrói `data/definitions/agriculture/fertilizer_n.json` (Parte G, G1).

Nitrogênio aplicado por hectare e por ano (média sobre a ÁREA TOTAL da cultura),
por nação do jogo x cultura primária de comida (wheat, rice, maize, potato,
soy). É a taxa que liga a agricultura ao gás/energia da Parte E: quanto mais N
por hectare, mais amônia/gás natural a economia precisa para alimentar o campo.

Fonte primária:
    IFA/FAO — "Global data on fertilizer use by crop and by country"
    (Ludemann et al., 2022), levantamento FUBC 2017-2018 + compilação dos
    relatórios históricos. Dados abertos no Dryad,
    doi:10.5061/dryad.2rbnzs7qh (CC0 1.0), arquivos `FUBC_1_to_9_data.csv`,
    `Meta_data_FUBC_1_to_9_data.csv` e `IFA_regions.csv`.
    Cache bruto em `tools/_tmp_agri_raw/fubc/` (NÃO versionado).

Regra de valor por par país/cultura (declarada no `method` de cada entrada):
    1. `Aver_N_rate_kg_ha` da linha, quando existe;
    2. senão `N_rate_kg_ha * N_pc_fert / 100` (taxa só na área fertilizada
       convertida para a área total).
    Vale o ano mais recente <= 2019 que tenha valor calculável por essa regra.
    Os relatórios FUBC 6, 7 e 8 (2006, 2010, 2014) só trazem N total aplicado
    (`N_k_t`), sem taxa nem área; por isso não geram valor (ficam de fora).

Mapeamento de cultura (ids do jogo): Wheat->wheat, Rice->rice, Maize->maize,
Soybeans->soy, Roots/tubers->potato. As demais categorias do levantamento
(Other cereals, Pulses, Fruits, Vegetables, Grassland...) não entram; os nomes
encontrados ficam registrados no `_meta`.

Regra declarada (país sem taxa FUBC):
    mediana dos n_kg_ha observados na MESMA `Region_IFA` (tabela
    `IFA_regions.csv`) e cultura; sem observado na região, mediana global da
    cultura. `data_status: "regional_default"`. País sem `Region_IFA` na tabela
    IFA também cai na mediana global, com o motivo no `method`.

Conferência de ordem de grandeza (não substitui o dado): compara, para seis
países grandes, o N médio de todas as culturas reportadas na FUBC com o
indicador do Banco Mundial AG.CON.FERT.ZS (kg de nutriente por ha de terra
arável, todos os nutrientes). A razão fica no `_meta` e é impressa.

Uso:
    python tools/build_agri_fertilizer.py            # usa o cache
    python tools/build_agri_fertilizer.py --fetch    # (re)baixa o cache

Reprodutibilidade: mesma entrada => mesmos bytes (nações ordenadas por id,
culturas na ordem fixa CROPS, JSON UTF-8 sem BOM, tab, LF). Nada aqui inventa
taxa: onde não há observação nem mediana, `n_kg_ha` fica null e `data_status`
= `no_data` (não ocorre com as fontes atuais).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import math
import re
import statistics
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from agri_common import nations, write_json  # noqa: E402

CACHE_DIR = ROOT / "tools" / "_tmp_agri_raw" / "fubc"
FUBC_CSV = CACHE_DIR / "FUBC_1_to_9_data.csv"
META_CSV = CACHE_DIR / "Meta_data_FUBC_1_to_9_data.csv"
IFA_REGIONS_CSV = CACHE_DIR / "IFA_regions.csv"
WB_CACHE = CACHE_DIR / "worldbank_AG.CON.FERT.ZS.json"
OUT_PATH = ROOT / "data" / "definitions" / "agriculture" / "fertilizer_n.json"
NATIONS_SOURCE = ROOT / "data" / "definitions" / "world" / "nations_source.csv"

RETRIEVED = "2026-11-18"
CROPS = ("wheat", "rice", "maize", "potato", "soy")

#: Último ano aceito (o levantamento 2017/18 traz linhas de 2016 a 2019).
MAX_YEAR = 2019

#: Nomes exatos de `Crop` que alimentam cada id do jogo. Variações de caixa ou
#: de barra do MESMO grupo entram; subcategorias "(other)" ficam de fora.
CROP_LABELS: dict[str, tuple[str, ...]] = {
    "wheat": ("Wheat",),
    "rice": ("Rice",),
    "maize": ("Maize",),
    "potato": ("Roots/tubers", "Roots & tubers", "Roots and Tubers", "Roots/Tubers"),
    "soy": ("Soybeans",),
}

#: Países da conferência de sanidade com o Banco Mundial.
WB_ISO3 = ("BRA", "USA", "IND", "CHN", "FRA", "NGA")

DATASET_API = "https://datadryad.org/api/v2/datasets/doi%3A10.5061%2Fdryad.2rbnzs7qh"
DRYAD_BASE = "https://datadryad.org"
DATASET_URL = "https://datadryad.org/dataset/doi:10.5061/dryad.2rbnzs7qh"
DOI = "10.5061/dryad.2rbnzs7qh"

#: Arquivos que o cache precisa ter (extraídos do .zip da versão do dataset).
ZIP_FILES = (
    "FUBC_1_to_9_data.csv",
    "Meta_data_FUBC_1_to_9_data.csv",
    "IFA_regions.csv",
    "Meta_data_IFA_regions.csv",
    "README_LUDEMANN_FUBC_DATA_2022.txt",
)

WB_URL = (
    "https://api.worldbank.org/v2/country/" + ";".join(WB_ISO3)
    + "/indicator/AG.CON.FERT.ZS?format=json&per_page=1000&date=2000:2026"
)

PAUSE_SECONDS = 0.6
HTTP_TIMEOUT = 120
HTTP_RETRIES = 3
USER_AGENT = "OpenGS-game-data-build/1.0 (agriculture dataset; research)"

# ---------------------------------------------------------------------------
# HTTP + cache
# ---------------------------------------------------------------------------


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


def http_json(url: str):
    payload = http_get(url)
    try:
        return json.loads(payload)
    except json.JSONDecodeError as error:
        raise SystemExit(f"resposta não é JSON em {url}: {error}") from error


def fetch_fubc() -> None:
    """Baixa o .zip da versão do dataset no Dryad e confere o sha256 de cada arquivo."""
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    meta = http_json(DATASET_API)
    version_href = meta["_links"]["stash:version"]["href"]
    listing = http_json(DRYAD_BASE + version_href + "/files")
    digests = {row["path"]: row["digest"] for row in listing["_embedded"]["stash:files"]}

    print(f"[fetch] IFA/FUBC <- {DRYAD_BASE + version_href + '/download'}")
    payload = http_get(DRYAD_BASE + version_href + "/download")
    print(f"[fetch] zip {len(payload)} bytes")
    with zipfile.ZipFile(io.BytesIO(payload)) as archive:
        for name in ZIP_FILES:
            data = archive.read(name)
            actual = hashlib.sha256(data).hexdigest()
            expected = digests.get(name)
            if expected and actual != expected:
                raise SystemExit(
                    f"[fetch] sha256 de {name} diverge do Dryad: {actual} != {expected}"
                )
            (CACHE_DIR / name).write_bytes(data)
            print(f"[fetch] {name:<38} {len(data):>9} bytes  sha256 {actual[:16]}")
            time.sleep(PAUSE_SECONDS)


def fetch_worldbank() -> None:
    """Baixa a série AG.CON.FERT.ZS dos seis países da conferência de sanidade."""
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    payload = http_get(WB_URL)
    # Guarda o JSON cru (indentado) para o cache ser auditável e determinístico.
    parsed = json.loads(payload)
    WB_CACHE.write_text(
        json.dumps(parsed, ensure_ascii=False, indent="\t") + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(f"[fetch] Banco Mundial AG.CON.FERT.ZS -> {WB_CACHE.name} ({len(payload)} bytes)")


def ensure_cache(fetch: bool) -> None:
    if fetch:
        fetch_fubc()
        fetch_worldbank()

    required = [FUBC_CSV, META_CSV, IFA_REGIONS_CSV, WB_CACHE]
    missing = [path.name for path in required if not path.exists()]
    if missing:
        raise SystemExit(
            "cache incompleto em tools/_tmp_agri_raw/fubc: falta(m) "
            + ", ".join(missing)
            + " — rode com --fetch para baixar"
        )


# ---------------------------------------------------------------------------
# Leitura do projeto
# ---------------------------------------------------------------------------


def load_json_file(path: Path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def english_names() -> dict[str, str]:
    """{iso3 do jogo: nome inglês} de data/definitions/world/nations_source.csv."""
    out: dict[str, str] = {}
    with open(NATIONS_SOURCE, encoding="utf-8", newline="") as fh:
        for row in csv.DictReader(fh):
            iso3 = row.get("iso3")
            name = (row.get("name_en") or "").strip()
            if iso3 and name:
                out[iso3] = name
    return out


def game_name_index(game: dict[str, str]) -> dict[str, str]:
    """{nome inglês em minúsculas: iso3} só para as nações do jogo.

    O nations_source.csv tem mais países que o cenário; casar por nome sem
    filtrar traria países que o jogo não tem (ex.: Mauritius/MUS).
    """
    return {
        name.casefold(): iso3
        for iso3, name in english_names().items()
        if iso3 in game
    }


def ifa_regions() -> dict[str, str]:
    """{iso3: Region_IFA} da tabela IFA (arquivo em latin-1, com nomes acentuados)."""
    out: dict[str, str] = {}
    with open(IFA_REGIONS_CSV, encoding="latin-1", newline="") as fh:
        for row in csv.DictReader(fh):
            iso3 = (row.get("ISO3_code") or "").strip()
            region = (row.get("Region_IFA") or "").strip()
            if iso3 and region:
                out[iso3] = region
    return out


def ifa_region_names() -> dict[str, str]:
    """{nome do país na tabela IFA: iso3} para o fallback por nome."""
    out: dict[str, str] = {}
    with open(IFA_REGIONS_CSV, encoding="latin-1", newline="") as fh:
        for row in csv.DictReader(fh):
            iso3 = (row.get("ISO3_code") or "").strip()
            name = (row.get("Country") or "").strip()
            if iso3 and name:
                out[name.casefold()] = iso3
    return out


# ---------------------------------------------------------------------------
# Parsing da FUBC
# ---------------------------------------------------------------------------

YEAR_RE = re.compile(r"^(\d{4})")
MISSING_TOKENS = {"", "NA", "N/A", "-", "NAN", "NULL"}


def parse_year(text: str) -> int | None:
    match = YEAR_RE.match((text or "").strip())
    return int(match.group(1)) if match else None


def parse_number(text: str) -> tuple[float | None, str]:
    """(valor, texto cru) — texto vazio/NA vira None; nunca inventa zero."""
    raw = (text or "").strip()
    if raw.upper() in MISSING_TOKENS:
        return None, raw
    try:
        value = float(raw)
    except ValueError:
        return None, raw
    if not math.isfinite(value):
        return None, raw
    return value, raw


def load_fubc_rows() -> list[dict]:
    with open(FUBC_CSV, encoding="utf-8-sig", newline="") as fh:
        reader = csv.DictReader(fh)
        expected = {
            "Country",
            "ISO3_code",
            "Region_IFA",
            "Year",
            "FUBC_report_number",
            "Year_FUBC_publication",
            "Crop",
            "Crop_area_k_ha",
            "N_k_t",
            "N_rate_kg_ha",
            "N_pc_fert",
            "Aver_N_rate_kg_ha",
        }
        missing = expected - set(reader.fieldnames or ())
        if missing:
            raise SystemExit(f"{FUBC_CSV.name}: colunas ausentes {sorted(missing)}")
        return list(reader)


def check_metadata() -> None:
    """Confere que as colunas usadas estão descritas no metadado do Dryad."""
    wanted = {
        "Country",
        "ISO3_code",
        "Region_IFA",
        "Year",
        "Crop",
        "N_rate_kg_ha",
        "N_pc_fert",
        "Aver_N_rate_kg_ha",
    }
    with open(META_CSV, encoding="utf-8-sig", newline="") as fh:
        described = {row.get("Parameter") for row in csv.DictReader(fh)}
    missing = wanted - described
    if missing:
        raise SystemExit(
            f"{META_CSV.name}: metadado não descreve {sorted(missing)} — cache trocado?"
        )
    print(f"[meta] metadado do Dryad descreve as {len(wanted)} colunas usadas")


# ---------------------------------------------------------------------------
# Casamento país FUBC <-> nação do jogo
# ---------------------------------------------------------------------------


def fubc_nation_index(rows: list[dict], game: dict[str, str]) -> dict[str, dict]:
    """{iso3 do jogo: {country, region_ifa}} — o que a FUBC cobre do jogo.

    Primeiro pelo ISO3_code; se ele não existir no jogo, pelo nome inglês
    oficial (`Country` da FUBC x `name_en` do nations_source.csv). Nada de
    alias silencioso: o que não casar fica de fora e é listado por
    `unmatched_fubc_countries`.
    """
    by_name = game_name_index(game)
    out: dict[str, dict] = {}
    for row in rows:
        iso3_raw = (row["ISO3_code"] or "").strip()
        country = (row["Country"] or "").strip()
        iso3 = iso3_raw if iso3_raw in game else by_name.get(country.casefold())
        if iso3 is None:
            continue
        out.setdefault(iso3, {"country": country, "region_ifa": (row["Region_IFA"] or "").strip()})
    return out


def unmatched_fubc_countries(rows: list[dict], game: dict[str, str]) -> list[tuple[str, str]]:
    """Países da FUBC que não casam com nenhuma nação do jogo (por ISO3 nem nome)."""
    by_name = game_name_index(game)
    found: set[tuple[str, str]] = set()
    for row in rows:
        iso3_raw = (row["ISO3_code"] or "").strip()
        country = (row["Country"] or "").strip()
        if iso3_raw in game or by_name.get(country.casefold()) in game:
            continue
        found.add((iso3_raw, country))
    return sorted(found)


def region_divergences(index: dict[str, dict]) -> list[str]:
    """Regiões em que a FUBC e o IFA_regions.csv não concordam (evidência)."""
    ifa = ifa_regions()
    out = []
    for iso3, row in sorted(index.items()):
        fubc_region = row.get("region_ifa") or ""
        ifa_region = ifa.get(iso3)
        if ifa_region and fubc_region and ifa_region != fubc_region:
            out.append(f"{iso3}: FUBC '{fubc_region}' x IFA '{ifa_region}'")
    return out


def nation_regions(game: dict[str, str]) -> tuple[dict[str, str | None], list[str]]:
    """{iso3 do jogo: Region_IFA ou None} e a lista de nações sem região IFA."""
    ifa = ifa_regions()
    ifa_names = ifa_region_names()
    names = english_names()

    out: dict[str, str | None] = {}
    unassigned: list[str] = []
    for iso3 in game:
        region = ifa.get(iso3)
        if region is None:
            fallback_iso3 = ifa_names.get((names.get(iso3) or "").casefold())
            if fallback_iso3:
                region = ifa.get(fallback_iso3)
        out[iso3] = region
        if region is None:
            unassigned.append(iso3)
    return out, unassigned


# ---------------------------------------------------------------------------
# Seleção das observações
# ---------------------------------------------------------------------------


def label_to_crop() -> dict[str, str]:
    return {label: crop for crop, labels in CROP_LABELS.items() for label in labels}


def pick_observations(rows: list[dict], game: dict[str, str], index: dict[str, dict]) -> dict[tuple[str, str], dict]:
    """{(iso3, crop): observação} — a linha mais recente (<= 2019) com valor.

    Desempate no mesmo ano: maior FUBC_report_number; se ainda empatar, a última
    linha do arquivo (ordem fixa do CSV, portanto determinístico).
    """
    by_label = label_to_crop()
    by_name = game_name_index(game)

    best: dict[tuple[str, str], dict] = {}
    for index_row, row in enumerate(rows):
        label = (row["Crop"] or "").strip()
        crop = by_label.get(label)
        if crop is None:
            continue
        iso3_raw = (row["ISO3_code"] or "").strip()
        iso3 = iso3_raw if iso3_raw in game else by_name.get((row["Country"] or "").strip().casefold())
        if iso3 is None or iso3 not in index:
            continue

        year = parse_year(row["Year"])
        if year is None or year > MAX_YEAR:
            continue
        report_text = (row["FUBC_report_number"] or "").strip()
        report = int(report_text) if report_text.isdigit() else 0
        published = parse_year(row.get("Year_FUBC_publication", ""))

        aver, aver_raw = parse_number(row["Aver_N_rate_kg_ha"])
        rate, rate_raw = parse_number(row["N_rate_kg_ha"])
        pc, pc_raw = parse_number(row["N_pc_fert"])
        if aver is not None:
            value, mode = aver, "aver"
        elif rate is not None and pc is not None:
            value, mode = rate * pc / 100.0, "calc"
        else:
            continue
        if value < 0:
            raise SystemExit(
                f"taxa negativa em {iso3}/{crop} {year}: {value} — cache corrompido?"
            )

        key = (iso3, crop)
        candidate = (year, report, index_row)
        current = best.get(key)
        if current is not None and candidate <= current["_sort"]:
            continue
        best[key] = {
            "_sort": candidate,
            "value": value,
            "year": year,
            "report": report,
            "published": published,
            "label": label,
            "country": row["Country"],
            "region": index[iso3]["region_ifa"],
            "mode": mode,
            "aver_raw": aver_raw,
            "rate_raw": rate_raw,
            "pc_raw": pc_raw,
            "area_raw": (row["Crop_area_k_ha"] or "").strip(),
            "n_k_t_raw": (row["N_k_t"] or "").strip(),
        }
    return best


def duplicate_pairs(rows: list[dict], game: dict[str, str]) -> list[str]:
    """Pares país/cultura/ano com mais de uma linha (evidência para o relatório)."""
    by_label = label_to_crop()
    by_name = game_name_index(game)
    seen: dict[tuple[str, str, int], set[tuple[str, float]]] = {}
    for row in rows:
        crop = by_label.get((row["Crop"] or "").strip())
        if crop is None:
            continue
        iso3 = (row["ISO3_code"] or "").strip()
        if iso3 not in game:
            iso3 = by_name.get((row["Country"] or "").strip().casefold(), "")
        if iso3 not in game:
            continue
        year = parse_year(row["Year"])
        if year is None:
            continue
        aver, _ = parse_number(row["Aver_N_rate_kg_ha"])
        rate, _ = parse_number(row["N_rate_kg_ha"])
        pc, _ = parse_number(row["N_pc_fert"])
        value = aver if aver is not None else (rate * pc / 100.0 if rate is not None and pc is not None else None)
        if value is None:
            continue
        key = (iso3, crop, year)
        seen.setdefault(key, set()).add(((row["FUBC_report_number"] or "").strip(), round(value, 4)))
    return [
        f"{iso3}/{crop}/{year}: {len(values)} linhas com valores distintos "
        f"{sorted(v for _, v in values)}"
        for (iso3, crop, year), values in sorted(seen.items())
        if len({round(v, 4) for _, v in values}) > 1
    ]


# ---------------------------------------------------------------------------
# Montagem do JSON
# ---------------------------------------------------------------------------


def round1(value: float) -> float:
    return round(float(value) + 0.0, 1)


def observed_entry(observation: dict) -> dict:
    report = observation["report"]
    if observation["mode"] == "aver":
        method = None
    else:
        method = (
            "regra declarada: N_rate_kg_ha x N_pc_fert/100 "
            f"({observation['rate_raw']} x {observation['pc_raw']}% = "
            f"{observation['value']:.1f}), pois Aver_N_rate_kg_ha vem 'NA' na linha"
        )
    if report and report < 9:
        published = observation.get("published")
        origin = (
            f"relatório FUBC {report}, publicado em {published}"
            if published
            else f"relatório FUBC {report}"
        )
        notes = (
            f"dado de {observation['year']} ({origin}); é a taxa mais "
            "recente que a FUBC publica para este par país/cultura"
        )
    else:
        notes = None
    return {
        "n_kg_ha": round1(observation["value"]),
        "year": observation["year"],
        "source": "IFA_FUBC_2018" if report == 9 else "IFA_FUBC_HISTORIC",
        "data_status": "observed",
        "method": method,
        "notes": notes,
    }


def value_pools(observed: dict[tuple[str, str], dict], regions: dict[str, str | None]) -> dict[str, dict]:
    """{crop: {'by_region': {region: [valores]}, 'global': [valores]}}."""
    pools: dict[str, dict] = {
        crop: {"by_region": {}, "global": []} for crop in CROPS
    }
    for (iso3, crop), observation in observed.items():
        value = round1(observation["value"])
        pools[crop]["global"].append(value)
        region = regions.get(iso3) or observation["region"]
        if region:
            pools[crop]["by_region"].setdefault(region, []).append(value)
    return pools


def rule_entry(crop: str, region: str | None, pools: dict) -> dict:
    global_values = pools[crop]["global"]
    regional_values = pools[crop]["by_region"].get(region, []) if region else []

    if regional_values:
        value = statistics.median(regional_values)
        method = (
            f"Regra declarada: mediana de {len(regional_values)} observações da "
            f"região IFA '{region}' para {crop}, em n_kg_ha médio sobre a área total "
            "da cultura; o país não tem taxa FUBC própria"
        )
    elif global_values:
        value = statistics.median(global_values)
        if region:
            basis = f"a região IFA '{region}' não tem observação de {crop}"
        else:
            basis = "o país não consta da tabela de regiões IFA (IFA_regions.csv)"
        method = (
            f"Regra declarada: {basis}; mediana global de {len(global_values)} "
            f"observações de {crop} (kg N/ha sobre a área total da cultura)"
        )
    else:
        return {
            "n_kg_ha": None,
            "year": None,
            "source": "NONE",
            "data_status": "no_data",
            "method": f"sem observação em '{region}' e sem mediana global para {crop}",
            "notes": "ausência explícita: nenhuma fonte cobre o par",
        }

    return {
        "n_kg_ha": round1(value),
        "year": None,
        "source": "RULE_REGIONAL_MEDIAN",
        "data_status": "regional_default",
        "method": method,
        "notes": None,
    }


def crop_labels_found(rows: list[dict]) -> tuple[dict[str, list[str]], list[str]]:
    """{crop: rótulos usados} e a lista ordenada dos rótulos que NÃO entram."""
    used: dict[str, list[str]] = {}
    by_label = label_to_crop()
    found = {str(row["Crop"]).strip() for row in rows}
    for crop in CROPS:
        used[crop] = [label for label in found if by_label.get(label) == crop]
    excluded = sorted(label for label in found if label not in by_label)
    return used, excluded


def build(game: dict[str, str], observed: dict[tuple[str, str], dict], regions: dict[str, str | None]):
    pools = value_pools(observed, regions)
    nations_block: dict[str, dict] = {}

    for iso3 in sorted(game):
        crops_block: dict[str, dict] = {}
        for crop in CROPS:
            observation = observed.get((iso3, crop))
            if observation is not None:
                crops_block[crop] = observed_entry(observation)
            else:
                crops_block[crop] = rule_entry(crop, regions[iso3], pools)
        nations_block[iso3] = {
            "name": game[iso3],
            "region_ifa": regions[iso3],
            "crops": crops_block,
        }
    return nations_block, pools


# ---------------------------------------------------------------------------
# Conferência de ordem de grandeza (Banco Mundial)
# ---------------------------------------------------------------------------


def worldbank_latest() -> dict[str, dict]:
    """{iso3: {'kg_ha': valor, 'year': ano}} do cache, último ano com valor."""
    payload = load_json_file(WB_CACHE)
    if not isinstance(payload, list) or len(payload) < 2 or not isinstance(payload[1], list):
        raise SystemExit(f"{WB_CACHE.name}: formato inesperado da API do Banco Mundial")
    best: dict[str, dict] = {}
    for row in payload[1]:
        iso3 = (row.get("countryiso3code") or "").strip().upper()
        value = row.get("value")
        year_text = str(row.get("date") or "")
        if iso3 not in WB_ISO3 or value is None or not year_text.isdigit():
            continue
        year = int(year_text)
        current = best.get(iso3)
        if current is None or year > current["year"]:
            best[iso3] = {"kg_ha": float(value), "year": year}
    missing = [iso3 for iso3 in WB_ISO3 if iso3 not in best]
    if missing:
        raise SystemExit(f"{WB_CACHE.name}: sem valor para {missing}")
    return best


def fubc_country_average(rows: list[dict], game: dict[str, str]) -> dict[str, dict]:
    """N médio (kg/ha) de TODAS as culturas reportadas, no ano mais recente com área.

    Soma N_k_t (kilo toneladas de N) e Crop_area_k_ha do mesmo ano e divide —
    é a média sobre a área total das culturas cobertas pela FUBC, comparável em
    ordem de grandeza com o indicador do Banco Mundial (que é de todos os
    nutrientes e de toda a terra arável).
    """
    by_name = game_name_index(game)
    per_country: dict[str, dict[int, list[float]]] = {}
    for row in rows:
        iso3 = (row["ISO3_code"] or "").strip()
        if iso3 not in game:
            iso3 = by_name.get((row["Country"] or "").strip().casefold(), "")
        if iso3 not in WB_ISO3:
            continue
        year = parse_year(row["Year"])
        n_value, _ = parse_number(row["N_k_t"])
        area, _ = parse_number(row["Crop_area_k_ha"])
        if year is None or year > MAX_YEAR or n_value is None or area is None or area <= 0:
            continue
        totals = per_country.setdefault(iso3, {}).setdefault(year, [0.0, 0.0])
        totals[0] += n_value
        totals[1] += area

    out: dict[str, dict] = {}
    for iso3, years in per_country.items():
        year = max(years)
        n_k_t, area_k_ha = years[year]
        out[iso3] = {"n_kg_ha": 1000.0 * n_k_t / area_k_ha, "year": year, "crops_area_k_ha": area_k_ha}
    return out


def crosscheck_block(rows: list[dict], game: dict[str, str]) -> dict:
    wb = worldbank_latest()
    fubc = fubc_country_average(rows, game)
    table: dict[str, dict] = {}
    for iso3 in WB_ISO3:
        if iso3 not in fubc:
            table[iso3] = {"status": "sem valor FUBC agregado"}
            continue
        ratio = fubc[iso3]["n_kg_ha"] / wb[iso3]["kg_ha"] if wb[iso3]["kg_ha"] else None
        table[iso3] = {
            "fubc_n_kg_ha_all_crops": round(fubc[iso3]["n_kg_ha"], 1),
            "fubc_year": fubc[iso3]["year"],
            "wb_kg_ha": round(wb[iso3]["kg_ha"], 1),
            "wb_year": wb[iso3]["year"],
            "ratio_fubc_over_wb": round(ratio, 3) if ratio is not None else None,
        }
    return {
        "purpose": (
            "conferência de ordem de grandeza apenas; NÃO substitui o dado da FUBC"
        ),
        "indicator": "AG.CON.FERT.ZS",
        "wb_unit": "kg de nutriente (N+P2O5+K2O) por hectare de terra arável, todas as culturas",
        "fubc_unit": "kg de N por hectare, média sobre a área total das culturas reportadas na FUBC",
        "source": "WORLD_BANK_CROSSCHECK",
        "retrieved": RETRIEVED,
        "rows": table,
    }


# ---------------------------------------------------------------------------
# Autoverificação / relatórios
# ---------------------------------------------------------------------------


def entry_rows(payload: dict):
    for iso3, nation in payload["nations"].items():
        for crop, row in nation["crops"].items():
            yield iso3, crop, row


def autocheck(payload: dict, observed: dict, unmatched: list[str]) -> list[str]:
    errors: list[str] = []
    nations_block = payload["nations"]
    if len(nations_block) != 179:
        errors.append(f"{len(nations_block)} nações (esperado 179)")

    total_entries = sum(len(nation["crops"]) for nation in nations_block.values())
    if total_entries != 179 * len(CROPS):
        errors.append(f"{total_entries} entradas (esperado {179 * len(CROPS)})")

    valid_status = {"observed", "regional_default", "no_data"}
    for iso3, nation in nations_block.items():
        if list(nation["crops"]) != list(CROPS):
            errors.append(f"{iso3}: culturas fora da ordem/lista fixa")
        for crop, row in nation["crops"].items():
            if row["data_status"] not in valid_status:
                errors.append(f"{iso3}/{crop}: data_status inválido {row['data_status']}")
            if row["data_status"] == "observed":
                if row["source"] not in {"IFA_FUBC_2018", "IFA_FUBC_HISTORIC"}:
                    errors.append(f"{iso3}/{crop}: observed com source {row['source']}")
                if not isinstance(row["year"], int):
                    errors.append(f"{iso3}/{crop}: observed sem ano inteiro")
            else:
                if row["year"] is not None:
                    errors.append(f"{iso3}/{crop}: não observado com ano")
                if row["source"] not in {"RULE_REGIONAL_MEDIAN", "NONE"}:
                    errors.append(f"{iso3}/{crop}: não observado com source {row['source']}")
            value = row["n_kg_ha"]
            if value is None:
                if row["data_status"] != "no_data":
                    errors.append(f"{iso3}/{crop}: n_kg_ha null sem no_data")
                continue
            if value < 0:
                errors.append(f"{iso3}/{crop}: n_kg_ha negativo ({value})")
            if abs(value * 10 - round(value * 10)) > 1e-9:
                errors.append(f"{iso3}/{crop}: n_kg_ha sem arredondamento a 1 casa")
            if row["data_status"] == "no_data":
                errors.append(f"{iso3}/{crop}: no_data com valor")
        if len(nation["crops"]) != len(CROPS):
            errors.append(f"{iso3}: {len(nation['crops'])} culturas (esperado {len(CROPS)})")

    for (iso3, crop) in observed:
        row = payload["nations"][iso3]["crops"][crop]
        if row["data_status"] != "observed":
            errors.append(f"{iso3}/{crop}: observação FUBC perdida na montagem")

    if unmatched:
        print("  aviso: países FUBC não casados: " + ", ".join(f"{i} ({n})" for i, n in unmatched))
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
    print("  por data_status: " + ", ".join(f"{k}={v}" for k, v in sorted(status_count.items())))
    print("  por fonte:       " + ", ".join(f"{k}={v}" for k, v in sorted(source_count.items())))

    print("\n[n_kg_ha por cultura] todas as entradas: min / mediana / max")
    for crop in CROPS:
        values = [
            row["n_kg_ha"]
            for _, row_crop, row in entry_rows(payload)
            if row_crop == crop and row["n_kg_ha"] is not None
        ]
        observed = [
            row["n_kg_ha"]
            for _, row_crop, row in entry_rows(payload)
            if row_crop == crop and row["data_status"] == "observed"
        ]
        if not values:
            print(f"  {crop:<7} sem valor")
            continue
        line = (
            f"  {crop:<7} {min(values):>6.1f} / {statistics.median(values):>6.1f} / "
            f"{max(values):>6.1f}"
        )
        if observed:
            line += (
                f"   | observado {len(observed):>3}/179: {min(observed):>5.1f} / "
                f"{statistics.median(observed):>5.1f} / {max(observed):>6.1f}"
            )
        else:
            line += "   | sem observado"
        print(line)

    negatives = sum(
        1 for _, _, row in entry_rows(payload) if row["n_kg_ha"] is not None and row["n_kg_ha"] < 0
    )
    zeros = sum(
        1
        for _, _, row in entry_rows(payload)
        if row["n_kg_ha"] is not None and row["data_status"] == "observed" and row["n_kg_ha"] == 0
    )
    print(f"  valores negativos: {negatives}")
    print(f"  observados exatamente 0 (sem N aplicado): {zeros}")


def print_coverage(payload: dict, observed: dict, regions: dict[str, str | None], unassigned: list[str]) -> None:
    print("\n[cobertura] observado x regra x sem dado, por cultura")
    for crop in CROPS:
        obs = sum(1 for (_, row_crop) in observed if row_crop == crop)
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
        print(f"  {crop:<7} observed={obs:>3}  regional_default={rule:>3}  no_data={nodata:>3}")

    countries = sorted({iso3 for (iso3, _) in observed})
    print(f"\n[cobertura] países com taxa FUBC observada: {len(countries)} de 179")
    print("  " + ", ".join(countries))

    print("\n[cobertura] observações por Region_IFA x cultura")
    header = "  " + f"{'região':<38}" + "".join(f"{crop[:6]:>8}" for crop in CROPS)
    print(header)
    region_names = sorted({region for region in regions.values() if region} | {"(sem região)"})
    for region in region_names:
        cells = []
        for crop in CROPS:
            count = sum(
                1 for (iso3, row_crop) in observed
                if row_crop == crop and (regions.get(iso3) or "(sem região)") == region
            )
            cells.append(f"{count:>8}")
        print("  " + f"{region:<38}" + "".join(cells))

    no_region_data = sum(
        1
        for _, _, row in entry_rows(payload)
        if row["data_status"] == "regional_default"
        and "não consta da tabela de regiões IFA" in (row["method"] or "")
    )
    print(f"\n[cobertura] nações sem Region_IFA na tabela IFA: {len(unassigned)} "
          f"({', '.join(unassigned) if unassigned else 'nenhuma'}); "
          f"entradas na mediana global por esse motivo: {no_region_data}")


def print_sample(payload: dict, observed: dict) -> None:
    print("\n[amostra] JSON x linha bruta do CSV (FUBC_1_to_9_data.csv)")
    samples = (
        ("BRA", "soy"),
        ("USA", "maize"),
        ("IND", "wheat"),
        ("CHN", "rice"),
        ("NGA", "maize"),
        ("UKR", "wheat"),
        ("EGY", "wheat"),
    )
    for iso3, crop in samples:
        row = payload["nations"][iso3]["crops"][crop]
        observation = observed.get((iso3, crop))
        print(
            f"  {iso3} {crop:<6} JSON n_kg_ha={row['n_kg_ha']} year={row['year']} "
            f"source={row['source']} status={row['data_status']}"
        )
        if observation is None:
            print("           (sem linha bruta: valor da regra regional)")
            continue
        print(
            f"           CSV  ano={observation['year']} Crop={observation['label']!r} "
            f"Aver_N_rate_kg_ha={observation['aver_raw']} N_rate_kg_ha={observation['rate_raw']} "
            f"N_pc_fert={observation['pc_raw']} -> "
            f"{observation['value']:.2f} ({observation['mode']})"
        )
        recomputed = observation["value"]
        if round1(recomputed) != row["n_kg_ha"]:
            print(f"           DIVERGÊNCIA: recomputado {round1(recomputed)} x JSON {row['n_kg_ha']}")


def print_crosscheck(crosscheck: dict) -> None:
    print("\n[sanidade] FUBC (N de todas as culturas) x Banco Mundial AG.CON.FERT.ZS")
    print("  (todos os nutrientes por ha de terra arável; razão só de ordem de grandeza)")
    print(f"  {'país':<5}{'FUBC kgN/ha':>13}{'ano':>6}{'WB kg/ha':>11}{'ano':>6}{'razão':>8}")
    for iso3, row in crosscheck["rows"].items():
        if "ratio_fubc_over_wb" not in row:
            print(f"  {iso3:<5} {row.get('status')}")
            continue
        print(
            f"  {iso3:<5}{row['fubc_n_kg_ha_all_crops']:>13.1f}{row['fubc_year']:>6}"
            f"{row['wb_kg_ha']:>11.1f}{row['wb_year']:>6}"
            f"{row['ratio_fubc_over_wb']:>8.3f}"
        )


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fetch", action="store_true", help="(re)baixa o cache do Dryad e do Banco Mundial")
    args = parser.parse_args(argv)

    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    game = nations()
    if len(game) != 179:
        print(f"aviso: nations() devolveu {len(game)} nações (esperado 179)")

    ensure_cache(args.fetch)
    check_metadata()

    rows = load_fubc_rows()
    print(f"[fubc] {len(rows)} linhas, {len({r['ISO3_code'] for r in rows})} países/territórios")

    index = fubc_nation_index(rows, game)
    unmatched = unmatched_fubc_countries(rows, game)
    observed = pick_observations(rows, game, index)
    regions, unassigned = nation_regions(game)
    dups = duplicate_pairs(rows, game)
    divergences = region_divergences(index)
    used_labels, excluded_labels = crop_labels_found(rows)

    nations_block, _pools = build(game, observed, regions)
    crosscheck = crosscheck_block(rows, game)

    payload = {
        "_meta": {
            "dataset": "fertilizer_n",
            "version": "g1",
            "generated_by": "tools/build_agri_fertilizer.py",
            "retrieved": RETRIEVED,
            "nations": len(nations_block),
            "crops": list(CROPS),
            "unit": "kg N por hectare por ano (média sobre a área total da cultura)",
            "rule": (
                "país sem FUBC: mediana dos observados da mesma Region_IFA e cultura; "
                "sem observado na região, mediana global"
            ),
            "crop_labels_used": {crop: sorted(labels) for crop, labels in used_labels.items()},
            "crop_labels_excluded": excluded_labels,
            "fubc_reports": {
                "9": "levantamento 2017/18 (anos 2016-2019, 64 países): traz Aver_N_rate_kg_ha",
                "6-8": "2006, 2010 e 2014 só trazem N total aplicado (N_k_t), sem taxa nem área — não geram valor",
                "1-5": "levantamentos de 1984 a 2001 (publicados em 1992-2002); usados só onde não há taxa de 2017/18",
            },
            "crosscheck_world_bank": crosscheck,
        },
        "sources": {
            "IFA_FUBC_2018": {
                "title": "Fertilizer use by crop and country for the 2017-2018 period",
                "publisher": "International Fertilizer Association (IFA)",
                "year": 2022,
                "doi": DOI,
                "url": DATASET_URL,
                "license": "CC0 1.0",
                "accessed": RETRIEVED,
            },
            "IFA_FUBC_HISTORIC": {
                "title": "Global data on fertilizer use by crop and by country — compilação histórica dos relatórios FUBC 1-8 (1984-2014)",
                "publisher": "International Fertilizer Association (IFA)",
                "year": 2022,
                "doi": DOI,
                "url": DATASET_URL,
                "license": "CC0 1.0",
                "accessed": RETRIEVED,
            },
            "RULE_REGIONAL_MEDIAN": {
                "title": "Regra declarada: mediana regional",
                "publisher": "OpenGS project",
                "year": 2026,
            },
            "WORLD_BANK_CROSSCHECK": {
                "title": "Fertilizer consumption (kg per hectare of arable land)",
                "publisher": "World Bank (FAOSTAT mirror)",
                "indicator": "AG.CON.FERT.ZS",
                "url": "https://api.worldbank.org/v2/country/all/indicator/AG.CON.FERT.ZS",
            },
        },
        "nations": nations_block,
    }

    write_json(OUT_PATH, payload)

    errors = autocheck(payload, observed, unmatched)
    print("\n[autocheck]")
    if errors:
        for error in errors:
            print(f"  FALHA: {error}")
    else:
        print(
            "  OK: 179 nações, 895 entradas, status/source válidos, "
            "n_kg_ha >= 0 a 1 casa, ano só em observed"
        )

    print_stats(payload)
    print_coverage(payload, observed, regions, unassigned)
    if dups:
        print(f"\n[nota] {len(dups)} pares país/cultura/ano com mais de uma linha de "
              "relatórios diferentes; vence o maior FUBC_report_number (ex.: "
              f"{dups[0]})")
    if divergences:
        print("\n[nota] divergência de Region_IFA entre a FUBC e a tabela IFA "
              f"(vale a tabela IFA): {', '.join(divergences)}")
    print_sample(payload, observed)
    print_crosscheck(crosscheck)
    print(f"\n[fertilizer_n] sha256 {sha256(OUT_PATH)}")
    print(f"[fertilizer_n] arquivo {OUT_PATH.relative_to(ROOT)}")

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
