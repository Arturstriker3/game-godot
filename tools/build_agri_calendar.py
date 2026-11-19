#!/usr/bin/env python3
"""Construtor de `data/definitions/agriculture/crop_calendar.json` (Parte G).

Calendário de plantio e colheita por país × cultura para as cinco culturas do
jogo (`wheat`, `rice`, `maize`, `potato`, `soy`), a partir de uma camada
primária curada, de duas fontes observadas e de uma regra declarada para o que
não é observado.

Fontes (cache local em `tools/_tmp_agri_raw/`; só `--fetch` baixa o que faltar):

0. `PRIMARY` — `tools/agri_calendar_primary.json`: calendário curado a partir de
   documento primário (USDA NASS/GIAN/WAP, FAO GIEWS, institutos nacionais),
   com trecho literal e URL por par. Tem prioridade sobre todas as outras e
   cobre os MAIORES produtores de cada cultura (top-15 do próprio FAOSTAT).
1. `FAO_CROP_CALENDAR` — FAO Crop Calendar por país/cultura/atividade, com
   linhas por zona agroecológica (AEZ). CC BY 4.0.
2. `JRC_ASAP` — ASAP national crop calendars (dekads 1..36). EU reuse notice.
3. `RULE_NEIGHBOR` — regra do projeto, só para quem não tem calendário
   observado: o país herda as janelas de um país observado DO MESMO HEMISFÉRIO
   E DA MESMA FAIXA de latitude (`agri_common.climate_band`). Nesta ordem:
   (a) mesma `region` do jogo (`nations.json`, ~continente), o mais próximo =
   `same_region`; (b) outra `region` = `same_band_other_region`, permitido
   apenas quando não há doador no próprio continente, e a entrada carrega
   `cross_continent: true` + `justification`. Faixa vizinha (clima diferente) e
   hemisfério oposto NÃO são mais aceitos. O doador e a distância vão em
   `rule` = {tier, donor, distance_km, ...}.

REGRA DE AGREGAÇÃO (todas as linhas de um par país×cultura):

- cada janela da fonte vira cobertura de meses 1..12 (com virada de ano);
- a cobertura é agrupada em blocos; blocos separados por >= 3 meses sem
  cobertura são safras distintas (no máximo 2 safras);
- plantio = bloco que começa no primeiro mês coberto e termina no último mês
  coberto; colheita idem;
- se plantio e colheita não formarem o mesmo número de blocos (1 ou 2), cai
  para 1 safra agregada (mínimo de início / máximo de fim) e registra a nota
  `multi_block_fallback` com o rótulo bruto em `window_labels`;
- `allYear=yes` na FAO, quando o par não tem nenhum mês publicado, gera
  `all_year: true` e `seasons: []` — não se inventa mês;
- prioridade FAO > JRC > regra; a fonte usada vai no `source` de cada safra e
  `source_year` é o maior `lastUpdated` do par (FAO). A JRC não publica ano por
  linha (climatologia), então `source_year` fica `null`;
- se a colheita começar antes do fim do plantio, a janela é mantida como está
  e o fato é registrado em `note`.

Determinístico: nações por id, culturas na ordem (wheat, rice, maize, potato,
soy), JSON com tab + LF via `agri_common.write_json`.

Uso:
    python tools/build_agri_calendar.py            # cache obrigatório
    python tools/build_agri_calendar.py --fetch    # baixa o que faltar

A camada primária `tools/agri_calendar_primary.json` é obrigatória e versionada:
sem ela o build para (ela é a garantia de que o top-15 de cada cultura sai de
documento, não de vizinho).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import itertools
import math
import re
import sys
import unicodedata
import zipfile
from pathlib import Path
from urllib.request import urlretrieve

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from agri_common import (  # noqa: E402
    DEF,
    OUT_DIR,
    climate_band,
    centroids,
    hemisphere,
    load_json,
    nations,
    season_months,
    write_json,
)

RAW_DIR = ROOT / "tools" / "_tmp_agri_raw"
PRIMARY_PATH = ROOT / "tools" / "agri_calendar_primary.json"
OUT_PATH = ROOT / "data" / "definitions" / "agriculture" / "crop_calendar.json"

RETRIEVED = "2026-11-18"
CROPS = ("wheat", "rice", "maize", "potato", "soy")

# Culturas usadas na amostra de conferência (critério de aceite 3).
SAMPLES = (
    ("USA", "maize"),
    ("USA", "soy"),
    ("USA", "wheat"),
    ("BRA", "soy"),
    ("IND", "rice"),
    ("IND", "wheat"),
    ("CHN", "wheat"),
    ("CHN", "rice"),
    ("RUS", "wheat"),
    ("AUS", "wheat"),
    ("ARG", "soy"),
    ("FRA", "wheat"),
    ("UKR", "wheat"),
    ("EGY", "wheat"),
)

SOURCES = {
    "fao_crop_calendar.csv": {
        "url": (
            "https://api.data.apps.fao.org/api/v2/bigquery"
            "?sql_url=https://data.apps.fao.org/catalog/dataset/"
            "86b7daf0-4fae-450b-b88f-76690e2fdc17/resource/"
            "0026de2e-c12c-457c-b4f5-8c4cd2ef7960/download/"
            "crop-calendar-parameterized-query.sql"
            "&crop_process=all&crop=all&download=true"
        ),
        "title": "FAO Crop Calendar (national summary)",
    },
    "crop_calendar_gaul0.zip": {
        "url": "https://mars.jrc.ec.europa.eu/asap/files/crop_calendar_gaul0.zip",
        "title": "ASAP national crop calendars",
    },
}

# Nomes exatos de cultura nas fontes -> id do jogo. Nada de casamento por
# prefixo silencioso: o que não está aqui é registrado como não mapeado.
FAO_CROP_NAMES = {
    "Wheat": "wheat",
    "Wheat, bread": "wheat",
    "Wheat, durum": "wheat",
    "Rice": "rice",
    "Corn": "maize",  # a FAO chama o milho de "Corn" neste dataset
    "Potato": "potato",
    "Soybean": "soy",
}
FAO_EXCLUDED = {
    "Corn, sweet": "milho doce, não o milho do jogo",
    "Sweet potato": "batata-doce, não a batata",
    "Wheat, Emmer": "trigo ancestral, não o trigo do jogo",
}

# JRC: nome base (sem parênteses) -> id do jogo.
JRC_CROP_NAMES = {
    "wheat": "wheat",
    "rice": "rice",
    "maize": "maize",
    "potato": "potato",
    "potatoes": "potato",
    "soybean": "soy",
    "soya": "soy",
    "soybeans": "soy",
}

# Fallback manual explícito de nome em inglês -> id do jogo. Chave = nome
# normalizado pela função `_norm`. Só entra aqui o que não casa direto.
NAME_ALIASES = {
    "cote d ivoire": "CIV",
    "democratic people s republic of korea": "PRK",
    "democratic peoples republic of korea": "PRK",
    "united republic of tanzania": "TZA",
    "timor leste": "TLS",
    "central africa": "CAF",  # ASAP: República Centro-Africana
    "congo": "COG",
    "d r congo": "COD",
    "viet nam": "VNM",
    "swaziland": "SWZ",
}

MONTH_ABBR = {
    "jan": 1,
    "feb": 2,
    "mar": 3,
    "apr": 4,
    "may": 5,
    "jun": 6,
    "jul": 7,
    "aug": 8,
    "sep": 9,
    "oct": 10,
    "nov": 11,
    "dec": 12,
}
MONTH_LABELS_PT = (
    "jan",
    "fev",
    "mar",
    "abr",
    "mai",
    "jun",
    "jul",
    "ago",
    "set",
    "out",
    "nov",
    "dez",
)

MIN_GAP_MONTHS = 3  # blocos separados por >= 3 meses sem cobertura = 2 safras
MAX_SEASONS = 2
EARTH_RADIUS_KM = 6371.0088
# Tiers da regra: mesma faixa de latitude e mesmo hemisfério SEMPRE; o tier 2
# só existe quando não há doador no próprio continente (declarado na entrada).
RULE_TIERS = ("same_region", "same_band_other_region")
# Maiores produtores por cultura que NÃO podem ficar na regra de vizinho.
TOP_N = 15


# --------------------------------------------------------------------------
# cache
# --------------------------------------------------------------------------


def ensure_cache(fetch: bool) -> dict[str, str]:
    """Garante os arquivos locais. Com cache presente, `--fetch` é no-op."""
    report: dict[str, str] = {}
    for name, meta in SOURCES.items():
        path = RAW_DIR / name
        if path.exists():
            print(f"[cache] {name}: presente ({path.stat().st_size} bytes) — nada a fazer")
            report[name] = "cache"
            continue
        if not fetch:
            raise SystemExit(
                f"[erro] cache ausente: {path}\n"
                f"       rode com --fetch para baixar de {meta['url']}"
            )
        print(f"[fetch] {name} <- {meta['url']}")
        RAW_DIR.mkdir(parents=True, exist_ok=True)
        urlretrieve(meta["url"], path)
        print(f"[fetch] {name}: {path.stat().st_size} bytes")
        report[name] = "downloaded"
    return report


# --------------------------------------------------------------------------
# nomes de país e de cultura
# --------------------------------------------------------------------------


def _norm(name: str) -> str:
    text = unicodedata.normalize("NFKD", name)
    text = "".join(ch for ch in text if not unicodedata.combining(ch))
    text = text.lower().replace("&", " and ")
    text = re.sub(r"\(.*?\)", " ", text)
    text = re.sub(r"[^a-z0-9]+", " ", text).strip()
    if text.startswith("the "):
        text = text[4:]
    return text


def match_index() -> dict[str, str]:
    """{nome normalizado do jogo: id} + aliases declarados."""
    index = {_norm(name): iso for iso, name in nations().items()}
    index.update(NAME_ALIASES)
    return index


def match_nation(raw_name: str, index: dict[str, str]) -> str | None:
    return index.get(_norm(raw_name))


def fao_crop(raw_name: str) -> str | None:
    return FAO_CROP_NAMES.get(raw_name.strip())


def jrc_crop(raw_name: str) -> str | None:
    base = re.sub(r"\(.*?\)", "", raw_name.lower()).strip()
    base = re.sub(r"\s+", " ", base)
    return JRC_CROP_NAMES.get(base)


# --------------------------------------------------------------------------
# leitura das fontes
# --------------------------------------------------------------------------


def _parse_month_token(token: str) -> int:
    key = token.strip()[:3].lower()
    if key not in MONTH_ABBR:
        raise ValueError(f"mês desconhecido na fonte: {token!r}")
    return MONTH_ABBR[key]


def parse_fao_window(text: str) -> tuple[int, int]:
    """'Nov 01 - Jan 10' -> (11, 1). Só o mês importa."""
    left, sep, right = text.partition(" - ")
    if not sep:
        raise ValueError(f"janela FAO inesperada: {text!r}")
    return _parse_month_token(left), _parse_month_token(right)


def _empty_pair() -> dict:
    return {"sowing": [], "harvesting": [], "all_year_rows": 0, "years": []}


def read_fao(index: dict[str, str]) -> tuple[dict[tuple[str, str], dict], dict]:
    path = RAW_DIR / "fao_crop_calendar.csv"
    pairs: dict[tuple[str, str], dict] = {}
    seen_countries: set[str] = set()
    unmatched: set[str] = set()
    crop_names_found: set[str] = set()
    rows = 0

    with open(path, encoding="utf-8-sig", newline="") as fh:
        for row in csv.DictReader(fh):
            rows += 1
            crop = fao_crop(row["cropName"])
            raw_country = row["country_name_en"].strip()
            seen_countries.add(raw_country)
            if crop is None:
                if row["cropName"].strip() in FAO_EXCLUDED:
                    crop_names_found.add(row["cropName"].strip())
                continue
            crop_names_found.add(row["cropName"].strip())
            iso = match_nation(raw_country, index)
            if iso is None:
                unmatched.add(raw_country)
                continue
            pair = pairs.setdefault((iso, crop), _empty_pair())
            process = row["cropProcess"].strip().lower()
            slot = "sowing" if process == "sowing" else "harvesting"
            if row["allYear"].strip().lower() == "yes":
                pair["all_year_rows"] += 1
                continue
            for column in ("period1", "period2", "period3", "period4", "period5"):
                label = row[column].strip()
                if not label:
                    continue
                start, end = parse_fao_window(label)
                pair[slot].append({"start": start, "end": end, "label": label})
            updated = row["lastUpdated"].strip()
            if updated[:4].isdigit():
                pair["years"].append(int(updated[:4]))

    stats = {
        "rows": rows,
        "countries_in_source": len(seen_countries),
        "crop_names_found": sorted(crop_names_found),
        "unmatched_countries": sorted(unmatched),
    }
    return pairs, stats


def dekad_month(dekad: int) -> int:
    """Dekad 1..36 -> mês do primeiro dia da dekad (1 = 1–10/jan)."""
    if not 1 <= dekad <= 36:
        raise ValueError(f"dekad fora de 1..36: {dekad}")
    return (dekad - 1) // 3 + 1


def read_jrc(index: dict[str, str]) -> tuple[dict[tuple[str, str], dict], dict]:
    path = RAW_DIR / "crop_calendar_gaul0.zip"
    pairs: dict[tuple[str, str], dict] = {}
    seen_countries: set[str] = set()
    unmatched: set[str] = set()
    crop_names_found: set[str] = set()
    variants: dict[str, set[str]] = {crop: set() for crop in CROPS}
    rows = 0

    with zipfile.ZipFile(path) as archive:
        text = archive.read("crop_calendar_gaul0.csv").decode("utf-8-sig")
    for line in text.splitlines():
        if not line.strip():
            continue
        fields = line.split(";")
        if fields[0].strip().lower() == "asap0_id":
            continue
        rows += 1
        raw_country = fields[1].strip()
        crop_name = fields[2].strip()
        seen_countries.add(raw_country)
        crop = jrc_crop(crop_name)
        if crop is None:
            continue
        crop_names_found.add(crop_name)
        variants[crop].add(crop_name)
        iso = match_nation(raw_country, index)
        if iso is None:
            unmatched.add(raw_country)
            continue
        sos_s, sos_e, eos_s, eos_e = (int(fields[i]) for i in range(3, 7))
        pair = pairs.setdefault((iso, crop), _empty_pair())
        label = f"{crop_name} sos {sos_s}-{sos_e} eos {eos_s}-{eos_e} (dekads)"
        pair["sowing"].append(
            {"start": dekad_month(sos_s), "end": dekad_month(sos_e), "label": label}
        )
        pair["harvesting"].append(
            {"start": dekad_month(eos_s), "end": dekad_month(eos_e), "label": label}
        )

    stats = {
        "rows": rows,
        "countries_in_source": len(seen_countries),
        "crop_names_found": sorted(crop_names_found),
        "variants": {crop: sorted(v) for crop, v in variants.items()},
        "unmatched_countries": sorted(unmatched),
    }
    return pairs, stats


# --------------------------------------------------------------------------
# agregação das janelas
# --------------------------------------------------------------------------


def _runs(covered: set[int]) -> list[tuple[int, int]]:
    """Runs contíguos de meses cobertos, começando depois de um mês vazio.

    O resultado pode conter um run que atravessa o ano (ex.: (11, 2)).
    """
    if len(covered) == 12:
        return [(1, 12)]
    if not covered:
        return []
    first_empty = next(month for month in range(1, 13) if month not in covered)
    runs: list[tuple[int, int]] = []
    run_start: int | None = None
    previous: int | None = None
    for step in range(1, 13):
        month = ((first_empty - 1 + step) % 12) + 1
        if month in covered:
            if run_start is None:
                run_start = month
            previous = month
        elif run_start is not None:
            runs.append((run_start, previous))
            run_start = None
            previous = None
    if run_start is not None:
        runs.append((run_start, previous))
    return runs


def month_blocks(windows: list[dict]) -> list[tuple[int, int]]:
    """Blocos de cobertura (início, fim) separados por >= 3 meses vazios."""
    covered: set[int] = set()
    for window in windows:
        covered.update(season_months(window["start"], window["end"]))
    if not covered:
        return []
    runs = _runs(covered)
    while len(runs) > 1:
        gaps = [
            (runs[i + 1][0] - runs[i][1] - 1) % 12 for i in range(len(runs) - 1)
        ]
        gaps.append((runs[0][0] - runs[-1][1] - 1) % 12)
        index = min(range(len(gaps)), key=gaps.__getitem__)
        if gaps[index] >= MIN_GAP_MONTHS:
            break
        if index == len(runs) - 1:
            # o bloco final encosta no inicial: vira um bloco que cruza o ano
            runs = [(runs[-1][0], runs[0][1])] + runs[1:-1]
        else:
            runs[index] = (runs[index][0], runs[index + 1][1])
            del runs[index + 1]
    return sorted(runs, key=lambda item: item[0])


def _labels_for(block: tuple[int, int], windows: list[dict]) -> list[str]:
    months = set(season_months(*block))
    labels: set[str] = set()
    for window in windows:
        span = set(season_months(window["start"], window["end"]))
        if months & span:
            labels.add(window["label"])
    return sorted(labels)


def _span(windows: list[dict]) -> dict:
    return {
        "start_month": min(window["start"] for window in windows),
        "end_month": max(window["end"] for window in windows),
    }


def _forward(anchor: int, month: int) -> int:
    """Deslocamento (0..11) de `month` a partir de `anchor`, no sentido do ano."""
    return (month - anchor) % 12


def pair_blocks(
    planting_blocks: list[tuple[int, int]], harvest_blocks: list[tuple[int, int]]
) -> list[tuple[tuple[int, int], tuple[int, int]]]:
    """Casa cada plantio com a colheita que começa depois dele, o mais cedo possível.

    Ordenar só por mês de início erra no hemisfério sul (plantio em novembro
    colhe em março, mês numericamente menor). Aqui as permutações de colheita
    são testadas e vence a que minimiza a distância para frente plantio->colheita.
    """
    best_score: int | None = None
    best_order: tuple[int, ...] | None = None
    for order in itertools.permutations(range(len(harvest_blocks))):
        score = 0
        for plant, index in zip(planting_blocks, order):
            delta = _forward(plant[0], harvest_blocks[index][0])
            score += delta if delta else 12
        if best_score is None or score < best_score:
            best_score = score
            best_order = order
    assert best_order is not None
    return [
        (plant, harvest_blocks[index])
        for plant, index in zip(planting_blocks, best_order)
    ]


def aggregate(pair: dict) -> tuple[list[dict], list[str]]:
    """Janelas de plantio/colheita -> safras (1 ou 2). Devolve (safras, avisos)."""
    planting = pair["sowing"]
    harvest = pair["harvesting"]
    notes: list[str] = []
    seasons: list[dict] = []

    if not planting and not harvest:
        # só linhas allYear=yes: a fonte declara safra contínua e não publica mês
        return [], notes

    planting_blocks = month_blocks(planting)
    harvest_blocks = month_blocks(harvest)

    if (
        planting_blocks
        and harvest_blocks
        and len(planting_blocks) == len(harvest_blocks)
        and len(planting_blocks) <= MAX_SEASONS
    ):
        for number, (planting_block, harvest_block) in enumerate(
            pair_blocks(planting_blocks, harvest_blocks), start=1
        ):
            seasons.append(
                {
                    "season": number,
                    "planting": {
                        "start_month": planting_block[0],
                        "end_month": planting_block[1],
                    },
                    "harvest": {
                        "start_month": harvest_block[0],
                        "end_month": harvest_block[1],
                    },
                    "window_labels": sorted(
                        set(_labels_for(planting_block, planting))
                        | set(_labels_for(harvest_block, harvest))
                    ),
                    "note": None,
                }
            )
    else:
        # não deu para separar com segurança (blocos desiguais ou > 2 safras)
        season = {
            "season": 1,
            "planting": _span(planting) if planting else None,
            "harvest": _span(harvest) if harvest else None,
            "window_labels": sorted(
                {window["label"] for window in planting + harvest}
            ),
            "note": None,
        }
        note = "multi_block_fallback: janela agregada (mín. de início/máx. de fim)"
        if len(planting_blocks) > MAX_SEASONS or len(harvest_blocks) > MAX_SEASONS:
            note += f" — a fonte publica {max(len(planting_blocks), len(harvest_blocks))} blocos"
        if not planting_blocks:
            note += " — a fonte não publica plantio"
        if not harvest_blocks:
            note += " — a fonte não publica colheita"
        season["note"] = note
        notes.append(note)
        seasons.append(season)

    for season in seasons:
        extra: list[str] = []
        plant = season["planting"]
        crop_harvest = season["harvest"]
        if plant and crop_harvest:
            same = (plant["start_month"], plant["end_month"]) == (
                crop_harvest["start_month"],
                crop_harvest["end_month"],
            )
            if same:
                extra.append(
                    "plantio e colheita com a mesma janela agregada (zonas/variantes "
                    "da fonte misturadas; possível inconsistência da fonte; mantido "
                    "como está)"
                )
            elif _forward(plant["start_month"], crop_harvest["start_month"]) < _forward(
                plant["start_month"], plant["end_month"]
            ):
                extra.append(
                    "colheita agregada começa antes do fim do plantio agregado "
                    "(zonas/variantes da fonte misturadas; mantido como está)"
                )
        if extra:
            season["note"] = "; ".join(([season["note"]] if season["note"] else []) + extra)
    return seasons, notes


# --------------------------------------------------------------------------
# montagem
# --------------------------------------------------------------------------


def _distance_km(origin: tuple[float, float], target: tuple[float, float]) -> float:
    lat1, lon1 = math.radians(origin[0]), math.radians(origin[1])
    lat2, lon2 = math.radians(target[0]), math.radians(target[1])
    delta_lat = lat2 - lat1
    delta_lon = lon2 - lon1
    hav = (
        math.sin(delta_lat / 2) ** 2
        + math.cos(lat1) * math.cos(lat2) * math.sin(delta_lon / 2) ** 2
    )
    return 2 * EARTH_RADIUS_KM * math.asin(math.sqrt(hav))


def pick_donor(
    iso: str,
    observed: list[str],
    geo: dict[str, tuple[float, float]],
    regions: dict[str, str],
) -> dict | None:
    """Escolhe o doador observado em dois níveis, nesta ordem.

    Regra (D-094, revisão): o doador é SEMPRE do mesmo hemisfério e da mesma
    faixa de latitude do país sem calendário.

    a) mesma faixa + mesmo hemisfério + mesma `region` do jogo (o mais próximo)
       -> `same_region`
    b) mesma faixa + mesmo hemisfério, outra `region` -> `same_band_other_region`
       com `cross_continent: true` e `justification` (só quando não há doador no
       próprio continente)
    Sem candidato: None (o par vira `no_data`; não se herda clima diferente).
    """
    latitude = geo[iso][0]
    side = hemisphere(latitude)
    band = climate_band(latitude)
    region = regions.get(iso)

    pool: list[tuple[str, str, float, str | None]] = []
    for donor in observed:
        donor_lat = geo[donor][0]
        if hemisphere(donor_lat) != side:
            continue
        if climate_band(donor_lat) != band:
            continue
        pool.append(
            (
                donor,
                climate_band(donor_lat),
                _distance_km(geo[iso], geo[donor]),
                regions.get(donor),
            )
        )

    same_region = [item for item in pool if region and item[3] == region]

    def winner(candidates: list[tuple[str, str, float, str | None]], tier: str) -> dict:
        donor, _, distance, _ = min(candidates, key=lambda item: (item[2], item[0]))
        return {"tier": tier, "donor": donor, "distance_km": round(distance, 1)}

    if same_region:
        return winner(same_region, "same_region")
    if pool:
        chosen = winner(pool, "same_band_other_region")
        chosen["cross_continent"] = True
        chosen["justification"] = (
            f"nenhum país observado de {band} no hemisfério {side} e na região "
            f"`{region}`; doador da mesma faixa e hemisfério em outra região"
        )
        return chosen
    return None


def build_entry(
    iso: str,
    crop: str,
    pair: dict | None,
    source: str,
    latitude: float,
) -> dict:
    entry: dict = {"hemisphere": hemisphere(latitude), "seasons": []}
    if pair is None:
        return entry
    seasons, _ = aggregate(pair)
    if not seasons and pair["all_year_rows"]:
        entry["all_year"] = True
        entry["seasons"] = []
        entry["data_status"] = "observed"
        entry["rule"] = None
        return entry
    if not seasons:
        return entry
    years = sorted(pair["years"])
    year = years[-1] if years else None
    for season in seasons:
        season["source"] = source
        season["source_year"] = year
    entry["seasons"] = seasons
    entry["data_status"] = "observed"
    entry["rule"] = None
    return entry


def build() -> dict:
    index = match_index()
    game = nations()
    geo = centroids()
    regions = {
        row["id"]: row.get("region") or ""
        for row in load_json(DEF / "scenario" / "nations.json")
    }
    primary = load_primary()

    fao_pairs, fao_stats = read_fao(index)
    jrc_pairs, jrc_stats = read_jrc(index)
    fao_stats["all_year_only_pairs"] = sorted(
        f"{iso}/{crop}"
        for (iso, crop), pair in fao_pairs.items()
        if pair["all_year_rows"] and not (pair["sowing"] or pair["harvesting"])
    )
    fao_stats["all_year_mixed_pairs"] = sorted(
        f"{iso}/{crop}"
        for (iso, crop), pair in fao_pairs.items()
        if pair["all_year_rows"] and (pair["sowing"] or pair["harvesting"])
    )
    fao_stats["all_year_shadowing_jrc"] = sorted(
        f"{iso}/{crop}"
        for (iso, crop), pair in fao_pairs.items()
        if pair["all_year_rows"]
        and not (pair["sowing"] or pair["harvesting"])
        and (iso, crop) in jrc_pairs
    )

    blocks: dict[str, dict] = {}
    for iso in sorted(game):
        latitude = geo[iso][0]
        crops_block: dict[str, dict] = {}
        for crop in CROPS:
            if (iso, crop) in primary["entries"]:
                entry = primary_entry(primary["entries"][(iso, crop)], latitude)
            elif (iso, crop) in fao_pairs:
                entry = build_entry(iso, crop, fao_pairs[(iso, crop)], "FAO_CROP_CALENDAR", latitude)
            elif (iso, crop) in jrc_pairs:
                entry = build_entry(iso, crop, jrc_pairs[(iso, crop)], "JRC_ASAP", latitude)
            else:
                entry = build_entry(iso, crop, None, "", latitude)
            crops_block[crop] = entry
        blocks[iso] = {
            "name": game[iso],
            "latitude": round(latitude, 4),
            "hemisphere": hemisphere(latitude),
            "crops": crops_block,
        }

    donors = apply_neighbour_rule(blocks, geo, regions)
    guard_top_producers(blocks, primary["doc"])

    payload = {
        "_meta": meta(blocks, fao_stats, jrc_stats, donors, primary["doc"]),
        "sources": sources(primary["doc"]),
        "nations": blocks,
    }
    validate(payload, geo)
    return payload


def guard_top_producers(blocks: dict[str, dict], primary_doc: dict) -> None:
    """Os maiores produtores de cada cultura não podem ficar na regra de vizinho.

    A lista de exceções (se houver) é declarada em
    `tools/agri_calendar_primary.json -> _meta.top15_exceptions`.
    """
    exceptions = set((primary_doc.get("_meta") or {}).get("top15_exceptions") or [])
    broken: list[str] = []
    for crop, iso_list in top_producers().items():
        for iso in iso_list:
            if f"{iso}/{crop}" in exceptions:
                continue
            entry = blocks[iso]["crops"][crop]
            if entry.get("data_status") != "observed":
                broken.append(f"{iso}/{crop} ({entry.get('data_status')})")
    if broken:
        raise SystemExit(
            "[erro] top-%d por cultura sem calendário observado: %s\n"
            "       acrescente a fonte primária em tools/agri_calendar_primary.json "
            "ou declare a exceção em _meta.top15_exceptions"
            % (TOP_N, ", ".join(broken))
        )


def apply_neighbour_rule(
    blocks: dict[str, dict],
    geo: dict[str, tuple[float, float]],
    regions: dict[str, str],
) -> dict:
    """Preenche `rule_neighbor`/`no_data`; devolve {doador: nº de empréstimos}."""
    usage: dict[str, int] = {}
    for crop in CROPS:
        observed = [
            iso
            for iso, block in blocks.items()
            if block["crops"][crop].get("data_status") == "observed"
        ]
        for iso, block in blocks.items():
            entry = block["crops"][crop]
            if entry.get("data_status"):
                continue
            latitude = geo[iso][0]
            band = climate_band(latitude)
            side = hemisphere(latitude)
            donor = pick_donor(iso, observed, geo, regions)
            if donor:
                donor_entry = blocks[donor["donor"]]["crops"][crop]
                clone = {
                    "hemisphere": entry["hemisphere"],
                    "seasons": clone_seasons(donor_entry.get("seasons", [])),
                }
                if donor_entry.get("all_year"):
                    clone["all_year"] = True
                clone["data_status"] = "rule_neighbor"
                clone["rule"] = donor
                block["crops"][crop] = clone
                usage[donor["donor"]] = usage.get(donor["donor"], 0) + 1
            else:
                entry["data_status"] = "no_data"
                entry["rule"] = None
                entry["reason"] = (
                    f"sem calendário observado de {crop} e nenhum país observado no "
                    f"hemisfério {side} na faixa `{band}` (clima igual é exigência da regra; "
                    "faixa vizinha não é usada)"
                )
    return usage


def clone_seasons(seasons: list[dict]) -> list[dict]:
    """Cópia profunda simples das safras, preservando a fonte do doador."""
    clone: list[dict] = []
    for season in seasons:
        item = {
            "season": season["season"],
            "planting": dict(season["planting"]) if season["planting"] else None,
            "harvest": dict(season["harvest"]) if season["harvest"] else None,
            "window_labels": list(season["window_labels"]),
            "note": season["note"],
            "source": season["source"],
            "source_year": season["source_year"],
        }
        clone.append(item)
    return clone


# --------------------------------------------------------------------------
# camada primária (tools/agri_calendar_primary.json)
# --------------------------------------------------------------------------


def load_primary() -> dict:
    """Lê a camada curada; devolve {'entries': {(iso, crop): entry}, 'doc': ...}."""
    if not PRIMARY_PATH.exists():
        raise SystemExit(f"[erro] camada primária ausente: {PRIMARY_PATH}")
    doc = load_json(PRIMARY_PATH)
    entries: dict[tuple[str, str], dict] = {}
    for raw in doc["entries"]:
        if raw.get("status") == "no_source":
            continue
        iso, crop = raw["nation"], raw["crop"]
        if iso not in nations():
            raise SystemExit(f"[erro] camada primária: nação desconhecida {iso!r}")
        if crop not in CROPS:
            raise SystemExit(f"[erro] camada primária: cultura fora do jogo {crop!r}")
        if (iso, crop) in entries:
            raise SystemExit(f"[erro] camada primária: par repetido {iso}/{crop}")
        if not raw.get("quote") or not raw.get("url"):
            raise SystemExit(f"[erro] camada primária: {iso}/{crop} sem quote/url")
        if raw.get("source") not in doc["sources"]:
            raise SystemExit(
                f"[erro] camada primária: {iso}/{crop} cita fonte "
                f"{raw.get('source')!r} fora de `sources`"
            )
        seasons = raw.get("seasons") or []
        if not 1 <= len(seasons) <= MAX_SEASONS:
            raise SystemExit(f"[erro] camada primária: {iso}/{crop} com {len(seasons)} safras")
        entries[(iso, crop)] = raw
    return {"entries": entries, "doc": doc}


def primary_entry(raw: dict, latitude: float) -> dict:
    """Entrada `observed` a partir de um item da camada primária."""
    seasons: list[dict] = []
    for number, season in enumerate(raw["seasons"], start=1):
        planting, harvest = season["planting"], season["harvest"]
        for window in (planting, harvest):
            if window is None:
                continue
            for value in window:
                if not isinstance(value, int) or not 1 <= value <= 12:
                    raise SystemExit(
                        f"[erro] camada primária: {raw['nation']}/{raw['crop']} "
                        f"mês inválido {value!r}"
                    )
        if planting and harvest and (planting[0], planting[1]) == (harvest[0], harvest[1]):
            raise SystemExit(
                f"[erro] camada primária: {raw['nation']}/{raw['crop']} "
                "plantio == colheita"
            )
        seasons.append(
            {
                "season": number,
                "planting": {"start_month": planting[0], "end_month": planting[1]}
                if planting
                else None,
                "harvest": {"start_month": harvest[0], "end_month": harvest[1]}
                if harvest
                else None,
                "window_labels": [season.get("label") or f"season_{number}"],
                "note": season.get("note") or raw.get("note"),
                "source": raw["source"],
                "source_year": raw["source_year"],
            }
        )
    return {
        "hemisphere": hemisphere(latitude),
        "seasons": seasons,
        "data_status": "observed",
        "rule": None,
        "evidence": {
            "report": raw.get("report"),
            "quote": raw.get("quote"),
            "url": raw.get("url"),
            "basis": raw.get("basis"),
            "retrieved": raw.get("retrieved") or RETRIEVED,
        },
    }


def top_producers(count: int = TOP_N) -> dict[str, list[str]]:
    """{cultura: [os `count` maiores produtores segundo crop_yields.json]}."""
    yields = load_json(OUT_DIR / "crop_yields.json")
    out: dict[str, list[str]] = {}
    for crop in CROPS:
        rows = [
            (entry["production_t"], iso)
            for iso, nation in yields["nations"].items()
            if (entry := nation["crops"][crop]).get("production_t")
        ]
        rows.sort(key=lambda item: (-item[0], item[1]))
        out[crop] = [iso for _, iso in rows[:count]]
    return out


# --------------------------------------------------------------------------
# metadados, fontes e validação
# --------------------------------------------------------------------------


def meta(
    blocks: dict[str, dict], fao: dict, jrc: dict, usage: dict, primary_doc: dict
) -> dict:
    observed = sum(
        1
        for block in blocks.values()
        for entry in block["crops"].values()
        if entry.get("data_status") == "observed"
    )
    primary_entries = sum(
        1
        for block in blocks.values()
        for entry in block["crops"].values()
        if entry.get("evidence")
    )
    primary_sources: dict[str, int] = {}
    for block in blocks.values():
        for entry in block["crops"].values():
            evidence = entry.get("evidence")
            if not evidence:
                continue
            source = entry["seasons"][0]["source"] if entry["seasons"] else ""
            primary_sources[source] = primary_sources.get(source, 0) + 1
    top = top_producers()
    exceptions = list((primary_doc.get("_meta") or {}).get("top15_exceptions") or [])
    top_observed = {
        crop: sum(
            1
            for iso in iso_list
            if blocks[iso]["crops"][crop].get("data_status") == "observed"
        )
        for crop, iso_list in top.items()
    }
    tiers: dict[str, dict] = {
        tier: {"entries": 0, "donors": {}} for tier in RULE_TIERS
    }
    for block in blocks.values():
        for entry in block["crops"].values():
            rule = entry.get("rule") or {}
            tier = rule.get("tier")
            if tier not in tiers:
                continue
            tiers[tier]["entries"] += 1
            donors = tiers[tier]["donors"]
            donors[rule["donor"]] = donors.get(rule["donor"], 0) + 1
    for tier in tiers.values():
        tier["donors"] = dict(sorted(tier["donors"].items()))
    return {
        "dataset": "crop_calendar",
        "version": "g2",
        "generated_by": "tools/build_agri_calendar.py",
        "retrieved": RETRIEVED,
        "nations": len(blocks),
        "crops": list(CROPS),
        "month_source": "janelas mensais (1..12); conversão de dekad = mês do 1º dia da dekad",
        "rule": (
            "país sem calendário observado herda as janelas de um país observado DO MESMO "
            "HEMISFÉRIO E DA MESMA FAIXA de latitude (agri_common.climate_band), nesta ordem: "
            "(a) mesma `region` do jogo (data/definitions/scenario/nations.json, ~continente), o "
            "mais próximo por distância geodésica entre centroides = tier 'same_region'; (b) sem "
            "doador no próprio continente, outra `region` da mesma faixa = "
            "'same_band_other_region', com 'cross_continent': true e 'justification'. Faixa de "
            "latitude vizinha (clima diferente) e hemisfério oposto NÃO são aceitos. "
            "data_status='rule_neighbor' e o doador fica em 'rule' = {tier, donor, distance_km, "
            "[cross_continent], [justification]}; sem candidato: data_status='no_data' com "
            "'reason' em pt-BR (nunca se inventa mês)"
        ),
        "rule_scope": (
            "a regra é só para os produtores pequenos; o top-%d de cada cultura (por produção do "
            "próprio crop_yields.json, FAOSTAT) tem de ser observado — o build falha se não for"
            % TOP_N
        ),
        "rule_tiers": tiers,
        "primary_layer": {
            "file": "tools/agri_calendar_primary.json",
            "entries": primary_entries,
            "by_source": dict(sorted(primary_sources.items())),
            "top15_observed": top_observed,
            "top15_n": TOP_N,
            "top15_exceptions": exceptions,
            "evidence": "cada entrada primária traz `evidence` = {report, quote, url, basis}",
        },
        "aggregation": (
            "todas as janelas do par país×cultura viram cobertura de meses; blocos separados "
            "por >= 3 meses sem cobertura viram safras distintas (máximo 2); plantio começa no "
            "primeiro mês coberto e termina no último, colheita idem; se plantio e colheita não "
            "tiverem o mesmo número de blocos (1 ou 2), usa-se 1 safra agregada "
            "(mín. de início / máx. de fim) com 'multi_block_fallback' em 'note' e o rótulo "
            "bruto em 'window_labels'. A camada primária NÃO passa por essa agregação: as safras "
            "dela entram como o documento as descreve"
        ),
        "source_priority": "PRIMARY > FAO_CROP_CALENDAR > JRC_ASAP > RULE_NEIGHBOR",
        "all_year": (
            "allYear=yes na FAO em um par país×cultura sem nenhum mês publicado vira "
            "'all_year': true com 'seasons': [] (a fonte não publica meses; nada é inferido). "
            "Quando o mesmo par tem linhas allYear e janelas datadas, as janelas datadas vencem "
            "e as linhas de ano inteiro são ignoradas — os pares afetados estão em "
            "'all_year_pairs.mixed'. Pares com a mesma cultura sem "
            "calendário observado herdam o all_year do doador"
        ),
        "all_year_pairs": {
            "only_year_round": fao["all_year_only_pairs"],
            "mixed_ignored": fao["all_year_mixed_pairs"],
            "fao_year_round_shadowing_jrc": fao["all_year_shadowing_jrc"],
        },
        "season_fields": (
            "season, planting/start_month+end_month, harvest/start_month+end_month, "
            "window_labels, note (pt-BR ou null), source, source_year"
        ),
        "source_year": (
            "FAO: maior 'lastUpdated' do par país×cultura (ano de referência da publicação); "
            "JRC_ASAP: null (a fonte não publica ano por linha, é climatologia); "
            "RULE_NEIGHBOR: source/source_year são os da safra herdada do doador"
        ),
        "crop_names_fao": {
            "map": FAO_CROP_NAMES,
            "names_seen": fao["crop_names_found"],
            "excluded": FAO_EXCLUDED,
            "note": (
                "este dataset da FAO não usa 'Maize' nem 'Potatoes'/'Soya': o milho aparece "
                "como 'Corn' e o trigo como 'Wheat', 'Wheat, bread' e 'Wheat, durum'"
            ),
        },
        "crop_names_jrc": {
            "map_base": JRC_CROP_NAMES,
            "found": jrc["crop_names_found"],
            "variants": jrc["variants"],
            "note": "nome base = nome sem o parêntese de variante; todas as variantes foram mapeadas",
        },
        "name_aliases": NAME_ALIASES,
        "source_rows": {
            "FAO_CROP_CALENDAR": fao["rows"],
            "JRC_ASAP": jrc["rows"],
        },
        "source_countries": {
            "FAO_CROP_CALENDAR": fao["countries_in_source"],
            "JRC_ASAP": jrc["countries_in_source"],
        },
        "unmatched_source_countries": {
            "FAO_CROP_CALENDAR": fao["unmatched_countries"],
            "JRC_ASAP": jrc["unmatched_countries"],
            "note": "nomes da fonte sem nação correspondente no jogo; não foram usados",
        },
        "observed_entries": observed,
        "rule_donors": dict(sorted(usage.items())),
        "latitude_source": (
            "agri_common.centroids(): média das cidades ponderada pela população de cenário"
        ),
    }


def sources(primary_doc: dict) -> dict:
    out = {
        "FAO_CROP_CALENDAR": {
            "title": "FAO Crop Calendar (national summary)",
            "publisher": "FAO",
            "url": (
                "https://data.apps.fao.org/catalog/dataset/"
                "crop-calendar-by-country-crop-and-activity"
            ),
            "license": "CC BY 4.0",
            "accessed": RETRIEVED,
        },
        "JRC_ASAP": {
            "title": "ASAP national crop calendars",
            "publisher": "European Commission JRC",
            "doi": "10.2905/JRC.PXQH3Q0",
            "url": "https://mars.jrc.ec.europa.eu/asap/files/crop_calendar_gaul0.zip",
            "license": "EU reuse notice",
            "accessed": RETRIEVED,
        },
        "RULE_NEIGHBOR": {
            "title": "Regra declarada: vizinho observado no mesmo hemisfério e faixa",
            "publisher": "OpenGS project",
            "year": 2026,
        },
    }
    for key, value in (primary_doc.get("sources") or {}).items():
        if key in out:
            raise SystemExit(f"[erro] camada primária reusa a chave de fonte {key!r}")
        out[key] = value
    return out


def validate(payload: dict, geo: dict[str, tuple[float, float]]) -> None:
    problems: list[str] = []
    for iso, block in payload["nations"].items():
        if block["hemisphere"] not in ("north", "south"):
            problems.append(f"{iso}: hemisfério inválido {block['hemisphere']!r}")
        for crop, entry in block["crops"].items():
            if not entry.get("data_status"):
                problems.append(f"{iso}/{crop}: sem data_status")
                continue
            if entry["data_status"] == "no_data" and not entry.get("reason"):
                problems.append(f"{iso}/{crop}: no_data sem motivo")
            if entry["data_status"] == "rule_neighbor":
                rule = entry.get("rule") or {}
                if not rule.get("donor"):
                    problems.append(f"{iso}/{crop}: rule_neighbor sem doador")
                if rule.get("tier") not in RULE_TIERS:
                    problems.append(f"{iso}/{crop}: tier inválido {rule.get('tier')!r}")
                if not isinstance(rule.get("distance_km"), (int, float)):
                    problems.append(f"{iso}/{crop}: distance_km inválida")
                donor = rule.get("donor")
                if donor in geo:
                    # a regra é mesma faixa + mesmo hemisfério, sem exceção
                    if hemisphere(geo[donor][0]) != block["hemisphere"]:
                        problems.append(f"{iso}/{crop}: doador {donor} em hemisfério oposto")
                    if climate_band(geo[donor][0]) != climate_band(geo[iso][0]):
                        problems.append(f"{iso}/{crop}: doador {donor} em faixa climática diferente")
                    if rule.get("tier") == "same_band_other_region" and not rule.get(
                        "justification"
                    ):
                        problems.append(f"{iso}/{crop}: cross-continent sem justificativa")
            if entry["data_status"] == "observed" and entry.get("rule") is not None:
                problems.append(f"{iso}/{crop}: observed com rule preenchida")
            if entry.get("all_year") and entry["seasons"]:
                problems.append(f"{iso}/{crop}: all_year com safras")
            for season in entry["seasons"]:
                for key in ("planting", "harvest"):
                    window = season[key]
                    if window is None:
                        continue
                    for edge in ("start_month", "end_month"):
                        value = window[edge]
                        if not isinstance(value, int) or isinstance(value, bool):
                            problems.append(f"{iso}/{crop}/{key}.{edge}: não inteiro ({value!r})")
                        elif not 1 <= value <= 12:
                            problems.append(f"{iso}/{crop}/{key}.{edge}: fora de 1..12 ({value})")
                if not season.get("source"):
                    problems.append(f"{iso}/{crop}: safra sem source")
    if problems:
        print("[erro] validação falhou:")
        for problem in problems[:20]:
            print(f"  - {problem}")
        raise SystemExit(1)


# --------------------------------------------------------------------------
# autocheck e amostra
# --------------------------------------------------------------------------


def autocheck(payload: dict) -> None:
    blocks = payload["nations"]
    total = sum(len(block["crops"]) for block in blocks.values())
    print(f"[check] nações={len(blocks)} culturas={len(CROPS)} entradas={total} (esperado 895)")

    print("[check] contagem por data_status:")
    for crop in CROPS:
        statuses = {"observed": 0, "rule_neighbor": 0, "no_data": 0}
        for block in blocks.values():
            statuses[block["crops"][crop]["data_status"]] += 1
        print(
            f"  {crop:7s} observed={statuses['observed']:3d} "
            f"rule_neighbor={statuses['rule_neighbor']:3d} no_data={statuses['no_data']:3d}"
        )

    bad_months = 0
    identical: list[str] = []
    null_windows = 0
    seasons_total = 0
    all_year: list[str] = []
    for iso, block in blocks.items():
        for crop, entry in block["crops"].items():
            if entry.get("all_year"):
                all_year.append(f"{iso}/{crop}")
            for season in entry["seasons"]:
                seasons_total += 1
                for key in ("planting", "harvest"):
                    window = season[key]
                    if window is None:
                        null_windows += 1
                        continue
                    for edge in ("start_month", "end_month"):
                        value = window[edge]
                        if value is None or not 1 <= value <= 12:
                            bad_months += 1
                plant = season["planting"]
                crop_harvest = season["harvest"]
                if plant and crop_harvest and (
                    plant["start_month"],
                    plant["end_month"],
                ) == (crop_harvest["start_month"], crop_harvest["end_month"]):
                    identical.append(f"{iso}/{crop}#{season['season']}")

    print(f"[check] safras={seasons_total} meses fora de 1..12 (ou nulos): {bad_months}")
    print(f"[check] janelas nulas (fonte não publica plantio ou colheita): {null_windows}")
    print(f"[check] safras com plantio == colheita idênticos: {len(identical)}")
    for item in identical[:15]:
        print(f"  - {item}")

    observed_by_source: dict[str, int] = {"FAO_CROP_CALENDAR": 0, "JRC_ASAP": 0}
    for block in blocks.values():
        for entry in block["crops"].values():
            if entry["data_status"] != "observed":
                continue
            sources_used = {season["source"] for season in entry["seasons"]}
            for source in sources_used:
                observed_by_source[source] = observed_by_source.get(source, 0) + 1
    print(f"[check] entradas observadas por fonte: {observed_by_source}")
    primary = payload["_meta"].get("primary_layer") or {}
    print(
        f"[check] camada primária: {primary.get('entries', 0)} entradas "
        f"de {len(primary.get('by_source') or {})} fontes"
    )
    top = primary.get("top15_observed") or {}
    if top:
        line = ", ".join(f"{crop}={n}/{primary.get('top15_n')}" for crop, n in top.items())
        print(f"[check] top-{primary.get('top15_n')} observado por cultura: {line}")
    if primary.get("top15_exceptions"):
        print(f"[check] exceções declaradas: {primary['top15_exceptions']}")
    print(f"[check] entradas all_year: {len(all_year)}")
    if all_year:
        print(f"  {', '.join(all_year[:20])}{' …' if len(all_year) > 20 else ''}")

    donors = payload["_meta"]["rule_donors"]
    if donors:
        print(f"[check] doadores usados na regra ({len(donors)} países):")
        line = ", ".join(f"{iso}({count})" for iso, count in donors.items())
        print(f"  {line}")
    else:
        print("[check] nenhum doador usado na regra")

    tiers = payload["_meta"]["rule_tiers"]
    print("[check] doadores por tier:")
    for tier, data in tiers.items():
        line = ", ".join(f"{iso}({count})" for iso, count in data["donors"].items())
        print(f"  {tier}: {data['entries']} entradas — {line or 'nenhum'}")

    gaps = [
        f"{iso}/{crop}"
        for iso, block in blocks.items()
        for crop, entry in block["crops"].items()
        if entry["data_status"] == "no_data"
    ]
    print(f"[check] sem calendário possível (no_data): {len(gaps)}")
    if gaps:
        print(f"  {', '.join(gaps)}")


def print_samples(payload: dict) -> None:
    blocks = payload["nations"]
    print("[amostra] conferência contra as fontes")
    for iso, crop in SAMPLES:
        block = blocks[iso]
        entry = block["crops"][crop]
        print(
            f"  {iso} {crop}: status={entry['data_status']} "
            f"lat={block['latitude']} hemisfério={block['hemisphere']}"
        )
        rule = entry.get("rule")
        if rule:
            print(
                f"    regra: tier={rule['tier']} doador={rule['donor']} "
                f"distância={rule['distance_km']} km"
            )
        if entry.get("reason"):
            print(f"    motivo: {entry['reason']}")
        if entry.get("all_year"):
            print("    all_year: safra contínua declarada pela fonte (sem meses)")
        if not entry["seasons"]:
            print("    safras: nenhuma")
            continue
        for season in entry["seasons"]:
            plant = season["planting"]
            crop_harvest = season["harvest"]

            def window_text(window: dict | None) -> str:
                if not window:
                    return "—"
                return (
                    f"{MONTH_LABELS_PT[window['start_month'] - 1]}.."
                    f"{MONTH_LABELS_PT[window['end_month'] - 1]}"
                    f" ({window['start_month']}-{window['end_month']})"
                )

            print(
                f"    safra {season['season']}: plantio {window_text(plant)} "
                f"colheita {window_text(crop_harvest)} "
                f"| fonte={season['source']} ano={season['source_year']}"
            )
            if season["window_labels"]:
                labels = ", ".join(season["window_labels"][:4])
                more = " …" if len(season["window_labels"]) > 4 else ""
                print(f"      rótulos: {labels}{more}")
            if season["note"]:
                print(f"      nota: {season['note']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--fetch",
        action="store_true",
        help="baixa o cache local que faltar (nunca rebaixa o existente)",
    )
    args = parser.parse_args()

    ensure_cache(args.fetch)
    payload = build()
    write_json(OUT_PATH, payload)

    text = OUT_PATH.read_bytes()
    print(f"[out] {OUT_PATH.relative_to(ROOT)} ({len(text)} bytes)")
    print(f"[out] sha256={hashlib.sha256(text).hexdigest()}")
    autocheck(payload)
    print_samples(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
