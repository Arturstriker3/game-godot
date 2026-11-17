#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gera data/definitions/economy/urban_land.json: uma entrada por cidade (834).

O que é observado e o que é derivado (sem inventar número apresentado como medição):

  * built_up_km2 OBSERVADO: 117 das 834 cidades do jogo têm área construída publicada
    no Atlas of Urban Expansion 2016 (Tabela 1 — Areas and Densities), coluna
    "Built-up Area Total (ha)" da época T3 de cada cidade (2009..2015). O dicionário
    OBSERVED_AREAS abaixo traz a linha exata do Atlas (nome | país), o ano e o valor em
    hectares; a URL da tabela e a página da cidade vão em refs de cada entrada.
  * built_up_km2 DERIVADO: as demais cidades usam população / density_proxy, com
    density_proxy fixo por faixa de renda da nação (PIB PPP per capita do nations.csv).
    Isso NÃO é medição: entra como built_up_source="derived_density_proxy",
    estimated=true, built_up_year=null, confidence="estimated".
  * DENSIDADE OBSERVADA: vem do PRÓPRIO Atlas — coluna "Built-up Area Density
    (persons/ha)" da Tabela 1, época T3 (mesmo ano e mesmo perímetro da área construída),
    convertida para hab/km2 (persons/ha * 100) e gravada com
    density_source="atlas_density". NÃO se usa população de aglomeração 2026 sobre a área
    do Atlas: os perímetros são diferentes e a densidade sairia inflada.
  * DENSIDADE DERIVADA: para as cidades sem Atlas a densidade é o próprio
    DENSITY_PROXY da faixa de renda (a área construída é population / proxy), gravada com
    density_source="derived_density_proxy" e estimated=true.
  * POPULAÇÃO: population continua sendo o valor de cities.csv (população de cenário
    rateada entre as cidades da nação). A aglomeração real vem de
    tools/agglomerations_2026.csv (citypopulation.de, 2026-01-01) e entra em
    agglomeration_population apenas como REFERÊNCIA de população: ela alimenta a
    população usada na área DERIVADA, mas nunca a densidade das cidades observadas.
  * expandable_reserve_km2: SEMPRE derivado (relevo não é modelado no projeto).
  * zoning_split: proporção típica por faixa de renda (proxy, não observação).
  * height_class: limiar de população (regra do projeto).

Uso:
    python tools/build_urban_land.py            # escreve o JSON
    python tools/build_urban_land.py --check    # não escreve, só reporta a cobertura
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import sys
import unicodedata
import urllib.error
import urllib.request
from typing import Dict, List, Optional, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")
CITIES_CSV = os.path.join(SCENARIO, "cities.csv")
NATIONS_CSV = os.path.join(SCENARIO, "nations.csv")
PORTS_CSV = os.path.join(SCENARIO, "ports.csv")
AGGLOMERATIONS_CSV = os.path.join(ROOT, "tools", "agglomerations_2026.csv")
OUT_JSON = os.path.join(ROOT, "data", "definitions", "economy", "urban_land.json")
CACHE = os.path.join(ROOT, "tools", "data", "urban_land_cache")

# ---------------------------------------------------------------------------
# Fontes
# ---------------------------------------------------------------------------

ATLAS_TABLE_URL = (
    "http://atlasofurbanexpansion.org/file-manager/userfiles/data_page/"
    "Areas_and_Densities_Tables/Areas_and_Densities_Table_1.csv"
)
ATLAS_CITY_URL = "http://www.atlasofurbanexpansion.org/cities/view/%s"
GHSL_UCDB_URL = "https://ghsl.jrc.ec.europa.eu/ghs_stat_ucdb2015mt_r2019a.php"
DEMOGRAPHIA_URL = "http://www.demographia.com/db-worldua.pdf"
AGGLOMERATION_URL = "https://www.citypopulation.de/en/world/agglomerations/"
AGGLOMERATION_YEAR = 2026

SOURCES = {
    "atlas_urban_expansion": {
        "title": (
            "Atlas of Urban Expansion 2016 — Areas and Densities, Table 1 "
            "(Built-up Area Total e Built-up Area Density, época T3 de cada cidade)"
        ),
        "url": ATLAS_TABLE_URL,
        "unit": (
            "hectares na fonte, convertido para km2 (ha / 100); densidade em "
            "persons/ha, convertida para hab/km2 (* 100)"
        ),
    },
    "atlas_urban_expansion_city_page": {
        "title": "Atlas of Urban Expansion 2016 — página da cidade (perfil do centro urbano)",
        "url": "http://www.atlasofurbanexpansion.org/cities",
        "unit": "hectares (km2 no JSON)",
    },
    "ghsl_ucdb": {
        "title": (
            "GHSL Urban Centre Database R2019A (JRC) — referência das faixas de densidade "
            "urbana usadas no proxy por faixa de renda (não fornece a área de cada cidade "
            "nesta versão: o download do JRC não estava acessível no ambiente de geração)"
        ),
        "url": GHSL_UCDB_URL,
        "unit": "hab/km2 (faixas)",
    },
    "demographia_world_urban_areas": {
        "title": "Demographia World Urban Areas — referência de faixas de densidade urbana (proxy)",
        "url": DEMOGRAPHIA_URL,
        "unit": "hab/km2 (faixas)",
    },
    "citypopulation_de_2026": {
        "title": (
            "citypopulation.de — Major Agglomerations of the World (Thomas Brinkhoff, "
            "referência 2026-01-01), tabela tools/agglomerations_2026.csv"
        ),
        "url": AGGLOMERATION_URL,
        "unit": "habitantes da aglomeração (população real, não rateada)",
    },
}

