#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gera data/definitions/economy/municipal_finance.json (uma entrada por nação do jogo).

O que este arquivo descreve, por nação:

  subnational_revenue_pct_gdp        receita total do governo subnacional (% do PIB)
  property_tax_pct_total_tax         imposto recorrente sobre imóveis (% da tributação total)
  central_transfers_pct_subnational  transferências do governo central (% da receita subnacional)

Fontes observadas, consultadas via API pública OECD SDMX (SDMX REST, formato
csvfilewithlabels). A extração exata foi congelada nos dicts OBSERVED_* abaixo; o dump
usado para conferi-la está em tools/data/municipal_finance_cache/observed_block.txt
(diretório de cache, não versionado).

  * OECD CFE — Subnational government finance (SNG dashboard)
      DSD_DASHBOARD@REV           R1  Revenue                    -> receita subnacional % do PIB
      DSD_DASHBOARD@REVENUE_CAT   R12 Grants and subsidies        -> transferências % receita subnacional
      Setor S13M (estado + municipal) quando publicado; senão S1313 (municipal).
      https://www.oecd.org/en/topics/subnational-finance.html
  * OECD CTP — Revenue Statistics (comparative tables)
      série 4100 "Recurrent taxes on immovable property" sobre o total de tributos,
      governo geral (S13), ambos em % do PIB no mesmo ano; a razão 4100/total é uma
      identidade aritmética dos dois valores publicados (não é interpolação).
      Dataflows: DF_RSOECD, DF_RSGLOBAL, DF_RSLAC, DF_RSASAP, DF_RSAFRICA.
      https://www.oecd.org/en/data/datasets/revenue-statistics.html

Regras de honestidade:

  * Nenhum valor é inventado. Todo campo tem origem observada (com refs url+ano) ou
    proxy por faixa de renda explicitamente marcada (`*_source: "proxy"`, `*_estimated: true`).
  * A confiança é POR CAMPO: cada campo leva `*_estimated` (true = proxy, false = observado ou
    ausente). A confiança da entrada (complete/partial/estimated/no_data) só resume a mistura.
  * Valor exatamente 0,00 na série 4100 é tratado como DADO AUSENTE (país normalmente não
    desagrega 4100 e o zero é preenchimento da tabela), portanto cai para proxy — nunca é
    apresentado como "imposto predial medido = 0".
  * Faixa de renda: gdp_ppp_per_capita = gdp_ppp_bn*1e9/population (nations.csv).
      low < 5.000 | lower_mid 5.000-12.000 | upper_mid 12.000-25.000 | high > 25.000
  * Nove nações não têm gdp_ppp_bn no nations.csv (CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN,
    YEM). Sem faixa de renda não há proxy possível: a entrada sai com os três campos null
    e confidence="no_data" + reason. Não se estima por região para "fechar" o arquivo.
  * IMF GFS é declarado em `sources` mas NÃO foi usado nesta versão: só a série OECD SDMX
    foi confirmada de forma reprodutível (ver NOTE).

Uso:
    python tools/build_municipal_finance.py            # escreve o JSON
    python tools/build_municipal_finance.py --check    # não escreve, só reporta cobertura
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import sys
from typing import Dict, Optional

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_CSV = os.path.join(ROOT, "data", "definitions", "scenario", "nations.csv")
OUT_JSON = os.path.join(ROOT, "data", "definitions", "economy", "municipal_finance.json")
CACHE = os.path.join(ROOT, "tools", "data", "municipal_finance_cache")

# --------------------------------------------------------------------------------------
# Federações (lista fixa do briefing): usada apenas como flag de descentralização no proxy.
# --------------------------------------------------------------------------------------
FEDERATIONS = frozenset({
    "USA", "BRA", "IND", "DEU", "AUS", "CAN", "MEX", "RUS", "ARG", "MYS",
    "NGA", "PAK", "ETH", "CHE", "AUT", "BEL", "VEN", "ARE",
})

# Faixas de renda (PIB PPC per capita, em USD correntes do nations.csv).
LOW_MAX = 5000.0
LOWER_MID_MAX = 12000.0
UPPER_MID_MAX = 25000.0

