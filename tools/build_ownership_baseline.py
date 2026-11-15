#!/usr/bin/env python
"""Gera data/definitions/economy/ownership_baseline.json a partir de fontes reais.

Fontes:
  * FMI — Direct Investment Positions by Counterpart Economy (DIP, ex-CDIS),
    API SDMX 2.1. Posição de IDE recebido (inward) e investido (outward), USD.
  * World Bank — NY.GDP.MKTP.CD (PIB nominal, current US$) para o mesmo ano.
  * Estatais (SOE): só entra percentual documentado; sem fonte verificada o campo
    fica null (nunca estimado).

O cache bruto das respostas HTTP fica em tools/data/ownership_cache/ (gitignored).

Uso:
    python tools/build_ownership_baseline.py
"""

from __future__ import annotations

import csv
import hashlib
import io
import json
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIONS_CSV = ROOT / "data" / "definitions" / "scenario" / "nations.csv"
OUT_JSON = ROOT / "data" / "definitions" / "economy" / "ownership_baseline.json"
CACHE = ROOT / "tools" / "data" / "ownership_cache"

DIP_URL = (
    "https://api.imf.org/external/sdmx/2.1/data/DIP/{key}"
    "?startPeriod={year}&endPeriod={year}&format=sdmx-csv"
)
WB_GDP_URL = (
    "https://api.worldbank.org/v2/country/all/indicator/NY.GDP.MKTP.CD"
    "?format=json&per_page=30000&date=2015:2025"
)
WB_COUNTRY_URL = "https://api.worldbank.org/v2/country?format=json&per_page=400"

UA = "opengs-game/1.0 (ownership baseline builder)"

# Dimensões da chave do DIP: COUNTRY.DV_TYPE.INDICATOR.COUNTERPART_COUNTRY.FREQUENCY
IND_INWARD = "INWD_D_NETLA_FALL_ALL"   # inward, net (liabilities less assets), all instruments, all entities
IND_OUTWARD = "OTWD_D_NETAL_FALL_ALL"  # outward, net (assets less liabilities), all instruments, all entities
CP_TOTAL = "G001"                      # contraparte agregada: total (mundo)

# nations.csv usa alguns ids próprios que não são ISO-3, e as duas fontes usam códigos
# diferentes para as mesmas economias. Por isso há um mapa por fonte.
# O arquivo de saída continua chaveado pelo id de nations.csv.
#   FMI DIP (SDMX): Palestine = WBG, Kosovo = KOS, Western Sahara = ESH
#   World Bank:     Palestine = PSE, Kosovo = XKX
IMF_CODE_MAP: dict[str, str | None] = {
    "NED": "NLD",   # Holanda
    "PSX": "WBG",   # Cisjordânia e Gaza (FMI usa WBG)
    "XKX": "KOS",   # Kosovo (FMI usa KOS)
    "SAH": "ESH",   # Saara Ocidental
    "SOL": None,    # Somalilândia: não é reportada pelo FMI nem pelo Banco Mundial
}
WB_CODE_MAP: dict[str, str | None] = {
    "NED": "NLD",   # Holanda
    "PSX": "PSE",   # Cisjordânia e Gaza (Banco Mundial usa PSE)
    "XKX": "XKX",   # Kosovo
    "SAH": "ESH",   # Saara Ocidental
    "SOL": None,    # Somalilândia: sem código no Banco Mundial
}

# DV_TYPE: "O" = reportado pelo próprio país; "SCC" = derivado de informações da contraparte (espelho).
DV_PREFERENCE = ("O", "SCC")

# Ano mais recente primeiro; o DIP desta base tem dado até 2024.
YEARS = (2024, 2023, 2022, 2021, 2020, 2019)
CHUNK_SIZE = 15

