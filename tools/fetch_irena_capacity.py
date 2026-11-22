#!/usr/bin/env python3
"""Extrai a capacidade elétrica instalada (MW) da IRENA por país e tecnologia.

Consulta a tabela PXWeb Country_ELECSTAT_2026_H2_PX (IRENASTAT, "Electricity
statistics by Country/area, Technology, Data Type, Grid connection and Year"),
filtra Data Type = Electrical Installed Capacity (MW), Grid connection = All e
os anos 2024 e 2025, e grava:

  tools/data/irena_capacity.csv            country_iso3,technology,capacity_mw,year
  tools/data/irena_capacity_manifest.json  procedência, citação e licença
  tools/data/irena_capacity_raw.json       resposta crua da API (proveniência)

Regras aplicadas:
  - só linhas com valor numérico > 0 (ausência de dado NÃO vira zero);
  - capacity_mw formatado com 6 casas decimais;
  - ordenação ASCII por country_iso3, technology, year;
  - UTF-8 sem BOM, quebras de linha LF (duas execuções geram o mesmo SHA-256).

Só a biblioteca padrão do Python é usada.

Uso: python tools/fetch_irena_capacity.py
"""

from __future__ import annotations

import csv
import hashlib
import io
import json
import math
import os
import re
import urllib.error
import urllib.request
from datetime import datetime, timezone

# Valor de uma celula da API: texto no CSV, numero/None no json-stat2.
CellValue = str | float | int | None

BASE_URL = "https://pxweb.irena.org/api/v1/en/IRENASTAT/Power%20Capacity%20and%20Generation"
TABLE_ID = "Country_ELECSTAT_2026_H2_PX"
TABLE_FILE = TABLE_ID + ".px"
TABLE_URL = BASE_URL + "/" + TABLE_FILE

YEARS = ("24", "25")
TECH_TOTAL_NON_RENEWABLE = "Total Non-Renewable"
TECH_TOTAL_RENEWABLE = "Total Renewable"
REPORT_COUNTRIES = ("BRA", "NOR", "CHN", "DEU", "NGA", "USA")

USER_AGENT = "opengs-game-data-fetcher/1.0 (research script, Python urllib)"
HTTP_TIMEOUT = 240

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "tools", "data")
CSV_PATH = os.path.join(DATA_DIR, "irena_capacity.csv")
MANIFEST_PATH = os.path.join(DATA_DIR, "irena_capacity_manifest.json")
RAW_PATH = os.path.join(DATA_DIR, "irena_capacity_raw.json")

CSV_HEADER = ("country_iso3", "technology", "capacity_mw", "year")

QUERY = {
    "query": [
        {"code": "Country/area", "selection": {"filter": "all", "values": ["*"]}},
        {"code": "Technology", "selection": {"filter": "all", "values": ["*"]}},
        {"code": "Data Type", "selection": {"filter": "item", "values": ["1"]}},
        {"code": "Grid connection", "selection": {"filter": "item", "values": ["0"]}},
        {"code": "Year", "selection": {"filter": "item", "values": list(YEARS)}},
    ],
    "response": {"format": "csv"},
}

# Termos de uso reais da IRENA (consultados em 2026-11: irena.org responde 403
# para acesso automatizado, o texto abaixo foi lido da página de termos e
# condições arquivada). Não é CC BY: uso livre apenas para fins não comerciais
# e com atribuição.
LICENSE_INFO = {
    "name": "IRENA Terms and Conditions - use of IRENA's Content",
    "url": "https://www.irena.org/terms-and-conditions",
    "archived_url": "https://web.archive.org/web/20260101010016/https://www.irena.org/terms-and-conditions",
    "last_updated": "6 Dec 2022",
    "section": "4.1 (Intellectual property rights)",
    "quote": (
        "Subject to article 4.3 below, Users may freely access, copy, share, download, "
        "reproduce, print and/or view IRENA's Content for non-commercial purposes, provided "
        "that appropriate attribution is given to IRENA. Use of IRENA's name in the full or "
        "abbreviated form is allowed for attribution purposes, in accordance with this Section "
        "and the Terms and Conditions. Use of IRENA's logo in connection with such attribution "
        "is expressly prohibited without IRENA's prior authorization. No other use shall be "
        "made of IRENA's Content without IRENA's advanced written permission."
    ),
    "summary": (
        "Uso livre (acesso, cópia, download, reprodução) para fins NAO comerciais com "
        "atribuição adequada a IRENA; uso do logo proibido sem autorização; nenhum outro uso "
        "sem permissão escrita. Nao e CC BY 4.0."
    ),
    "requires_attribution": True,
    "commercial_use": "not allowed without prior written permission from IRENA (art. 4.5, info@irena.org)",
    "no_endorsement": True,
    "notes": (
        "A pagina https://www.irena.org/Data e o PDF IRENA_statistics_terms_of_use.pdf "
        "responderam HTTP 403/404 para acesso automatizado na data da extracao; o texto acima "
        "veio da pagina oficial de Termos e Condicoes (art. 4.1), ultima atualizacao 6 Dec 2022."
    ),
}

