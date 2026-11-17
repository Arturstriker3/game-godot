#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gera data/definitions/economy/housing.json — condições habitacionais por nação do jogo.

Fontes reais (nada é inventado; o que não tem medição vai como proxy marcado):

  Homeownership (share de domicílios próprios, 0..1)
      OECD Affordable Housing Database  HM1.3 Housing tenures  (share of households by
      tenure type, 2024 or latest) — 41 países com dado publicado; valor = own outright +
      owner with mortgage. https://oe.cd/ahd
      Hungria: HM1.3.A1 (2018, última coluna publicada antes da revisão do indicador).
      Israel: OECD EGDNA (contagens de domicílios por status de posse) -> razão calculada.
      Japão: Statistics Bureau, 2023 Housing and Land Survey (持ち家住宅率 60,9%).
      Sem valor observado -> proxy por faixa de renda (marcada estimated=true).

  Aluguel/renda (mediana do ônus de aluguel como fração da renda disponível)
      OECD Affordable Housing Database  HC1.2.A1  (median rent burden, latest year).

  Tamanho do domicílio (média de moradores por domicílio)
      UN DESA, Database on Household Size and Composition 2022 (survey mais recente
      de cada país). Sem linha no banco -> proxy por faixa de renda.

  População em favela/assentamento precário (% da população urbana)
      World Bank SDG 11.1.1  EN.POP.SLUM.UR.ZS (ano mais recente com valor não nulo).
      Países de alta renda não são medidos pelo indicador -> null + slum_source="no_data".

  Estoque por padrão (housing_stock_split)
      DERIVADO do Gini de renda (inequality.json) por lognormal com média = 2,0 cestas
      (âncora de income_distribution.json) e sigma = 2*erfinv(Gini). É modelo, não medição.

Os valores observados de homeownership, aluguel e tamanho do domicílio estão fixados nos
dicts OBSERVED_* abaixo (transcritos dos arquivos oficiais; a URL de cada fonte fica em
refs de cada nação). Só a série de favelas é baixada da API do World Bank e guardada em
cache cru em tools/data/housing_cache/.

Uso:
    python tools/build_housing.py             # escrita do JSON
    python tools/build_housing.py --check     # não escreve, só reporta cobertura
    python tools/build_housing.py --refresh   # ignora o cache do World Bank
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
import time
import urllib.request
from typing import Dict, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "scenario", "nations.csv")
INEQUALITY_JSON = os.path.join(ROOT, "data", "definitions", "economy", "inequality.json")
OUT_JSON = os.path.join(ROOT, "data", "definitions", "economy", "housing.json")
CACHE = os.path.join(ROOT, "tools", "data", "housing_cache")

SLUM_INDICATOR = "EN.POP.SLUM.UR.ZS"
WB_API = ("https://api.worldbank.org/v2/country/all/indicator/%s"
          "?format=json&per_page=30000&date=2000:2025")
USER_AGENT = "opengs-game/housing-builder (python urllib)"

# nations.csv usa NED/PSX; o World Bank publica NLD/PSE.
ISO_ALIASES = {
    "NED": "NLD",
    "PSX": "PSE",
}

URL_AHD_TENURE = ("https://webfs.oecd.org/els-com/Affordable_Housing_Database/"
                  "HM1-3-Housing-tenures.xlsx")
URL_AHD_COST = ("https://webfs.oecd.org/els-com/Affordable_Housing_Database/"
                "HC1-2-Housing-costs-over-income.xlsx")
URL_OECD_EGDNA = ("https://sdmx.oecd.org/public/rest/data/OECD.SDD.NAD,"
                  "DSD_EGDNA_SOCDEM@DF_SOCIODEMOGRAPHIC_HHOWN,1.0/"
                  "?dimensionAtObservation=AllDimensions&format=csvfile")
URL_UN_DESA = ("https://www.un.org/development/desa/pd/sites/www.un.org.development.desa.pd/"
               "files/undesa_pd_2022_hh-size-composition.xlsx")
URL_WB_SLUM = "https://data.worldbank.org/indicator/%s" % SLUM_INDICATOR
URL_JPN_SURVEY = "https://www.stat.go.jp/data/jyutaku/2023/pdf/kihon_gaiyou.pdf"