# Setores estatais DOCUMENTADOS (qualitativo, sem percentual inventado). Cada nação traz a
# sua fonte citável. Percentuais aparecem apenas quando a própria fonte os declara; nos demais
# casos a descrição fica em "state-owned"/"state-controlled".
STATE_SECTORS: dict[str, dict] = {
    "CHN": {
        "sectors": [
            "power grid & utilities: State Grid Corporation of China, China Southern Power Grid (central SOEs under SASAC)",
            "oil & gas: CNPC/PetroChina, Sinopec, CNOOC (central SOEs)",
            "telecom: China Mobile, China Telecom, China Unicom (central SOEs)",
            "steel & metals: China Baowu Steel Group (central SOE)",
            "aviation: Air China, China Eastern, China Southern (central SOEs)",
            "finance: ICBC, China Construction Bank, Bank of China, Agricultural Bank of China (state-controlled)",
        ],
        "ref": {
            "title": "SASAC — State-owned Assets Supervision and Administration Commission of the State Council (central SOEs)",
            "url": "http://en.sasac.gov.cn/",
            "year": 2026,
        },
    },
    "SAU": {
        "sectors": [
            "oil & gas: Saudi Aramco — Government holds 81.48% directly (2025 annual report); state entities incl. PIF hold more",
            "mining: Ma'aden (state/PIF-controlled)",
            "utilities: Saudi Electricity Company (state/PIF-controlled)",
            "telecom: stc (state/PIF-controlled)",
            "sovereign wealth fund: Public Investment Fund (PIF)",
        ],
        "ref": {
            "title": "Saudi Aramco — Annual Report 2025, governance section (Government shareholding)",
            "url": "https://aramco.com/-/media/publications/corporate-reports/reports-and-presentations/2025/fy/sections/ara-2025-governance-english.pdf",
            "year": 2025,
        },
    },
    "NOR": {
        "sectors": [
            "oil & gas: Equinor — Norwegian state holds 67%",
            "power: Statkraft, Statnett (state-owned)",
            "telecom: Telenor (state majority shareholder)",
        ],
        "ref": {
            "title": "State Ownership Report 2023 — The State's direct ownership of companies (Ministry of Trade, Industry and Fisheries)",
            "url": "https://www.regjeringen.no/contentassets/1a9b9fca3f5542c08bd576c844f6034b/eng/255892-statens-eierrapport-2023-eng-oppslag.pdf",
            "year": 2023,
        },
    },
    "RUS": {
        "sectors": [
            "oil & gas: Gazprom — Russian Federation controls 50.23%",
            "oil & gas: Rosneft (state-controlled via Rosneftegaz)",
            "banking: Sberbank (state-controlled via the Ministry of Finance)",
            "rail: Russian Railways (RZD, 100% state-owned)",
            "defence & industry: Rostec (state corporation)",
        ],
        "ref": {
            "title": "Gazprom Group Sustainability Report — Appendix: ownership structure (Russian Federation 50.23%)",
            "url": "https://sustainability.gazpromreport.ru/en/2020/3-about-the-gazprom-group/appendix",
            "year": 2020,
        },
    },
    "ZAF": {
        "sectors": [
            "power: Eskom (state-owned utility)",
            "rail & ports: Transnet (state-owned)",
            "telecom: Telkom (state shareholder), Broadband Infraco (state-owned)",
            "defence: Denel (state-owned)",
        ],
        "ref": {
            "title": "Eskom — Company information (state-owned enterprise, shareholder oversight by government department)",
            "url": "https://www.eskom.co.za/about-eskom/company-information",
            "year": 2026,
        },
    },
    "BRA": {
        "sectors": [
            "oil & gas: Petrobras — Federal Government controls it (50.26% of common shares at 29 Feb 2024)",
            "banking: Banco do Brasil, Caixa Econômica Federal (federal-controlled)",
            "development bank: BNDES (federal-owned)",
        ],
        "ref": {
            "title": "Petrobras — Company profile (Federal Government control and shareholding)",
            "url": "https://petrobras.com.br/en/quem-somos/perfil",
            "year": 2024,
        },
    },
    "IND": {
        "sectors": [
            "coal: Coal India Limited (central public sector enterprise)",
            "oil & gas: ONGC, Indian Oil Corporation (IOCL)",
            "power: NTPC, Power Grid Corporation of India",
            "banking: State Bank of India (Government majority shareholder)",
            "rail: Indian Railways (department of the Government of India)",
            "insurance: Life Insurance Corporation of India (LIC)",
        ],
        "ref": {
            "title": "Department of Public Enterprises — list of central public sector enterprises (schedule-wise)",
            "url": "https://www.dpe.gov.in/static/uploads/2025/07/f4d0fdf0c0cd39c03249f972c08cdf5b.pdf",
            "year": 2025,
        },
    },
    "DEU": {
        "sectors": [
            "rail: Deutsche Bahn AG (wholly owned by the Federal Republic of Germany)",
            "telecom: Deutsche Telekom (state stake held by the Federal Government and KfW)",
            "post & logistics: Deutsche Post DHL (state stake held via KfW)",
        ],
        "ref": {
            "title": "Deutsche Bahn AG — Issuer Profile: DB is wholly owned by the Federal Republic of Germany",
            "url": "https://ir.deutschebahn.com/fileadmin/Rating/2025/Issuer_Profile_Deutsche_Bahn_AG_20260928.pdf",
            "year": 2026,
        },
    },
    "FRA": {
        "sectors": [
            "energy: EDF — French State 100% (renationalised in 2023)",
            "rail: SNCF (state-owned)",
            "telecom: Orange (state stake held directly and via Bpifrance)",
        ],
        "ref": {
            "title": "EDF — Capital structure (French State 100% of share capital)",
            "url": "https://www.edf.fr/en/the-edf-group/dedicated-sections/investors/regulated-information/capital-structure",
            "year": 2023,
        },
    },
    "MEX": {
        "sectors": [
            "oil & gas: Pemex (100% state-owned)",
            "power: CFE — Comisión Federal de Electricidad (100% owned by the Mexican government)",
        ],
        "ref": {
            "title": "CFE — international offering memorandum (CFE remains 100% owned by the Mexican government; Pemex is the state-owned oil company)",
            "url": "https://www.cfe.gob.mx/finanzas/financiamiento/Documents/Emisiones%20Internacionales/CFE%20Formosa%202019%20-%20Final%20OM%20(Final).pdf",
            "year": 2019,
        },
    },
    "JPN": {
        "sectors": [
            "post: Japan Post Holdings (government legally required to hold more than one-third of shares)",
            "telecom: NTT (Ministry of Finance holds a state stake)",
            "rail: JR Hokkaido, JR Shikoku, JR Kyushu and JR Freight remain state-held; JR East/West/Central are private since 1987",
        ],
        "ref": {
            "title": "Japan Post Holdings — Investor Relations FAQ (government shareholding obligation)",
            "url": "https://www.japanpost.jp/en/ir/faq/",
            "year": 2026,
        },
    },
    "USA": {
        "sectors": [
            "postal: USPS — independent establishment of the executive branch of the US Government",
            "passenger rail: Amtrak (federally owned since 1971)",
        ],
        "ref": {
            "title": "USPS — Who we are (independent establishment of the executive branch)",
            "url": "https://about.usps.com/who/",
            "year": 2026,
        },
    },
}