NOTE_TEMPLATE = (
    "Área urbana construída e densidade por cidade. {observed} das {total} cidades têm "
    "área OBSERVADA no Atlas of Urban Expansion 2016 (Tabela 1, 'Built-up Area Total' e "
    "'Built-up Area Density' da época T3, hectares convertidos para km2; anos 2009-2015) — "
    "cada uma com a linha do "
    "Atlas, a URL da tabela e a página da cidade em refs. As outras {derived} cidades têm "
    "área DERIVADA de população / density_proxy, com density_proxy fixo por faixa de renda "
    "da nação (PIB PPP per capita): essas entradas são explicitamente estimated=true e "
    "confidence='estimated', nunca medição. Nove nações não têm gdp_ppp_bn em "
    "nations.csv (CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN, YEM): suas 26 cidades usam a "
    "faixa intermediária lower_mid como fallback declarado. "
    "DENSIDADE: nas {observed} cidades OBSERVADAS usa-se a densidade do próprio Atlas — "
    "coluna 'Built-up Area Density (persons/ha)' da Tabela 1 na época T3 (mesmo ano e mesmo "
    "perímetro da área construída), x100 = hab/km2, density_source='atlas_density' — e não "
    "a população de aglomeração 2026, cujo perímetro é maior e inflaria a densidade. Nas "
    "{derived} cidades DERIVADAS a densidade é o próprio DENSITY_PROXY da faixa de renda "
    "(a área construída é população / proxy), density_source='derived_density_proxy', "
    "estimativa e não medição. O campo agglomeration_population (população real da "
    "aglomeração, citypopulation.de referência 2026-01-01, {agg} cidades) fica apenas como "
    "REFERÊNCIA de população: ele alimenta a população usada na área DERIVADA, mas não a "
    "densidade das observadas. As {rateada} cidades sem aglomeração na fonte usam o campo "
    "population de cities.csv — que é a população de CENÁRIO rateada entre as cidades da "
    "nação (a soma das cidades de cada nação é exatamente a população da nação em "
    "nations.csv). O campo population continua sendo sempre o valor de cities.csv. "
    "Casamento cidade->aglomeração: nome "
    "normalizado (sem acentos, sem pontuação e sem o sufixo 'City' quando necessário) mais "
    "código ISO3 da nação, com alias NED/NLD, PSX/PSE, SOL/SOM (Somalilândia, nunca SLB), "
    "SAH/MAR, CYN/CYP e TMP/TLS; empate resolvido pelo menor rank. A tabela só lista "
    "aglomerações de 1 milhão de habitantes ou mais, o que explica as {rateada} cidades "
    "sem correspondência. "
    "expandable_reserve_km2 é sempre derivada (0,6 x built_up, x0,55 se costeira, x0,40 "
    "se ilha, teto de 3x a área construída): relevo não é modelado no projeto (height.png "
    "é um placeholder plano), então a reserva usa apenas costa e landmass. zoning_split é "
    "proporção típica por faixa de renda (proxy inspirado no Atlas/OECD), não observação "
    "por cidade. height_class é regra de limiar populacional. coastal = a cidade aparece "
    "como port_of em ports.csv (aproximação documentada: cidade com porto é costeira). "
    "Nenhum valor observado foi estimado ou arredondado para caber em faixa. Cidades "
    "grandes que NÃO estão na amostra de 200 centros do Atlas (ex.: Bengaluru, Jakarta, "
    "Chongqing, Shenzhen, Chennai, Surat) entram como derivadas (com a população real da "
    "aglomeração quando ela existe): não há valor publicado "
    "do Atlas para elas nesta coleta, e nenhuma outra fonte de área construída estava "
    "acessível no ambiente de geração (o download do GHSL UCDB R2019A falhou)."
)

METHOD = {
    "built_up_area": (
        "Observada (ver coverage.cities_with_observed_area): Atlas of Urban Expansion "
        "2016, Tabela 1, coluna 'Built-up Area Total (ha)' da época T3 (2009..2015) / "
        "100 = km2. Derivada (ver coverage.cities_with_derived_area): população / "
        "density_proxy, com density_proxy por faixa "
        "de renda da nação (PIB PPP per capita = gdp_ppp_bn*1e9 / population): "
        "low <5000 USD = 15000 hab/km2; lower_mid 5000-12000 = 11000; "
        "upper_mid 12000-25000 = 8500; high >25000 = 6000. Faixas de referência "
        "inspiradas em GHSL UCDB / Demographia — é proxy, não medição. Nove nações do "
        "nations.csv não têm gdp_ppp_bn (CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN, YEM); "
        "para elas não há faixa calculável e aplica-se a faixa intermediária lower_mid "
        "como fallback declarado (valor central do método, não estimativa de renda). "
        "A população usada é a da aglomeração quando ela existe (agglomeration_population); "
        "senão o campo population de cities.csv (rateado)."
    ),
    "density": (
        "Duas origens distintas, sem misturar perímetros. (1) Cidades OBSERVADAS "
        "(built_up_source='atlas_urban_expansion'): density_hab_km2 = round(persons_per_ha "
        "* 100, 0), com persons_per_ha lido da coluna 'Built-up Area Density (persons/ha)' "
        "da Tabela 1 do Atlas of Urban Expansion 2016 na MESMA época T3 da área construída "
        "— mesmo ano e mesmo perímetro, portanto a densidade é a publicada pelo Atlas "
        "(density_source='atlas_density'). Ex.: Mumbai 369 persons/ha = 36.900 hab/km2. "
        "(2) Cidades DERIVADAS (built_up_source='derived_density_proxy'): como "
        "built_up_km2 = população / density_proxy, a densidade é o próprio DENSITY_PROXY da "
        "faixa de renda (low 15000, lower_mid 11000, upper_mid 8500, high 6000 hab/km2), "
        "density_source='derived_density_proxy', estimativa e não medição. "
        "aglomeration_population (população real da aglomeração 2026, citypopulation.de) "
        "NÃO é usada para densidade: fica só como referência de população (e como "
        "população usada na área das cidades derivadas). Motivo: a aglomeração 2026 cobre "
        "um perímetro maior que o 'urban extent' do Atlas 2014, e dividi-la pela área do "
        "Atlas inflava a densidade (Mumbai saía 52.879 em vez dos 36.900 do Atlas; Pokhara "
        "452.100 em vez de 16.700). O campo population continua sendo sempre o valor de "
        "cities.csv. Nenhuma densidade acima de 60.000 hab/km2 é aceita: o máximo do Atlas "
        "na amostra é Dhaka com 552 persons/ha = 55.200 hab/km2 e o proxy chega a 15.000, "
        "então o gerador e o validador tratam >60.000 como ERRO."
    ),
    "expandable_reserve": (
        "Sempre derivada: reserve = built_up_km2 * factor, factor = 0,60 (base); "
        "*0,55 se coastal; *0,40 se a landmass é ilha/micro (landmass fora de "
        "continent/asia/africa/europe/south_america/north_america/oceania); teto de "
        "3,0 * built_up_km2. RELEVO NÃO É MODELADO (height.png é placeholder plano), "
        "por isso a reserva considera só costa e landmass."
    ),
    "zoning_split": (
        "Proporção típica por faixa de renda (proxy, zoning_source='income_group_proxy'): "
        "low 0,68/0,17/0,15; lower_mid 0,63/0,22/0,15; upper_mid 0,58/0,27/0,15; "
        "high 0,55/0,32/0,13 (residencial/comercial/industrial). Referência de ordem de "
        "grandeza: Atlas of Urban Expansion (Angel et al.) e literatura OECD de uso do "
        "solo; não é observação por cidade."
    ),
    "height_class": (
        "Regra de limiar populacional (height_class_source='population_threshold'): "
        "population >= 5.000.000 => high; population >= 1.000.000 ou capital => mid; "
        "senão low. Nenhuma exceção 'observada' foi aplicada nesta versão."
    ),
    "coastal": (
        "true se o ID da cidade aparece na coluna port_of de ports.csv (cidade com porto "
        "é tratada como costeira); senão false."
    ),
}

# ---------------------------------------------------------------------------
# Faixas de renda (PIB PPP per capita, USD)
# ---------------------------------------------------------------------------

INCOME_BANDS = (
    (5000.0, "low"),
    (12000.0, "lower_mid"),
    (25000.0, "upper_mid"),
    (float("inf"), "high"),
)

DENSITY_PROXY = {
    "low": 15000.0,
    "lower_mid": 11000.0,
    "upper_mid": 8500.0,
    "high": 6000.0,
}

