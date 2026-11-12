"""Curadoria explícita de cidades do cenário mundial (fonte única).

Este módulo é lido por `tools/import_world_sources.py` (seleção automática) e
por `tools/build_world_cities.py` (projeção + promoção para o cenário). A lista
fica aqui, e não numa planilha, para que a decisão sobreviva a uma regeneração
completa: cidade excluída não volta, cidade forçada sempre entra.

Egito (decisão de 2026-11):
  - SAEM `EGY-POR` (Port Said) e `EGY-SUE` (Suez). O eixo do canal de Suez terá
    mecânica própria de navegação/portos, fora do modelo de cidades; enquanto
    isso, as duas cidades não entram no cenário. Ambas ficam no Delta, já coberto
    por Cairo/Tanta/Al Mansurah — nenhum vazio novo é criado (≤ 700 km).
  - ENTRA `EGY-MAT` (Marsa Matruh), capital da província de Matruh, no litoral
    oeste. É a cidade real que mais reduz o maior vazio egípcio (o deserto
    ocidental/litoral a oeste de Alexandria, ~240 km sem cidade): com ela, o
    litoral de Matruh passa a ter um centro a < 700 km de Siwa e de Sallum.
    Frente à fronteira líbia (~220 km) e à costa, respeita ≥ 40 km de fronteira
    e o porto encosta no tecido urbano (porta "city", como Alexandria).

Israel/Palestina (decisão de 2026-11):
  - SAEM `ISR-JER` (Jerusalém) e `PSX-EAS` (Jerusalém Oriental). O ponto de
    Jerusalém cai no limite do atlas admin-0 e aparecia com bandeira de Israel
    dentro da área verde da Palestina; a cidade é disputada e não é um ponto
    neutro para o mapa. As duas entradas representavam o mesmo núcleo urbano.
  - ISRAEL fica com `ISR-TLV` (Tel Aviv, capital do jogo) e `ISR-HAI` (Haifa).
    Tel Aviv usa a população da aglomeração de Gush Dan (4.156.900 hab., 2021,
    CBS de Israel), porque é o núcleo econômico real e o modelo de cidade é a
    aglomeração; a fonte está declarada na nota.
  - PALESTINA fica com `PSX-GAZ` (Gaza, mantém) e `PSX-RAM` (Ramallah, capital
    do jogo). Ramallah usa a população da aglomeração Ramallah–Al-Bireh
    (97.532 hab., projeção PCBS 2024: 44.762 + 52.770), porque o modelo de
    cidade é a aglomeração — não a cidade-proper de 44.762. Ramallah está na
    Cisjordânia, a ~0 km da linha de 1967 no raster de 4,9 km/texel; a regra
    de fronteira já permite capital/maior cidade e a cidade é marcada no
    relatório (não é uma exceção nova).

Creta/Grécia (decisão de 2026-11):
  - ENTRA `GRC-HER` (Heraklion), a maior cidade da ilha, com a população do
    município no censo de 2021 (ELSTAT). Creta tem a província admin-1 2831 no
    atlas; Heraklion é a única cidade da ilha, então a bacia/catchment da
    província fica com ela (nenhuma herança de outra cidade).
  - ENTRA o porto `GRC-HER` (mesmo id da cidade, como Atenas), em `port_of =
    GRC-HER`, no ponto de costa ao norte da ilha (Heraklion, 35,344N 25,133E).
    O ponto é a costa real do porto veneziano; o nav_mask o alcança pelo
    Mediterrâneo (célula de costa, como os demais portos de cidade).

População de Marsa Matruh: 193.293 hab. — estimativa municipal de 2023-07-01
(CAPMAS, a mesma série usada para as demais cidades egípcias) publicada em
citypopulation.de (Cities & Towns). Não é estimativa nossa.

Curadoria r2 (2026-11-12)
-------------------------
Ilhas remotas: saem Saint-Denis (Reunião), Fort-de-France (Martinica),
Mamoudzou (Mayotte), Port Louis (Maurício), Port Mathurin (Rodrigues),
Longyearbyen (Svalbard), Angra do Heroísmo (Açores), Funchal (Madeira),
Ogasawara, Hagåtña (Guam), San Andrés, Hanga Roa (Páscoa) e as cinco cidades da
Polinésia Francesa. Com as cidades fora, a regra de ilhas do atlas converte a
terra em mar (as grandes — Reunião, Svalbard, Bioko — têm caixa explícita em
`tools/build_atlas_layers.py`). Malta fica só com Valletta (Victoria, em Gozo,
sai), e a Groenlândia fica com Nuuk + Ilulissat (sítio UNESCO; Aasiaat,
Qaqortoq e Sisimiut saem).

Cidades novas (16): Brest (BLR) e Białystok (POL) na fronteira; Gdańsk (POL)
com porto; Kaliningrado (RUS) com porto em Baltiysk; Lviv (UKR); Sevastopol
(RUS) e Kherson (UKR) na Crimeia/frente sul; Donetsk e Mariupol (RUS, ocupadas)
e Zaporizhzhia (UKR) na frente leste (Kharkiv e Dnipro já existiam); Cluj-Napoca
(ROU); Kaohsiung (TWN) com porto; Palma (ESP), Ajaccio (FRA), Cagliari (ITA) e
Bari (ITA). As populações são censos/estimativas oficiais da fonte declarada em
cada `source`, não números nossos.

Pendência de território: Sevastopol, Donetsk e Mariupol pertencem a RUS como
cidade, mas a província admin-1 do atlas continua UKR. O jogo hoje pinta o
território pela cidade de mesma `home_nation`; como a cidade é RUS e a província
é UKR, ela fica com a cidade e sem a província até o usuário ajustar a frente.
"""