SOURCES = {
    "oecd_ahd_hm1_3_tenures": {
        "title": ("OECD Affordable Housing Database — HM1.3 Housing tenures "
                  "(share of households by tenure type, 2024 or latest year available)"),
        "url": URL_AHD_TENURE,
        "unit": "share of households (own outright + owner with mortgage), 0..1",
    },
    "oecd_egdna_hh_ownership": {
        "title": ("OECD EGDNA — Distribution of households in income quintiles by "
                  "home ownership status (household counts)"),
        "url": URL_OECD_EGDNA,
        "unit": "razão (OWN_OUTRIGHT + OWN_MORTGAGE) / total de domicílios, 0..1",
    },
    "national_census_jpn": {
        "title": ("Statistics Bureau of Japan — 2023 Housing and Land Survey "
                  "(持ち家住宅率 / share of owner-occupied dwellings)"),
        "url": URL_JPN_SURVEY,
        "unit": "share of dwellings, 0..1",
    },
    "oecd_ahd_hc1_2_costs": {
        "title": ("OECD Affordable Housing Database — HC1.2 Housing costs over income "
                  "(HC1.2.A1: median rent burden as share of disposable income)"),
        "url": URL_AHD_COST,
        "unit": "fração da renda disponível, 0..1",
    },
    "un_desa_hh_size": {
        "title": ("UN DESA, Population Division — Database on Household Size and "
                  "Composition 2022 (average household size per survey)"),
        "url": URL_UN_DESA,
        "unit": "moradores por domicílio",
    },
    "worldbank_sdg_11_1_1": {
        "title": "World Bank — SDG 11.1.1: Population living in slums (EN.POP.SLUM.UR.ZS)",
        "url": URL_WB_SLUM,
        "unit": "% da população urbana",
    },
    "regional_proxy_band": {
        "title": ("proxy por faixa de renda (PIB PPP per capita) — faixa de referência "
                  "OCDE AHD / UN DESA"),
        "url": URL_AHD_TENURE,
        "unit": "0..1 (homeownership, aluguel) / moradores por domicílio",
        "method": "valor de faixa aplicado quando não há medição da nação; NÃO é medição",
    },
    "derived_lognormal": {
        "title": "derivação: lognormal(média = 2,0 cestas; sigma = 2*erfinv(Gini))",
        "url": "https://data.worldbank.org/indicator/SI.POV.GINI",
        "unit": "fração do estoque (popular/medium/high)",
        "method": ("modelo reprodutível a partir do Gini de inequality.json; "
                   "NÃO é medição de estoque habitacional"),
    },
}

METHOD = {
    "homeownership": (
        "Taxa de domicílios próprios (0..1). Observada: OECD AHD HM1.3 (own outright + "
        "owner with mortgage), Hungria HM1.3.A1 (2018), Israel derivada das contagens do "
        "OECD EGDNA (2021), Japão no survey nacional de 2023. Sem observação: proxy por "
        "faixa de PIB PPP per capita — low 0,78 | lower_mid 0,72 | upper_mid 0,66 | "
        "high 0,62 — marcada estimated=true. Nenhum valor observado é inventado: todos "
        "saem de tabela publicada, com url e ano em refs."
    ),
    "slum": (
        "World Bank SDG 11.1.1 (EN.POP.SLUM.UR.ZS), ano mais recente com valor não nulo "
        "entre 2000 e 2025. Onde o World Bank não publica valor (várias nações de alta "
        "renda e pequenas ilhas) o campo fica null com slum_source='no_data' — ausência "
        "explícita, nunca zero. Onde o próprio World Bank publica 0,0 (ex.: DEU, CHE, NOR, "
        "QAT, SGP) o valor é mantido como publicado, sem reinterpretação."
    ),
    "household_size": (
        "UN DESA, Database on Household Size and Composition 2022: média de moradores por "
        "domicílio do survey mais recente de cada país (o ano do survey vai em "
        "household_size_year e pode ser antigo — é medição real, não valor atual). Sem "
        "linha no banco: proxy por faixa de renda — low 5,0 | lower_mid 4,2 | "
        "upper_mid 3,2 | high 2,3."
    ),
    "rent_to_income": (
        "Mediana do ônus de aluguel (aluguel de mercado e subsidiado) como fração da renda "
        "disponível, OECD AHD HC1.2.A1, ano mais recente disponível. É ônus de quem aluga, "
        "não de todos os domicílios. Sem série: proxy por faixa de renda — low 0,35 | "
        "lower_mid 0,32 | upper_mid 0,28 | high 0,25 (estimativa, não medição)."
    ),
    "housing_stock_split": (
        "DERIVADO, não medido. Parte do Gini g (0..1) de inequality.json. Relação lognormal "
        "exata: Gini = erf(sigma/2), logo sigma = 2*erfinv(g) = sqrt(2)*Phi^-1((g+1)/2). "
        "Média aritmética fixada em 2,0 cestas (âncora de income_distribution.json) => "
        "mu = ln(2) - sigma^2/2. popular = P(x < 1,0 cesta) = Phi(-mu/sigma); "
        "high = P(x > 7,0 cestas) = 1 - Phi((ln 7 - mu)/sigma); medium = 1 - popular - "
        "high. Arredonda-se a 4 casas e o medium fecha a soma em 1,0. Como o Gini vem do "
        "mesmo inequality.json que o núcleo econômico usa, o estoque é coerente com a "
        "distribuição de renda do jogo."
    ),
}