# 9 nações do nations.csv não têm gdp_ppp_bn (CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN, YEM).
# Sem PIB per capita não há faixa de renda calculável: aplica-se a faixa INTERMEDIÁRIA como
# fallback declarado (não é estimativa de renda, é o valor central das quatro faixas do método).
FALLBACK_BAND = "lower_mid"

ZONING_PROXY = {
    "low": {"residential": 0.68, "commercial": 0.17, "industrial": 0.15},
    "lower_mid": {"residential": 0.63, "commercial": 0.22, "industrial": 0.15},
    "upper_mid": {"residential": 0.58, "commercial": 0.27, "industrial": 0.15},
    "high": {"residential": 0.55, "commercial": 0.32, "industrial": 0.13},
}

# landmass que NÃO é ilha/micro (massas continentais e subcontinentes do mapa)
MAINLAND_LANDMASSES = {
    "continent", "asia", "africa", "europe",
    "south_america", "north_america", "oceania",
}

# Reserva expansível: base e penalidades (ver METHOD["expandable_reserve"])
RESERVE_BASE_FACTOR = 0.60
RESERVE_COASTAL_FACTOR = 0.55
RESERVE_ISLAND_FACTOR = 0.40
RESERVE_CAP_MULTIPLE = 3.0

# Teto de plausibilidade: é ERRO (não aviso). O maior valor da coluna de densidade do Atlas
# na amostra é Dhaka, 552 persons/ha = 55.200 hab/km2; o proxy de renda chega a 15.000.
# Acima de 60.000 a densidade é artefato de recorte/conversão, nunca medição.
MAX_OBSERVED_DENSITY = 60000.0

# Cache da Tabela 1 baixada da fonte viva: city|country|t3date|builtup_ha_t3|density_per_ha_t3
# Sem a coluna de densidade o cache antigo era inútil para a densidade (população
# inconsistente) — este arquivo é regravado a cada download bem-sucedido.
ATLAS_TABLE_CACHE = os.path.join(CACHE, "atlas_aue2016_table1_density.tsv")

# ---------------------------------------------------------------------------
# Casamento cidade -> aglomeração real (tools/agglomerations_2026.csv)
# ---------------------------------------------------------------------------

# Códigos do nations.csv -> códigos ISO3 que podem aparecer na tabela de aglomerações
# (a tabela usa códigos próprios: p.ex. NED e PSX, não NLD e PSE).
ISO3_ALIASES = {
    "NED": ("NED", "NLD"),
    "PSX": ("PSX", "PSE"),
    # ATENÇÃO: nosso SOL é Somalilândia (nunca casar com "SLB", que é Ilhas Salomão);
    # a tabela publica Hargeisa/Berbera sob SOM.
    "SOL": ("SOL", "SOM"),
    "SAH": ("SAH", "MAR"),   # Saara Ocidental: El Aiún aparece sob MAR
    "CYN": ("CYN", "CYP"),
    "TMP": ("TMP", "TLS"),
}

# Exônimos: nome da cidade no jogo (cities.csv) -> nome usado pela fonte
# citypopulation.de (agglomerations_2026.csv). Usado só para o CASAMENTO de população;
# não altera o nome exibido.
CITY_EXONYMS = {
    "Ho Chi Minh City": "Saigon",
}

# palavras que não identificam país ao validar o campo `country` da tabela
COUNTRY_STOPWORDS = {
    "republic", "peoples", "people", "democratic", "federal", "kingdom", "state",
    "states", "arab", "islamic", "union", "socialist", "the", "and", "of", "saint",
    "st", "islands", "island", "territories", "territory",
}

# ---------------------------------------------------------------------------
# Núcleo OBSERVADO — Atlas of Urban Expansion 2016, Tabela 1
# (id da cidade: (linha do Atlas "nome", "país", ano T3, built-up total T3 em ha, slug))
# Cada linha abaixo corresponde exatamente ao registro da tabela
# http://atlasofurbanexpansion.org/file-manager/userfiles/data_page/Areas_and_Densities_Tables/Areas_and_Densities_Table_1.csv
# ---------------------------------------------------------------------------

