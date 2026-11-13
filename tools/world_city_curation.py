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
Ogasawara, San Andrés, Hanga Roa (Páscoa) e as cinco cidades da Polinésia
Francesa. Com as cidades fora, a regra de ilhas do atlas converte a terra em
mar (as grandes — Reunião, Svalbard, Bioko — têm caixa explícita em
`tools/build_atlas_layers.py`). Malta fica só com Valletta (Victoria, em Gozo,
sai), e a Groenlândia fica com Nuuk + Ilulissat (sítio UNESCO; Aasiaat,
Qaqortoq e Sisimiut saem). A r3 devolveu Hagåtña (Guam), Naha (Okinawa) e
manteve Miyakojima.

Cidades novas (16): Brest (BLR) e Białystok (POL) na fronteira; Gdańsk (POL)
com porto; Kaliningrado (RUS) com porto em Baltiysk; Lviv (UKR); Sevastopol
(RUS) e Kherson (UKR) na Crimeia/frente sul; Donetsk e Mariupol (RUS, ocupadas)
e Zaporizhzhia (UKR) na frente leste (Kharkiv e Dnipro já existiam); Cluj-Napoca
(ROU); Kaohsiung (TWN) com porto; Palma (ESP), Ajaccio (FRA), Cagliari (ITA) e
Bari (ITA). As populações são censos/estimativas oficiais da fonte declarada em
cada `source`, não números nossos.

Pendência de território (RESOLVIDA na r4, 2026-11-12): Sevastopol, Donetsk e
Mariupol pertencem a RUS como cidade e a província admin-1 do atlas era UKR.
Agora o atlas reatribui o dono no pipeline
(`world_nation_curation.OCCUPIED_REGIONS`/`OCCUPIED_FRONT`, lido por
`build_atlas_layers.py`): a Crimeia já era RUS no Natural Earth, Luhansk
inteira passa a RUS e Donetsk/Zaporizhzhia/Kherson são divididas na frente
aproximada (2025–2026, ISW/DeepStateMAP). Donetsk e Mariupol administram a
parte ocupada da UA-14; Zaporizhzhia e Kherson (UKR) ficam com a parte
ucraniana. Nada é editado à mão no JSON.

Curadoria r3 (2026-11-12)
-------------------------
Ilhas devolvidas: **Okinawa** (Naha, `JPN-NAH`, com porto), **Miyakojima**
(cidade `JPN-MIY` já existia; a ilha é mantida e ganha seed) e **Guam**
(`GUM-HAG`, Hagåtña, com porto em Apra Harbor; Guam volta a ser território dos
EUA em `world_nation_curation.py`). As três ficam no atlas e no `nav_mask` por
`ISLAND_KEEP_SEEDS` + cidade/porto.

Cidades novas: **Oradea** (`ROU-ORA`, fronteira com a Hungria) e **Ruse**
(`BGR-RUS`, no Danúbio, a leste de Bucareste).

Microestados (cidades): as cidades dos microestados anexados continuam no
cenário como cidade do anexador, exceto quando estão coladas a outra cidade
(< 60 km). Distâncias medidas no plano do jogo: Vaticano–Roma **5 km** (sai,
`VAT-VATI`), Mônaco–Gênova 142 km, Andorra-a-Velha–Toulouse 121 km, San
Marino–Roma 226 km, Vaduz–Zürich 77 km (ficam; são a cidade do anexador).

Portos dos ex-microestados: `ITA-ROM`/`ITA-ROME` passam a Roma (`ITA-ROM`) e
`ITA-PAD` passa para a cidade italiana que administra o território do porto
(Milão, `ITA-MIL` — Pádua/Veneza não estão no roster). O mapa de `port_of`
fica em `PORT_OF_OVERRIDES`. A r4 fundiu o porto duplicado de Roma: fica só
`ITA-ROM` (foz do Tibre, Fiumicino/Ostia) e `ITA-ROME` sai em `EXCLUDED_PORTS`.

Curadoria r5 (2026-11-12)
-------------------------
Portos duplicados: a fonte de 2026 (`ports_source.csv`) mantém os portos do
cenário da fase 1 (114) e acrescenta 23 pontos "snap na costa" para cidades com
`port = yes` que ainda não tinham porto. Quando a cidade já tinha porto legado,
os dois registros ficaram no cenário — dois pinos para o MESMO porto. Em todos
os casos o ponto legado é o **centro da cidade** (terra; 24–44 km de distância
assinada à costa medida no `world_sdf.png`) e o ponto de 2026 é a **costa
real**, a ~5–10 km dela no raster. Fica o ponto de 2026
(`EXCLUDED_PORTS`), com o `port_of` apontando para a cidade final do cenário:

