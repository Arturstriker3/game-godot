#!/usr/bin/env python3
"""Coleta indicadores de energia e saneamento do WDI (Banco Mundial) via API Data360.

Saida:
  tools/data/wdi_energy.json           - valores por pais ISO-3 (ano mais recente nao vazio)
  tools/data/wdi_energy_manifest.json  - citacao, licenca, codigos e contagem por indicador

Fonte: World Bank, World Development Indicators (WDI), via Data360 API
(https://data360api.worldbank.org/data360/data, DATABASE_ID=WB_WDI). Licenca CC BY 4.0.

Regras:
  - Somente stdlib; sem dependencias externas.
  - Uma consulta por indicador, paginada (top=1000 + skip), com timeout de 120s e 2 retries.
  - Para cada pais, guarda o ano mais recente com OBS_VALUE numerico.
  - Agregados regionais/de renda (ex.: WLD, SSA, EAS) sao excluidos: a lista de paises vem
    do endpoint de paises do Banco Mundial (region.id == "NA" marca agregado).
  - Idempotente e deterministico: mesmas chaves ordenadas, floats com ate 3 casas,
    UTF-8 sem BOM, LF, indent=1. So fetched_at_utc (e a data da citacao) muda entre execucoes.

Uso:
  python tools/fetch_wdi_energy.py
"""

from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

DATA360_DATA_URL = "https://data360api.worldbank.org/data360/data"
DATABASE_ID = "WB_WDI"
COUNTRY_LIST_URL = "https://api.worldbank.org/v2/country?format=json&per_page=400"
USER_AGENT = "opengs-game-wdi-energy-fetch/1.0 (stdlib urllib)"
HTTP_TIMEOUT = 120
HTTP_RETRIES = 2
PAGE_SIZE = 1000

PROJECT_ROOT = Path(__file__).resolve().parent.parent
OUT_DATA = PROJECT_ROOT / "tools" / "data" / "wdi_energy.json"
OUT_MANIFEST = PROJECT_ROOT / "tools" / "data" / "wdi_energy_manifest.json"

# (codigo WDI, chave de saida, definicao curta em pt-BR)
INDICATORS: list[tuple[str, str, str]] = [
    ("EG.ELC.ACCS.ZS", "electricity_access_pct",
     "Acesso a eletricidade, % da populacao"),
    ("EG.ELC.ACCS.UR.ZS", "electricity_access_urban_pct",
     "Acesso a eletricidade, % da populacao urbana"),
    ("EG.ELC.ACCS.RU.ZS", "electricity_access_rural_pct",
     "Acesso a eletricidade, % da populacao rural"),
    ("EG.ELC.LOSS.ZS", "tnd_losses_pct",
     "Perdas de transmissao e distribuicao, % da producao"),
    ("SH.H2O.SMDW.ZS", "water_safely_managed_pct",
     "Agua potavel gerida com seguranca, % da populacao (JMP WHO/UNICEF)"),
    ("SH.H2O.SMDW.UR.ZS", "water_safely_managed_urban_pct",
     "Agua potavel gerida com seguranca, % da populacao urbana (JMP WHO/UNICEF)"),
    ("SH.H2O.SMDW.RU.ZS", "water_safely_managed_rural_pct",
     "Agua potavel gerida com seguranca, % da populacao rural (JMP WHO/UNICEF)"),
]

REPORT_COUNTRIES = ("BRA", "NOR", "CHN", "DEU", "NGA", "USA")

# Valores que a API pode devolver no lugar de um numero.
EMPTY_MARKERS = {"", "..", "na", "n/a", "null", "none"}


def data360_code(wdi_code: str) -> str:
    """Converte o codigo WDI (com pontos) no codigo Data360 (WB_WDI_ + underscores)."""
    return "WB_WDI_" + wdi_code.replace(".", "_")


