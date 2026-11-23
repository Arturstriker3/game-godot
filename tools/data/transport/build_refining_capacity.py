"""Capacidade de refino por pais (Energy Institute, Statistical Review 2025).

Entrada:
  * ``tools/data/refining/EI-Stats-Review-2025.pdf`` -- PDF oficial da 74a
    edicao (cache local NAO versionado), tabela "Oil: Refining capacity";
  * ``data/definitions/scenario/cities.json`` -- populacao simulada/exibida de
    cada cidade (fracao simulada da nacao, a mesma do parque eletrico).

Saida:
  * ``data/definitions/transport/refining_capacity.json`` -- capacidade 2024 por
    nacao do jogo (ISO3), em mil barris/dia, mais a conversao declarada;
  * ``data/definitions/transport/_sources.json`` -- registro da fonte com URL,
    edicao, data de acesso, hash do PDF, termos de uso do EI e o texto de
    atribuicao exigido.

Regras (declaradas, sem numero inventado):
  * a serie 2024 da tabela e a capacidade de destilacao atmosferica no fim do
    ano, base dia-calendario (nota de rodape da propria tabela);
  * agregados regionais ("Other ...", "Total ...", "of which: OECD",
    "Non-OECD", "European Union") NAO sao rateados entre paises sem dado: eles
    ficam registrados em ``meta.regional_aggregates_not_allocated`` / ``meta.world``
    e o pais sem linha fica sem refino (ausencia honesta);
  * conversao ``t/dia = mil_b/dia x 0,1364 t/barril x 1000`` -- 1 barril de
    petroleo cru = 0,1364 t (7,33 bbl/t), fator da U.S. EIA (Energy Explained,
    Units and calculators); fonte declarada em ``meta.conversion``;
  * a capacidade de cada nacao e multiplicada pela FRACAO SIMULADA da nacao
    (``sum(sim_population) / sum(population)`` das cidades dela), a mesma
    razao que ``sim/power.cpp`` aplica ao parque eletrico (D-116/D-119), para
    MW/t e dinheiro viverem na mesma escala da economia;
  * a alocacao por cidade e do servico de seed (``core/economy/refining_service.gd``):
    cidades-porto do ``ports.json`` por populacao simulada; nacao sem porto usa
    a maior cidade. A regra fica em ``meta.allocation``.

Uso:
    python tools/data/transport/build_refining_capacity.py

O PDF e o cache ficam em ``tools/data/refining/`` (gitignored) e NAO sao
commitados: o dado derivado e o registro da fonte bastam para reproduzir.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
CACHE_DIR = os.path.join(ROOT, "tools", "data", "refining")
PDF_PATH = os.path.join(CACHE_DIR, "EI-Stats-Review-2025.pdf")
CITIES_PATH = os.path.join(ROOT, "data", "definitions", "scenario", "cities.json")
NATIONS_PATH = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
OUT_DIR = os.path.join(ROOT, "data", "definitions", "transport")
OUT_PATH = os.path.join(OUT_DIR, "refining_capacity.json")
SOURCES_PATH = os.path.join(OUT_DIR, "_sources.json")

# URL oficial da 74a edicao (secao "Data downloads and archive" do EI).
SOURCE_URL = ("https://www.energyinst.org/__data/assets/pdf_file/0007/1658077/"
              "Statistical-Review-of-World-Energy.pdf")
LANDING_URL = "https://www.energyinst.org/statistical-review/resources-and-data-downloads"
TERMS_URL = "https://www.energyinst.org/terms"
CITATION = ("Energy Institute (2025), Statistical Review of World Energy 2025, "
            "Energy Institute, London.")
ATTRIBUTION = ("Fonte: Energy Institute, Statistical Review of World Energy 2025 "
               "(74a edicao). (c) Energy Institute 2025.")

# 1 barril de petroleo cru = 0,1364 t (7,33 bbl/t) -- U.S. EIA, Energy
# Explained, "Units and calculators" (factores de conversao do petroleo cru).
T_PER_BARREL = 0.1364
B_CONVERSION_SOURCE = {
    "value": T_PER_BARREL,
    "unit": "t por barril de petroleo cru",
    "barrels_per_tonne": round(1.0 / T_PER_BARREL, 2),
    "publisher": "U.S. Energy Information Administration (EIA)",
    "dataset": "Energy Explained - Units and calculators",
    "url": "https://www.eia.gov/energyexplained/units-and-calculators/",
    "note": ("fator de conversao de volume para massa do petroleo cru; a "
             "densidade do barril depende do grau API, 0,1364 t e o fator "
             "medio declarado pela EIA"),
}

# Nome da tabela -> tag do jogo (ISO3 de nations.json). Nomes que nao existem
# como nacao no jogo (Curacao, Antilhas Holandesas) ficam de fora e sao
# declarados em meta.not_allocated_no_game_nation.
NAME_TO_TAG = {
    # North America
    "Canada": "CAN", "Mexico": "MEX", "US": "USA",
    # S. & Cent. America
    "Argentina": "ARG", "Brazil": "BRA", "Chile": "CHL", "Colombia": "COL",
    "Curaçao": "CUW", "Ecuador": "ECU", "Netherlands Antilles": "ANT",
    "Peru": "PER", "Trinidad & Tobago": "TTO", "Venezuela": "VEN",
    # Europe
    "Austria": "AUT", "Belgium": "BEL", "Bulgaria": "BGR",
    "Czech Republic": "CZE", "Denmark": "DNK", "Finland": "FIN",
    "France": "FRA", "Germany": "DEU", "Greece": "GRC", "Hungary": "HUN",
    "Ireland": "IRL", "Italy": "ITA", "Lithuania": "LTU",
    "Netherlands": "NED", "Norway": "NOR", "Poland": "POL",
    "Portugal": "PRT", "Romania": "ROU", "Slovakia": "SVK", "Spain": "ESP",
    "Sweden": "SWE", "Switzerland": "CHE", "Türkiye": "TUR",
    "Ukraine": "UKR", "United Kingdom": "GBR",
    # CIS
    "Azerbaijan": "AZE", "Belarus": "BLR", "Kazakhstan": "KAZ",
    "Russian Federation": "RUS", "Turkmenistan": "TKM", "Uzbekistan": "UZB",
    # Middle East
    "Bahrain": "BHR", "Iran": "IRN", "Iraq": "IRQ", "Israel": "ISR",
    "Kuwait": "KWT", "Oman": "OMN", "Qatar": "QAT", "Saudi Arabia": "SAU",
    "United Arab Emirates": "ARE",
    # Africa
    "Algeria": "DZA", "Egypt": "EGY", "Morocco": "MAR", "Nigeria": "NGA",
    "South Africa": "ZAF",
    # Asia Pacific
    "Australia": "AUS", "Bangladesh": "BGD", "China": "CHN", "India": "IND",
    "Indonesia": "IDN", "Japan": "JPN", "Malaysia": "MYS",
    "New Zealand": "NZL", "Pakistan": "PAK", "Philippines": "PHL",
    "Singapore": "SGP", "South Korea": "KOR", "Taiwan": "TWN",
    "Thailand": "THA", "Vietnam": "VNM",
}

AGGREGATE_PREFIXES = ("Total ", "Other ", "of which")


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path, data, indent=1):
    """JSON deterministico: indent fixo, LF, UTF-8 sem BOM, newline final."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = json.dumps(data, indent=indent, ensure_ascii=False,
                      sort_keys=False) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def page_text(text):
    """Texto da pagina que contem a tabela de capacidade de refino."""
    for line in text.splitlines():
        if line.strip() == "Oil Refining capacity":
            return text
    return None