# id -> motivo (documentado no _meta.json e no relatório do importador)
EXCLUDED_CITIES = {
    "EGY-POR": "Port Said: eixo do canal de Suez terá mecânica própria",
    "EGY-SUE": "Suez: eixo do canal de Suez terá mecânica própria",
    "ISR-JER": "Jerusalém: núcleo disputado, aparecia com bandeira de Israel na área da Palestina",
    "PSX-EAS": "Jerusalém Oriental: mesmo núcleo de Jerusalém, substituída por Ramallah como capital",
    # Curadoria r2 (2026-11-12): ilhas remotas removidas por decisão do usuário.
    "FRA-SAI": "Saint-Denis (Reunião): ilha removida do mapa",
    "FRA-FOR": "Fort-de-France (Martinica): ilha removida do mapa",
    "FRA-MAM": "Mamoudzou (Mayotte): ilha removida do mapa",
    "MUS-POR": "Port Louis (Maurício): nação MUS removida",
    "MUS-PORT": "Port Mathurin (Rodrigues): nação MUS removida",
    "MLT-VIC": "Victoria (Gozo): Malta fica só com Valletta",
    "NOR-LON": "Longyearbyen (Svalbard): arquipélago removido do mapa",
    "PRT-ANG": "Angra do Heroísmo (Açores): arquipélago removido do mapa",
    "PRT-FUN": "Funchal (Madeira): ilha removida do mapa",
    "JPN-OGA": "Ogasawara: ilhas removidas do mapa",
    "GUM-HAG": "Hagåtña (Guam): território removido",
    "COL-SAN": "San Andrés: ilha removida do mapa",
    "CHL-HAN": "Hanga Roa (Ilha de Páscoa): ilha removida do mapa",
    "PYF-PAP": "Papeete (Polinésia Francesa): território removido por inteiro",
    "PYF-VAI": "Vaitoare (Polinésia Francesa): território removido por inteiro",
    "PYF-OTE": "Otepa (Polinésia Francesa): território removido por inteiro",
    "PYF-ATU": "Atuona (Polinésia Francesa): território removido por inteiro",
    "PYF-RIK": "Rikitea (Polinésia Francesa): território removido por inteiro",
    # Groenlândia fica com 2 cidades: Nuuk e Ilulissat (a mais relevante
    # depois da capital — sítio UNESCO e centro do turismo no Ártico).
    "GRL-AAS": "Aasiaat: Groenlândia fica com Nuuk + Ilulissat",
    "GRL-QAQ": "Qaqortoq: Groenlândia fica com Nuuk + Ilulissat",
    "GRL-SIS": "Sisimiut: Groenlândia fica com Nuuk + Ilulissat",
}