SOURCES = {
    "imf_dip": {
        "title": "IMF Direct Investment Positions by Counterpart Economy (DIP, ex-CDIS)",
        "url": "https://data.imf.org/?sk=40313609-F037-48C1-84B1-E1F1CE54D6D5",
        "unit": "USD",
    },
    "worldbank_gdp": {
        "title": "World Bank GDP (current US$)",
        "url": "https://data.worldbank.org/indicator/NY.GDP.MKTP.CD",
        "unit": "USD",
    },
    "oecd_soe": {
        "title": "OECD Ownership and Governance of State-Owned Enterprises",
        "url": "https://www.oecd.org/",
        "unit": "percent",
    },
    "imf_fiscal_monitor_soe": {
        "title": "IMF Fiscal Monitor Oct 2020 ch.3 — State-Owned Enterprises",
        "url": "https://www.imf.org/en/Publications/FM/Issues/2020/09/30/october-2020-fiscal-monitor",
        "unit": "percent",
    },
    "national_study": {
        "title": "estudo/instituto nacional (ver refs por nação)",
        "unit": "percent",
    },
}

NOTE = (
    "Estoque de IDE por nação vem do FMI DIP (ex-CDIS): posição líquida total do ano mais recente "
    "disponível por país (2024 quando existe, senão 2023/2022/2021/2020/2019). O total usa a contraparte "
    "agregada G001 (mundo); quando G001 não é reportada, o total é a soma das posições por contraparte "
    "individual (campo fdi_method indica qual dos dois). fdi_basis=own_report quando o país reporta "
    "(DV_TYPE=O); fdi_basis=mirror_estimate quando só existe a série derivada das contrapartes (DV_TYPE=SCC). "
    "fdi_inward_pct_gdp / fdi_outward_pct_gdp = estoque / PIB nominal (World Bank NY.GDP.MKTP.CD) do MESMO ano "
    "do estoque; sem PIB no ano o percentual fica null. "
    "main_investors traz as 5 maiores origens da posição inward no mesmo ano e a mesma base do total; "
    "investor_share_basis=sum_counterparts indica os casos em que a soma das contrapartes excede o total "
    "reportado pela fonte (inconsistência do próprio dado) e por isso as participações usam a soma. "
    "Peso de estatais (SOE): nenhum país recebeu percentual nesta versão. As bases públicas disponíveis "
    "(OECD, IMF Fiscal Monitor out/2020 cap.3) não trazem, por país e em percentual do PIB, um valor "
    "verificável nesta rodada; por isso soe_share_gdp/soe_share_employment ficam null com "
    "soe_no_data/soe_reason ('sem fonte publicada de peso de estatal'). Nenhum percentual de estatal foi "
    "estimado. "
    "state_sectors é outro conceito: lista QUALITATIVA de setores/empresas estatais documentados, com "
    "state_sectors_ref (title/url/year) da fonte usada. state_sectors NÃO implica percentual: convive com "
    "soe_no_data=true. Percentuais dentro de state_sectors aparecem apenas quando a própria fonte citada os "
    "declara. Onde não houve fonte verificável, state_sectors e state_sectors_ref ficam null. "
    "Nenhum valor deste arquivo é sintético."
)


