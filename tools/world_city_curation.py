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

População de Marsa Matruh: 193.293 hab. — estimativa municipal de 2023-07-01
(CAPMAS, a mesma série usada para as demais cidades egípcias) publicada em
citypopulation.de (Cities & Towns). Não é estimativa nossa.
"""

# id -> motivo (documentado no _meta.json e no relatório do importador)
EXCLUDED_CITIES = {
    "EGY-POR": "Port Said: eixo do canal de Suez terá mecânica própria",
    "EGY-SUE": "Suez: eixo do canal de Suez terá mecânica própria",
}

# Nomes normalizados (só [a-z0-9]) por nação: pega a mesma cidade caso uma
# regeneração da fonte lhe dê outro id ou use o topônimo local (Būr Sa‘īd,
# As Suways). A comparação é feita pelo importador com `key()` e pelo
# build_world_cities com a mesma normalização.
EXCLUDED_CITY_NAMES = {
    "EGY": {"portsaid", "bursaid", "suez", "assuways", "suways", "elsuweis"},
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
]