* Guangzhou: fica `CHN-GUAN` (foz do rio das Pérolas, a caminho de Nansha);
  sai `CHN-GUA` (centro da cidade, ~25 km rio acima).
* Xangai: fica `CHN-SHAN` (Waigaoqiao, na foz do Yangtzé); sai `CHN-SHA`
  (centro, no rio Huangpu, ~34 km da costa).
* Medan: fica `IDN-MEDA` (Belawan, o porto real); sai `IDN-MED` (centro de
  Medan, ~20 km da costa; a `landmass` "java" do legado nem casava com a
  cidade, que é "indonesia").
* Nagoya: fica `JPN-NAGO` (Nagoya Port, baía de Ise); sai `JPN-NAG` (centro,
  ~44 km da costa).
* Bursa: fica `TUR-BURS` (costa de Mudanya, o porto de Bursa no mar de
  Mármara); sai `TUR-BUR` (centro, ~17 km da costa; `landmass` "continent" não
  casava com a cidade, que é "asia").
* Los Angeles: fica `USA-LOSA` (costa de Playa del Rey/Dockweiler, em mar
  aberto); sai `USA-LOS` (centro, ~34 km da costa). San Pedro/Long Beach ficam
  ~25 km ao sul, fora da célula do mapa (≈8 km/px); `USA-LOS` é pior que o
  ponto costeiro.
* Caracas: fica `VEN-LAG` (La Guaira, o porto real na costa); sai `VEN-CCS`
  (centro de Caracas, ~12 km da costa). `VEN-LAG` é um dos 18 portos legados
  do contrato e continua intacto.

O detector geral (`same name`, `<15 km`, mesmo `port_of` com >1 porto) não
achou outro caso: os demais grupos de `port_of` (Durrës/Tirana,
Baku/Sumqayıt, Fukuoka/Kitakyushu, Reykjavík/Reykjanesbær etc.) são portos
distintos e reais, longe entre si; a atribuição de `port_of` sem limite de
distância (Hamamatsu→Nagoya, Jacksonville→Miami) é uma pendência de dados
separada, não um pino duplicado.

Curadoria r6 (2026-11-12)
-------------------------
Sudão do Sul (`SSD`) e Somalilândia (`SOL`) entram como nações jogáveis (ver
`world_nation_curation.NATION_SUPPLEMENT`). Como não existiam no roster, a fonte
de cidades nunca teve linhas delas; as cidades entram por `FORCED_CITIES`:

* SSD: **Juba** (capital; peso = censo 2008, 230.195), **Malakal** (126.483) e
  **Wau** (118.331) — os três números são do último censo do Sudão (2008) e
  servem de PESO de aglomeração; a população exibida é o rateio da população
  nacional de 2026 (`build_world_cities.distribute_national_population`).
* SOL: **Hargeisa** (capital) e **Berbera** (porto, `FORCED_PORTS`, ponto de
  costa no golfo de Aden). Não há censo publicado: os pesos (1,2 M e 0,25 M) são
  estimativas públicas declaradas, não números de jogo.

Nenhuma capital é duplicada: `CAPITAL_OVERRIDES` aponta SSD→SSD-JUB e
SOL→SOL-HAR, e as demais cidades entram com `capital = "0"`.

Curadoria r7 (2026-11-13)
-------------------------
Espanha/Gibraltar (pedido do usuário): o sul da Espanha não tinha nenhuma
cidade — a mais próxima é Madrid, a 514 km do Estreito (Sevilla, Málaga e Cádiz
nunca estiveram no roster). ENTRA `ESP-ALG` (**Algeciras**), a cidade espanhola
do próprio Estreito de Gibraltar, com porto na Baía de Gibraltar. A escolha é
Algeciras e não Cádiz porque as duas respeitam os ≥ 40 km de qualquer outra
cidade (não há cidade ESP no sul) e Algeciras é o núcleo urbano/portuário que
está de fato no estreito.