# ---------------------------------------------------------------- HTTP / cache

def _fetch(url: str, cache_path: Path, accept: str | None = None) -> str:
    if cache_path.exists():
        text = cache_path.read_text(encoding="utf-8-sig")
        if text.strip():
            return text
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    headers = {"User-Agent": UA}
    if accept:
        headers["Accept"] = accept
    last = None
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=300) as resp:
                text = resp.read().decode("utf-8-sig")
            cache_path.write_text(text, encoding="utf-8")
            return text
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as exc:  # pragma: no cover
            last = exc
            time.sleep(2 + attempt * 3)
    raise RuntimeError(f"falha ao baixar {url}: {last}")


def _dip_key(countries: list[str], indicators: list[str], counterpart: str = "") -> str:
    return f"{'+'.join(countries)}..{'+'.join(indicators)}.{counterpart}."


def fetch_dip(countries: list[str], indicators: list[str], counterpart: str, year: int, tag: str) -> str:
    key = _dip_key(countries, indicators, counterpart)
    # O sufixo de hash evita reaproveitar cache de um chunk cuja composição mudou.
    digest = hashlib.md5(key.encode("utf-8")).hexdigest()[:8]
    return _fetch(
        DIP_URL.format(key=key, year=year),
        CACHE / "dip" / f"{tag}_{digest}_{year}.csv",
        accept="text/csv",
    )


# ------------------------------------------------------------------ parsing

def _csv_obs(text: str) -> list[dict[str, str]]:
    return [row for row in csv.DictReader(io.StringIO(text)) if row.get("OBS_VALUE")]


def _is_country_code(code: str) -> bool:
    """Código de economia individual (ISO-3). Agregados do FMI (G001, GX451, TX983...) não passam."""
    return len(code) == 3 and code.isalpha() and code.isupper()


def load_nations() -> list[dict[str, str]]:
    with NATIONS_CSV.open(encoding="utf-8-sig", newline="") as fh:
        return list(csv.DictReader(fh))


def load_gdp() -> tuple[dict[str, dict[int, float]], dict[str, str]]:
    text = _fetch(WB_GDP_URL, CACHE / "wb_gdp.json")
    payload = json.loads(text)
    gdp: dict[str, dict[int, float]] = {}
    if not isinstance(payload, list) or len(payload) < 2 or not payload[1]:
        return gdp, {}
    for row in payload[1]:
        if row.get("value") is None:
            continue
        iso = row.get("countryiso3code") or ""
        if not iso:
            continue
        gdp.setdefault(iso, {})[int(row["date"])] = float(row["value"])
    return gdp, {}