# --------------------------------------------------------------------------------------
# Proxy por faixa de renda. São ORDENAMENTOS DE GRANDEZA documentados a partir da
# literatura de descentralização fiscal (OECD/Kiel Institute): NÃO são medições do país.
# --------------------------------------------------------------------------------------
PROXY_SUBNATIONAL_FEDERAL = {"high": 10.0, "upper_mid": 8.0, "lower_mid": 3.0, "low": 2.0}
PROXY_SUBNATIONAL_UNITARY = {"high": 6.0, "upper_mid": 4.5, "lower_mid": 3.0, "low": 2.0}
PROXY_PROPERTY_TAX = {"high": 5.5, "upper_mid": 3.0, "lower_mid": 1.5, "low": 0.8}
PROXY_TRANSFERS_FEDERAL = {"high": 40.0, "upper_mid": 45.0, "lower_mid": 60.0, "low": 55.0}
PROXY_TRANSFERS_UNITARY = {"high": 60.0, "upper_mid": 65.0, "lower_mid": 60.0, "low": 55.0}

BAND_LABEL = {
    "low": "renda baixa (<5.000)",
    "lower_mid": "renda média-baixa (5.000-12.000)",
    "upper_mid": "renda média-alta (12.000-25.000)",
    "high": "renda alta (>25.000)",
}

# Ano de referência das faixas proxy = último período coberto pelas séries observadas.
PROXY_REF_YEAR = 2024

# Código da nação no nations.csv -> código usado pelas tabelas OECD.
AREA_ALIAS = {"NED": "NLD"}