CITATION = "IRENA, Renewable Energy Statistics / Electricity statistics (IRENASTAT), {year}"

_NUM_PLAIN = re.compile(r"^-?\d+(?:\.\d+)?$")
_NUM_THOUSANDS = re.compile(r"^-?\d{1,3}(?:,\d{3})+(?:\.\d+)?$")
_NUM_DECIMAL_COMMA = re.compile(r"^-?\d+(?:,\d+)$")
_MISSING_MARKERS = {"", ".", "..", "...", "-", ":", "n/a", "na", "n.a."}


class FetchError(RuntimeError):
    """Falha ao obter ou interpretar a resposta da API PXWeb."""


def _request(url: str, payload: dict | None = None) -> tuple[bytes, str, str]:
    data = None
    headers = {"User-Agent": USER_AGENT, "Accept": "*/*"}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            return resp.read(), resp.headers.get("Content-Type", ""), str(resp.status)
    except urllib.error.HTTPError as exc:
        raise FetchError(f"HTTP {exc.code} em {url}: {exc.reason}") from exc
    except urllib.error.URLError as exc:
        raise FetchError(f"falha de rede em {url}: {exc.reason}") from exc


def _charset_of(content_type: str) -> str:
    for part in content_type.split(";")[1:]:
        key, _, value = part.strip().partition("=")
        if key.lower() == "charset" and value:
            return value.strip().strip('"')
    return "utf-8"


def _decode(body: bytes, content_type: str) -> tuple[str, str]:
    """Decodifica a resposta e devolve (texto, charset usado)."""
    charset = _charset_of(content_type)
    for candidate in (charset, "utf-8-sig", "cp1252", "latin-1"):
        try:
            return body.decode(candidate), candidate
        except (UnicodeDecodeError, LookupError):
            continue
    return body.decode("latin-1", errors="replace"), "latin-1/replace"