NOTE = (
    "Condições habitacionais por nação (179 do cenário). Fontes primárias: OECD Affordable "
    "Housing Database HM1.3 (tenure) e HC1.2 (ônus de aluguel), OECD EGDNA (Israel), "
    "Statistics Bureau of Japan 2023 (Japão), UN DESA Household Size and Composition 2022 e "
    "World Bank SDG 11.1.1 (favelas). Campo sem medição da nação recebe um proxy por faixa "
    "de PIB PPP per capita; `estimated: true` marca especificamente a taxa de casa própria "
    "estimada (confidence='estimated') e `proxy_fields` lista, nação por nação, quais dos "
    "campos vieram de proxy — nenhum proxy é apresentado como medição. "
    "housing_stock_split é SEMPRE derivado do Gini de inequality.json (lognormal, média = "
    "2,0 cestas), não um dado de estoque. Nove nações não têm gdp_ppp_bn no nations.csv "
    "(CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN, YEM): usam a faixa 'unknown', com os mesmos "
    "valores da faixa 'low' (padrão conservador) e marcação de estimativa. Japão: a OECD "
    "publica 84% (base domiciliar) mas o survey nacional de 2023 publica 60,9% de "
    "domicílios próprios; usamos o dado nacional. Nenhum valor observado foi ajustado, "
    "interpolado ou inventado."
)

# Faixas de renda por PIB PPP per capita (USD/ano) — ver income_band().
BAND_HIGH_MIN = 25000.0
BAND_UPPER_MID_MIN = 12000.0
BAND_LOWER_MID_MIN = 5000.0

# Proxy por faixa (estimativa documentada; NÃO é medição).
HOMEOWNERSHIP_PROXY = {
    "low": 0.78,
    "lower_mid": 0.72,
    "upper_mid": 0.66,
    "high": 0.62,
    "unknown": 0.78,
}
RENT_PROXY = {
    "low": 0.35,
    "lower_mid": 0.32,
    "upper_mid": 0.28,
    "high": 0.25,
    "unknown": 0.35,
}
HOUSEHOLD_SIZE_PROXY = {
    "low": 5.0,
    "lower_mid": 4.2,
    "upper_mid": 3.2,
    "high": 2.3,
    "unknown": 5.0,
}

BAND_LABEL = {
    "low": "baixa renda (< 5.000 USD PPP per capita)",
    "lower_mid": "renda média-baixa (5.000–12.000)",
    "upper_mid": "renda média-alta (12.000–25.000)",
    "high": "alta renda (> 25.000)",
    "unknown": "faixa desconhecida (sem PIB no nations.csv) — proxy de padrão conservador",
}