# Nomes normalizados (só [a-z0-9]) por nação: pega a mesma cidade caso uma
# regeneração da fonte lhe dê outro id ou use o topônimo local (Būr Sa‘īd,
# As Suways). A comparação é feita pelo importador com `key()` e pelo
# build_world_cities com a mesma normalização.
EXCLUDED_CITY_NAMES = {
    "EGY": {"portsaid", "bursaid", "suez", "assuways", "suways", "elsuweis"},
    "ISR": {"jerusalem", "yerushalayim", "alquds", "baytalmqdis"},
    "PSX": {"jerusalem", "eastjerusalem", "yerushalayim", "alquds",
            "baytalmqdis"},
    "FRA": {"saintdenis", "fortdefrance", "mamoudzou"},
    "MUS": {"portlouis", "portmathurin"},
    "MLT": {"victoria"},
    "NOR": {"longyearbyen"},
    "PRT": {"angradoheroismo", "funchal"},
    "JPN": {"ogasawara"},
    "GUM": {"hagatna"},
    "COL": {"sanandres"},
    "CHL": {"hangaroa"},
    "PYF": {"papeete", "vaitoare", "otepa", "atuona", "rikitea"},
    "GRL": {"aasiaat", "qaqortoq", "sisimiut"},
}

# Capital do jogo por nação quando a capital do GeoNames é uma cidade excluída
# ou quando a decisão é explicitamente outra (Tel Aviv/Ramallah). O id aponta
# para a entrada de FORCED_CITIES que recebe `capital = "1"`.
CAPITAL_OVERRIDES = {
    "ISR": "ISR-TLV",
    "PSX": "PSX-RAM",
}