def load_country_names() -> dict[str, str]:
    text = _fetch(WB_COUNTRY_URL, CACHE / "wb_countries.json")
    payload = json.loads(text)
    names: dict[str, str] = {}
    if isinstance(payload, list) and len(payload) > 1 and payload[1]:
        for row in payload[1]:
            iso = row.get("id") or ""
            if len(iso) == 3 and iso.isalpha() and iso.isupper():
                names[iso] = row.get("name") or iso
    return names


def collect_dip(nations: list[dict[str, str]]) -> dict[str, dict[int, dict]]:
    """Retorna {código internacional: {ano: {'inward': {(dv, cp): valor}, 'outward': ...}}}.

    Primeiro baixa 2024/2023 para todas as nações; só as que ficarem sem posição
    descem para anos anteriores (cada download fica em cache).
    """
    iso_list = [n["imf_code"] for n in nations if n.get("imf_code")]
    data: dict[str, dict[int, dict]] = {}

    def absorb(text: str) -> None:
        for row in _csv_obs(text):
            iso = row["COUNTRY"]
            year = int(row["TIME_PERIOD"])
            slot = data.setdefault(iso, {}).setdefault(year, {"inward": {}, "outward": {}})
            bucket = "inward" if row["INDICATOR"] == IND_INWARD else "outward"
            slot[bucket][(row["DV_TYPE"], row["COUNTERPART_COUNTRY"])] = float(row["OBS_VALUE"])

    def fetch_for(countries: list[str], year: int, prefix: str) -> None:
        for start in range(0, len(countries), CHUNK_SIZE):
            chunk = countries[start : start + CHUNK_SIZE]
            tag = f"{prefix}{start // CHUNK_SIZE:02d}"
            # A) inward com todas as contrapartes (total + principais investidores)
            absorb(fetch_dip(chunk, [IND_INWARD], "", year, tag + "_in"))
            # B) totais (G001) de inward e outward
            absorb(fetch_dip(chunk, [IND_OUTWARD, IND_INWARD], CP_TOTAL, year, tag + "_tot"))

    def has_position(iso: str) -> bool:
        for slot in data.get(iso, {}).values():
            if any(_pick_basis(slot, b) for b in ("inward", "outward")):
                return True
        return False

    fetch_for(iso_list, 2024, "y2024_")
    fetch_for(iso_list, 2023, "y2023_")
    missing = [iso for iso in iso_list if not has_position(iso)]
    for year in (2022, 2021, 2020, 2019):
        if not missing:
            break
        fetch_for(missing, year, f"y{year}_")
        missing = [iso for iso in missing if not has_position(iso)]
    return data


# ------------------------------------------------------------------- montagem

def _pick_basis(slot: dict, bucket: str) -> str | None:
    for dv in DV_PREFERENCE:
        if (dv, CP_TOTAL) in slot[bucket]:
            return dv
    for dv in DV_PREFERENCE:
        if any(k[0] == dv and _is_country_code(k[1]) for k in slot[bucket]):
            return dv
    return None


def _total_and_method(slot: dict, bucket: str, basis: str) -> tuple[float | None, str | None]:
    if (basis, CP_TOTAL) in slot[bucket]:
        return slot[bucket][(basis, CP_TOTAL)], "reported_total"
    parts = [
        v
        for (dv, cp), v in slot[bucket].items()
        if dv == basis and _is_country_code(cp)
    ]
    if parts:
        return sum(parts), "sum_counterparties"
    return None, None