# --------------------------------------------------------------------- observados
# (valor, ano, fonte) — homeownership 0..1, share de domicílios próprios.
OBSERVED_HOMEOWNERSHIP: Dict[str, Tuple[float, int, str]] = {
    "AUS": (0.6266, 2021, "oecd"),
    "AUT": (0.4788, 2024, "oecd"),
    "BEL": (0.6582, 2024, "oecd"),
    "BGR": (0.8525, 2024, "oecd"),
    "CAN": (0.6867, 2022, "oecd"),
    "CHE": (0.3820, 2023, "oecd"),
    "CHL": (0.5702, 2022, "oecd"),
    "COL": (0.3591, 2024, "oecd"),
    "CRI": (0.7328, 2024, "oecd"),
    "CYP": (0.6341, 2024, "oecd"),
    "CZE": (0.7181, 2024, "oecd"),
    "DEU": (0.4096, 2024, "oecd"),
    "DNK": (0.5222, 2024, "oecd"),
    "ESP": (0.7364, 2024, "oecd"),
    "EST": (0.7497, 2024, "oecd"),
    "FIN": (0.6100, 2024, "oecd"),
    "FRA": (0.5856, 2024, "oecd"),
    "GBR": (0.6838, 2022, "oecd"),
    "GRC": (0.6817, 2024, "oecd"),
    "HRV": (0.9031, 2024, "oecd"),
    "IRL": (0.6818, 2024, "oecd"),
    "ISL": (0.7835, 2020, "oecd"),
    "ITA": (0.7520, 2024, "oecd"),
    "KOR": (0.5802, 2023, "oecd"),
    "LTU": (0.8683, 2024, "oecd"),
    "LUX": (0.6230, 2024, "oecd"),
    "LVA": (0.8068, 2024, "oecd"),
    "MEX": (0.6958, 2022, "oecd"),
    "MLT": (0.6600, 2024, "oecd"),
    "NED": (0.5792, 2024, "oecd"),
    "NOR": (0.7229, 2024, "oecd"),
    "NZL": (0.6390, 2024, "oecd"),
    "POL": (0.8474, 2024, "oecd"),
    "PRT": (0.7214, 2024, "oecd"),
    "ROU": (0.9277, 2024, "oecd"),
    "SVK": (0.9345, 2024, "oecd"),
    "SVN": (0.7390, 2024, "oecd"),
    "SWE": (0.5819, 2024, "oecd"),
    "TUR": (0.5571, 2020, "oecd"),
    "USA": (0.6531, 2023, "oecd"),
    # Hungria: 73,25% (próprio quitado) + 11,54% (financiado) em 2018 = 84,79%
    # (OECD AHD HM1.3.A1); 2019+ sob revisão no indicador HM1.3.1.
    "HUN": (0.8479, 2018, "oecd"),
    # Israel: (OWN_OUTRIGHT + OWN_MORTGAGE) / total de domicílios = 62,75% em 2021,
    # calculado a partir das contagens do OECD EGDNA (não publica taxa pronta).
    "ISR": (0.6275, 2021, "oecd"),
    # Japão: 持ち家住宅率 60,9% — Statistics Bureau, 2023 Housing and Land Survey.
    "JPN": (0.6090, 2023, "national"),
}

# (fração da renda disponível, ano) — mediana do ônus de aluguel (OECD AHD HC1.2.A1).
OBSERVED_RENT: Dict[str, Tuple[float, int]] = {
    "AUS": (0.2313, 2021),
    "AUT": (0.1937, 2024),
    "BEL": (0.2453, 2024),
    "BGR": (0.1137, 2024),
    "CAN": (0.2451, 2022),
    "CHE": (0.2456, 2023),
    "CHL": (0.2727, 2022),
    "COL": (0.2057, 2024),
    "CRI": (0.1775, 2024),
    "CYP": (0.2284, 2024),
    "CZE": (0.1344, 2024),
    "DEU": (0.1839, 2024),
    "DNK": (0.2780, 2024),
    "ESP": (0.2345, 2024),
    "EST": (0.2075, 2024),
    "FIN": (0.3060, 2024),
    "FRA": (0.2177, 2024),
    "GBR": (0.2304, 2022),
    "GRC": (0.2428, 2024),
    "HRV": (0.1435, 2024),
    "HUN": (0.2409, 2018),
    "IRL": (0.1879, 2024),
    "ISL": (0.2762, 2018),
    "ITA": (0.1837, 2024),
    "JPN": (0.1993, 2023),
    "LTU": (0.1581, 2024),
    "LUX": (0.2553, 2024),
    "LVA": (0.0774, 2024),
    "MEX": (0.1508, 2022),
    "MLT": (0.2067, 2024),
    "NED": (0.2550, 2024),
    "NOR": (0.2846, 2024),
    "NZL": (0.2590, 2017),
    "POL": (0.1798, 2024),
    "PRT": (0.2055, 2024),
    "ROU": (0.1996, 2024),
    "SVK": (0.2189, 2024),
    "SVN": (0.1536, 2024),
    "SWE": (0.2830, 2024),
    "TUR": (0.1867, 2020),
    "USA": (0.2438, 2023),
}

