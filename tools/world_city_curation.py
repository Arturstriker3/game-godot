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
"""

# id -> motivo (documentado no _meta.json e no relatório do importador)
EXCLUDED_CITIES = {
    "EGY-POR": "Port Said: eixo do canal de Suez terá mecânica própria",
    "EGY-SUE": "Suez: eixo do canal de Suez terá mecânica própria",
    "ISR-JER": "Jerusalém: núcleo disputado, aparecia com bandeira de Israel na área da Palestina",
    "PSX-EAS": "Jerusalém Oriental: mesmo núcleo de Jerusalém, substituída por Ramallah como capital",
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
]