def fetch_json(url: str) -> dict:
    body, _content_type, _status = _request(url)
    try:
        return json.loads(body.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise FetchError(f"resposta nao e JSON valido em {url}: {exc}") from exc


def parse_number(raw: str | float | int | None) -> float | None:
    """Converte o valor textual da API em float; None quando ausente/invalido."""
    if raw is None:
        return None
    if isinstance(raw, (int, float)):
        return float(raw)
    text = raw.strip().replace("\u00a0", "").replace(" ", "").replace("\u2019", "")
    if text.lower() in _MISSING_MARKERS:
        return None
    if _NUM_THOUSANDS.match(text):
        text = text.replace(",", "")
    elif _NUM_DECIMAL_COMMA.match(text):
        text = text.replace(",", ".")
    elif not _NUM_PLAIN.match(text):
        return None
    try:
        return float(text)
    except ValueError:
        return None


def parse_cells_from_csv(text: str) -> list[tuple[str, str, str, CellValue]]:
    """Converte o CSV do PXWeb em (pais, tecnologia, ano, valor)."""
    reader = csv.reader(io.StringIO(text))
    try:
        header = next(reader)
    except StopIteration as exc:
        raise FetchError("resposta CSV vazia") from exc
    if len(header) < 6:
        raise FetchError(f"cabecalho CSV inesperado: {header}")
    cells: list[tuple[str, str, str, CellValue]] = []
    for row in reader:
        if len(row) < 6:
            raise FetchError(f"linha CSV inesperada: {row}")
        cells.append((row[0], row[1], row[4], row[5]))
    return cells


def _category_order(category: dict) -> list[str]:
    """Ordem dos codigos de uma dimensao json-stat2 (dict index ou lista)."""
    index = category.get("index", {})
    if isinstance(index, dict):
        return [code for code, _pos in sorted(index.items(), key=lambda item: item[1])]
    return list(index)


def parse_cells_from_jsonstat2(dataset: dict) -> list[tuple[str, str, str, CellValue]]:
    """Converte json-stat2 em (pais, tecnologia, ano, valor)."""
    dims = dataset.get("id")
    sizes = dataset.get("size")
    dimensions = dataset.get("dimension", {})
    if not dims or not sizes:
        raise FetchError("json-stat2 sem 'id'/'size'")
    try:
        country_pos = dims.index("Country/area")
        tech_pos = dims.index("Technology")
        year_pos = dims.index("Year")
    except ValueError as exc:
        raise FetchError("json-stat2 sem as dimensoes esperadas") from exc

    orders: list[list[str]] = []
    labels: list[dict] = []
    for dim in dims:
        category = dimensions.get(dim, {}).get("category", {})
        orders.append(_category_order(category))
        labels.append(category.get("label", {}))

    strides = [1] * len(sizes)
    for pos in range(len(sizes) - 2, -1, -1):
        strides[pos] = strides[pos + 1] * sizes[pos + 1]

    raw_values = dataset.get("value", [])
    if isinstance(raw_values, dict):
        values: list[object] = [raw_values.get(str(i)) for i in range(math.prod(sizes))]
    else:
        values = list(raw_values)

    cells: list[tuple[str, str, str, CellValue]] = []
    for flat, value in enumerate(values):
        coords = [(flat // strides[pos]) % sizes[pos] for pos in range(len(sizes))]
        country_code = orders[country_pos][coords[country_pos]]
        tech_code = orders[tech_pos][coords[tech_pos]]
        year_code = orders[year_pos][coords[year_pos]]
        country = labels[country_pos].get(country_code, country_code)
        tech = labels[tech_pos].get(tech_code, tech_code)
        year = labels[year_pos].get(year_code, year_code)
        cells.append((country, tech, str(year), value))
    return cells


def fetch_table_cells() -> tuple[list[tuple[str, str, str, CellValue]], str, dict]:
    """Baixa a tabela; tenta CSV e, se falhar, json-stat2. Devolve células, formato e resposta crua."""
    try:
        body, content_type, _status = _request(TABLE_URL, QUERY)
        text, charset = _decode(body, content_type)
        cells = parse_cells_from_csv(text)
        raw = {"format_requested": "csv", "format_used": "csv", "content_type": content_type,
               "charset_used": charset, "content": text}
        return cells, "csv", raw
    except (FetchError, ValueError) as csv_error:
        csv_failure = str(csv_error)

    fallback = json.loads(json.dumps(QUERY))
    fallback["response"]["format"] = "json-stat2"
    body, content_type, _status = _request(TABLE_URL, fallback)
    text, charset = _decode(body, content_type)
    try:
        dataset = json.loads(text)
    except json.JSONDecodeError as exc:
        raise FetchError(f"json-stat2 invalido: {exc} (falha anterior: {csv_failure})") from exc
    cells = parse_cells_from_jsonstat2(dataset)
    raw = {"format_requested": "csv", "format_used": "json-stat2", "csv_failure": csv_failure,
           "content_type": content_type, "charset_used": charset, "content": dataset}
    return cells, "json-stat2", raw


def build_rows(cells: list[tuple[str, str, str, CellValue]], country_map: dict[str, str],
               tech_names: set[str], years: set[str]) -> tuple[list[tuple[str, str, float, int]], int, list[str]]:
    """Filtra/normaliza as células em linhas do CSV. Devolve linhas, descartes e nomes desconhecidos."""
    rows: list[tuple[str, str, float, int]] = []
    skipped_nonpositive = 0
    skipped_unparsed = 0
    unknown: list[str] = []
    for country, tech, year, raw_value in cells:
        iso3 = country_map.get(country)
        if iso3 is None:
            unknown.append(f"pais:{country}")
            continue
        if tech not in tech_names:
            unknown.append(f"tecnologia:{tech}")
            continue
        if year not in years:
            unknown.append(f"ano:{year}")
            continue
        value = parse_number(raw_value)
        if value is None:
            skipped_unparsed += 1
            continue
        if value <= 0:
            skipped_nonpositive += 1
            continue
        rows.append((iso3, tech, value, int(year)))
    rows.sort(key=lambda row: (row[0], row[1], row[3]))
    return rows, skipped_nonpositive + skipped_unparsed, unknown


def write_csv(rows: list[tuple[str, str, float, int]]) -> str:
    os.makedirs(DATA_DIR, exist_ok=True)
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\n", quoting=csv.QUOTE_MINIMAL)
    writer.writerow(CSV_HEADER)
    for iso3, tech, value, year in rows:
        writer.writerow((iso3, tech, f"{value:.6f}", str(year)))
    payload = buffer.getvalue().encode("utf-8")
    with open(CSV_PATH, "wb") as handle:
        handle.write(payload)
    return hashlib.sha256(payload).hexdigest()


def write_json(path: str, data: object) -> None:
    os.makedirs(DATA_DIR, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def main() -> int:
    accessed_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    fetched_at = datetime.now(timezone.utc)

    catalog = fetch_json(BASE_URL)
    entry = next((item for item in catalog if item.get("id") == TABLE_FILE), None)
    if entry is None:
        raise FetchError(f"tabela {TABLE_FILE} nao encontrada no catalogo")
    table_title = entry.get("text", "")
    table_updated = entry.get("updated", "")

    metadata = fetch_json(TABLE_URL)
    variables = {var["code"]: var for var in metadata.get("variables", [])}
    country_var = variables["Country/area"]
    country_map = {name: code for code, name in zip(country_var["values"], country_var["valueTexts"])}
    year_var = variables["Year"]
    year_labels = {year_var["valueTexts"][year_var["values"].index(code)] for code in YEARS}
    tech_var = variables["Technology"]
    tech_names = set(tech_var["valueTexts"])

    missing_tech = {TECH_TOTAL_NON_RENEWABLE, TECH_TOTAL_RENEWABLE} - tech_names
    if missing_tech:
        raise FetchError(f"tecnologias de total ausentes na tabela: {sorted(missing_tech)}")

    cells, format_used, raw_payload = fetch_table_cells()
    rows, skipped, unknown = build_rows(cells, country_map, tech_names, year_labels)
    if unknown:
        sample = ", ".join(sorted(set(unknown))[:10])
        raise FetchError(f"{len(unknown)} celulas com rotulos desconhecidos (ex.: {sample})")

    write_json(RAW_PATH, {
        "source_url": TABLE_URL,
        "table_id": TABLE_ID,
        "accessed_at_utc": accessed_at,
        "response_format_used": format_used,
        "response": raw_payload,
    })

    sha256 = write_csv(rows)

    countries = sorted({row[0] for row in rows})
    technologies = sorted({row[1] for row in rows})
    years_present = sorted({row[3] for row in rows})
    data_years = sorted(int(label) for label in year_labels)

    if not rows:
        raise FetchError("nenhuma linha valida extraida")
    if 2025 not in years_present:
        raise FetchError(f"ano 2025 ausente na saida (anos presentes: {years_present})")

    manifest = {
        "source_url": TABLE_URL,
        "catalog_url": BASE_URL,
        "table_id": TABLE_ID,
        "table_title": table_title,
        "table_updated": table_updated,
        "query_json": QUERY,
        "response_format_used": format_used,
        "accessed_at_utc": accessed_at,
        "license": LICENSE_INFO,
        "citation": CITATION.format(year=fetched_at.year),
        "citation_short": f"IRENA, Electricity statistics (IRENASTAT), {fetched_at.year}",
        "filters": {
            "data_type": "Electrical Installed Capacity (MW)",
            "data_type_code": "1",
            "grid_connection": "All",
            "grid_connection_code": "0",
            "years": data_years,
        },
        "units": {"capacity_mw": "MW"},
        "csv": {
            "path": "tools/data/irena_capacity.csv",
            "columns": list(CSV_HEADER),
            "encoding": "utf-8 (sem BOM)",
            "line_endings": "LF",
            "capacity_decimals": 6,
            "ordering": "country_iso3, technology, year (ASCII)",
            "rule": "somente linhas com capacity_mw > 0; ausencia de dado nao vira zero",
            "sha256": sha256,
        },
        "counts": {
            "row_count": len(rows),
            "country_count": len(countries),
            "technology_count": len(technologies),
            "year_count": len(years_present),
            "years": years_present,
            "cells_received": len(cells),
            "cells_discarded_missing_or_nonpositive": skipped,
        },
        "validation": {
            "min_countries_170": len(countries) >= 170,
            "has_year_2025": 2025 in years_present,
            "has_year_2024": 2024 in years_present,
        },
    }
    write_json(MANIFEST_PATH, manifest)

    print(f"fonte: {TABLE_URL}")
    print(f"tabela: {TABLE_ID} | atualizada: {table_updated} | formato de resposta: {format_used}")
    print(f"linhas: {len(rows)} | paises: {len(countries)} | tecnologias: {len(technologies)}"
          f" | anos: {years_present} | celulas descartadas: {skipped}")
    print("")
    print(f"{'pais':<5} {'ano':<5} {'Total Non-Renewable':>21} {'Total Renewable':>17} {'total geral':>14}")
    for iso3 in REPORT_COUNTRIES:
        for year in (2024, 2025):
            non_renewable = sum(r[2] for r in rows if r[0] == iso3 and r[1] == TECH_TOTAL_NON_RENEWABLE and r[3] == year)
            renewable = sum(r[2] for r in rows if r[0] == iso3 and r[1] == TECH_TOTAL_RENEWABLE and r[3] == year)
            print(f"{iso3:<5} {year:<5} {non_renewable:>21,.2f} {renewable:>17,.2f} {non_renewable + renewable:>14,.2f}")
    print("")
    print(f"csv: {CSV_PATH}")
    print(f"sha256: {sha256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