# (moradores por domicílio, ano do survey) — UN DESA Household Size and Composition 2022.
OBSERVED_HOUSEHOLD_SIZE: Dict[str, Tuple[float, int]] = {
    "AFG": (8.04, 2015),
    "AGO": (4.82, 2016),
    "ALB": (3.30, 2017),
    "ARG": (2.95, 2019),
    "ARM": (3.54, 2016),
    "AUS": (2.55, 2016),
    "AUT": (2.27, 2011),
    "AZE": (4.55, 2009),
    "BDI": (4.83, 2016),
    "BEL": (2.32, 2011),
    "BEN": (5.19, 2018),
    "BFA": (5.92, 2014),
    "BGD": (4.26, 2019),
    "BGR": (2.34, 2011),
    "BHS": (3.40, 2010),
    "BIH": (3.50, 2011),
    "BLR": (2.16, 2019),
    "BLZ": (3.75, 2015),
    "BOL": (3.53, 2012),
    "BRA": (3.31, 2010),
    "BRB": (2.85, 2012),
    "BTN": (4.59, 2010),
    "BWA": (3.52, 2011),
    "CAF": (5.59, 2018),
    "CAN": (2.45, 2016),
    "CHE": (2.24, 2000),
    "CHL": (3.04, 2017),
    "CHN": (3.38, 2000),
    "CMR": (4.99, 2018),
    "COD": (5.17, 2017),
    "COG": (4.22, 2014),
    "COL": (3.53, 2015),
    "CRI": (3.21, 2018),
    "CUB": (2.71, 2019),
    "CYP": (2.76, 2011),
    "CZE": (2.34, 2011),
    "DEU": (2.14, 2011),
    "DJI": (5.89, 2006),
    "DOM": (3.09, 2019),
    "DZA": (4.94, 2018),
    "ECU": (3.78, 2010),
    "EGY": (4.13, 2014),
    "ESP": (2.58, 2011),
    "EST": (2.30, 2011),
    "ETH": (4.73, 2019),
    "FIN": (2.07, 2010),
    "FJI": (4.57, 2014),
    "FRA": (2.22, 2015),
    "GAB": (4.10, 2012),
    "GBR": (2.35, 2011),
    "GEO": (3.42, 2018),
    "GHA": (3.83, 2019),
    "GIN": (6.39, 2021),
    "GMB": (8.07, 2020),
    "GNQ": (5.30, 2000),
    "GRC": (2.56, 2011),
    "GTM": (4.81, 2015),
    "GUY": (3.50, 2019),
    "HND": (3.87, 2019),
    "HRV": (2.80, 2011),
    "HTI": (4.29, 2017),
    "HUN": (2.36, 2011),
    "IDN": (3.86, 2017),
    "IND": (4.38, 2020),
    "IRL": (2.83, 2016),
    "IRN": (3.51, 2011),
    "IRQ": (6.35, 2018),
    "ISR": (3.14, 2008),
    "ITA": (2.40, 2011),
    "JAM": (3.06, 2011),
    "JOR": (4.72, 2017),
    "JPN": (2.33, 2015),
    "KAZ": (3.44, 2015),
    "KEN": (3.75, 2020),
    "KGZ": (4.05, 2018),
    "KHM": (4.61, 2014),
    "KOR": (2.34, 2020),
    "LAO": (4.70, 2017),
    "LBR": (4.58, 2019),
    "LSO": (3.73, 2018),
    "LTU": (2.38, 2011),
    "LUX": (2.41, 2011),
    "LVA": (2.26, 2021),
    "MAR": (4.58, 2014),
    "MDA": (2.89, 2014),
    "MDG": (4.50, 2018),
    "MEX": (3.74, 2015),
    "MKD": (3.45, 2018),
    "MLI": (5.81, 2018),
    "MLT": (2.67, 2011),
    "MMR": (4.22, 2016),
    "MNE": (3.29, 2018),
    "MNG": (3.63, 2018),
    "MOZ": (4.52, 2018),
    "MRT": (5.77, 2015),
    "MWI": (4.28, 2019),
    "MYS": (4.57, 2000),
    "NAM": (4.24, 2013),
    "NED": (2.23, 2011),
    "NER": (5.92, 2012),
    "NGA": (4.66, 2018),
    "NIC": (4.92, 2005),
    "NOR": (2.22, 2011),
    "NPL": (4.25, 2019),
    "NZL": (2.60, 2018),
    "OMN": (8.02, 2003),
    "PAK": (6.80, 2013),
    "PAN": (3.64, 2013),
    "PER": (3.46, 2017),
    "PHL": (4.23, 2017),
    "PNG": (5.43, 2000),
    "POL": (2.82, 2011),
    "PRK": (3.93, 2008),
    "PRT": (2.58, 2011),
    "PRY": (3.87, 2016),
    "PSX": (5.06, 2019),
    "QAT": (4.67, 2012),
    "ROU": (2.67, 2011),
    "RUS": (2.58, 2010),
    "RWA": (4.28, 2020),
    "SDN": (5.89, 2014),
    "SEN": (8.66, 2017),
    "SGP": (2.95, 2020),
    "SLE": (5.29, 2019),
    "SLV": (3.92, 2014),
    "SOM": (6.16, 2011),
    "SRB": (2.85, 2019),
    "SSD": (5.98, 2010),
    "SUR": (3.86, 2018),
    "SVK": (2.90, 2011),
    "SVN": (2.47, 2015),
    "SWZ": (4.04, 2014),
    "SYR": (5.65, 2006),
    "TCD": (5.97, 2019),
    "TGO": (4.35, 2017),
    "THA": (2.84, 2019),
    "TJK": (5.99, 2017),
    "TKM": (5.22, 2019),
    "TLS": (5.27, 2016),
    "TTO": (3.29, 2011),
    "TUN": (3.89, 2018),
    "TUR": (4.07, 2004),
    "TZA": (4.85, 2015),
    "UGA": (4.87, 2019),
    "UKR": (2.53, 2012),
    "URY": (3.02, 2012),
    "USA": (2.49, 2015),
    "UZB": (5.22, 2006),
    "VEN": (4.33, 2001),
    "VNM": (3.44, 2020),
    "XKX": (4.90, 2019),
    "YEM": (6.67, 2013),
    "ZAF": (3.36, 2016),
    "ZMB": (5.00, 2018),
    "ZWE": (4.02, 2019),
    # GNB (Guiné-Bissau) não consta do banco UN DESA -> proxy por faixa de renda.
}