def extract_table():
    try:
        import pypdf  # noqa: PLC0415 (dependencia opcional declarada no docstring)
    except ImportError as error:  # pragma: no cover - ambiente sem pypdf
        raise SystemExit("pypdf ausente: instale pypdf ou use o pdftotext") from error

    reader = pypdf.PdfReader(PDF_PATH)
    found = None
    for page in reader.pages:
        text = page.extract_text() or ""
        if page_text(text):
            found = text
            break
    if found is None:
        raise SystemExit("tabela 'Oil Refining capacity' nao encontrada no PDF")

    rows = []
    aggregates = []
    for line in found.splitlines():
        tokens = line.split()
        # 11 anos + crescimento 2024 + crescimento 2014-24 + share 2024.
        if len(tokens) < 15:
            continue
        name = " ".join(tokens[:-14]).strip()
        if not name or name.startswith("Thousand barrels"):
            continue
        is_aggregate = (name.startswith(AGGREGATE_PREFIXES)
                        or name in ("Non-OECD", "European Union"))
        if not is_aggregate and name not in NAME_TO_TAG:
            continue
        years = tokens[-14:-3]
        value = parse_year(years[10])
        record = {"name": name, "thousand_bpd": value, "year": 2024}
        if is_aggregate:
            aggregates.append(record)
        else:
            rows.append(record)
    return rows, aggregates


def parse_year(token):
    """Valor numerico da coluna 2024; marcador de ausencia vira None."""
    if re.fullmatch(r"\d+", token):
        return int(token)
    return None