População: 249.847 hab. — aglomeração urbana da Baía de Algeciras (INE, padrón
municipal de 2025-01-01: Algeciras 126.589 + La Línea de la Concepción 64.499 +
San Roque 34.310 + Los Barrios 24.449). É o peso de aglomeração
(`sim_population`); a população exibida é o rateio da população nacional de
2026 (`build_world_cities.distribute_national_population`).

Porto `ESP-ALG` (`FORCED_PORTS`, `port_of = ESP-ALG`): Muelle Juan Carlos I,
na Baía de Gibraltar, a ~1 km do centro — porto dentro da cidade (como
Heraklion/Gdańsk). A regra de 15–25 km vale para porto separado de uma cidade
cujo centro fica longe da costa; aqui não se aplica.
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
    # Curadoria r3 (2026-11-12): microestados fundidos em ITA/FRA/ESP/CHE.
    # Só sai a cidade colada a outra (< 60 km). Vaticano fica a 5 km de Roma;
    # Mônaco (142 km de Gênova), Andorra-a-Velha (121 km de Toulouse), San
    # Marino (226 km de Roma) e Vaduz (77 km de Zürich) continuam como cidade
    # do anexador — não há outra cidade no mesmo núcleo.
    "VAT-VATI": "Cidade do Vaticano: 5 km de Roma (mesmo núcleo urbano)",
}

# Cidades legadas (as 40 do cenário da fase 1) que a curadoria remove: o
# contrato de 40 passa a exigir `40 - len(LEGACY_EXCLUDED)` e a cidade sai do
# cenário. Hoje só o Vaticano — o resto do bloco legado continua intacto.
LEGACY_EXCLUDED = {
    "VAT-VATI": "colada a Roma (5 km); cidade passa a ser ITA-ROM",
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
    "COL": {"sanandres"},
    "CHL": {"hangaroa"},
    "PYF": {"papeete", "vaitoare", "otepa", "atuona", "rikitea"},
    "GRL": {"aasiaat", "qaqortoq", "sisimiut"},
    "VAT": {"vaticancity"},
}