def load_nations() -> list:
    with open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def load_gini() -> Dict[str, Optional[float]]:
    with open(INEQUALITY_JSON, "r", encoding="utf-8") as handle:
        payload = json.load(handle)
    out: Dict[str, Optional[float]] = {}
    for code, entry in (payload.get("nations") or {}).items():
        gini = entry.get("gini")
        out[code] = float(gini) if isinstance(gini, (int, float)) else None
    return out


def income_band(nation: dict) -> str:
    """Faixa de PIB PPP per capita; 'unknown' quando o nations.csv não traz PIB."""
    raw_pop = (nation.get("population") or "").strip()
    raw_gdp = (nation.get("gdp_ppp_bn") or "").strip()
    if not raw_pop or not raw_gdp:
        return "unknown"
    population = int(float(raw_pop))
    if population <= 0:
        return "unknown"
    per_capita = float(raw_gdp) * 1e9 / population
    if per_capita < BAND_LOWER_MID_MIN:
        return "low"
    if per_capita < BAND_UPPER_MID_MIN:
        return "lower_mid"
    if per_capita <= BAND_HIGH_MIN:
        return "upper_mid"
    return "high"


# --------------------------------------------------------------------- favelas (World Bank)
def fetch_slum(refresh: bool = False) -> Dict[str, Tuple[float, int]]:
    """Baixa (ou lê do cache) EN.POP.SLUM.UR.ZS e devolve {ISO3: (% urbana, ano)}."""
    path = os.path.join(CACHE, "wb_%s.json" % SLUM_INDICATOR.lower().replace(".", "_"))
    payload = None
    if not refresh and os.path.exists(path):
        with open(path, "r", encoding="utf-8") as handle:
            payload = json.load(handle)
    if payload is None:
        last_error = None
        for attempt in range(3):
            try:
                request = urllib.request.Request(
                    WB_API % SLUM_INDICATOR,
                    headers={"User-Agent": USER_AGENT, "Accept": "application/json"},
                )
                with urllib.request.urlopen(request, timeout=180) as response:
                    payload = json.loads(response.read().decode("utf-8"))
                break
            except Exception as exc:  # rede instável: tenta de novo
                last_error = exc
                time.sleep(3 * (attempt + 1))
        if payload is None:
            raise SystemExit(
                "falha ao baixar %s: %s\n(rode com rede ou com o cache em %s)"
                % (SLUM_INDICATOR, last_error, CACHE))
        os.makedirs(CACHE, exist_ok=True)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle)

    if not isinstance(payload, list) or len(payload) < 2 or not payload[1]:
        raise SystemExit("resposta inesperada do World Bank para %s" % SLUM_INDICATOR)

    series: Dict[str, Dict[int, float]] = {}
    for row in payload[1]:
        value = row.get("value")
        code = (row.get("countryiso3code") or "").strip().upper()
        if value is None or not code:
            continue
        series.setdefault(code, {})[int(row["date"])] = float(value)
    return {code: (years[max(years)], max(years)) for code, years in series.items()}