def build() -> dict:
    nations = load_nations()
    for nation in nations:
        nation["imf_code"] = IMF_CODE_MAP.get(nation["id"], nation["id"])
        nation["wb_code"] = WB_CODE_MAP.get(nation["id"], nation["id"])
    gdp, _ = load_gdp()
    names = load_country_names()
    for nation in nations:
        for code in (nation["imf_code"], nation["wb_code"]):
            if code and code not in names:
                names[code] = nation["name"]
    dip = collect_dip(nations)

    entries: dict[str, dict] = {}
    for nation in nations:
        iso = nation["id"]
        name = nation["name"]
        code = nation["imf_code"]
        wb_code = nation["wb_code"]
        by_year = dip.get(code, {}) if code else {}


        # O ano é escolhido pela posição de IDE recebido (métrica principal); sem inward,
        # cai para o ano mais recente que tenha outward.
        chosen_year = None
        for year in YEARS:
            slot = by_year.get(year)
            if slot and _pick_basis(slot, "inward"):
                chosen_year = year
                break
        if chosen_year is None:
            for year in YEARS:
                slot = by_year.get(year)
                if slot and _pick_basis(slot, "outward"):
                    chosen_year = year
                    break

        entry: dict = {
            "name": name,
            "fdi_inward_stock_usd": None,
            "fdi_outward_stock_usd": None,
            "fdi_year": None,
            "fdi_source": None,
            "fdi_basis": None,
            "fdi_method": None,
            "gdp_usd": None,
            "gdp_year": None,
            "fdi_inward_pct_gdp": None,
            "fdi_outward_pct_gdp": None,
            "main_investors": [],
            "soe_share_gdp": None,
            "soe_share_employment": None,
            "soe_source": None,
            "state_sectors": None,
            # Sem fonte publicada de peso de estatal verificável: ausência explícita, nunca estimativa.
            "soe_no_data": True,
            "soe_reason": "sem fonte publicada de peso de estatal",
            "confidence": "partial",
            "refs": [],
        }

        # Setores estatais documentados (qualitativo). Independe do peso numérico, que segue null.
        documented = STATE_SECTORS.get(iso)
        if documented:
            entry["state_sectors"] = list(documented["sectors"])
            entry["state_sectors_ref"] = dict(documented["ref"])

        if chosen_year is None:
            if not code:
                reason = (
                    "sem código FMI/World Bank utilizável: a economia não é reportada pelo FMI DIP "
                    "nem pelo World Bank (nenhum dado observado)"
                )
            else:
                reason = "sem posição de IDE no FMI DIP (nem reportada, nem espelho) de 2019 a 2024"
            entry.update(
                {
                    "no_data": True,
                    "reason": reason,
                    "confidence": "partial",
                }
            )
            entries[iso] = entry
            continue

        slot = by_year[chosen_year]
        basis_in = _pick_basis(slot, "inward")
        basis_out = _pick_basis(slot, "outward")
        inward, method_in = (None, None)
        outward, method_out = (None, None)
        if basis_in:
            inward, method_in = _total_and_method(slot, "inward", basis_in)
        if basis_out:
            outward, method_out = _total_and_method(slot, "outward", basis_out)

        notes: list[str] = []
        if inward is not None and inward < 0:
            notes.append(f"inward negativo reportado ({inward:.0f} USD)")
            inward = None
        if outward is not None and outward < 0:
            notes.append(f"outward negativo reportado ({outward:.0f} USD)")
            outward = None

        def _basis_label(dv: str | None) -> str | None:
            if dv == "O":
                return "own_report"
            if dv == "SCC":
                return "mirror_estimate"
            return None

        # fdi_basis descreve a série do inward (métrica principal). Quando o outward vem de
        # outra série (ex.: inward reportado e outward só espelho), isso fica explícito.
        basis_label = _basis_label(basis_in or basis_out)
        basis_out_label = _basis_label(basis_out)
        if inward is not None and outward is not None and basis_out_label != basis_label:
            entry["fdi_basis_outward"] = basis_out_label
            notes.append(
                "inward em " + str(basis_label) + " e outward em " + str(basis_out_label)
            )

        entry["fdi_inward_stock_usd"] = inward
        entry["fdi_outward_stock_usd"] = outward
        entry["fdi_year"] = chosen_year
        entry["fdi_source"] = "imf_dip"
        entry["fdi_basis"] = basis_label
        entry["fdi_method"] = method_in or method_out

        gdp_value = gdp.get(wb_code, {}).get(chosen_year) if wb_code else None
        if gdp_value is None and wb_code:
            # Sem PIB no ano do estoque: procura o ano mais próximo já publicado (documentado em gdp_year).
            candidates = sorted(gdp.get(wb_code, {}).keys(), key=lambda y: abs(y - chosen_year))
            if candidates and abs(candidates[0] - chosen_year) <= 2:
                gdp_value = gdp[wb_code][candidates[0]]
                entry["gdp_year"] = candidates[0]
        elif gdp_value is not None:
            entry["gdp_year"] = chosen_year

        entry["source"] = "imf_dip"
        entry["refs"].append(
            {
                "title": (
                    "IMF DIP — posição de IDE de " + name + " por economia contraparte ("
                    + str(chosen_year) + ")"
                ),
                "url": "https://data.imf.org/?sk=40313609-F037-48C1-84B1-E1F1CE54D6D5",
                "year": chosen_year,
            }
        )

        if gdp_value:
            entry["gdp_usd"] = gdp_value
            entry["refs"].append(
                {
                    "title": f"World Bank GDP (current US$) — {name} ({entry['gdp_year']})",
                    "url": "https://data.worldbank.org/indicator/NY.GDP.MKTP.CD",
                    "year": entry["gdp_year"],
                }
            )
            if inward is not None:
                entry["fdi_inward_pct_gdp"] = round(inward / gdp_value * 100.0, 4)
            if outward is not None:
                entry["fdi_outward_pct_gdp"] = round(outward / gdp_value * 100.0, 4)
            if entry["gdp_year"] != chosen_year:
                entry["pct_note"] = (
                    "PIB de " + str(entry["gdp_year"]) + " (ano publicado mais próximo); "
                    "estoque de " + str(chosen_year)
                )
        else:
            entry["pct_missing_reason"] = (
                "sem PIB nominal do World Bank para o ano do estoque (±2 anos): "
                "economia não coberta ou dado ausente"
            )

        if inward and inward > 0 and basis_in:
            positions = [
                (cp, v)
                for (dv, cp), v in slot["inward"].items()
                if dv == basis_in and _is_country_code(cp) and v > 0
            ]
            positions.sort(key=lambda item: item[1], reverse=True)
            positions = positions[:5]
            # Denominador das participações: total reportado. Quando a soma das posições
            # individuais supera o total (inconsistência da própria fonte), usa a soma.
            detail_sum = sum(
                v
                for (dv, cp), v in slot["inward"].items()
                if dv == basis_in and _is_country_code(cp) and v > 0
            )
            denominator = inward
            basis_shares = "reported_total"
            if detail_sum > inward * 1.001:
                denominator = detail_sum
                basis_shares = "sum_counterparts"
                entry["investor_share_note"] = (
                    "soma das posições por contraparte (" + f"{detail_sum:.0f}" + " USD) excede o total "
                    "reportado (" + f"{inward:.0f}" + " USD); participações calculadas sobre a soma"
                )
            entry["investor_share_basis"] = basis_shares
            entry["main_investors"] = [
                {
                    "country": cp,
                    "name": names.get(cp, cp),
                    "position_usd": value,
                    "share": round(value / denominator, 4),
                }
                for cp, value in positions
            ]
        elif inward:
            entry["main_investors_note"] = (
                "total de IDE reportado sem detalhe por contraparte no FMI DIP"
            )

        if notes:
            entry["fdi_note"] = "; ".join(notes)
        if any(n.startswith(("inward negativo", "outward negativo")) for n in notes):
            entry["confidence"] = "partial"
        elif inward is not None and entry["gdp_usd"] and basis_label == "own_report":
            entry["confidence"] = "complete"
        else:
            entry["confidence"] = "partial"

        entries[iso] = entry

    with_fdi = sum(
        1
        for e in entries.values()
        if e["fdi_inward_stock_usd"] is not None or e["fdi_outward_stock_usd"] is not None
    )
    with_soe = sum(1 for e in entries.values() if e["soe_share_gdp"] is not None)
    with_sectors = sum(1 for e in entries.values() if e.get("state_sectors"))

    return {
        "version": 1,
        "note": NOTE,
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "coverage": {
            "nations_total": len(entries),
            "nations_with_fdi": with_fdi,
            "nations_with_soe": with_soe,
            "nations_with_state_sectors": with_sectors,
            "nations_no_data": len(entries) - with_fdi,
        },
        "sources": SOURCES,
        "nations": entries,
    }


def main() -> int:
    payload = build()
    OUT_JSON.parent.mkdir(parents=True, exist_ok=True)
    OUT_JSON.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    cov = payload["coverage"]
    print(
        "escrito",
        OUT_JSON.relative_to(ROOT),
        "| nações:",
        cov["nations_total"],
        "| com IDE:",
        cov["nations_with_fdi"],
        "| com SOE:",
        cov["nations_with_soe"],
        "| sem dado:",
        cov["nations_no_data"],
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