def http_json(url: str, retries: int = HTTP_RETRIES) -> object:
    """GET que devolve JSON, com ate `retries` novas tentativas em erro de rede."""
    last_error: Exception | None = None
    for attempt in range(retries + 1):
        request = urllib.request.Request(
            url,
            headers={"User-Agent": USER_AGENT, "Accept": "application/json"},
        )
        try:
            with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
                return json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError,
                json.JSONDecodeError, OSError) as error:  # rede, HTTP e payload invalido
            last_error = error
            if attempt < retries:
                time.sleep(2 * (attempt + 1))
    raise RuntimeError(f"falha de rede em {url}: {last_error}") from last_error


def fetch_indicator_rows(series_code: str) -> tuple[list[dict], int]:
    """Baixa todas as linhas de um indicador, paginando; devolve (linhas, count declarado)."""
    rows: list[dict] = []
    declared_count: int | None = None
    skip = 0
    while True:
        url = (
            f"{DATA360_DATA_URL}?DATABASE_ID={DATABASE_ID}"
            f"&INDICATOR={series_code}&top={PAGE_SIZE}&skip={skip}"
        )
        payload = http_json(url)
        if not isinstance(payload, dict):
            raise RuntimeError(f"payload inesperado para {series_code}: {type(payload).__name__}")
        declared_count = int(payload.get("count") or 0)
        page = payload.get("value") or []
        rows.extend(page)
        if not page or len(rows) >= declared_count:
            break
        skip += PAGE_SIZE

    # Sem linhas duplicadas nem faltantes: a paginacao tem que fechar exatamente com `count`.
    unique = {(row.get("REF_AREA"), row.get("TIME_PERIOD")) for row in rows}
    if len(unique) != len(rows) or len(rows) != declared_count:
        raise RuntimeError(
            f"{series_code}: paginacao inconsistente "
            f"(linhas={len(rows)}, unicas={len(unique)}, count={declared_count})"
        )
    return rows, declared_count


def fetch_country_filter() -> tuple[set[str], set[str]]:
    """Lista de paises (region.id != 'NA') e de agregados (region.id == 'NA') do Banco Mundial."""
    payload = http_json(COUNTRY_LIST_URL)
    if not isinstance(payload, list) or len(payload) < 2 or not isinstance(payload[1], list):
        raise RuntimeError("formato inesperado na lista de paises do Banco Mundial")
    countries: set[str] = set()
    aggregates: set[str] = set()
    for entry in payload[1]:
        code = entry.get("id")
        region = (entry.get("region") or {}).get("id")
        if not isinstance(code, str) or len(code) != 3:
            continue
        if region == "NA":
            aggregates.add(code.upper())
        else:
            countries.add(code.upper())
    if not countries:
        raise RuntimeError("lista de paises do Banco Mundial veio vazia")
    return countries, aggregates


def parse_value(raw: object) -> float | None:
    """Converte OBS_VALUE em float; devolve None para ausencia/vazio."""
    if raw is None:
        return None
    text = str(raw).strip()
    if text.lower() in EMPTY_MARKERS:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def latest_non_empty_year(rows: list[dict], allowed: set[str]) -> dict[str, dict]:
    """Area -> {value, year} com o ano mais recente nao vazio (somente areas permitidas)."""
    latest: dict[str, dict] = {}
    for row in rows:
        area = str(row.get("REF_AREA") or "").upper()
        if len(area) != 3 or area not in allowed:
            continue
        year_raw = str(row.get("TIME_PERIOD") or "").strip()
        if not year_raw.isdigit():
            continue
        year = int(year_raw)
        value = parse_value(row.get("OBS_VALUE"))
        if value is None:
            continue
        current = latest.get(area)
        if current is None or year > current["year"]:
            latest[area] = {"value": value, "year": year}
    return latest


def build_access_date(now: datetime) -> str:
    return now.strftime("%Y-%m-%d")


def write_json(path: Path, payload: object) -> None:
    """UTF-8 sem BOM, LF, indent=1, com newline final."""
    text = json.dumps(payload, ensure_ascii=False, indent=1, sort_keys=False) + "\n"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(text.encode("utf-8"))