def simulated_fractions(cities, tags):
    """Fracao simulada por nacao (mesma razao usada pelo parque eletrico)."""
    sim = {}
    shown = {}
    for city in cities:
        tag = str(city.get("nation", ""))
        pop = float(city.get("population") or 0.0)
        sim_pop = float(city.get("sim_population") or 0.0)
        if not tag or pop <= 0.0 or sim_pop <= 0.0:
            continue
        sim[tag] = sim.get(tag, 0.0) + sim_pop
        shown[tag] = shown.get(tag, 0.0) + pop
    fractions = {}
    for tag in tags:
        if tag in sim and shown.get(tag, 0.0) > 0.0:
            fractions[tag] = min(1.0, sim[tag] / shown[tag])
        else:
            fractions[tag] = 1.0
    return fractions


def main():
    if not os.path.exists(PDF_PATH):
        raise SystemExit("PDF ausente: baixe a 74a edicao em %s e salve como %s"
                         % (LANDING_URL, PDF_PATH))
    rows, aggregates = extract_table()
    cities = read_json(CITIES_PATH)
    nations = read_json(NATIONS_PATH)
    game_tags = {str(nation.get("id", "")) for nation in nations
                 if str(nation.get("id", ""))}
    fractions = simulated_fractions(cities, game_tags)

    by_country = {}
    not_allocated = []
    unmapped = []
    for row in rows:
        name = row["name"]
        tag = NAME_TO_TAG.get(name)
        if tag is None:
            unmapped.append(name)
            continue
        if tag not in game_tags:
            not_allocated.append({
                "name": name,
                "country_iso3": tag,
                "thousand_bpd": row["thousand_bpd"],
                "reason": "pais nao existe como nacao no cenario do jogo",
            })
            continue
        kbpd = row["thousand_bpd"]
        if kbpd is None:
            # Linha sem valor em 2024: ausencia declarada, nao zero.
            not_allocated.append({
                "name": name,
                "country_iso3": tag,
                "thousand_bpd": None,
                "reason": "tabela sem valor para 2024 (marcador de ausencia)",
            })
            continue
        fraction = fractions.get(tag, 1.0)
        capacity_t = kbpd * T_PER_BARREL * 1000.0
        by_country[tag] = {
            "name": name,
            "country_iso3": tag,
            "thousand_bpd": kbpd,
            "year": 2024,
            "simulated_fraction": round(fraction, 6),
            "capacity_unscaled_t_per_day": round(capacity_t, 3),
            "capacity_t_per_day": round(capacity_t * fraction, 3),
        }

    world = next((row for row in aggregates if row["name"] == "Total World"),
                 None)
    named_total = sum(row["thousand_bpd"] for row in by_country.values())
    other_total = sum(row["thousand_bpd"] or 0 for row in aggregates
                      if row["name"].startswith("Other "))
    non_game_total = sum(row["thousand_bpd"] or 0 for row in not_allocated)
    world_total = world["thousand_bpd"] if world else None
    residual = (world_total - named_total) if world else None
    accessed = datetime.fromtimestamp(os.path.getmtime(PDF_PATH),
                                      tz=timezone.utc)

    payload = {
        "meta": {
            "generated_by": "tools/data/transport/build_refining_capacity.py",
            "dataset": "Energy Institute, Statistical Review of World Energy 2025",
            "edition": "74a edicao (2025)",
            "table": "Oil: Refining capacity",
            "reference_year": 2024,
            "unit": "mil barris/dia (capacidade de destilacao atmosferica no fim do ano, dia-calendario)",
            "source": {
                "url": SOURCE_URL,
                "landing_url": LANDING_URL,
                "accessed_at_utc": accessed.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "sha256": sha256_of(PDF_PATH),
                "citation": CITATION,
                "attribution": ATTRIBUTION,
                "terms_url": TERMS_URL,
                "note": ("o PDF NAO e versionado; tools/data/refining/ e cache "
                         "local (gitignored)"),
            },
            "conversion": {
                "t_per_barrel": T_PER_BARREL,
                "formula": "capacity_t_per_day = thousand_bpd * 0.1364 * 1000",
                "source": B_CONVERSION_SOURCE,
            },
            "simulated_fraction": {
                "rule": "sum(sim_population) / sum(population) das cidades da nacao",
                "same_as": "D-116/D-119 - fracao simulada aplicada por sim/power.cpp ao parque eletrico",
                "applied_to": "capacity_t_per_day (coluna derivada; thousand_bpd e o dado bruto)",
            },
            "allocation": {
                "service": "core/economy/refining_service.gd",
                "rule": ("capacidade nacional distribuida entre ate 3 cidades-porto "
                         "do ports.json (host city), peso = populacao simulada; "
                         "nacao sem porto usa a maior cidade. Comecam como "
                         "construcoes 'refinery' com a receita gasoline_a, "
                         "operando no dia 1 (output_scale 1, sem rampa)."),
                "owner": "private",
                "order": "seed_industry() ANTES do refino (farinha/pao nao bloqueiam)",
            },
            "regional_aggregates_not_allocated": [
                row for row in aggregates
                if row["name"] not in ("Total World", "of which: OECD",
                                       "Non-OECD", "European Union")
            ],
            "world": world,
            "not_allocated": not_allocated,
            "unmapped_names": unmapped,
            "check": {
                "countries_allocated": len(by_country),
                "named_total_thousand_bpd": named_total,
                "world_total_thousand_bpd": world_total,
                "residual_thousand_bpd": residual,
                "residual_other_aggregates_thousand_bpd": other_total,
                "residual_non_game_nations_thousand_bpd": non_game_total,
                "residual_source_rounding_thousand_bpd": (
                    (residual - other_total - non_game_total)
                    if residual is not None else None),
                "residual_explained": ("agregados 'Other ...' regionais nao "
                                       "rateados + nacoes fora do cenario "
                                       "(Curacao); a diferenca restante e o "
                                       "arredondamento da propria tabela"),
            },
        },
        "by_country": dict(sorted(by_country.items())),
    }
    write_json(OUT_PATH, payload)

    sources = {
        "meta": {
            "generated_by": "tools/data/transport/build_refining_capacity.py",
            "verified_on": accessed.strftime("%Y-%m-%d"),
            "rule": ("toda fonte usada precisa de URL, edicao, data de acesso e "
                     "licenca/termos verificados; fonte com licenca incompativel "
                     "com uso comercial entra como used_restricted com blocker"),
            "counts": {"used_restricted": 1},
        },
        "sources": [
            {
                "id": "ei_stats_review_2025_refining",
                "title": "Statistical Review of World Energy 2025 - Oil: Refining capacity (2024)",
                "publisher": "Energy Institute (EI)",
                "edition": "74a edicao (2025)",
                "url": SOURCE_URL,
                "landing_url": LANDING_URL,
                "accessed_at_utc": accessed.strftime("%Y-%m-%dT%H:%M:%SZ"),
                "sha256": sha256_of(PDF_PATH),
                "reference_period": ("2024 (capacidade de destilacao atmosferica "
                                     "no fim do ano, base dia-calendario)"),
                "unit": "mil barris/dia",
                "license": {
                    "name": ("Energy Institute Terms and Conditions - copyright; "
                             "citacao obrigatoria, sem redistribuicao dos arquivos"),
                    "url": TERMS_URL,
                    "commercial_ok": False,
                    "verified_on": accessed.strftime("%Y-%m-%d"),
                    "quote": ("The EI is the owner or the licensee of all "
                              "intellectual property rights in our Sites, and in "
                              "the material published on it... All such rights are "
                              "reserved."),
                    "evidence": ("Termos do EI, secao Copyright: 'Copies of documents "
                                 "contained on the Sites may only be made available "
                                 "for information purposes and solely for the private "
                                 "use of the Sites' users. Any use or reproduction of "
                                 "such documents for any other purpose whatsoever is "
                                 "expressly forbidden without the written permission "
                                 "of the EI.' A secao de downloads tambem declara a "
                                 "citacao exigida: 'Energy Institute (2025), "
                                 "Statistical Review of World Energy 2025, Energy "
                                 "Institute, London.'"),
                    "attribution_required": ATTRIBUTION,
                },
                "citation": CITATION,
                "used_in": ["data/definitions/transport/refining_capacity.json"],
                "transform": ("capacidade 2024 (mil b/d) por pais; x 0,1364 t/barril "
                              "(EIA) x 1000 e x fracao simulada da nacao (D-116/D-119); "
                              "agregados regionais NAO rateados"),
                "status": "used_restricted",
                "blocker": ("termos do EI reservam direitos e proibem reproducao "
                            "sem autorizacao escrita; so os numeros derivados por "
                            "pais entram no jogo, com atribuicao obrigatoria, e o "
                            "PDF/planilha nao sao redistribuidos"),
            }
        ],
    }
    write_json(SOURCES_PATH, sources)

    print("[refino] %d nacoes, %s mil b/d nomeados de %s no mundo (residual %s)"
          % (len(by_country), named_total,
             world_total if world_total is not None else "?",
             residual if residual is not None else "?"))
    rounding = residual - other_total - non_game_total
    if rounding is None or abs(rounding) > 10:
        raise SystemExit("soma das linhas nao fecha com a Total World "
                         "(arredondamento %s mil b/d)" % rounding)
    if unmapped:
        print("[refino] nomes sem mapa: %s" % ", ".join(unmapped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