OBSERVED_AREAS: Dict[str, Tuple[str, str, int, float, str]] = {
    "AFG-KAB": ("Kabul", "Afghanistan", 2014, 22872, "Kabul"),  # row Kabul|Afghanistan: Built-up Area Total T3 = 22,872 ha (2014)
    "AGO-LUA": ("Luanda", "Angola", 2014, 36891, "Luanda"),  # row Luanda|Angola: 36,891 ha (2014)
    "ARG-BUE": ("Buenos Aires", "Argentina", 2014, 147306, "Buenos_Aires"),  # row Buenos Aires|Argentina: 147,306 ha (2014)
    "ARG-COR": ("Cordoba", "Argentina", 2014, 24542, "Cordoba"),  # row Cordoba|Argentina: 24,542 ha (2014)
    "AUS-SYD": ("Sydney", "Australia", 2014, 110033, "Sydney"),  # row Sydney|Australia: 110,033 ha (2014)
    "AUT-VIE": ("Vienna", "Austria", 2013, 36563, "Vienna"),  # row Vienna|Austria: 36,563 ha (2013)
    "AZE-BAK": ("Baku", "Azerbaijan", 2014, 18670, "Baku"),  # row Baku|Azerbaijan: 18,670 ha (2014)
    "BEL-ANR": ("Antwerp", "Belgium", 2013, 43115, "Antwerp"),  # row Antwerp|Belgium: 43,115 ha (2013)
    "BGD-DHA": ("Dhaka", "Bangladesh", 2014, 24676, "Dhaka"),  # row Dhaka|Bangladesh: 24,676 ha (2014)
    "BLR-GOM": ("Gomel", "Belarus", 2013, 9573, "Gomel"),  # row Gomel|Belarus: 9,573 ha (2013) — a cidade do jogo é Homyel'
    "BOL-COC": ("Cochabamba", "Bolivia", 2013, 16736, "Cochabamba"),  # row Cochabamba|Bolivia: 16,736 ha (2013)
    "BRA-BEL": ("Belo Horizonte", "Brazil", 2013, 48701, "Belo_Horizonte"),  # row Belo Horizonte|Brazil: 48,701 ha (2013)
    "BRA-CUR": ("Curitiba", "Brazil", 2014, 44527, "Curitiba"),  # row Curitiba|Brazil: 44,527 ha (2014)
    "BRA-PAL": ("Palmas", "Brazil", 2013, 4228, "Palmas"),  # row Palmas|Brazil: 4,228 ha (2013)
    "BRA-SAO": ("Sao Paulo", "Brazil", 2014, 172428, "Sao_Paulo"),  # row Sao Paulo|Brazil: 172,428 ha (2014)
    "CAN-MON": ("Montreal", "Canada", 2013, 89185, "Montreal"),  # row Montreal|Canada: 89,185 ha (2013)
    "CAN-VIC": ("Victoria", "Canada", 2013, 13351, "Victoria"),  # row Victoria|Canada: 13,351 ha (2013)
    "CHL-SAN": ("Santiago", "Chile", 2014, 60381, "Santiago"),  # row Santiago|Chile: 60,381 ha (2014)
    "CHN-BEI": ("Beijing, Beijing", "China", 2013, 265434, "Beijing_Beijing"),  # row Beijing, Beijing|China: 265,434 ha (2013)
    "CHN-CHE": ("Chengdu, Sichuan", "China", 2009, 114992, "Chengdu_Sichuan"),  # row Chengdu, Sichuan|China: 114,992 ha (2009)
    "CHN-GUA": ("Guangzhou, Guangdong", "China", 2014, 323148, "Guangzhou_Guangdong"),  # row Guangzhou, Guangdong|China: 323,148 ha (2014)
    "CHN-HAN": ("Hangzhou, Zhejiang", "China", 2013, 196261, "Hangzhou_Zhejiang"),  # row Hangzhou, Zhejiang|China: 196,261 ha (2013)
    "CHN-JIN": ("Jinan, Shandong", "China", 2013, 45089, "Jinan_Shandong"),  # row Jinan, Shandong|China: 45,089 ha (2013)
    "CHN-SHA": ("Shanghai, Shanghai", "China", 2015, 320046, "Shanghai_Shanghai"),  # row Shanghai, Shanghai|China: 320,046 ha (2015)
    "CHN-TIA": ("Tianjin,  Tianjin", "China", 2013, 152442, "Tianjin_Tianjin"),  # row Tianjin, Tianjin|China: 152,442 ha (2013)
    "CHN-WUH": ("Wuhan, Hubei", "China", 2013, 102833, "Wuhan_Hubei"),  # row Wuhan, Hubei|China: 102,833 ha (2013)
    "COD-KIN": ("Kinshasa", "Congo Dem. Rep.", 2013, 33763, "Kinshasa"),  # row Kinshasa|Congo Dem. Rep.: 33,763 ha (2013)
    "COD-LUB": ("Lubumbashi", "Congo Dem. Rep.", 2013, 15865, "Lubumbashi"),  # row Lubumbashi|Congo Dem. Rep.: 15,865 ha (2013)
    "COL-BOG": ("Bogota", "Colombia", 2010, 31895, "Bogota"),  # row Bogota|Colombia: 31,895 ha (2010)
    "DEU-BER": ("Berlin", "Germany", 2013, 68743, "Berlin"),  # row Berlin|Germany: 68,743 ha (2013)
    "DZA-ALG": ("Algiers", "Algeria", 2014, 27798, "Algiers"),  # row Algiers|Algeria: 27,798 ha (2014)
    "ECU-QUI": ("Quito", "Ecuador", 2013, 22665, "Quito"),  # row Quito|Ecuador: 22,665 ha (2013)
    "EGY-ALE": ("Alexandria", "Egypt", 2013, 22782, "Alexandria"),  # row Alexandria|Egypt: 22,782 ha (2013)
    "EGY-CAI": ("Cairo", "Egypt", 2013, 93093, "Cairo"),  # row Cairo|Egypt: 93,093 ha (2013)
    "ESP-MAD": ("Madrid", "Spain", 2010, 56019, "Madrid"),  # row Madrid|Spain: 56,019 ha (2010)
    "ETH-ADD": ("Addis Ababa", "Ethiopia", 2010, 21133, "Addis_Ababa"),  # row Addis Ababa|Ethiopia: 21,133 ha (2010)
    "FJI-SUV": ("Suva", "Fiji", 2014, 2846, "Suva"),  # row Suva|Fiji: 2,846 ha (2014)
    "FRA-PAR": ("Paris", "France", 2014, 198626, "Paris"),  # row Paris|France: 198,626 ha (2014)
    "GBR-LDN": ("London", "United Kingdom", 2013, 177273, "London"),  # row London|United Kingdom: 177,273 ha (2013)
    "GBR-MAN": ("Manchester", "United Kingdom", 2010, 51040, "Manchester"),  # row Manchester|United Kingdom: 51,040 ha (2010)
    "GBR-SHE": ("Sheffield", "United Kingdom", 2013, 27394, "Sheffield"),  # row Sheffield|United Kingdom: 27,394 ha (2013)
    "GHA-ACC": ("Accra", "Ghana", 2014, 61781, "Accra"),  # row Accra|Ghana: 61,781 ha (2014)
    "GRC-THE": ("Thessaloniki", "Greece", 2011, 10568, "Thessaloniki"),  # row Thessaloniki|Greece: 10,568 ha (2011)
    "GTM-GUA": ("Guatemala City", "Guatemala", 2013, 26506, "Guatemala_City"),  # row Guatemala City|Guatemala: 26,506 ha (2013)
    "HKG-HON": ("Hong Kong, Hong Kong", "China", 2013, 9254, "Hong_Kong_Hong_Kong"),  # row Hong Kong, Hong Kong|China: 9,254 ha (2013)
    "HUN-BUD": ("Budapest", "Hungary", 2013, 52029, "Budapest"),  # row Budapest|Hungary: 52,029 ha (2013)
    "IDN-MED": ("Medan", "Indonesia", 2013, 39096, "Medan"),  # row Medan|Indonesia: 39,096 ha (2013)
    "IDN-PAL": ("Palembang", "Indonesia", 2013, 20960, "Palembang"),  # row Palembang|Indonesia: 20,960 ha (2013)
    "IND-AHM": ("Ahmedabad", "India", 2013, 21103, "Ahmedabad"),  # row Ahmedabad|India: 21,103 ha (2013)
    "IND-COI": ("Coimbatore", "India", 2014, 12191, "Coimbatore"),  # row Coimbatore|India: 12,191 ha (2014)
    "IND-HYD": ("Hyderabad", "India", 2014, 46168, "Hyderabad"),  # row Hyderabad|India: 46,168 ha (2014)
    "IND-JAI": ("Jaipur", "India", 2014, 19218, "Jaipur"),  # row Jaipur|India: 19,218 ha (2014)
    "IND-KAN": ("Kanpur", "India", 2014, 14405, "Kanpur"),  # row Kanpur|India: 14,405 ha (2014)
    "IND-KOL": ("Kolkata", "India", 2014, 60278, "Kolkata"),  # row Kolkata|India: 60,278 ha (2014)
    "IND-MUM": ("Mumbai", "India", 2014, 53135, "Mumbai"),  # row Mumbai|India: 53,135 ha (2014)
    "IND-PUN": ("Pune", "India", 2011, 32338, "Pune"),  # row Pune|India: 32,338 ha (2011)
    "IRN-AHV": ("Ahvaz", "Iran", 2013, 12873, "Ahvaz"),  # row Ahvaz|Iran: 12,873 ha (2013)
    "IRN-QOM": ("Qom", "Iran", 2010, 8404, "Qom"),  # row Qom|Iran: 8,404 ha (2010)
    "IRN-TEH": ("Tehran", "Iran", 2010, 61367, "Tehran"),  # row Tehran|Iran: 61,367 ha (2010)
    "IRQ-BAG": ("Baghdad", "Iraq", 2013, 56288, "Baghdad"),  # row Baghdad|Iraq: 56,288 ha (2013)
    "ISR-TLV": ("Tel Aviv", "Israel", 2014, 33397, "Tel_Aviv"),  # row Tel Aviv|Israel: 33,397 ha (2014)
    "ITA-MIL": ("Milan", "Italy", 2013, 178364, "Milan"),  # row Milan|Italy: 178,364 ha (2013)
    "ITA-PAL": ("Palermo", "Italy", 2013, 13249, "Palermo"),  # row Palermo|Italy: 13,249 ha (2013)
    "JPN-FUK": ("Fukuoka", "Japan", 2014, 34507, "Fukuoka"),  # row Fukuoka|Japan: 34,507 ha (2014)
    "JPN-OSA": ("Osaka", "Japan", 2014, 95077, "Osaka"),  # row Osaka|Japan: 95,077 ha (2014)
    "JPN-TOK": ("Tokyo", "Japan", 2014, 448929, "Tokyo"),  # row Tokyo|Japan: 448,929 ha (2014)
    "KAZ-SHY": ("Shymkent", "Kazakhstan", 2013, 19457, "Shymkent"),  # row Shymkent|Kazakhstan: 19,457 ha (2013)
    "KOR-BUS": ("Busan", "Korea Rep.", 2013, 32657, "Busan"),  # row Busan|Korea Rep.: 32,657 ha (2013)
    "KOR-GWA": ("Gwangju", "Korea Rep.", 2015, 16033, "Gwangju"),  # row Gwangju|Korea Rep.: 16,033 ha (2015)
    "KOR-SEO": ("Seoul", "Korea Rep.", 2014, 199578, "Seoul"),  # row Seoul|Korea Rep.: 199,578 ha (2014)
    "LTU-KAU": ("Kaunas", "Lithuania", 2014, 8090, "Kaunas"),  # row Kaunas|Lithuania: 8,090 ha (2014)
    "MAR-RAK": ("Marrakesh", "Morocco", 2014, 9999, "Marrakesh"),  # row Marrakesh|Morocco: 9,999 ha (2014)
    "MEX-MEXI": ("Mexico City", "Mexico", 2014, 161821, "Mexico_City"),  # row Mexico City|Mexico: 161,821 ha (2014)
    "MEX-TIJ": ("Tijuana", "Mexico", 2014, 28626, "Tijuana"),  # row Tijuana|Mexico: 28,626 ha (2014)
    "MLI-BAM": ("Bamako", "Mali", 2013, 17644, "Bamako"),  # row Bamako|Mali: 17,644 ha (2013)
    "MMR-MYE": ("Myeik", "Myanmar", 2014, 1079, "Myeik"),  # row Myeik|Myanmar: 1,079 ha (2014)
    "MNG-ULA": ("Ulaanbaatar", "Mongolia", 2014, 15493, "Ulaanbaatar"),  # row Ulaanbaatar|Mongolia: 15,493 ha (2014) — cidade do jogo: Ulan Bator
    "MOZ-BEI": ("Beira", "Mozambique", 2013, 6208, "Beira"),  # row Beira|Mozambique: 6,208 ha (2013)
    "NGA-IBA": ("Ibadan", "Nigeria", 2013, 36811, "Ibadan"),  # row Ibadan|Nigeria: 36,811 ha (2013)
    "NGA-LAG": ("Lagos", "Nigeria", 2013, 52788, "Lagos"),  # row Lagos|Nigeria: 52,788 ha (2013)
    "NIC-LEO": ("Leon", "Nicaragua", 2010, 1544, "Leon"),  # row Leon|Nicaragua: 1,544 ha (2010)
    "NPL-POK": ("Pokhara", "Nepal", 2013, 1630, "Pokhara"),  # row Pokhara|Nepal: 1,630 ha (2013)
    "NZL-AUC": ("Auckland", "New Zealand", 2014, 34602, "Auckland"),  # row Auckland|New Zealand: 34,602 ha (2014)
    "PAK-KAR": ("Karachi", "Pakistan", 2013, 35018, "Karachi"),  # row Karachi|Pakistan: 35,018 ha (2013)
    "PAK-LAH": ("Lahore", "Pakistan", 2013, 24775, "Lahore"),  # row Lahore|Pakistan: 24,775 ha (2013)
    "PHL-CEB": ("Cebu City", "Philippines", 2014, 12462, "Cebu_City"),  # row Cebu City|Philippines: 12,462 ha (2014)
    "PHL-MAN": ("Manila", "Philippines", 2014, 70334, "Manila"),  # row Manila|Philippines: 70,334 ha (2014)
    "POL-WAR": ("Warsaw", "Poland", 2013, 47779, "Warsaw"),  # row Warsaw|Poland: 47,779 ha (2013)
    "PRK-PYO": ("Pyongyang", "Korea Dem. Rep.", 2014, 11463, "Pyongyang"),  # row Pyongyang|Korea Dem. Rep.: 11,463 ha (2014)
    "RUS-MOS": ("Moscow", "Russia", 2014, 223436, "Moscow"),  # row Moscow|Russia: 223,436 ha (2014)
    "RUS-SAI": ("Saint Petersburg", "Russia", 2014, 68500, "Saint_Petersburg"),  # row Saint Petersburg|Russia: 68,500 ha (2014)
    "RWA-KIG": ("Kigali", "Rwanda", 2014, 8272, "Kigali"),  # row Kigali|Rwanda: 8,272 ha (2014)
    "SAU-RIY": ("Riyadh", "Saudi Arabia", 2013, 95861, "Riyadh"),  # row Riyadh|Saudi Arabia: 95,861 ha (2013)
    "SDN-KHA": ("Khartoum", "Sudan", 2014, 58271, "Khartoum"),  # row Khartoum|Sudan: 58,271 ha (2014)
    "SGP-SIN": ("Singapore", "Singapore", 2013, 27392, "Singapore"),  # row Singapore|Singapore: 27,392 ha (2013)
    "SLV-SAN": ("San Salvador", "El Salvador", 2014, 16889, "San_Salvador"),  # row San Salvador|El Salvador: 16,889 ha (2014)
    "SRB-BEL": ("Belgrade", "Serbia", 2014, 12565, "Belgrade"),  # row Belgrade|Serbia: 12,565 ha (2014)
    "THA-BAN": ("Bangkok", "Thailand", 2015, 172912, "Bangkok"),  # row Bangkok|Thailand: 172,912 ha (2015)
    "TUR-IST": ("Istanbul", "Turkey", 2013, 102589, "Istanbul"),  # row Istanbul|Turkey: 102,589 ha (2013)
    "TUR-KAY": ("Kayseri", "Turkey", 2013, 10054, "Kayseri"),  # row Kayseri|Turkey: 10,054 ha (2013)
    "TWN-TAI": ("Taipei, Taiwan", "China", 2014, 70235, "Taipei_Taiwan"),  # row Taipei, Taiwan|China: 70,235 ha (2014)
    "TZA-ARU": ("Arusha", "Tanzania", 2013, 1563, "Arusha"),  # row Arusha|Tanzania: 1,563 ha (2013)
    "UGA-KAM": ("Kampala", "Uganda", 2015, 30040, "Kampala"),  # row Kampala|Uganda: 30,040 ha (2015)
    "USA-CHI": ("Chicago", "United States", 2014, 510972, "Chicago"),  # row Chicago|United States: 510,972 ha (2014)
    "USA-HOU": ("Houston", "United States", 2014, 272394, "Houston"),  # row Houston|United States: 272,394 ha (2014)
    "USA-LOS": ("Los Angeles", "United States", 2014, 459047, "Los_Angeles"),  # row Los Angeles|United States: 459,047 ha (2014)
    "USA-MIN": ("Minneapolis", "United States", 2014, 142874, "Minneapolis_St_Paul"),  # row Minneapolis|United States: 142,874 ha (2014)
    "USA-NEW": ("New York", "United States", 2011, 747852, "New_York"),  # row New York|United States: 747,852 ha (2011) — cidade do jogo: New York City
    "USA-PHI": ("Philadelphia", "United States", 2014, 298214, "Philadelphia"),  # row Philadelphia|United States: 298,214 ha (2014)
    "USA-POR": ("Portland, OR", "United States", 2014, 88455, "Portland_OR"),  # row Portland, OR|United States: 88,455 ha (2014)
    "UZB-TAS": ("Tashkent", "Uzbekistan", 2013, 63449, "Tashkent"),  # row Tashkent|Uzbekistan: 63,449 ha (2013)
    "VEN-CCS": ("Caracas", "Venezuela", 2014, 16352, "Caracas"),  # row Caracas|Venezuela: 16,352 ha (2014)
    "VNM-HOC": ("Ho Chi Minh City", "Vietnam", 2015, 63478, "Ho_Chi_Minh_City"),  # row Ho Chi Minh City|Vietnam: 63,478 ha (2015)
    "YEM-SAN": ("Sana", "Yemen", 2014, 22113, "Sana"),  # row Sana|Yemen: 22,113 ha (2014)
    "ZAF-GQE": ("Port Elizabeth", "South Africa", 2013, 18141, "Port_Elizabeth"),  # row Port Elizabeth|South Africa: 18,141 ha (2013) — cidade do jogo: Gqeberha
    "ZAF-JOH": ("Johannesburg", "South Africa", 2013, 181191, "Johannesburg"),  # row Johannesburg|South Africa: 181,191 ha (2013)
    "ZMB-NDO": ("Ndola", "Zambia", 2014, 4738, "Ndola"),  # row Ndola|Zambia: 4,738 ha (2014)
    # ATENÇÃO: a lista acima tem 117 cidades. Nenhuma outra cidade tem área observada:
    # as demais recebem derived_density_proxy (ver NOTE/METHOD).
}