def main() -> int:
    now = datetime.now(timezone.utc)
    fetched_at = now.strftime("%Y-%m-%dT%H:%M:%SZ")
    access_date = build_access_date(now)
    citation = (
        "World Bank, World Development Indicators (WDI), "
        f"accessed via Data360 API, {access_date}"
    )

    countries, aggregates = fetch_country_filter()

    per_country: dict[str, dict] = {}
    indicator_meta: list[dict] = []
    no_data: list[dict] = []

    for wdi_code, key, definition in INDICATORS:
        series_code = data360_code(wdi_code)
        try:
            rows, declared_count = fetch_indicator_rows(series_code)
        except RuntimeError as error:
            no_data.append({
                "key": key,
                "wdi_code": wdi_code,
                "data360_code": series_code,
                "error": str(error),
            })
            print(f"[no_data] {key} ({series_code}): {error}")
            continue

        latest = latest_non_empty_year(rows, countries)
        if not latest:
            no_data.append({
                "key": key,
                "wdi_code": wdi_code,
                "data360_code": series_code,
                "error": f"resposta com count={declared_count} mas nenhum valor numerico para paises",
            })
            print(f"[no_data] {key} ({series_code}): sem valores numericos")
            continue

        years = [item["year"] for item in latest.values()]
        for area, item in latest.items():
            per_country.setdefault(area, {})[key] = {
                "value": round(item["value"], 3),
                "year": item["year"],
                "source": series_code,
            }

        indicator_meta.append({
            "key": key,
            "wdi_code": wdi_code,
            "data360_code": series_code,
            "definition_pt": definition,
            "units": "percent",
            "countries_with_data": len(latest),
            "rows_downloaded": len(rows),
            "year_min": min(years),
            "year_max": max(years),
            "status": "ok",
        })
        print(f"[ok] {key:<34} {series_code:<28} paises={len(latest):<4} anos={min(years)}-{max(years)}")

    if not per_country:
        print("ERRO: nenhum indicador coletado; nada foi gravado.", file=sys.stderr)
        return 1

    ordered_indicators = {iso3: per_country[iso3] for iso3 in sorted(per_country)}

    dataset = {
        "meta": {
            "source": "World Bank WDI via Data360 API",
            "license": "CC BY 4.0",
            "citation": citation,
            "method": "latest_non_empty_year_per_country",
            "fetched_at_utc": fetched_at,
        },
        "indicators": ordered_indicators,
    }

    manifest = {
        "source": "World Bank WDI via Data360 API",
        "license": "CC BY 4.0",
        "citation": citation,
        "fetched_at_utc": fetched_at,
        "method": "latest_non_empty_year_per_country",
        "database_id": DATABASE_ID,
        "endpoints": {
            "data": DATA360_DATA_URL,
            "country_list": COUNTRY_LIST_URL,
        },
        "country_filter": (
            "apenas paises/economias com region.id != 'NA' na lista de paises do Banco Mundial; "
            "agregados regionais e de renda (WLD, SSA, EAS, ...) ficam fora"
        ),
        "countries": {
            "countries_allowed": len(countries),
            "aggregates_excluded": len(aggregates),
            "with_any_indicator": len(ordered_indicators),
        },
        "indicators": indicator_meta,
        "indicator_counts": {item["key"]: item["countries_with_data"] for item in indicator_meta},
        "indicators_without_data": no_data,
    }

    write_json(OUT_DATA, dataset)
    write_json(OUT_MANIFEST, manifest)

    print()
    print(f"paises com algum indicador: {len(ordered_indicators)}")
    for code in REPORT_COUNTRIES:
        entry = ordered_indicators.get(code, {})
        parts = [
            f"{key}={entry[key]['value']} ({entry[key]['year']})"
            for key in (item[1] for item in INDICATORS)
            if key in entry
        ]
        print(f"{code}: " + ("; ".join(parts) if parts else "sem dado"))
    if no_data:
        print("indicadores sem dado: " + ", ".join(item["key"] for item in no_data))
    print(f"gravado: {OUT_DATA.relative_to(PROJECT_ROOT)} e {OUT_MANIFEST.relative_to(PROJECT_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