# Capital do jogo por nação quando a capital do GeoNames é uma cidade excluída
# ou quando a decisão é explicitamente outra (Tel Aviv/Ramallah). O id aponta
# para a entrada de FORCED_CITIES que recebe `capital = "1"`.
CAPITAL_OVERRIDES = {
    "ISR": "ISR-TLV",
    "PSX": "PSX-RAM",
    "SSD": "SSD-JUB",
    "SOL": "SOL-HAR",
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
        "note": ("curadoria r2/r4: cidade russa na Crimeia (pedido do usuário); "
                 "a província admin-1 (UA-40) já é RUS no atlas desde o "
                 "Natural Earth, então a cidade administra o território"),
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
        "note": ("curadoria r2/r4: cidade ucraniana que defende o front sul "
                 "(pedido do usuário); a província UA-65 fica UKR na margem "
                 "direita do Dnipro (OCCUPIED_FRONT)"),
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
        "note": ("curadoria r2/r4: cidade ocupada na linha de frente (dono "
                 "RUS, pedido do usuário). A parte ocupada da província UA-14 "
                 "é RUS no atlas (OCCUPIED_REGIONS), então a cidade "
                 "administra o território ocupado"),
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
        "note": ("curadoria r2/r4: cidade ocupada no litoral de Azov (dono "
                 "RUS, pedido do usuário); porto real na costa. A parte "
                 "ocupada da província UA-14 é RUS no atlas "
                 "(OCCUPIED_REGIONS), então a cidade administra o território"),
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
        "note": ("curadoria r2/r4: cidade ucraniana que defende o front leste "
                 "(pedido do usuário); a província UA-23 fica UKR ao norte da "
                 "frente (OCCUPIED_FRONT), então a cidade administra a parte "
                 "ucraniana"),
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
    # ---------------------------------------------------------------- r3 (2026-11-12)
    # Ilhas devolvidas: Okinawa e Guam ganham cidade + porto; Miyakojima já
    # tinha cidade (JPN-MIY) e é mantida com seed no atlas.
    {
        "nation": "JPN",
        "id": "JPN-NAH",
        "name_en": "Naha",
        "name_pt": "Naha",
        "name_es": "Naha",
        "lat": 26.2124,
        "lon": 127.6809,
        "population": 317625,
        "population_year": 2020,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "japan",
        "pop_src": "city_proper",
        "note": ("curadoria r3: capital de Okinawa e maior cidade da ilha "
                 "(pedido do usuário); porto real de Naha na costa oeste "
                 "(port_of = JPN-NAH)"),
        "source": ("Statistics Bureau of Japan — censo 2020, Naha-shi "
                   "(317.625 hab.); coordenadas 26.2124N 127.6809E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "USA",
        "id": "GUM-HAG",
        "name_en": "Hagåtña",
        "name_pt": "Hagåtña",
        "name_es": "Agaña",
        "lat": 13.4744,
        "lon": 144.7504,
        "population": 943,
        "population_year": 2020,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "guam",
        "pop_src": "city_proper",
        "note": ("curadoria r3: capital de Guam, devolvida como território dos "
                 "EUA (pedido do usuário); porto comercial real em Apra Harbor "
                 "(port_of = GUM-HAG). A população é a do CDP de Hagåtña, não a "
                 "da ilha"),
        "source": ("U.S. Census Bureau — 2020 Census of Guam, Hagåtña CDP "
                   "(943 hab.); coordenadas 13.4744N 144.7504E; Apra Harbor "
                   "13.4430N 144.6520E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ROU",
        "id": "ROU-ORA",
        "name_en": "Oradea",
        "name_pt": "Oradea",
        "name_es": "Oradea",
        "lat": 47.0722,
        "lon": 21.9211,
        "population": 183105,
        "population_year": 2021,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r3: capital do condado de Bihor, a ~10 km da "
                 "fronteira húngara (pedido do usuário); fecha o noroeste "
                 "romeno entre Cluj-Napoca e a fronteira"),
        "source": ("INS — censo 2021 (183.105 hab.); coordenadas "
                   "47.0722N 21.9211E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "BGR",
        "id": "BGR-RUS",
        "name_en": "Ruse",
        "name_pt": "Ruse",
        "name_es": "Ruse",
        "lat": 43.8486,
        "lon": 25.9540,
        "population": 123134,
        "population_year": 2021,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "city_proper",
        "note": ("curadoria r3: maior cidade búlgara do Danúbio, a ~65 km a "
                 "leste de Bucareste (pedido do usuário); o Danúbio é a "
                 "fronteira BGR-ROU, então a cidade encosta na fronteira "
                 "(como Oradea, decisão explícita do usuário)"),
        "source": ("NSI Bulgária — censo 2021 (123.134 hab.); coordenadas "
                   "43.8486N 25.9540E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    # ---------------------------------------------------------------- r6 (2026-11-12)
    # Sudão do Sul (SSD) e Somalilândia (SOL) não existiam no roster, então a
    # fonte de cidades nunca foi gerada para elas. As cidades entram pela
    # curadoria (o importador as traria do GeoNames se a fonte fosse refeita).
    # PESO = população da aglomeração usada como unidade de simulação; a
    # população EXIBIDA é o rateio da população nacional de 2026 (build).
    # Sudão do Sul: censo de 2008 (último censo; Southern Sudan Centre for
    # Census) — números reais, usados como peso relativo.
    {
        "nation": "SSD",
        "id": "SSD-JUB",
        "name_en": "Juba",
        "name_pt": "Juba",
        "name_es": "Juba",
        "lat": 4.8517,
        "lon": 31.5825,
        "population": 230195,
        "population_year": 2008,
        "capital": "1",
        "port": "no",
        "role": "capital",
        "landmass": "africa",
        "pop_src": "city_proper",
        "note": ("curadoria r6: capital do Sudão do Sul (pedido do usuário); "
                 "peso = censo 2008, a população exibida vem do rateio nacional"),
        "source": ("censo 2008 do Sudão (Juba 230.195 hab.); coordenadas "
                   "4.8517N 31.5825E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "SSD",
        "id": "SSD-MAL",
        "name_en": "Malakal",
        "name_pt": "Malakal",
        "name_es": "Malakal",
        "lat": 9.5334,
        "lon": 31.6605,
        "population": 126483,
        "population_year": 2008,
        "capital": "0",
        "port": "no",
        "role": "regional",
        "landmass": "africa",
        "pop_src": "city_proper",
        "note": ("curadoria r6: centro do Alto Nilo (pedido do usuário); "
                 "peso = censo 2008"),
        "source": ("censo 2008 do Sudão (Malakal 126.483 hab.); coordenadas "
                   "9.5334N 31.6605E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "SSD",
        "id": "SSD-WAU",
        "name_en": "Wau",
        "name_pt": "Wau",
        "name_es": "Wau",
        "lat": 7.7011,
        "lon": 27.9894,
        "population": 118331,
        "population_year": 2008,
        "capital": "0",
        "port": "no",
        "role": "regional",
        "landmass": "africa",
        "pop_src": "city_proper",
        "note": ("curadoria r6: centro do Bahr el Ghazal Ocidental (pedido do "
                 "usuário); peso = censo 2008"),
        "source": ("censo 2008 do Sudão (Wau 118.331 hab.); coordenadas "
                   "7.7011N 27.9894E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    # Somalilândia: não há censo publicado; os pesos são estimativas públicas
    # (Hargeisa ~1,2 M; Berbera ~0,25 M) e a população exibida vem do rateio
    # nacional (5,7 M), não destes números.
    {
        "nation": "SOL",
        "id": "SOL-HAR",
        "name_en": "Hargeisa",
        "name_pt": "Hargeisa",
        "name_es": "Hargeisa",
        "lat": 9.5600,
        "lon": 44.0650,
        "population": 1200000,
        "population_year": 2023,
        "capital": "1",
        "port": "no",
        "role": "capital",
        "landmass": "africa",
        "pop_src": "estimate",
        "note": ("curadoria r6: capital da Somalilândia (pedido do usuário); "
                 "peso = estimativa pública, população exibida = rateio nacional"),
        "source": ("estimativa pública (~1,2 M, 2023); coordenadas "
                   "9.5600N 44.0650E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "SOL",
        "id": "SOL-BER",
        "name_en": "Berbera",
        "name_pt": "Berbera",
        "name_es": "Berbera",
        "lat": 10.4356,
        "lon": 45.0143,
        "population": 250000,
        "population_year": 2023,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "africa",
        "pop_src": "estimate",
        "note": ("curadoria r6: único porto de águas profundas do golfo de "
                 "Aden (pedido do usuário); peso = estimativa pública"),
        "source": ("estimativa pública (~250 mil, 2023); porto 10.439N "
                   "45.010E; coordenadas 10.4356N 45.0143E"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    # ---------------------------------------------------------------- r7 (2026-11-13)
    # Sul da Espanha: não havia cidade ESP abaixo de Madrid/Valencia. Algeciras
    # é o núcleo espanhol do Estreito de Gibraltar (cidade + porto da Baía de
    # Gibraltar); Cádiz ficaria igualmente a > 40 km de tudo, mas está a ~100 km
    # a oeste do estreito. Peso = aglomeração urbana da Baía de Algeciras.
    {
        "nation": "ESP",
        "id": "ESP-ALG",
        "name_en": "Algeciras",
        "name_pt": "Algeciras",
        "name_es": "Algeciras",
        "lat": 36.1333,
        "lon": -5.4500,
        "population": 249847,
        "population_year": 2025,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "continent",
        "pop_src": "agglomeration",
        "note": ("curadoria r7: fecha o sul da Espanha, no Estreito de "
                 "Gibraltar (pedido do usuário); peso = aglomeração da Baía de "
                 "Algeciras; porto próprio na baía (port_of = ESP-ALG)"),
        "source": ("INE — padrón municipal 2025-01-01: Algeciras 126.589 + "
                   "La Línea de la Concepción 64.499 + San Roque 34.310 + "
                   "Los Barrios 24.449 = 249.847 hab. (aglomeração urbana da "
                   "Baía de Algeciras); coordenadas 36.1333N 5.4500W"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
]

# Portos repetidos: dois registros para o MESMO porto (mesma nação + mesmo
# `port_of`) são um bug de fonte — o mapa desenha dois pinos no mesmo lugar.
# Fica o id canônico; o removido é sempre o pior situado (centro da cidade em
# terra). As decisões da r4/r5 estão explicadas no docstring do módulo.
#
# Roma (r4): `ITA-ROME` é o ponto "snap na costa" da fonte de 2026, a ~2 km da
# praia de Ostia; o porto de Roma do cenário é `ITA-ROM`, na foz do Tibre
# (Fiumicino/Ostia), o mesmo ponto legado. Aqui o legado era o melhor ponto.
EXCLUDED_PORTS = {
    "ITA-ROME": "duplicado de ITA-ROM (mesmo porto de Roma; ponto snap offshore)",
    # r5 (2026-11-12): pares legado (centro da cidade) × 2026 (costa real).
    # Fica o ponto de 2026; ver o docstring para a posição de cada porto.
    "CHN-GUA": "duplicado de CHN-GUAN (Guangzhou; era o centro da cidade, no rio)",
    "CHN-SHA": "duplicado de CHN-SHAN (Xangai; era o centro, no rio Huangpu)",
    "IDN-MED": "duplicado de IDN-MEDA (Medan; era o centro, ~20 km da costa)",
    "JPN-NAG": "duplicado de JPN-NAGO (Nagoya; era o centro, ~44 km da costa)",
    "TUR-BUR": "duplicado de TUR-BURS (Bursa; era o centro, ~17 km do mar)",
    "USA-LOS": "duplicado de USA-LOSA (Los Angeles; era o centro, ~34 km da costa)",
    # Extra do detector geral (<15 km do mesmo port_of): Caracas não é porto;
    # o porto real é La Guaira, que já é um dos 18 portos legados.
    "VEN-CCS": "duplicado de VEN-LAG (Caracas; o porto real é La Guaira)",
}

# Portos cujo `port_of` não pode ser inferido (a cidade de mesmo nome não
# existe no roster): mapa id do porto -> id da cidade na FONTE
# (`cities_source.csv`). A promoção traduz o alvo para o id final do cenário
# com o mesmo `plan_city_ids` das cidades (ITA-ROME -> ITA-ROM). O build falha
# se o alvo não existir na fonte ou for de outra nação.
PORT_OF_OVERRIDES = {
    # Porto de Roma: aponta para a cidade Roma (`ITA-ROM` no cenário; a fonte
    # chama a cidade de `ITA-ROME`).
    "ITA-ROM": "ITA-ROME",
    # Pádua/Veneza não estão no roster. O porto fica no litoral de Veneza e o
    # território (Veneto) é administrado pela cidade italiana mais próxima,
    # Milão — a mesma regra de atribuição de província do jogo.
    "ITA-PAD": "ITA-MILA",
}

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
    # ---------------------------------------------------------------- r3 (2026-11-12)
    {
        "nation": "JPN",
        "id": "JPN-NAH",
        "name": "Naha",
        "lat": 26.2167,
        "lon": 127.6667,
        "landmass": "japan",
        "port_of": "JPN-NAH",
        "source": ("porto de Naha, costa oeste de Okinawa "
                   "(26.2167N 127.6667E); port_of = JPN-NAH"),
    },
    {
        "nation": "USA",
        "id": "GUM-HAG",
        "name": "Hagåtña",
        "lat": 13.4430,
        "lon": 144.6520,
        "landmass": "guam",
        "port_of": "GUM-HAG",
        "source": ("Apra Harbor, porto comercial de Guam "
                   "(13.443N 144.652E); port_of = GUM-HAG"),
    },
    # ---------------------------------------------------------------- r6 (2026-11-12)
    # Berbera: porto de águas profundas do golfo de Aden, o único da
    # Somalilândia. O ponto é a costa real (o centro da cidade fica ~1 km
    # terra adentro); o nav_mask alcança a célula pelo golfo.
    {
        "nation": "SOL",
        "id": "SOL-BER",
        "name": "Berbera",
        "lat": 10.4390,
        "lon": 45.0100,
        "landmass": "africa",
        "port_of": "SOL-BER",
        "source": ("porto de Berbera, golfo de Aden (10.439N 45.010E); "
                   "port_of = SOL-BER"),
    },
    # ---------------------------------------------------------------- r7 (2026-11-13)
    # Algeciras: o ponto é o Muelle Juan Carlos I, na Baía de Gibraltar (a ~1 km
    # do centro). O porto é da própria cidade, como Heraklion/Gdańsk.
    {
        "nation": "ESP",
        "id": "ESP-ALG",
        "name": "Algeciras",
        "lat": 36.1280,
        "lon": -5.4410,
        "landmass": "continent",
        "port_of": "ESP-ALG",
        "source": ("Muelle Juan Carlos I, porto de Algeciras na Baía de "
                   "Gibraltar (36.128N 5.441W); port_of = ESP-ALG"),
    },
]