# ---------------------------------------------------------------------------
# Leitura e cálculo
# ---------------------------------------------------------------------------

def read_csv(path: str) -> List[dict]:
    with open(path, "r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def income_band(gdp_ppp_per_capita: float) -> str:
    for limit, name in INCOME_BANDS:
        if gdp_ppp_per_capita < limit:
            return name
    return "high"


def nation_band(nation: dict) -> Tuple[str, bool]:
    """Faixa de renda da nação; (banda, usou_fallback) quando falta PIB PPP."""
    gdp = (nation.get("gdp_ppp_bn") or "").strip()
    population = (nation.get("population") or "").strip()
    try:
        gdp_ppp_bn = float(gdp)
        population_value = float(population)
    except ValueError:
        return FALLBACK_BAND, True
    if gdp_ppp_bn <= 0.0 or population_value <= 0.0:
        return FALLBACK_BAND, True
    return income_band(gdp_ppp_bn * 1e9 / population_value), False


def normalize_name(text: str) -> str:
    """minúsculas, sem diacríticos, pontuação/espaços colapsados."""
    decomposed = unicodedata.normalize("NFKD", text)
    stripped = "".join(char for char in decomposed if not unicodedata.combining(char))
    cleaned = re.sub(r"[^a-z0-9]+", " ", stripped.lower())
    return " ".join(cleaned.split())


def compact_name(text: str) -> str:
    """Igual a normalize_name, mas sem espaços ('Sana'a' -> 'sanaa')."""
    return re.sub(r"[^a-z0-9]+", "", normalize_name(text))


def name_variants(text: str) -> List[str]:
    """Nome normalizado e, se houver, a variante sem o sufixo 'City'."""
    key = normalize_name(text)
    variants = [key]
    if key.endswith(" city") and len(key) > 5:
        variants.append(key[:-5].strip())
    return [variant for variant in variants if variant]


def country_matches(nation_name: str, country: str) -> bool:
    """Valida o campo `country` da aglomeração contra o nome da nação (fallback)."""
    left = normalize_name(nation_name)
    right = normalize_name(country or "")
    if not left or not right:
        return False
    if left == right or right in left or left in right:
        return True
    left_tokens = {token for token in left.split()
                   if len(token) >= 4 and token not in COUNTRY_STOPWORDS}
    right_tokens = {token for token in right.split()
                    if len(token) >= 4 and token not in COUNTRY_STOPWORDS}
    return bool(left_tokens & right_tokens)


def load_agglomerations() -> Tuple[Dict[str, List[dict]], Dict[str, List[dict]]]:
    """Indexa a tabela de aglomerações por nome normalizado e por nome compacto."""
    by_name: Dict[str, List[dict]] = {}
    by_compact: Dict[str, List[dict]] = {}
    for row in read_csv(AGGLOMERATIONS_CSV):
        population = (row.get("population") or "").strip()
        if not population.isdigit():
            continue
        for key in {normalize_name(row.get("english", "")), normalize_name(row.get("name", ""))}:
            if key:
                by_name.setdefault(key, []).append(row)
        for key in {compact_name(row.get("english", "")), compact_name(row.get("name", ""))}:
            if key:
                by_compact.setdefault(key, []).append(row)
    return by_name, by_compact


def match_agglomeration(city: dict, nation: dict,
                        by_name: Dict[str, List[dict]],
                        by_compact: Dict[str, List[dict]]) -> Optional[dict]:
    """Acha a aglomeração real da cidade (ou None). Prefere o menor `rank`."""
    iso_codes = ISO3_ALIASES.get(city["nation"], (city["nation"],))
    variants = name_variants(city["name"])
    exonym = CITY_EXONYMS.get(city["name"])
    if exonym:
        variants = variants + name_variants(exonym)
    candidates: List[dict] = []
    for variant in variants:
        candidates = [row for row in by_name.get(variant, []) if row.get("iso3") in iso_codes]
        if candidates:
            break
    if not candidates:
        for variant in variants:
            candidates = [row for row in by_compact.get(compact_name(variant), [])
                          if row.get("iso3") in iso_codes]
            if candidates:
                break
    if not candidates:
        # último recurso: nome global + validação do país no campo `country`
        for variant in variants:
            candidates = [row for row in by_name.get(variant, [])
                          if country_matches(nation.get("name", ""), row.get("country", ""))]
            if candidates:
                break
    if not candidates:
        return None
    return min(candidates, key=lambda row: int(row.get("rank") or 10 ** 6))


# ---------------------------------------------------------------------------
# Tabela 1 do Atlas of Urban Expansion (fonte viva + cache)
# Colunas (índice na linha): 0 cidade, 1 país, 7 data T3, 10 população T3,
# 14 built-up area total T3 (ha), 38 built-up area density T3 (persons/ha).
# Os grupos de colunas repetem T1/T2/T3 e "annual change"; só a T3 é usada.
# ---------------------------------------------------------------------------

ATLAS_COL_CITY = 0
ATLAS_COL_COUNTRY = 1
ATLAS_COL_T3_DATE = 7
ATLAS_COL_POP_T3 = 10
ATLAS_COL_BUILTUP_T3 = 14
ATLAS_COL_DENSITY_T3 = 38
ATLAS_MIN_COLUMNS = 39
ATLAS_POP_CHECK_TOLERANCE = 5.0  # %, só para pegar mistura de coluna (T2 x T3)


def _atlas_number(text: str) -> Optional[float]:
    cleaned = (text or "").strip().replace(",", "")
    if not cleaned:
        return None
    try:
        return float(cleaned)
    except ValueError:
        return None


def fetch_atlas_table() -> List[List[str]]:
    """Baixa o CSV da Tabela 1 na fonte viva. Levanta OSError/URLError se falhar."""
    request = urllib.request.Request(
        ATLAS_TABLE_URL,
        headers={"User-Agent": "Mozilla/5.0 (opengs-game urban_land builder)"},
    )
    with urllib.request.urlopen(request, timeout=120) as response:
        raw = response.read()
    text = raw.decode("utf-8", "replace")
    return [row for row in csv.reader(io.StringIO(text)) if row]


def _atlas_records(rows: List[List[str]]) -> Dict[Tuple[str, str], dict]:
    """Indexa por (cidade, país) as colunas T3 de área construída e densidade."""
    records: Dict[Tuple[str, str], dict] = {}
    for row in rows:
        if len(row) < ATLAS_MIN_COLUMNS:
            continue
        name = row[ATLAS_COL_CITY].strip()
        country = row[ATLAS_COL_COUNTRY].strip()
        built_up_ha = _atlas_number(row[ATLAS_COL_BUILTUP_T3])
        if not name or not country or built_up_ha is None:
            continue
        records[(name, country)] = {
            "t3date": row[ATLAS_COL_T3_DATE].strip(),
            "population_t3": _atlas_number(row[ATLAS_COL_POP_T3]),
            "built_up_ha_t3": built_up_ha,
            "density_per_ha_t3": _atlas_number(row[ATLAS_COL_DENSITY_T3]),
        }
    return records


def write_atlas_cache(records: Dict[Tuple[str, str], dict]) -> None:
    os.makedirs(CACHE, exist_ok=True)
    with open(ATLAS_TABLE_CACHE, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("city\tcountry\tt3date\tbuiltup_ha_t3\tdensity_per_ha_t3\n")
        for (name, country) in sorted(records, key=lambda key: (key[1], key[0])):
            record = records[(name, country)]
            handle.write("%s\t%s\t%s\t%g\t%s\n" % (
                name, country, record["t3date"], record["built_up_ha_t3"],
                "" if record["density_per_ha_t3"] is None
                else "%g" % record["density_per_ha_t3"]))


def read_atlas_cache() -> Dict[Tuple[str, str], dict]:
    records: Dict[Tuple[str, str], dict] = {}
    with open(ATLAS_TABLE_CACHE, "r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            name = (row.get("city") or "").strip()
            country = (row.get("country") or "").strip()
            if not name or not country:
                continue
            records[(name, country)] = {
                "t3date": (row.get("t3date") or "").strip(),
                "population_t3": None,
                "built_up_ha_t3": _atlas_number(row.get("builtup_ha_t3") or ""),
                "density_per_ha_t3": _atlas_number(row.get("density_per_ha_t3") or ""),
            }
    return records


def load_atlas_table() -> Tuple[Dict[Tuple[str, str], dict], str]:
    """Tabela 1 do Atlas: sempre tenta a fonte viva; cai para o cache se a rede falhar."""
    try:
        records = _atlas_records(fetch_atlas_table())
        if not records:
            raise OSError("tabela vazia")
    except (OSError, urllib.error.URLError) as exc:
        if not os.path.exists(ATLAS_TABLE_CACHE):
            raise SystemExit(
                "não foi possível baixar a Tabela 1 do Atlas (%s) e não há cache em %s"
                % (exc, os.path.relpath(ATLAS_TABLE_CACHE, ROOT)))
        print("AVISO: fonte viva indisponível (%s); usando cache %s"
              % (exc, os.path.relpath(ATLAS_TABLE_CACHE, ROOT)))
        return read_atlas_cache(), "cache"
    write_atlas_cache(records)
    return records, "fonte viva"


def build_city(city: dict, band: str, coastal: bool,
               agglomeration: Optional[dict] = None,
               atlas_records: Optional[Dict[Tuple[str, str], dict]] = None) -> dict:
    city_id = city["id"]
    population = int(city["population"])
    capital = city["capital"].strip() == "1"
    landmass = city["landmass"].strip().lower()

    agglomeration_population = (int(agglomeration["population"])
                                if agglomeration is not None else None)
    # população usada SÓ na área construída derivada (nunca na densidade observada)
    density_population = (agglomeration_population
                          if agglomeration_population is not None else population)

    observed = OBSERVED_AREAS.get(city_id)
    if observed is not None:
        atlas_name, atlas_country, year, area_ha, slug = observed
        built_up_km2 = round(float(area_ha) / 100.0, 1)
        source = "atlas_urban_expansion"
        built_up_year: Optional[int] = int(year)
        estimated = False
        confidence = "complete" if year >= 2015 else "partial"

        record = (atlas_records or {}).get((atlas_name, atlas_country))
        if record is None:
            raise SystemExit(
                "Atlas: linha %s (%s) não está na Tabela 1 baixada" % (atlas_name, atlas_country))
        density_per_ha = record.get("density_per_ha_t3")
        if density_per_ha is None:
            raise SystemExit(
                "Atlas: linha %s (%s) sem 'Built-up Area Density' na época T3"
                % (atlas_name, atlas_country))
        cached_area = record.get("built_up_ha_t3")
        if cached_area is None or abs(float(cached_area) - float(area_ha)) > 0.5:
            raise SystemExit(
                "Atlas: %s (%s) tem built-up %r ha na tabela e %r ha em OBSERVED_AREAS"
                % (atlas_name, atlas_country, cached_area, area_ha))
        # guarda contra mistura de coluna (T2 x T3): densidade x área = população T3 do Atlas
        atlas_population = record.get("population_t3")
        if atlas_population:
            implied = float(density_per_ha) * float(area_ha)
            if abs(implied - atlas_population) / atlas_population * 100.0 > ATLAS_POP_CHECK_TOLERANCE:
                raise SystemExit(
                    "Atlas: %s (%s) inconsistente: densidade %.0f persons/ha x %.0f ha = %.0f, "
                    "mas a população T3 da tabela é %.0f"
                    % (atlas_name, atlas_country, density_per_ha, float(area_ha),
                       implied, atlas_population))

        # Densidade do PRÓPRIO Atlas: mesmo ano e mesmo perímetro da área construída.
        density = float(round(float(density_per_ha) * 100.0, 0))
        density_source = "atlas_density"
        refs = [{
            "title": "Atlas of Urban Expansion 2016 — Areas and Densities, Table 1: "
                     "%s (%s), Built-up Area Total %s (%d ha = %.1f km2), "
                     "Built-up Area Density %s (%g persons/ha = %.0f hab/km2)"
                     % (atlas_name, atlas_country, year, area_ha, built_up_km2,
                        year, density_per_ha, density),
            "url": ATLAS_TABLE_URL,
            "year": int(year),
        }, {
            "title": "Atlas of Urban Expansion — página da cidade %s (%s)"
                     % (atlas_name, atlas_country),
            "url": ATLAS_CITY_URL % slug,
            "year": int(year),
        }]
    else:
        built_up_km2 = round(density_population / DENSITY_PROXY[band], 1)
        source = "derived_density_proxy"
        built_up_year = None
        estimated = True
        confidence = "estimated"
        # built_up = população / proxy, logo a densidade É o proxy da faixa (estimativa).
        density = float(DENSITY_PROXY[band])
        density_source = "derived_density_proxy"
        refs = [{
            "title": "Faixa de densidade urbana por nível de renda (%s: %g hab/km2) — "
                     "proxy do GHSL UCDB/Demographia, não é medição desta cidade"
                     % (band, DENSITY_PROXY[band]),
            "url": GHSL_UCDB_URL,
            "year": 2019,
        }]

    if agglomeration is not None:
        refs.append({
            "title": "citypopulation.de — Major Agglomerations of the World: %s (%s), "
                     "rank %s, população %d (referência %d-01-01)"
                     % (agglomeration.get("english") or agglomeration.get("name"),
                        agglomeration.get("country"), agglomeration.get("rank"),
                        agglomeration_population, AGGLOMERATION_YEAR),
            "url": AGGLOMERATION_URL,
            "year": AGGLOMERATION_YEAR,
        })

    factor = RESERVE_BASE_FACTOR
    if coastal:
        factor *= RESERVE_COASTAL_FACTOR
    if landmass not in MAINLAND_LANDMASSES:
        factor *= RESERVE_ISLAND_FACTOR
    reserve = round(min(built_up_km2 * factor, built_up_km2 * RESERVE_CAP_MULTIPLE), 1)

    if population >= 5_000_000:
        height_class = "high"
    elif population >= 1_000_000 or capital:
        height_class = "mid"
    else:
        height_class = "low"

    return {
        "name": city["name"],
        "nation": city["nation"],
        "population": population,
        "agglomeration_population": agglomeration_population,
        "built_up_km2": built_up_km2,
        "built_up_source": source,
        "built_up_year": built_up_year,
        "density_hab_km2": density,
        "density_source": density_source,
        "expandable_reserve_km2": reserve,
        "reserve_method": "derived:coast+landmass",
        "coastal": bool(coastal),
        "zoning_split": dict(ZONING_PROXY[band]),
        "zoning_source": "income_group_proxy",
        "height_class": height_class,
        "height_class_source": "population_threshold",
        "estimated": estimated,
        "confidence": confidence,
        "refs": refs,
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Gera urban_land.json (área construída, reserva, zoneamento e gabarito por cidade).")
    parser.add_argument("--check", action="store_true", help="não escreve o JSON, só reporta a cobertura")
    args = parser.parse_args(argv)

    cities = read_csv(CITIES_CSV)
    nations = {row["id"]: row for row in read_csv(NATIONS_CSV)}
    ports = read_csv(PORTS_CSV)
    by_name, by_compact = load_agglomerations()
    atlas_records, atlas_origin = load_atlas_table()
    print("Tabela 1 do Atlas: %d linhas (%s)" % (len(atlas_records), atlas_origin))

    coastal_ids = {row["port_of"].strip() for row in ports if row["port_of"].strip()}

    unknown = sorted(set(OBSERVED_AREAS) - {city["id"] for city in cities})
    if unknown:
        raise SystemExit("OBSERVED_AREAS tem IDs que não existem em cities.csv: %s"
                         % ", ".join(unknown))

    entries: Dict[str, dict] = {}
    by_band: Dict[str, int] = {}
    fallback_cities = 0
    unmatched: List[dict] = []
    for city in cities:
        nation = nations.get(city["nation"])
        if nation is None:
            raise SystemExit("nação %r da cidade %s ausente em nations.csv"
                             % (city["nation"], city["id"]))
        band, used_fallback = nation_band(nation)
        if used_fallback:
            fallback_cities += 1
        by_band[band] = by_band.get(band, 0) + 1
        agglomeration = match_agglomeration(city, nation, by_name, by_compact)
        if agglomeration is None:
            unmatched.append(city)
        entries[city["id"]] = build_city(city, band, city["id"] in coastal_ids,
                                         agglomeration, atlas_records)

    observed = sum(1 for entry in entries.values()
                   if entry["built_up_source"] != "derived_density_proxy")
    derived = len(entries) - observed
    coastal = sum(1 for entry in entries.values() if entry["coastal"])
    with_agglomeration = sum(1 for entry in entries.values()
                             if entry["agglomeration_population"] is not None)
    scenario_rateada = len(entries) - with_agglomeration
    density_by_source: Dict[str, int] = {}
    for entry in entries.values():
        key = entry["density_source"]
        density_by_source[key] = density_by_source.get(key, 0) + 1

    # Teto de plausibilidade: ERRO, não aviso. Atlas (máx. 55.200) e proxy (máx. 15.000)
    # nunca passam de 60.000 — se passar, há artefato de conversão/recorte no JSON.
    dense = sorted(((entry["density_hab_km2"], city_id)
                    for city_id, entry in entries.items()
                    if entry["density_hab_km2"] > MAX_OBSERVED_DENSITY),
                   reverse=True)
    if dense:
        raise SystemExit(
            "ERRO: %d cidade(s) acima de %g hab/km2: %s"
            % (len(dense), MAX_OBSERVED_DENSITY,
               ", ".join("%s %s (%.0f, %s)"
                         % (entries[city_id]["name"], city_id, value,
                            entries[city_id]["density_source"])
                         for value, city_id in dense)))

    payload = {
        "version": 1,
        "note": NOTE_TEMPLATE.format(observed=observed, derived=derived, total=len(entries),
                                     agg=with_agglomeration, rateada=scenario_rateada),
        "units": {"area": "km2", "density": "hab/km2"},
        "coverage": {
            "cities_total": len(entries),
            "cities_with_observed_area": observed,
            "cities_with_derived_area": derived,
            "cities_with_agglomeration_population": with_agglomeration,
        },
        "sources": SOURCES,
        "method": METHOD,
        "cities": entries,
    }

    print("cidades: %d | área observada: %d | área derivada: %d | costeiras: %d"
          % (len(entries), observed, derived, coastal))
    print("população real da aglomeração (referência): %d cidades | rateada de cenário: %d"
          % (with_agglomeration, scenario_rateada))
    print("faixas de renda: %s"
          % ", ".join("%s=%d" % (band, by_band[band]) for band in sorted(by_band)))
    print("nações sem PIB PPP (fallback %s): %d cidades"
          % (FALLBACK_BAND, fallback_cities))
    print("density_source: %s"
          % ", ".join("%s=%d" % (name, density_by_source[name])
                      for name in sorted(density_by_source)))
    print("densidade máxima: %.0f hab/km2 (teto %g = erro)"
          % (max(entry["density_hab_km2"] for entry in entries.values()),
             MAX_OBSERVED_DENSITY))

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