# --------------------------------------------------------------------- lognormal
def _inverse_normal_cdf(p: float) -> float:
    """Phi^-1(p) — aproximação de Acklam (erro absoluto < 1,2e-9)."""
    if p <= 0.0:
        return float("-inf")
    if p >= 1.0:
        return float("inf")
    a = (-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
         1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00)
    b = (-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
         6.680131188771972e+01, -1.328068155288572e+01)
    c = (-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
         -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00)
    d = (7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
         3.754408661907416e+00)
    low = 0.02425
    high = 1.0 - low
    if p < low:
        q = math.sqrt(-2.0 * math.log(p))
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / \
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0)
    elif p > high:
        q = math.sqrt(-2.0 * math.log(1.0 - p))
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / \
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0)
    else:
        q = p - 0.5
        r = q * q
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / \
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0)
    # uma iteração de Halley refina para precisão de máquina
    error = 0.5 * math.erfc(-x / math.sqrt(2.0)) - p
    u = error * math.sqrt(2.0 * math.pi) * math.exp(x * x / 2.0)
    return x - u / (1.0 + x * u / 2.0)


def erfinv(y: float) -> float:
    """Inversa da função erro: erfinv(y) = Phi^-1((1+y)/2) / sqrt(2)."""
    return _inverse_normal_cdf((1.0 + y) / 2.0) / math.sqrt(2.0)


def normal_cdf(z: float) -> float:
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


def stock_split(gini_0_100: Optional[float], basic: float = 1.0,
                luxury: float = 7.0, mean_baskets: float = 2.0) -> Dict[str, float]:
    """popular/medium/high derivados do Gini (lognormal ancorada em `mean_baskets`)."""
    gini = 0.0 if gini_0_100 is None else max(0.0, min(0.999999, gini_0_100 / 100.0))
    sigma = 2.0 * erfinv(gini)
    if sigma <= 1e-9:
        return {"popular": 0.0, "medium": 1.0, "high": 0.0}
    mu = math.log(mean_baskets) - sigma * sigma / 2.0
    popular = normal_cdf((math.log(basic) - mu) / sigma)
    high = 1.0 - normal_cdf((math.log(luxury) - mu) / sigma)
    popular = min(1.0, max(0.0, round(popular, 4)))
    high = min(1.0, max(0.0, round(high, 4)))
    if popular + high > 1.0:  # caso extremo: mantém a soma em 1,0
        scale = 1.0 / (popular + high)
        popular = round(popular * scale, 4)
        high = round(high * scale, 4)
    medium = round(1.0 - popular - high, 4)
    return {"popular": popular, "medium": medium, "high": high}


# --------------------------------------------------------------------- montagem
def proxy_ref(band: str) -> dict:
    return {
        "title": ("Proxy de faixa de renda — %s (faixa de referência OCDE AHD / UN DESA, "
                  "não medição da nação)" % BAND_LABEL[band]),
        "url": URL_AHD_TENURE,
        "year": 2025,
    }