# Cidades que entram sempre, mesmo fora da cota/da seleção automática.
FORCED_CITIES = [
    {
        "nation": "EGY",
        "id": "EGY-MAT",
        "name_en": "Marsa Matruh",
        "name_pt": "Marsa Matruh",
        "name_es": "Marsa Matruh",
        "lat": 31.333,
        "lon": 27.217,
        "population": 193293,
        "population_year": 2023,
        "capital": "0",
        "port": "city",
        "role": "port",
        "landmass": "africa",
        "pop_src": "city_proper",
        "note": ("curadoria: fecha o vazio do litoral oeste (capital de Matruh); "
                 "Port Said/Suez saem (canal de Suez terá mecânica própria)"),
        "source": ("CAPMAS 2023-07-01 (estimativa municipal, via citypopulation.de); "
                   "coordenadas 31.333N 27.217E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ISR",
        "id": "ISR-TLV",
        "name_en": "Tel Aviv",
        "name_pt": "Tel Aviv",
        "name_es": "Tel Aviv",
        "lat": 32.0853,
        "lon": 34.7818,
        "population": 4156900,
        "population_year": 2021,
        "capital": "1",
        "port": "city",
        "role": "capital",
        "landmass": "asia",
        "pop_src": "agglomeration",
        "note": ("curadoria: capital do jogo; aglomeração de Gush Dan "
                 "(não a cidade-proper de ~474 mil)"),
        "source": ("CBS Israel — Gush Dan 2021 (4.156.900 hab., via Wikipedia); "
                   "coordenadas 32.0853N 34.7818E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "PSX",
        "id": "PSX-RAM",
        "name_en": "Ramallah",
        "name_pt": "Ramallah",
        "name_es": "Ramallah",
        "lat": 31.9038,
        "lon": 35.2034,
        "population": 97532,
        "population_year": 2024,
        "capital": "1",
        "port": "no",
        "role": "capital",
        "landmass": "asia",
        "pop_src": "agglomeration",
        "note": ("curadoria: capital do jogo; aglomeração Ramallah–Al-Bireh "
                 "(as duas localidades formam uma aglomeração única), não a "
                 "cidade-proper (44.762); a ~0 km da linha de 1967 no raster "
                 "(capital permitida pela regra de fronteira)"),
        "source": ("PCBS — projeção de meio de ano 2024 por localidade: "
                   "Ramallah 44.762 + al-Bireh 52.770 = 97.532 hab.; "
                   "coordenadas 31.9038N 35.2034E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "GRC",
        "id": "GRC-HER",
        "name_en": "Heraklion",
        "name_pt": "Heraklion",
        "name_es": "Heraklion",
        "lat": 35.3387,
        "lon": 25.1442,
        "population": 179302,
        "population_year": 2021,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "europe",
        "pop_src": "city_proper",
        "note": ("curadoria: maior cidade de Creta; província 2831 do atlas, "
                 "porto próprio ao norte (port_of = GRC-HER)"),
        "source": ("ELSTAT — censo 2021, município de Heraklion (179.302 hab.); "
                   "coordenadas 35.3387N 25.1442E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    # ---------------------------------------------------------------- r2 (2026-11-12)
    # Fronteira Bielorrússia/Polônia: as duas cidades reais da fronteira, para
    # não deixar o vazio entre Minsk/Varsóvia e cobrir os dois lados.
    {
        "nation": "BLR",
        "id": "BLR-BRE",
        "name_en": "Brest",
        "name_pt": "Brest",
        "name_es": "Brest",
        "lat": 52.0976,
        "lon": 23.7341,
        "population": 344470,
        "population_year": 2024,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: cidade na fronteira com a Polônia (Terespol), "
                 "a pedido do usuário; não havia cidade bielorrussa no oeste"),
        "source": ("Belstat — população estimada em 2024-01-01 (344.470 hab.); "
                   "coordenadas 52.0976N 23.7341E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "POL",
        "id": "POL-BIA",
        "name_en": "Białystok",
        "name_pt": "Białystok",
        "name_es": "Białystok",
        "lat": 53.1325,
        "lon": 23.1688,
        "population": 292600,
        "population_year": 2023,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: maior cidade do nordeste polonês, ~50 km da "
                 "fronteira com a Bielorrússia; fecha o vazio entre Varsóvia "
                 "e a fronteira"),
        "source": ("GUS — população em 2023-12-31 (292.600 hab.); "
                   "coordenadas 53.1325N 23.1688E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "POL",
        "id": "POL-GDA",
        "name_en": "Gdańsk",
        "name_pt": "Gdańsk",
        "name_es": "Gdańsk",
        "lat": 54.3520,
        "lon": 18.6466,
        "population": 486022,
        "population_year": 2023,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: porto polonês no Báltico (pedido do usuário); "
                 "porto em Nowy Port, na costa da própria cidade"),
        "source": ("GUS — população em 2023-12-31 (486.022 hab.); "
                   "coordenadas 54.3520N 18.6466E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "RUS",
        "id": "RUS-KGD",
        "name_en": "Kaliningrad",
        "name_pt": "Kaliningrado",
        "name_es": "Kaliningrado",
        "lat": 54.7104,
        "lon": 20.4522,
        "population": 490449,
        "population_year": 2021,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: exclave russo no Báltico (pedido do usuário); "
                 "porto real em Baltiysk, na costa, com port_of = RUS-KGD"),
        "source": ("Rosstat — censo 2021 (490.449 hab.); "
                   "coordenadas 54.7104N 20.4522E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "UKR",
        "id": "UKR-LVI",
        "name_en": "Lviv",
        "name_pt": "Lviv",
        "name_es": "Leópolis",
        "lat": 49.8397,
        "lon": 24.0297,
        "population": 717273,
        "population_year": 2022,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: maior cidade do oeste ucraniano, a ~70 km da "
                 "fronteira polonesa; fecha o vazio entre Kyiv e a fronteira"),
        "source": ("State Statistics Service of Ukraine — estimativa em "
                   "2022-01-01 (717.273 hab.); coordenadas 49.8397N 24.0297E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "RUS",
        "id": "RUS-SEV",
        "name_en": "Sevastopol",
        "name_pt": "Sebastopol",
        "name_es": "Sebastopol",
        "lat": 44.6167,
        "lon": 33.5254,
        "population": 547820,
        "population_year": 2021,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: cidade russa na Crimeia (pedido do usuário); "
                 "a província admin-1 continua UKR no atlas — pendência de "
                 "território documentada, por ora só a posse da cidade muda"),
        "source": ("Rosstat — censo 2021 da Crimeia (547.820 hab.); "
                   "coordenadas 44.6167N 33.5254E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "UKR",
        "id": "UKR-KHE",
        "name_en": "Kherson",
        "name_pt": "Kherson",
        "name_es": "Jersón",
        "lat": 46.6354,
        "lon": 32.6169,
        "population": 279131,
        "population_year": 2022,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: cidade ucraniana que defende o front sul "
                 "(pedido do usuário); a província continua UKR"),
        "source": ("State Statistics Service of Ukraine — estimativa em "
                   "2022-01-01 (279.131 hab.); coordenadas 46.6354N 32.6169E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "RUS",
        "id": "RUS-DON",
        "name_en": "Donetsk",
        "name_pt": "Donetsk",
        "name_es": "Donetsk",
        "lat": 48.0159,
        "lon": 37.8028,
        "population": 901645,
        "population_year": 2022,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: cidade ocupada na linha de frente (dono RUS, "
                 "pedido do usuário). A província admin-1 é UKR no atlas: "
                 "pendência de território, por ora só a posse da cidade muda"),
        "source": ("State Statistics Service of Ukraine — estimativa em "
                   "2022-01-01 (901.645 hab.); coordenadas 48.0159N 37.8028E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "RUS",
        "id": "RUS-MAR",
        "name_en": "Mariupol",
        "name_pt": "Mariupol",
        "name_es": "Mariúpol",
        "lat": 47.0951,
        "lon": 37.5413,
        "population": 425681,
        "population_year": 2022,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: cidade ocupada no litoral de Azov (dono RUS, "
                 "pedido do usuário); porto real na costa. A província admin-1 "
                 "é UKR no atlas — pendência de território documentada"),
        "source": ("State Statistics Service of Ukraine — estimativa em "
                   "2022-01-01 (425.681 hab.); coordenadas 47.0951N 37.5413E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "UKR",
        "id": "UKR-ZAP",
        "name_en": "Zaporizhzhia",
        "name_pt": "Zaporizhzhia",
        "name_es": "Zaporiyia",
        "lat": 47.8388,
        "lon": 35.1396,
        "population": 710052,
        "population_year": 2022,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: cidade ucraniana que defende o front leste "
                 "(pedido do usuário); Kharkiv e Dnipro já existiam no cenário"),
        "source": ("State Statistics Service of Ukraine — estimativa em "
                   "2022-01-01 (710.052 hab.); coordenadas 47.8388N 35.1396E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ROU",
        "id": "ROU-CLJ",
        "name_en": "Cluj-Napoca",
        "name_pt": "Cluj-Napoca",
        "name_es": "Cluj-Napoca",
        "lat": 46.7712,
        "lon": 23.6236,
        "population": 286598,
        "population_year": 2021,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: capital da Transilvânia, a ~110 km da fronteira "
                 "húngara; fecha o vazio do noroeste romeno"),
        "source": ("INS — censo 2021 (286.598 hab.); coordenadas "
                   "46.7712N 23.6236E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "TWN",
        "id": "TWN-KAO",
        "name_en": "Kaohsiung",
        "name_pt": "Kaohsiung",
        "name_es": "Kaohsiung",
        "lat": 22.6273,
        "lon": 120.3014,
        "population": 2734000,
        "population_year": 2023,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "taiwan",
        "pop_src": "city_proper",
        "note": ("curadoria r2: maior cidade do sul de Taiwan (pedido do "
                 "usuário); porto real no litoral oeste, port_of = TWN-KAO"),
        "source": ("MOI Taiwan — população do município em 2023-12 "
                   "(2.734.000 hab.); coordenadas 22.6273N 120.3014E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ESP",
        "id": "ESP-PMI",
        "name_en": "Palma",
        "name_pt": "Palma",
        "name_es": "Palma",
        "lat": 39.5696,
        "lon": 2.6502,
        "population": 423350,
        "population_year": 2023,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "mallorca",
        "pop_src": "city_proper",
        "note": ("curadoria r2: capital de Maiorca (pedido do usuário); a ilha "
                 "não tinha cidade e ficava verde sem dono econômico"),
        "source": ("INE — padrón municipal em 2023-01-01 (423.350 hab.); "
                   "coordenadas 39.5696N 2.6502E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "FRA",
        "id": "FRA-AJA",
        "name_en": "Ajaccio",
        "name_pt": "Ajaccio",
        "name_es": "Ajaccio",
        "lat": 41.9192,
        "lon": 8.7386,
        "population": 73822,
        "population_year": 2021,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "corsica",
        "pop_src": "city_proper",
        "note": ("curadoria r2: capital da Córsega (pedido do usuário); a ilha "
                 "não tinha cidade"),
        "source": ("INSEE — censo 2021, município de Ajaccio (73.822 hab.); "
                   "coordenadas 41.9192N 8.7386E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ITA",
        "id": "ITA-CAG",
        "name_en": "Cagliari",
        "name_pt": "Cagliari",
        "name_es": "Cagliari",
        "lat": 39.2238,
        "lon": 9.1217,
        "population": 148269,
        "population_year": 2023,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "sardinia",
        "pop_src": "city_proper",
        "note": ("curadoria r2: capital da Sardenha (pedido do usuário); a "
                 "ilha não tinha cidade"),
        "source": ("ISTAT — população residente em 2023-01-01 (148.269 hab.); "
                   "coordenadas 39.2238N 9.1217E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ITA",
        "id": "ITA-BAR",
        "name_en": "Bari",
        "name_pt": "Bari",
        "name_es": "Bari",
        "lat": 41.1171,
        "lon": 16.8719,
        "population": 316015,
        "population_year": 2023,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r2: maior cidade da Apúlia, fecha o vazio do sul "
                 "da Itália entre Nápoles e a costa adriática"),
        "source": ("ISTAT — população residente em 2023-01-01 (316.015 hab.); "
                   "coordenadas 41.1171N 16.8719E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
]

# Portos que entram sempre. O ponto é a costa real (não o centro da cidade):
# o mapa usa `port_of` para o badge da cidade e o nav_mask alcança a célula.
FORCED_PORTS = [
    {
        "nation": "GRC",
        "id": "GRC-HER",
        "name": "Heraklion",
        "lat": 35.344,
        "lon": 25.133,
        "landmass": "europe",
        "port_of": "GRC-HER",
        "source": ("costa norte de Creta (porto de Heraklion, 35.344N 25.133E); "
                   "port_of = GRC-HER"),
    },
    # ---------------------------------------------------------------- r2 (2026-11-12)
    {
        "nation": "POL",
        "id": "POL-GDA",
        "name": "Gdańsk",
        "lat": 54.4050,
        "lon": 18.6650,
        "landmass": "continent",
        "port_of": "POL-GDA",
        "source": ("Nowy Port, Gdańsk (54.405N 18.665E); port_of = POL-GDA"),
    },
    {
        "nation": "RUS",
        "id": "RUS-KGD",
        "name": "Kaliningrad",
        "lat": 54.6400,
        "lon": 19.9090,
        "landmass": "continent",
        "port_of": "RUS-KGD",
        "source": ("porto de Baltiysk, costa do Báltico (54.640N 19.909E); "
                   "port_of = RUS-KGD"),
    },
    {
        "nation": "TWN",
        "id": "TWN-KAO",
        "name": "Kaohsiung",
        "lat": 22.6050,
        "lon": 120.2760,
        "landmass": "taiwan",
        "port_of": "TWN-KAO",
        "source": ("porto de Kaohsiung, litoral oeste (22.605N 120.276E); "
                   "port_of = TWN-KAO"),
    },
    {
        "nation": "RUS",
        "id": "RUS-MAR",
        "name": "Mariupol",
        "lat": 47.0900,
        "lon": 37.5500,
        "landmass": "continent",
        "port_of": "RUS-MAR",
        "source": ("porto de Mariupol, mar de Azov (47.090N 37.550E); "
                   "port_of = RUS-MAR"),
    },
    {
        "nation": "RUS",
        "id": "RUS-SEV",
        "name": "Sevastopol",
        "lat": 44.6100,
        "lon": 33.5200,
        "landmass": "continent",
        "port_of": "RUS-SEV",
        "source": ("baía de Sevastopol, mar Negro (44.610N 33.520E); "
                   "port_of = RUS-SEV"),
    },
]