# --------------------------------------------------------------------------------------
# Bloco observado. Cada linha é (valor, ano, setor/dataflow); o comentário nomeia a fonte.
# Valores extraídos das respostas OECD SDMX guardadas em tools/data/municipal_finance_cache/.
OBSERVED_SUBNATIONAL = {
    "AUS": (16.79, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
    "AUT": (19.14, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
    "BEL": (25.66, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
    "CAN": (31.79, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
    "CHE": (20.40, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
    "CHL": (4.14, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "COL": (11.35, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "CRI": (1.68, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "CZE": (12.86, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "DEU": (22.73, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
    "DNK": (31.42, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "ESP": (23.14, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
    "EST": (10.03, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "FIN": (22.71, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "FRA": (10.65, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "GBR": (9.01, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "GRC": (4.03, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "HUN": (5.29, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "IRL": (2.17, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "ISL": (13.45, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "ISR": (5.59, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "ITA": (14.39, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "JPN": (15.64, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "KOR": (14.77, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "LTU": (9.90, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "LUX": (5.56, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "LVA": (11.35, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "MEX": (12.06, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
    "NOR": (14.61, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "NZL": (4.41, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "POL": (14.91, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "PRT": (6.69, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "SVK": (7.94, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "SVN": (8.41, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "SWE": (24.70, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "TUR": (3.37, 2024, "S1313"),  # OECD SNG REV R1 % do PIB
    "USA": (17.14, 2024, "S13M"),  # OECD SNG REV R1 % do PIB
}

OBSERVED_TRANSFERS = {
    "AUS": (36.32, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "AUT": (73.51, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "BEL": (57.60, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "CAN": (28.65, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "CHE": (25.82, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "CHL": (50.13, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "COL": (53.66, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "CRI": (22.34, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "CZE": (82.07, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "DEU": (28.14, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "DNK": (57.01, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "ESP": (53.13, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "EST": (87.44, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "FIN": (55.73, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "FRA": (27.03, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "GBR": (63.09, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "GRC": (68.84, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "HUN": (46.55, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "IRL": (69.69, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "ISL": (10.31, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "ISR": (50.21, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "ITA": (59.17, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "JPN": (45.77, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "KOR": (60.36, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "LTU": (88.07, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "LUX": (50.57, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "LVA": (36.06, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "MEX": (89.76, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "NOR": (45.69, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "NZL": (30.24, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "POL": (57.62, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "PRT": (45.55, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "SVK": (78.43, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "SVN": (77.70, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "SWE": (39.27, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "TUR": (71.43, 2024, "S1313"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
    "USA": (25.54, 2024, "S13M"),  # OECD SNG REVENUE_CAT R12 % da receita subnacional
}

# Peso do imposto sobre imóveis: razão 4100/(_T) em % do PIB, mesmo ano, governo geral (S13).
# ATENÇÃO: países cuja tabela publica 4100 exatamente 0,00 NÃO entram aqui — zero exato é
# tratado como dado ausente (a maioria não desagrega a série 4100) e cai para proxy.
OBSERVED_PROPERTY_TAX = {
    "ARG": (0.92, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "ATG": (2.33, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "AUS": (5.66, 2023, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "AUT": (0.45, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "AZE": (1.36, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "BEL": (2.96, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "BFA": (0.17, 2023, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "BGR": (0.74, 2022, "DF_RSGLOBAL"),  # Revenue Statistics 4100 / tributos totais
    "BHS": (6.63, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "BLZ": (0.46, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "BRA": (1.92, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "BRB": (5.07, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "BTN": (1.48, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "CAN": (7.56, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "CHE": (0.66, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "CHL": (4.69, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "CHN": (2.71, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "CIV": (2.61, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "CMR": (0.05, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "COD": (0.77, 2023, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "COL": (3.68, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "CRI": (1.41, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "CZE": (0.85, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "DEU": (0.98, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "DNK": (2.02, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "DOM": (0.49, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "ECU": (0.86, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "EGY": (1.58, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "ESP": (2.43, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "EST": (0.41, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "FIN": (1.92, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "FRA": (4.48, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "GAB": (1.70, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "GBR": (8.14, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "GEO": (3.00, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "GIN": (0.32, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "GRC": (3.59, 2023, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "GRD": (1.29, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "GTM": (1.39, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "GUY": (1.74, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "HND": (0.47, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "HRV": (1.93, 2021, "DF_RSGLOBAL"),  # Revenue Statistics 4100 / tributos totais
    "HUN": (0.96, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "IDN": (1.24, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "IRL": (1.90, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "ISL": (5.43, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "ISR": (5.45, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "ITA": (2.46, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "JAM": (1.06, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "JPN": (5.65, 2023, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "KAZ": (2.47, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "KEN": (0.00, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "KGZ": (0.98, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "KHM": (0.64, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "KOR": (3.45, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "LAO": (0.49, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "LBR": (0.86, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "LCA": (0.78, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "LTU": (0.90, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "LUX": (0.13, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "LVA": (1.68, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "MAR": (2.70, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "MEX": (1.07, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "MLI": (0.09, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "MNG": (2.56, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "MOZ": (0.00, 2016, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "MRT": (2.75, 2018, "DF_RSGLOBAL"),  # Revenue Statistics 4100 / tributos totais
    "NAM": (0.01, 2023, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "NED": (1.48, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "NER": (2.00, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "NIC": (1.18, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "NOR": (0.84, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "NZL": (6.49, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "PAN": (1.26, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "PER": (1.63, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "PHL": (2.10, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "POL": (2.82, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "PRT": (1.62, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "PRY": (1.09, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "RWA": (1.14, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "SEN": (0.17, 2021, "DF_RSGLOBAL"),  # Revenue Statistics 4100 / tributos totais
    "SGP": (6.39, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "SLV": (0.04, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "SUR": (0.01, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "SVK": (1.36, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "SVN": (1.17, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "SWE": (1.57, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "SWZ": (2.43, 2023, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "TGO": (0.51, 2024, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "THA": (1.49, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "TTO": (0.24, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "TUN": (0.03, 2023, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
    "TUR": (0.41, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "UKR": (2.19, 2022, "DF_RSGLOBAL"),  # Revenue Statistics 4100 / tributos totais
    "URY": (3.67, 2024, "DF_RSLAC"),  # Revenue Statistics 4100 / tributos totais
    "USA": (10.43, 2024, "DF_RSOECD"),  # Revenue Statistics 4100 / tributos totais
    "VNM": (0.20, 2024, "DF_RSASAP"),  # Revenue Statistics 4100 / tributos totais
    "ZAF": (3.41, 2023, "DF_RSAFRICA"),  # Revenue Statistics 4100 / tributos totais
}

SOURCES = {
    "oecd_subnational": {
        "title": "OECD — Subnational government finance (SNG dashboard: revenue, revenue by category)",
        "url": "https://www.oecd.org/en/topics/subnational-finance.html",
        "unit": "% do PIB (R1) e % da receita do mesmo setor subnacional (R12)",
    },
    "oecd_revenue_statistics": {
        "title": "OECD Revenue Statistics — Recurrent taxes on immovable property (4100) / total de tributos (_T)",
        "url": "https://www.oecd.org/en/data/datasets/revenue-statistics.html",
        "unit": "% (razão calculada entre dois valores publicados em % do PIB)",
    },
    "imf_gfs": {
        "title": "IMF Government Finance Statistics (GFS) — declarada, não utilizada nesta versão",
        "url": "https://data.imf.org/gfs",
        "unit": "% (não aplicada)",
    },
    "proxy_band": {
        "title": "faixa típica por renda per capita PPC + flag de federação (estimativa, não medição)",
        "url": "https://www.oecd.org/en/topics/subnational-finance.html",
        "unit": "% do PIB / % da receita subnacional / % da tributação total",
        "method": "valor fixo por faixa de renda aplicado quando não há observação; NÃO é medição da nação",
    },
}

METHOD = {
    "subnational_revenue": (
        "Observado: OECD SNG dashboard, R1 (receita total) em % do PIB, setor S13M "
        "(estado+municipal) quando publicado, senão S1313 (municipal); ano mais recente. "
        "Proxy (estimated): faixa de renda per capita PPC + flag de federação."
    ),
    "property_tax": (
        "Observado: OECD Revenue Statistics, impostos recorrentes sobre imóveis (4100) "
        "dividido pelo total de tributos (_T), ambos em % do PIB, governo geral (S13), mesmo ano; "
        "a divisão é identidade aritmética dos dois valores publicados. Zero exato = ausência de "
        "dado (não é medição). Proxy (estimated): faixa de renda per capita PPC."
    ),
    "central_transfers": (
        "Observado: OECD SNG dashboard, R12 'Grants and subsidies' em % da receita do mesmo "
        "setor subnacional (S13M quando existir, senão S1313). "
        "Proxy (estimated): faixa de renda per capita PPC + flag de federação."
    ),
}

NOTE = (
    "Finanças municipais por nação. Observados: OECD Subnational government finance (SNG "
    "dashboard) para receita subnacional (% do PIB) e transferências (% da receita subnacional); "
    "OECD Revenue Statistics, série 4100 sobre o total de tributos, para o peso do imposto sobre "
    "imóveis. Onde não há observação, aplica-se proxy por faixa de renda per capita PPC + flag de "
    "federação — nunca apresentado como medição: cada campo proxy leva `*_estimated=true` "
    "(subnational_estimated / property_tax_estimated / transfers_estimated) e cada campo "
    "observado leva `*_estimated=false`. A confiança da ENTRADA resume a mistura e não substitui "
    "as flags por campo: complete = os três campos observados; partial = misto (ao menos um "
    "observado e ao menos um proxy); estimated = os três campos proxy; no_data = os três campos "
    "null. `estimated` (entrada) é true SOMENTE quando os três campos são proxy. Valor 0,00 "
    "exato na série 4100 é tratado como dado ausente e cai para proxy. Nove nações sem gdp_ppp_bn "
    "no nations.csv (CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN, YEM) ficam com os três campos null, "
    "source=no_data e confidence=no_data: sem faixa de renda não se inventa proxy. A cobertura "
    "observada é desigual por construção: receita subnacional e transferências só existem para "
    "membros da OECD; o peso do imposto sobre imóveis cobre também países das séries Revenue "
    "Statistics LAC/África/Ásia-Pacífico/Global — por isso o bloco `coverage` conta observado/"
    "proxy/null por campo, além das entradas 100% observadas. IMF GFS não foi usada nesta versão. "
    "Em campo proxy, refs[].year é o último ano de referência da série (2024), não uma medição."
)


def parse_float(text: Optional[str]) -> Optional[float]:
    if text is None:
        return None
    text = text.strip()
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def income_band(nation: dict) -> Optional[str]:
    """Faixa de renda pelo PIB PPC per capita; None quando falta população ou PIB."""
    population = parse_float(nation.get("population"))
    gdp_bn = parse_float(nation.get("gdp_ppp_bn"))
    if not population or not gdp_bn:
        return None
    per_capita = gdp_bn * 1e9 / population
    if per_capita < LOW_MAX:
        return "low"
    if per_capita < LOWER_MID_MAX:
        return "lower_mid"
    if per_capita < UPPER_MID_MAX:
        return "upper_mid"
    return "high"


def proxy_subnational(band: str, federal: bool) -> float:
    table = PROXY_SUBNATIONAL_FEDERAL if federal else PROXY_SUBNATIONAL_UNITARY
    return table[band]


def proxy_transfers(band: str, federal: bool) -> float:
    table = PROXY_TRANSFERS_FEDERAL if federal else PROXY_TRANSFERS_UNITARY
    return table[band]


# --------------------------------------------------------------------------------------
# URLs de referência (todas verificadas: respondem 200 com o dado do país/ano).
# --------------------------------------------------------------------------------------
SNG_REV_URL = ("https://sdmx.oecd.org/public/rest/data/OECD.CFE.RDG,DSD_DASHBOARD@REV,1.0/"
               "%s.R1..%s...PT_B1GQ?startPeriod=%d&format=csvfilewithlabels")
SNG_CAT_URL = ("https://sdmx.oecd.org/public/rest/data/OECD.CFE.RDG,DSD_DASHBOARD@REVENUE_CAT,1.0/"
               "%s.R12..%s...PT_OTR_SECTOR?startPeriod=%d&format=csvfilewithlabels")

RS_FLOWS = {
    "DF_RSOECD": "DSD_REV_COMP_OECD@DF_RSOECD,2.0",
    "DF_RSGLOBAL": "DSD_REV_COMP_GLOBAL@DF_RSGLOBAL,2.1",
    "DF_RSLAC": "DSD_REV_COMP_LAC@DF_RSLAC,2.0",
    "DF_RSASAP": "DSD_REV_COMP_ASAP@DF_RSASAP,2.1",
    "DF_RSAFRICA": "DSD_REV_COMP_AFRICA@DF_RSAFRICA,2.0",
}
RS_URL = ("https://sdmx.oecd.org/public/rest/data/OECD.CTP.TPS,%s/"
          "%s.TAX_REV.S13.%s..PT_B1GQ.A?startPeriod=%d&format=csvfilewithlabels")

SECTOR_LABEL = {
    "S13M": "estado + municipal",
    "S1313": "municipal",
}

PROXY_SUBNATIONAL_URL = "https://www.oecd.org/en/topics/subnational-finance.html"
PROXY_PROPERTY_URL = "https://www.oecd.org/en/data/datasets/revenue-statistics.html"
PROXY_TRANSFERS_URL = "https://www.oecd.org/en/topics/subnational-finance.html"


def subnational_ref(code: str, name: str, year: int, sector: str) -> dict:
    area = AREA_ALIAS.get(code, code)
    return {
        "title": ("OECD SNG dashboard — receita do governo subnacional (%s) em %% do PIB — %s (%d)"
                  % (SECTOR_LABEL.get(sector, sector), name, year)),
        "url": SNG_REV_URL % (area, sector, year),
        "year": year,
    }


def transfers_ref(code: str, name: str, year: int, sector: str) -> dict:
    area = AREA_ALIAS.get(code, code)
    return {
        "title": ("OECD SNG dashboard — 'Grants and subsidies' em %% da receita subnacional (%s) — %s (%d)"
                  % (SECTOR_LABEL.get(sector, sector), name, year)),
        "url": SNG_CAT_URL % (area, sector, year),
        "year": year,
    }


def property_refs(code: str, name: str, year: int, flow: str) -> list:
    area = AREA_ALIAS.get(code, code)
    dataflow = RS_FLOWS[flow]
    series_label = {
        "DF_RSOECD": "OECD Revenue Statistics",
        "DF_RSGLOBAL": "Global Revenue Statistics",
        "DF_RSLAC": "Revenue Statistics in Latin America and the Caribbean",
        "DF_RSASAP": "Revenue Statistics in Asia and the Pacific",
        "DF_RSAFRICA": "Revenue Statistics in Africa",
    }[flow]
    return [
        {
            "title": ("%s — impostos recorrentes sobre imóveis (4100) em %% do PIB — %s (%d)"
                      % (series_label, name, year)),
            "url": RS_URL % (dataflow, area, "T_4100", year),
            "year": year,
        },
        {
            "title": ("%s — total de tributos (_T) em %% do PIB, denominador do peso do imóvel — %s (%d)"
                      % (series_label, name, year)),
            "url": RS_URL % (dataflow, area, "_T", year),
            "year": year,
        },
    ]


def proxy_refs(name: str, band: str, federal: bool, fields: list) -> list:
    refs = []
    status = "federação" if federal else "unitário"
    for field in fields:
        if field == "subnational":
            refs.append({
                "title": ("estimativa (não é medição): receita subnacional %% do PIB — faixa %s, %s — %s"
                          % (BAND_LABEL[band], status, name)),
                "url": PROXY_SUBNATIONAL_URL,
                "year": PROXY_REF_YEAR,
            })
        elif field == "property":
            refs.append({
                "title": ("estimativa (não é medição): imposto sobre imóveis %% da tributação — faixa %s — %s"
                          % (BAND_LABEL[band], name)),
                "url": PROXY_PROPERTY_URL,
                "year": PROXY_REF_YEAR,
            })
        else:
            refs.append({
                "title": ("estimativa (não é medição): transferências %% da receita subnacional — faixa %s, %s — %s"
                          % (BAND_LABEL[band], status, name)),
                "url": PROXY_TRANSFERS_URL,
                "year": PROXY_REF_YEAR,
            })
    return refs


def build_nation(nation: dict) -> dict:
    code = nation["id"]
    name = nation["name"]
    band = income_band(nation)
    federal = code in FEDERATIONS

    sub_value: Optional[float] = None
    sub_source = "no_data"
    sub_year: Optional[int] = None
    property_value: Optional[float] = None
    property_source = "no_data"
    property_year: Optional[int] = None
    transfers_value: Optional[float] = None
    transfers_source = "no_data"
    transfers_year: Optional[int] = None

    refs: list = []
    proxied: list = []

    observed = OBSERVED_SUBNATIONAL.get(code)
    if observed:
        sub_value, sub_year, sector = observed
        sub_source = "oecd_subnational"
        refs.append(subnational_ref(code, name, sub_year, sector))
    elif band:
        sub_value = proxy_subnational(band, federal)
        sub_source = "proxy"
        proxied.append("subnational")

    observed = OBSERVED_PROPERTY_TAX.get(code)
    if observed:
        property_value, property_year, flow = observed
        property_source = "oecd_revenue_statistics"
        refs.extend(property_refs(code, name, property_year, flow))
    elif band:
        property_value = PROXY_PROPERTY_TAX[band]
        property_source = "proxy"
        proxied.append("property")

    observed = OBSERVED_TRANSFERS.get(code)
    if observed:
        transfers_value, transfers_year, sector = observed
        transfers_source = "oecd_subnational"
        refs.append(transfers_ref(code, name, transfers_year, sector))
    elif band:
        transfers_value = proxy_transfers(band, federal)
        transfers_source = "proxy"
        proxied.append("transfers")

    if band and proxied:
        refs.extend(proxy_refs(name, band, federal, proxied))

    has_value = None not in (sub_value, property_value, transfers_value)

    field_pairs = (
        (sub_source, sub_value),
        (property_source, property_value),
        (transfers_source, transfers_value),
    )
    observed_count = sum(1 for source, value in field_pairs
                         if value is not None and source != "proxy")
    proxy_count = sum(1 for source, value in field_pairs
                      if value is not None and source == "proxy")
    # `estimated` na entrada só quando NADA é observado: os três campos são proxy.
    estimated = proxy_count == 3 and observed_count == 0
    if observed_count == 0 and proxy_count == 0:
        confidence = "no_data"
    elif observed_count == 3:
        confidence = "complete"
    elif proxy_count == 3:
        confidence = "estimated"
    else:
        confidence = "partial"

    entry = {
        "name": name,
        "subnational_revenue_pct_gdp": round(sub_value, 2) if sub_value is not None else None,
        "subnational_source": sub_source,
        "subnational_estimated": sub_source == "proxy",
        "subnational_year": sub_year,
        "property_tax_pct_total_tax": round(property_value, 2) if property_value is not None else None,
        "property_tax_source": property_source,
        "property_tax_estimated": property_source == "proxy",
        "property_tax_year": property_year,
        "central_transfers_pct_subnational": round(transfers_value, 2) if transfers_value is not None else None,
        "transfers_source": transfers_source,
        "transfers_estimated": transfers_source == "proxy",
        "transfers_year": transfers_year,
        "estimated": estimated,
        "confidence": confidence,
        "refs": refs,
    }
    if not has_value:
        entry["reason"] = (
            "gdp_ppp_bn ausente no nations.csv: faixa de renda indeterminada, então nenhum proxy "
            "é aplicável e não há observação OECD/IMF para esta nação"
        )
    return entry


FIELD_KEYS = {
    "subnational_revenue_pct_gdp": ("subnational_source", "subnational_estimated"),
    "property_tax_pct_total_tax": ("property_tax_source", "property_tax_estimated"),
    "central_transfers_pct_subnational": ("transfers_source", "transfers_estimated"),
}


def coverage_of(entries: Dict[str, dict]) -> dict:
    """Cobertura honesta: conta observado/proxy/null POR CAMPO, não "nações observadas"."""
    fields = {}
    for value_key, (source_key, flag_key) in FIELD_KEYS.items():
        observed = proxy = null = 0
        for entry in entries.values():
            if entry[value_key] is None:
                null += 1
            elif entry[flag_key]:
                proxy += 1
            else:
                observed += 1
        fields[value_key] = {"observed": observed, "proxy": proxy, "null": null}

    by_confidence = {"complete": 0, "partial": 0, "estimated": 0, "no_data": 0}
    for entry in entries.values():
        by_confidence[entry["confidence"]] = by_confidence.get(entry["confidence"], 0) + 1

    return {
        "nations_total": len(entries),
        "nations_by_confidence": by_confidence,
        "nations_fully_observed": by_confidence["complete"],
        "fields": fields,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Gera municipal_finance.json a partir das regras observado+proxy.")
    parser.add_argument("--check", action="store_true", help="não escreve o JSON")
    args = parser.parse_args(argv)

    with open(NATIONS_CSV, "r", encoding="utf-8", newline="") as handle:
        nations = list(csv.DictReader(handle))

    entries: Dict[str, dict] = {}
    for nation in nations:
        entries[nation["id"]] = build_nation(nation)

    coverage = coverage_of(entries)
    payload = {
        "version": 1,
        "note": NOTE,
        "coverage": coverage,
        "sources": SOURCES,
        "method": METHOD,
        "nations": entries,
    }

    observed_any = sum(1 for entry in entries.values()
                       if entry["confidence"] in ("complete", "partial"))
    fully_observed = coverage["nations_fully_observed"]
    partial = coverage["nations_by_confidence"]["partial"]
    estimated_all = coverage["nations_by_confidence"]["estimated"]
    no_data = coverage["nations_by_confidence"]["no_data"]

    print("nações: %d" % coverage["nations_total"])
    print("por campo (observado / proxy / null):")
    for value_key in FIELD_KEYS:
        counts = coverage["fields"][value_key]
        print("  %-36s %3d / %3d / %3d"
              % (value_key, counts["observed"], counts["proxy"], counts["null"]))
    print("  nações com ao menos um campo observado: %d (100%% observadas: %d | mistas: %d)"
          % (observed_any, fully_observed, partial))
    print("  totalmente proxy: %d | sem dado: %d" % (estimated_all, no_data))
    if no_data:
        print("  sem dado (gdp_ppp_bn ausente): %s"
              % ", ".join(sorted(code for code, entry in entries.items() if entry["confidence"] == "no_data")))

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