def build_nation(nation: dict, gini_by_code: Dict[str, Optional[float]],
                 slum: Dict[str, Tuple[float, int]]) -> dict:
    code = nation["id"]
    band = income_band(nation)
    refs = []
    proxy_fields = []

    home = OBSERVED_HOMEOWNERSHIP.get(code)
    if home:
        home_rate: Optional[float] = round(float(home[0]), 4)
        home_year: Optional[int] = int(home[1])
        if home[2] == "national":
            home_source = "national_census"
            refs.append({
                "title": ("Statistics Bureau of Japan — 2023 Housing and Land Survey "
                          "(持ち家住宅率 60,9%)"),
                "url": URL_JPN_SURVEY,
                "year": 2023,
            })
        else:
            home_source = "oecd"
            if code == "ISR":
                refs.append({
                    "title": ("OECD EGDNA — households by home ownership status, Israel "
                              "(%d): (OWN_OUTRIGHT + OWN_MORTGAGE) / total" % home_year),
                    "url": URL_OECD_EGDNA,
                    "year": home_year,
                })
            else:
                refs.append({
                    "title": ("OECD Affordable Housing Database HM1.3 — %s (%d): own "
                              "outright + owner with mortgage"
                              % (nation["name"], home_year)),
                    "url": URL_AHD_TENURE,
                    "year": home_year,
                })
    else:
        home_rate = HOMEOWNERSHIP_PROXY[band]
        home_year = None
        home_source = "regional_proxy"
        proxy_fields.append("homeownership")

    rent = OBSERVED_RENT.get(code)
    if rent:
        rent_ratio: Optional[float] = round(float(rent[0]), 4)
        rent_year: Optional[int] = int(rent[1])
        rent_source = "oecd"
        refs.append({
            "title": ("OECD AHD HC1.2 — median rent burden, %s (%d)"
                      % (nation["name"], rent_year)),
            "url": URL_AHD_COST,
            "year": rent_year,
        })
    else:
        rent_ratio = RENT_PROXY[band]
        rent_year = None
        rent_source = "regional_proxy"
        proxy_fields.append("rent")

    household = OBSERVED_HOUSEHOLD_SIZE.get(code)
    if household:
        household_size: Optional[float] = round(float(household[0]), 2)
        household_year: Optional[int] = int(household[1])
        household_source = "un_desa"
        refs.append({
            "title": ("UN DESA Household Size and Composition 2022 — %s (survey %d)"
                      % (nation["name"], household_year)),
            "url": URL_UN_DESA,
            "year": household_year,
        })
    else:
        household_size = HOUSEHOLD_SIZE_PROXY[band]
        household_year = None
        household_source = "regional_proxy"
        proxy_fields.append("household_size")

    wb_code = ISO_ALIASES.get(code, code)
    slum_hit = slum.get(wb_code)
    if slum_hit:
        slum_pct: Optional[float] = round(float(slum_hit[0]), 2)
        slum_year: Optional[int] = int(slum_hit[1])
        slum_source = "worldbank_sdg_11_1_1"
        refs.append({
            "title": ("World Bank SDG 11.1.1 (EN.POP.SLUM.UR.ZS) — %s (%d)"
                      % (nation["name"], slum_year)),
            "url": ("https://api.worldbank.org/v2/country/%s/indicator/%s?format=json&date=%d"
                    % (wb_code, SLUM_INDICATOR, slum_year)),
            "year": slum_year,
        })
    else:
        slum_pct = None
        slum_year = None
        slum_source = "no_data"

    split = stock_split(gini_by_code.get(code))

    if home_source == "regional_proxy":
        confidence = "estimated"
    elif not proxy_fields and home_year is not None and home_year >= 2020:
        confidence = "complete"
    else:
        confidence = "partial"

    if not refs:
        refs.append(proxy_ref(band))

    entry = {
        "name": nation["name"],
        "homeownership_rate": home_rate,
        "homeownership_source": home_source,
        "homeownership_year": home_year,
        "rent_to_income_ratio": rent_ratio,
        "rent_source": rent_source,
        "rent_year": rent_year,
        "slum_pct_urban": slum_pct,
        "slum_source": slum_source,
        "slum_year": slum_year,
        "household_size": household_size,
        "household_size_source": household_source,
        "household_size_year": household_year,
        "housing_stock_split": split,
        "stock_split_source": "derived:lognormal_from_gini",
        "estimated": confidence == "estimated",
        "confidence": confidence,
        "refs": refs,
        "band": band,
        "proxy_fields": proxy_fields,
    }
    if confidence == "estimated":
        entry["method"] = (
            "Estimativa por faixa de renda (%s); campos em proxy: %s. "
            "housing_stock_split é derivado do Gini (ver method do arquivo)."
            % (band, ", ".join(proxy_fields))
        )
    return entry


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Gera housing.json a partir das fontes reais.")
    parser.add_argument("--check", action="store_true", help="não escreve o JSON")
    parser.add_argument("--refresh", action="store_true", help="ignora o cache local")
    args = parser.parse_args(argv)

    nations = load_nations()
    gini_by_code = load_gini()
    slum = fetch_slum(args.refresh)

    entries = {}
    for nation in nations:
        entries[nation["id"]] = build_nation(nation, gini_by_code, slum)

    observed_home = sum(1 for e in entries.values()
                        if e["homeownership_source"] != "regional_proxy")
    observed_slum = sum(1 for e in entries.values() if e["slum_pct_urban"] is not None)
    observed_hh = sum(1 for e in entries.values()
                      if e["household_size_source"] == "un_desa")
    estimated = sum(1 for e in entries.values() if e["estimated"])

    payload = {
        "version": 1,
        "note": NOTE,
        "coverage": {
            "nations_total": len(entries),
            "nations_with_observed_homeownership": observed_home,
            "nations_with_observed_slum": observed_slum,
            "nations_with_observed_household_size": observed_hh,
        },
        "sources": SOURCES,
        "method": METHOD,
        "nations": entries,
    }

    print("nações: %d | casa própria observada: %d | estimada por faixa: %d"
          % (len(entries), observed_home, estimated))
    print("favela observada (SDG 11.1.1): %d | sem dado: %d"
          % (observed_slum, len(entries) - observed_slum))
    print("tamanho do domicílio observado (UN DESA): %d" % observed_hh)
    if len(entries) != 179:
        print("AVISO: nations.csv trouxe %d nações (esperado 179)" % len(entries))

    if args.check:
        print("--check: JSON não foi escrito")
        return 0

    os.makedirs(os.path.dirname(OUT_JSON), exist_ok=True)
    with open(OUT_JSON, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    print("escrito:", os.path.relpath(OUT_JSON, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
