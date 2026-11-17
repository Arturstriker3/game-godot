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

Curadoria r8 (2026-11-13, item 14)
----------------------------------
Auditoria de cidades faltantes nas nações grandes/populosas: para cada nação
com área >= 500.000 km² ou população >= 30 M (67 nações), comparam-se as
cidades do cenário com as 10 maiores aglomerações de 2026-01-01 de
`citypopulation.de` (Thomas Brinkhoff, Major Agglomerations of the World) e
listam-se as ausentes com > 1 M hab. a >= 150 km de QUALQUER cidade do cenário.
O auditor reprodutível é `tools/audit_missing_cities.py`; a lista completa, a
tabela e as exclusões de borda estão em `docs/plans/world_all_data.md` §21.

Espanha: ENTRAM `ESP-SEV` (Sevilha, 1,31 M) e `ESP-MLG` (Málaga, 1,12 M), a
pedido do usuário, mesmo com 147,4 km e 112,7 km de Algeciras — a regra de
150 km do item 14(b) é para a revisão automática, não para o pedido explícito.
Málaga tem porto próprio (Porto de Málaga). Sevilha tem porto fluvial no
Guadalquivir; o `nav_mask` não carrega o rio, então o ponto do porto fica na
foz (Sanlúcar de Barrameda, ~80 km), que é o acesso marítimo real do porto.
Cádiz NÃO entra: Sevilha tem acesso marítimo pelo estuário.

Cidades novas (24): Nürnberg, Boston, Fez, Pádua, Miami, George Town, Mendoza,
Samarcanda, Cochabamba, Bordéus, Marraquexe, Konya, Krasnodar, Benguela, Santa
Cruz de la Sierra, Xiamen, Kermanshah, Bucaramanga, Sendai, Antalya, Torreón,
Cúcuta, Agadir e Ciudad Juárez. Todas usam a aglomeração 2026-01-01 como peso
(`pop_src = agglomeration`) e as coordenadas do centro urbano (Wikidata).
Nenhuma duplica cidade existente (< 25 km). Krasnodar entra apesar do teto
histórico de 40 cidades por nação: a Rússia já estava em 44 desde as forçadas
da r2, e o teto nunca foi uma validação do build — é uma regra da seleção
automática.

Portos: os portos legados `USA-BOS` (Boston), `USA-MIA` (Miami), `CHN-XIA`
(Xiamen), `JPN-SEN` (Sendai) e `ITA-PAD` (Pádua/Veneza) passam a apontar para
as cidades novas (`PORT_OF_OVERRIDES`). Os portos novos são George Town
(Penang), Benguela, Antalya, Agadir, Málaga e Sevilha (`FORCED_PORTS`).
Pádua é o caso do r3: o porto de Veneza ficava com Milão porque Pádua não
estava no roster; agora a cidade existe e o porto passa a ela.

Curadoria r9 (2026-11-17, recalibração de áreas)
-----------------------------------------------
Singapura (`SGP`) é ERRO DE DADO, não consequência da correção de área: a
cidade-estado tem um dos maiores portos de contêineres do mundo (PSA) e o
cenário não tinha nenhum porto com `nation = SGP`. No mundo de
`sim/trade.cpp` a travessia entre massas de terra exige um porto nacional em
CADA ponta; `SGP-SIN` é a única cidade da landmass `singapore`, então sem porto
a nação não tinha rota marítima nenhuma e caía para a produção da própria
fazenda (157,3 t/dia, com a área real de ~125 km²). ENTRA o porto `SGP-SIN`
(`FORCED_PORTS`, `port_of = SGP-SIN`), no cais de Tanjong Pagar, costa sul da
ilha voltada para o Estreito de Singapura (1,2626N 103,8544E): célula de ÁGUA
do componente principal no `nav_mask` (o cais alcança o mar aberto pelo
estreito) e 4,9 km da costa no `world_sdf.png` (dentro dos 10 km de tolerância
do validador). Não entra cidade nova: `SGP-SIN` já existia.

Auditoria da mesma lacuna (relatada, sem mudança nesta rodada): `HKG`, `MDV` e
`MUS` não estão no roster do cenário (MUS foi removida na r2); `BHR`, `QAT` e
`BRN` têm cidade costeira e NENHUM porto, mas não são ilhas isoladas — a cidade
compartilha a landmass `asia` com os vizinhos, então a troca de massa de terra
não é o que as isola. `MLT` já tem porto (`MLT-VAL`).

Curadoria r10 (2026-11-17, recalibração 3 — portos e cidades)
------------------------------------------------------------
Duas frentes, ambas de dado real:

1. **Nações-ilha/costeiras sem porto nenhum** (§13 do `recal_areas_pop.md`):
   `PHL`, `CUB`, `NZL`, `MDG`, `JAM`, `LKA`, `BHS`, `FJI`, `TTO`, `BRB`, `SLB`,
   `BHR`, `QAT` e `BRN` ganham UM porto principal, no cais da cidade do roster
   mais adequada (`FORCED_PORTS`, seção r10). A regra do `sim/trade.cpp` exige
   porto da nação em CADA ponta da travessia; sem porto, uma nação sozinha na
   sua massa de terra não compra nem vende fora dela. Cada entrada traz a fonte
   do cais e foi conferida contra `nav_mask.png` (célula de água do componente
   principal, o mesmo teste do `nav_strait_test.gd`) e `world_sdf.png` (≤ 10 km
   de terra, `COAST_TOLERANCE_KM` do validador).

2. **Portos com `landmass` trocada** (`PORT_LANDMASS_FIX`): 13 portos legados
   cujo rótulo veio do rótulo ANTIGO da cidade (Voronoi legado) e não casa com
   a cidade que o porto serve — `sim/logistics.cpp::nearest_port` filtra por
   `port.landmass == landmass` da cidade, então esses portos NUNCA eram
   escolhidos. A causa não é ilha: é a troca de rótulo da normalização
   (`continent` × `asia`, `java` × `indonesia`), que alcançou as cidades e não
   os portos. Os outros 2 (Baku e Sumqayit) estão no **Cáspio**, mar fechado:
   corrigir o rótulo os ativaria numa rota oceânica inexistente, então SAEM do
   cenário (`EXCLUDED_PORTS`).
"""

# id do porto -> (landmass correta, motivo). O porto só serve a cidade da
# MESMA nação e MESMA landmass (`sim/logistics.cpp`); a correção é o rótulo da
# cidade que o porto declara em `port_of`.
PORT_LANDMASS_FIX = {
    # --- IDN: o rótulo `java` do Voronoi antigo não existe em cidade nenhuma
    # do cenário (as 19 cidades de IDN são `indonesia`, a massa única do
    # arquipélago no modelo). 9 dos 10 portos ficaram em `java` e só IDN-MEDA
    # (Sumatra) servia alguma cidade.
    "IDN-BAND": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    "IDN-BAT": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    "IDN-JAK": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    "IDN-MAK": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    "IDN-MAL": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    "IDN-PAL": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    "IDN-SEM": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    "IDN-SUR": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    "IDN-YOG": ("indonesia", "legado `java`: nenhuma cidade tem essa landmass"),
    # --- TUR: as cidades de TUR são `asia` (continente da fonte); os portos
    # ficaram no rótulo `continent` de antes da normalização (o mesmo `europe →
    # continent` do tools/normalize_landmass.py, que só passou pelas cidades).
    "TUR-ADA": ("asia", "legado `continent` x cidade `asia`"),
    "TUR-IST": ("asia", "legado `continent` x cidade `asia`"),
    "TUR-IZM": ("asia", "legado `continent` x cidade `asia`"),
    # --- GEO: mesma troca de rótulo (`continent` -> `asia`); Batumi é Mar
    # Negro, que alcança o oceano pelo Bósforo (nav_strait_test).
    "GEO-BAT": ("asia", "legado `continent` x cidade `asia`"),
    # --- AZE: NÃO entra aqui. Os dois portos (Baku, Sumqayit) estão no Cáspio,
    # mar fechado; ativá-los abriria comércio oceânico por uma rota inexistente.
    # Saem do cenário em `EXCLUDED_PORTS`.
}

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

# Correções de POPULAÇÃO da fonte (Q-016, 2026-11-17). O GeoNames repete o valor
# de outra cidade em alguns registros; o valor correto e a fonte estão aqui. O
# importador aplica em `cities_source.csv` e o build_world_cities reaplica na
# projeção, para a correção sobreviver a uma regeneração da fonte.
# `method`/`confidence` declaram como o número foi obtido, não são decoração.
POPULATION_OVERRIDES = {
    "AFG-MAZ": {
        "population": 484500,
        "population_year": 2020,
        "method": "official_projection",
        "confidence": "high",
        "source": ("CSO Afghanistan, projeção 2020-09-21, via "
                   "citypopulation.de/en/afghanistan/cities/ (acesso 2026-11-17); "
                   "GeoNames trazia 523.300, o valor de Kandahar"),
        "note": ("Q-016: GeoNames duplicava a população de Kandahar (523.300); "
                 "CSO projeta 484.500 para Mazār-e Sharīf"),
    },
    # recal3 (2026-11-17): `sim_population` com a população da UNIDADE
    # ADMINISTRATIVA (prefeitura/distrito/província) no lugar da aglomeração.
    # O valor de referência é `tools/agglomerations_2026.csv` — a mesma tabela
    # que o pipeline já usa para `agglomeration_population` (snapshot de
    # Thomas Brinkhoff / citypopulation.de, 2026-01-01).
    "CHN-WEI": {
        "population": 3375000,
        "population_year": 2026,
        "method": "agglomeration_table",
        "confidence": "high",
        "source": ("citypopulation.de (T. Brinkhoff), 2026-01-01: Wéifāng "
                   "3.375.000 (Adm-Urb), via tools/agglomerations_2026.csv, "
                   "rank 178; GeoNames trazia 9.386.705 = população da "
                   "PREFEITURA (censo 2020)"),
        "note": ("Q-016 §4.3 propôs; recal3 aplicou. 9.386.705 é a prefeitura "
                 "inteira, não a cidade"),
    },
    "PAK-PES": {
        "population": 2175000,
        "population_year": 2026,
        "method": "agglomeration_table",
        "confidence": "high",
        "source": ("citypopulation.de (T. Brinkhoff), 2026-01-01: Peshāwar "
                   "2.175.000 (CUA), via tools/agglomerations_2026.csv, rank "
                   "282; GeoNames trazia 4.758.762 = DISTRITO (censo 2023)"),
        "note": ("Q-016 §4.3 propôs; recal3 aplicou. 4.758.762 é o distrito "
                 "inteiro, não a cidade"),
    },
    "CHN-HAR": {
        "population": 5600000,
        "population_year": 2026,
        "method": "agglomeration_table",
        "confidence": "high",
        "source": ("citypopulation.de (T. Brinkhoff), 2026-01-01: Hā'ĕrbīn "
                   "5.600.000 (Adm-Urb), via tools/agglomerations_2026.csv, "
                   "rank 100; o cenário trazia 10.009.854 = prefeitura "
                   "(censo 2020)"),
        "note": ("recal3: mesmo padrão de Weifang (unidade administrativa); "
                 "razão sim/aglomeração 1,79"),
    },
    "TUR-DIYA": {
        "population": 1120000,
        "population_year": 2026,
        "method": "agglomeration_table",
        "confidence": "high",
        "source": ("citypopulation.de (T. Brinkhoff), 2026-01-01: Diyarbakır "
                   "1.120.000 (Aggl), via tools/agglomerations_2026.csv, rank "
                   "585; o cenário trazia 1.833.684 = PROVÍNCIA"),
        "note": ("recal3: mesmo padrão de Weifang (unidade administrativa); "
                 "razão sim/aglomeração 1,64"),
    },
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
    # ---------------------------------------------------------------- r8 (2026-11-13, item 14)
    # Auditoria das 10 maiores aglomerações de 2026-01-01 (citypopulation.de)
    # nas 67 nações com área >= 500.000 km² ou população >= 30 M. Entram as
    # ausentes > 1 M hab. a >= 150 km de qualquer cidade do cenário; a lista
    # completa e as exclusões de borda estão em docs/plans/world_all_data.md §21.
    {
        "nation": "ESP",
        "id": "ESP-SEV",
        "name_en": "Seville",
        "name_pt": "Sevilha",
        "name_es": "Sevilla",
        "lat": 37.3886,
        "lon": -5.9950,
        "population": 1310000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "continent",
        "pop_src": "agglomeration",
        "note": ("curadoria r8 (item 14a): pedido do usuário; 4ª aglomeração "
                 "da Espanha, a 147,4 km de Algeciras (a regra de 150 km do "
                 "item 14b é para a revisão automática, não para o pedido). "
                 "Porto fluvial do Guadalquivir representado na foz (~80 km), "
                 "porque o nav_mask não carrega o rio; Cádiz não entra"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Sevilla 1.310.000; "
                   "coordenadas 37.3886N 5.9950W (Wikidata Q8717)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ESP",
        "id": "ESP-MLG",
        "name_en": "Málaga",
        "name_pt": "Málaga",
        "name_es": "Málaga",
        "lat": 36.7167,
        "lon": -4.4167,
        "population": 1120000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "continent",
        "pop_src": "agglomeration",
        "note": ("curadoria r8 (item 14a): pedido do usuário; 5ª aglomeração "
                 "da Espanha, a 112,7 km de Algeciras; porto próprio (Porto de "
                 "Málaga, port_of = ESP-MLG)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Málaga 1.120.000; "
                   "coordenadas 36.7167N 4.4167W (Wikidata Q8851)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "DEU",
        "id": "DEU-NUR",
        "name_en": "Nuremberg",
        "name_pt": "Nuremberga",
        "name_es": "Núremberg",
        "lat": 49.4539,
        "lon": 11.0775,
        "population": 1130000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 8ª aglomeração da Alemanha, a 150,7 km de "
                 "Munique (a >1 M mais próxima) — preenche o vazio da Francônia"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Nürnberg 1.130.000; "
                   "coordenadas 49.4539N 11.0775E (Wikidata Q2090)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "USA",
        "id": "USA-BOS",
        "name_en": "Boston",
        "name_pt": "Boston",
        "name_es": "Boston",
        "lat": 42.3603,
        "lon": -71.0578,
        "population": 7850000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "north_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 6ª aglomeração dos EUA, a 158,6 km de "
                 "Portland (ME), a cidade mais próxima do cenário; porto "
                 "legado USA-BOS passa a ela (PORT_OF_OVERRIDES)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Boston 7.850.000; "
                   "coordenadas 42.3603N 71.0578W (Wikidata Q100)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "MAR",
        "id": "MAR-FEZ",
        "name_en": "Fez",
        "name_pt": "Fez",
        "name_es": "Fez",
        "lat": 34.0333,
        "lon": -5.0000,
        "population": 1330000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "africa",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 4ª aglomeração de Marrocos, a 168,9 km de "
                 "Rabat; fecha o vazio do interior norte (Fez-Meknès)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Fès 1.330.000; "
                   "coordenadas 34.0333N 5.0000W (Wikidata Q233633)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ITA",
        "id": "ITA-PADU",
        "name_en": "Padua",
        "name_pt": "Pádua",
        "name_es": "Padua",
        "lat": 45.4078,
        "lon": 11.8733,
        "population": 1060000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "continent",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 5ª aglomeração da Itália, a 170 km de San "
                 "Marino; o porto de Veneza/Pádua (ITA-PAD) deixa de ir para "
                 "Milão e passa a Pádua (PORT_OF_OVERRIDES)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Padova 1.060.000; "
                   "coordenadas 45.4078N 11.8733E (Wikidata Q617)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "USA",
        "id": "USA-MIA",
        "name_en": "Miami",
        "name_pt": "Miami",
        "name_es": "Miami",
        "lat": 25.7742,
        "lon": -80.1936,
        "population": 6700000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "north_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 10ª aglomeração dos EUA, a 174 km da cidade "
                 "mais próxima do cenário (Lucaya, Bahamas); porto legado "
                 "USA-MIA passa a ela (PORT_OF_OVERRIDES)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Miami 6.700.000; "
                   "coordenadas 25.7742N 80.1936W (Wikidata Q8652)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "MYS",
        "id": "MYS-GEO",
        "name_en": "George Town",
        "name_pt": "George Town",
        "name_es": "George Town",
        "lat": 5.4145,
        "lon": 100.3292,
        "population": 2100000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "asia",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 2ª aglomeração da Malásia, a 177,7 km de Hat "
                 "Yai (Tailândia); capital de Penang, com porto em Butterworth "
                 "(port_of = MYS-GEO)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): George Town 2.100.000; "
                   "coordenadas 5.4145N 100.3292E (Wikidata Q61092)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "ARG",
        "id": "ARG-MEN",
        "name_en": "Mendoza",
        "name_pt": "Mendoza",
        "name_es": "Mendoza",
        "lat": -32.8897,
        "lon": -68.8444,
        "population": 1190000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "south_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 4ª aglomeração da Argentina, a 179,2 km de "
                 "Santiago (Chile); fecha o vazio de Cuyo, aos pés dos Andes"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Mendoza 1.190.000; "
                   "coordenadas 32.8897S 68.8444W (Wikidata Q44237)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "UZB",
        "id": "UZB-SAM",
        "name_en": "Samarkand",
        "name_pt": "Samarcanda",
        "name_es": "Samarcanda",
        "lat": 39.6547,
        "lon": 66.9758,
        "population": 1230000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "asia",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 3ª aglomeração do Uzbequistão, a 199,5 km de "
                 "Dushanbe (Tajiquistão); fecha o vazio entre Tashkent e a "
                 "fronteira tajique"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Samarqand 1.230.000; "
                   "coordenadas 39.6547N 66.9758E (Wikidata Q5753)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "BOL",
        "id": "BOL-COC",
        "name_en": "Cochabamba",
        "name_pt": "Cochabamba",
        "name_es": "Cochabamba",
        "lat": -17.3935,
        "lon": -66.1570,
        "population": 1290000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "south_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 3ª aglomeração da Bolívia, a 205,8 km de "
                 "Sucre; fecha o vale central entre La Paz e Santa Cruz"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Cochabamba 1.290.000; "
                   "coordenadas 17.3935S 66.1570W (Wikidata Q183124)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "FRA",
        "id": "FRA-BOR",
        "name_en": "Bordeaux",
        "name_pt": "Bordéus",
        "name_es": "Burdeos",
        "lat": 44.8378,
        "lon": -0.5794,
        "population": 1170000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 6ª aglomeração da França, a 211,9 km de "
                 "Toulouse; preenche o vazio do sudoeste (Aquitânia). Porto "
                 "de estuário (Gironde), não costeiro: sem ponto no cenário"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Bordeaux 1.170.000; "
                   "coordenadas 44.8378N 0.5794W (Wikidata Q1479)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "MAR",
        "id": "MAR-RAK",
        "name_en": "Marrakesh",
        "name_pt": "Marraquexe",
        "name_es": "Marrakech",
        "lat": 31.6295,
        "lon": -7.9811,
        "population": 1150000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "africa",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 6ª aglomeração de Marrocos, a 220,5 km de "
                 "Casablanca; centro do sul (Marrakech-Safi)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Marrakech 1.150.000; "
                   "coordenadas 31.6295N 7.9811W (Wikidata Q101625)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "TUR",
        "id": "TUR-KON",
        "name_en": "Konya",
        "name_pt": "Cônia",
        "name_es": "Konya",
        "lat": 37.8728,
        "lon": 32.4921,
        "population": 1420000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "asia",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 8ª aglomeração da Turquia, a 229,4 km de "
                 "Ankara; maior cidade da Anatólia Central"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Konya 1.420.000; "
                   "coordenadas 37.8728N 32.4921E (Wikidata Q79857)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "RUS",
        "id": "RUS-KRA",
        "name_en": "Krasnodar",
        "name_pt": "Krasnodar",
        "name_es": "Krasnodar",
        "lat": 45.0333,
        "lon": 38.9833,
        "population": 1390000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "continent",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: top-10 da Rússia (1,39 M), a 249,2 km de "
                 "Rostov-on-Don; maior cidade do Kuban, no sul russo. A "
                 "Rússia já tinha 44 cidades (o teto histórico de 40 foi "
                 "superado pelas forçadas da r2); esta é a 45ª, explícita"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Krasnodar 1.390.000; "
                   "coordenadas 45.0333N 38.9833E (Wikidata Q3646)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "AGO",
        "id": "AGO-BEN",
        "name_en": "Benguela",
        "name_pt": "Benguela",
        "name_es": "Benguela",
        "lat": -12.5500,
        "lon": 13.4167,
        "population": 1370000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "africa",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 2ª aglomeração de Angola, a 253,2 km de "
                 "Huambo; porto de Benguela (port_of = AGO-BEN)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Benguela 1.370.000; "
                   "coordenadas 12.5500S 13.4167E (Wikidata Q214248)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "BOL",
        "id": "BOL-SCZ",
        "name_en": "Santa Cruz de la Sierra",
        "name_pt": "Santa Cruz de la Sierra",
        "name_es": "Santa Cruz de la Sierra",
        "lat": -17.7892,
        "lon": -63.1975,
        "population": 1990000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "south_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 1ª aglomeração da Bolívia, a 258,1 km de "
                 "Sucre; maior cidade do país e centro das terras baixas"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Santa Cruz 1.990.000; "
                   "coordenadas 17.7892S 63.1975W (Wikidata Q170688)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "CHN",
        "id": "CHN-XIA",
        "name_en": "Xiamen",
        "name_pt": "Xiamen",
        "name_es": "Xiamen",
        "lat": 24.4797,
        "lon": 118.0819,
        "population": 15800000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "asia",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 5ª aglomeração da China, a 266,5 km de "
                 "Taipei; fecha o vazio do litoral de Fujian (Xiamen-Quanzhou-"
                 "Putian); porto legado CHN-XIA passa a ela"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Xiàmén 15.800.000; "
                   "coordenadas 24.4797N 118.0819E (Wikidata Q68744)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "IRN",
        "id": "IRN-KSH",
        "name_en": "Kermanshah",
        "name_pt": "Kermanshah",
        "name_es": "Kermanshah",
        "lat": 34.3167,
        "lon": 47.0686,
        "population": 1160000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "asia",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 8ª aglomeração do Irã, a 269,0 km de Bagdá; "
                 "fecha o vazio do oeste iraniano (Kermanshah)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Kermānshāh 1.160.000; "
                   "coordenadas 34.3167N 47.0686E (Wikidata Q180078)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "COL",
        "id": "COL-BUC",
        "name_en": "Bucaramanga",
        "name_pt": "Bucaramanga",
        "name_es": "Bucaramanga",
        "lat": 7.1186,
        "lon": -73.1161,
        "population": 1400000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "south_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 5ª aglomeração da Colômbia, a 288,1 km de "
                 "Medellín; capital de Santander, no vale do Magdalena"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Bucaramanga 1.400.000; "
                   "coordenadas 7.1186N 73.1161W (Wikidata Q243766)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "JPN",
        "id": "JPN-SEN",
        "name_en": "Sendai",
        "name_pt": "Sendai",
        "name_es": "Sendai",
        "lat": 38.2682,
        "lon": 140.8694,
        "population": 1520000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "japan",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 9ª aglomeração do Japão, a 305 km de Tóquio; "
                 "maior cidade de Tohoku; porto legado JPN-SEN passa a ela"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Sendai 1.520.000; "
                   "coordenadas 38.2682N 140.8694E (Wikidata Q46747)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "TUR",
        "id": "TUR-ANT",
        "name_en": "Antalya",
        "name_pt": "Antália",
        "name_es": "Antalya",
        "lat": 36.9081,
        "lon": 30.6956,
        "population": 1430000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "asia",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 7ª aglomeração da Turquia, a 307 km de "
                 "Nicósia; maior cidade da costa mediterrânea turca, com "
                 "porto próprio (port_of = TUR-ANT)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Antalya 1.430.000; "
                   "coordenadas 36.9081N 30.6956E (Wikidata Q6487)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "MEX",
        "id": "MEX-TOR",
        "name_en": "Torreón",
        "name_pt": "Torreón",
        "name_es": "Torreón",
        "lat": 25.5439,
        "lon": -103.4190,
        "population": 1460000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "north_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 10ª aglomeração do México, a 311,3 km de "
                 "Monterrey; centro da Comarca Lagunera, no norte árido"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Torreón 1.460.000; "
                   "coordenadas 25.5439N 103.4190W (Wikidata Q386991)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "COL",
        "id": "COL-CUC",
        "name_en": "Cúcuta",
        "name_pt": "Cúcuta",
        "name_es": "Cúcuta",
        "lat": 7.9075,
        "lon": -72.5047,
        "population": 1160000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "south_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 7ª aglomeração da Colômbia, a 318,2 km de "
                 "Maracaibo; cidade de fronteira com a Venezuela (~15 km) — "
                 "exceção explícita de curadoria, como Brest/Białystok na r2"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Cúcuta 1.160.000; "
                   "coordenadas 7.9075N 72.5047W (Wikidata Q216847)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "MAR",
        "id": "MAR-AGA",
        "name_en": "Agadir",
        "name_pt": "Agadir",
        "name_es": "Agadir",
        "lat": 30.4214,
        "lon": -9.5831,
        "population": 1300000,
        "population_year": 2026,
        "capital": "0",
        "port": "yes",
        "role": "port",
        "landmass": "africa",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 5ª aglomeração de Marrocos, a 336,2 km de "
                 "Tindouf (Argélia); maior cidade do sul, com porto próprio "
                 "(port_of = MAR-AGA)"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Agadir 1.300.000; "
                   "coordenadas 30.4214N 9.5831W (Wikidata Q170525)"),
        "legacy": "no",
        "initial_farm_tons": "",
    },
    {
        "nation": "MEX",
        "id": "MEX-JUZ",
        "name_en": "Ciudad Juárez",
        "name_pt": "Cidade Juárez",
        "name_es": "Ciudad Juárez",
        "lat": 31.6685,
        "lon": -106.4200,
        "population": 1660000,
        "population_year": 2026,
        "capital": "0",
        "port": "no",
        "role": "city",
        "landmass": "north_america",
        "pop_src": "agglomeration",
        "note": ("curadoria r8: 9ª aglomeração do México, a 338,8 km de "
                 "Chihuahua; aglomeração transfronteiriça com El Paso (EUA) — "
                 "exceção explícita de curadoria"),
        "source": ("Thomas Brinkhoff, Major Agglomerations of the World, "
                   "citypopulation.de (2026-01-01): Ciudad Juárez 1.660.000; "
                   "coordenadas 31.6685N 106.4200W (Wikidata Q26590)"),
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
    # r10 (recal3, 2026-11-17): o Cáspio é MAR FECHADO no modelo — no
    # `nav_mask.png` a bacia é um componente de água próprio (2.183 células)
    # separado do oceano (2.110.356), então um porto em Baku/Sumqayit não tem
    # rota marítima nenhuma. Com o rótulo corrigido para `asia` (a massa da
    # cidade) o sim passava a abrir comércio oceânico por uma rota que não
    # existe: o Azerbaijão compra comida no mercado mundial e LBY/EGY pioram
    # (medido: LBY 1,000 -> 0,878 na média de 30 dias; sem os dois portos,
    # LBY 1,000). O Azerbaijão não tem porto oceânico; os dois saem do cenário.
    "AZE-BAK": "porto no Cáspio (mar fechado; sem rota até o oceano)",
    "AZE-SUM": "porto no Cáspio (mar fechado; sem rota até o oceano)",
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
    # Pádua/Veneza: o porto fica no litoral de Veneza. Na r3, Pádua não estava
    # no roster e o porto ia para Milão; na r8 a cidade `ITA-PADU` entra e o
    # porto passa a ela (a cidade mais próxima do porto no roster).
    "ITA-PAD": "ITA-PADU",
    # r8 (item 14): portos legados de cidades que entraram no cenário. Sem o
    # override, `port_of` continuaria apontando para a cidade antiga mais
    # próxima (Portland, Jacksonville, Hong Kong, Tóquio), que não é o porto.
    "USA-BOS": "USA-BOS",
    "USA-MIA": "USA-MIA",
    "CHN-XIA": "CHN-XIA",
    "JPN-SEN": "JPN-SEN",
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
    # ---------------------------------------------------------------- r8 (2026-11-13, item 14)
    # Portos novos das cidades costeiras da r8. O ponto é a costa/porto real,
    # não o centro da cidade; o nav_mask alcança a célula pelo mar em frente.
    {
        "nation": "ESP",
        "id": "ESP-SEV",
        "name": "Seville",
        "lat": 36.7750,
        "lon": -6.3550,
        "landmass": "continent",
        "port_of": "ESP-SEV",
        "source": ("foz do Guadalquivir, Sanlúcar de Barrameda (36.775N "
                   "6.355W): acesso marítimo do porto fluvial de Sevilha, a "
                   "~80 km da cidade; port_of = ESP-SEV"),
    },
    {
        "nation": "ESP",
        "id": "ESP-MLG",
        "name": "Málaga",
        "lat": 36.7120,
        "lon": -4.4150,
        "landmass": "continent",
        "port_of": "ESP-MLG",
        "source": ("Porto de Málaga, costa mediterrânea (36.712N 4.415W); "
                   "port_of = ESP-MLG"),
    },
    {
        "nation": "MYS",
        "id": "MYS-GEO",
        "name": "George Town",
        "lat": 5.3940,
        "lon": 100.3660,
        "landmass": "asia",
        "port_of": "MYS-GEO",
        "source": ("Porto de Penang, Butterworth (5.394N 100.366E); "
                   "port_of = MYS-GEO"),
    },
    {
        "nation": "AGO",
        "id": "AGO-BEN",
        "name": "Benguela",
        "lat": -12.5780,
        "lon": 13.4050,
        "landmass": "africa",
        "port_of": "AGO-BEN",
        "source": ("Porto de Benguela, costa atlântica (12.578S 13.405E); "
                   "port_of = AGO-BEN"),
    },
    {
        "nation": "TUR",
        "id": "TUR-ANT",
        "name": "Antalya",
        "lat": 36.8850,
        "lon": 30.7050,
        "landmass": "asia",
        "port_of": "TUR-ANT",
        "source": ("Porto de Antalya, costa mediterrânea (36.885N 30.705E); "
                   "port_of = TUR-ANT"),
    },
    {
        "nation": "MAR",
        "id": "MAR-AGA",
        "name": "Agadir",
        "lat": 30.4200,
        "lon": -9.6050,
        "landmass": "africa",
        "port_of": "MAR-AGA",
        "source": ("Porto de Agadir, costa atlântica (30.420N 9.605W); "
                   "port_of = MAR-AGA"),
    },
    # ---------------------------------------------------------------- r9 (2026-11-17)
    # Singapura: erro de dado (a nação não tinha porto nenhum). O ponto é o cais
    # de Tanjong Pagar, na costa sul voltada para o Estreito de Singapura —
    # água do componente principal no nav_mask, 4,9 km da costa no atlas.
    {
        "nation": "SGP",
        "id": "SGP-SIN",
        "name": "Singapore",
        "lat": 1.2626,
        "lon": 103.8544,
        "landmass": "singapore",
        "port_of": "SGP-SIN",
        "source": ("PSA Singapore, Tanjong Pagar Terminal, Estreito de "
                   "Singapura (1.2626N 103.8544E); port_of = SGP-SIN"),
    },
    # ---------------------------------------------------------------- r10 (2026-11-17)
    # Nações-ilha/costeiras sem porto (§13 do recal_areas_pop.md). Um porto
    # principal por nação, na cidade do roster mais adequada. Cada ponto é o
    # cais/terminal da fonte; o par (célula de água no nav_mask, distância à
    # terra no world_sdf) está no relatório da r10.
    {
        "nation": "PHL",
        "id": "PHL-MAN",
        "name": "Manila",
        "lat": 14.5900,
        "lon": 120.9600,
        "landmass": "philippines",
        "port_of": "PHL-MAN",
        "source": ("Port of Manila (South Harbor), Baía de Manila "
                   "(14.5967N 120.9544E, artigo do porto); ponto deslocado "
                   "para a célula de água do nav_mask; port_of = PHL-MAN"),
    },
    {
        "nation": "CUB",
        "id": "CUB-HAV",
        "name": "Havana",
        "lat": 23.1450,
        "lon": -82.3700,
        "landmass": "cuba",
        "port_of": "CUB-HAV",
        "source": ("Havana Harbor, entrada da baía (23.1330N 82.3418W, artigo "
                   "Havana Harbor); ponto na boca do porto (23.1450N "
                   "82.3700W); port_of = CUB-HAV"),
    },
    {
        "nation": "NZL",
        "id": "NZL-AUC",
        "name": "Auckland",
        "lat": -36.8438,
        "lon": 174.7769,
        "landmass": "new_zealand",
        "port_of": "NZL-AUC",
        "source": ("Port of Auckland, Waitematā Harbour (-36.843774S "
                   "174.776859E, artigo do porto); port_of = NZL-AUC"),
    },
    {
        "nation": "MDG",
        "id": "MDG-TOA",
        "name": "Toamasina",
        "lat": -18.1553,
        "lon": 49.4267,
        "landmass": "madagascar",
        "port_of": "MDG-TOA",
        "source": ("Port autonome de Toamasina, bacia portuária (-18.15525S "
                   "49.426722E, artigo fr); port_of = MDG-TOA"),
    },
    {
        "nation": "JAM",
        "id": "JAM-KIN",
        "name": "Kingston",
        "lat": 17.9500,
        "lon": -76.8000,
        "landmass": "jamaica",
        "port_of": "JAM-KIN",
        "source": ("Kingston Harbour (17.95N 76.80W, artigo Kingston Harbour); "
                   "port_of = JAM-KIN"),
    },
    {
        "nation": "LKA",
        "id": "LKA-COL",
        "name": "Colombo",
        "lat": 6.9528,
        "lon": 79.8447,
        "landmass": "sri_lanka",
        "port_of": "LKA-COL",
        "source": ("Port of Colombo (6.952778N 79.844722E, artigo do porto); "
                   "port_of = LKA-COL"),
    },
    {
        "nation": "BHS",
        "id": "BHS-NAS",
        "name": "Nassau",
        "lat": 25.0791,
        "lon": -77.3466,
        "landmass": "bahamas",
        "port_of": "BHS-NAS",
        "source": ("Nassau Harbour, Prince George Wharf (25.08N 77.35W); "
                   "port_of = BHS-NAS"),
    },
    {
        "nation": "FJI",
        "id": "FJI-SUV",
        "name": "Suva",
        "lat": -18.1350,
        "lon": 178.4250,
        "landmass": "fiji",
        "port_of": "FJI-SUV",
        "source": ("Port of Suva, King's Wharf, baía de Suva (-18.135S "
                   "178.425E); port_of = FJI-SUV"),
    },
    {
        "nation": "TTO",
        "id": "TTO-POR",
        "name": "Port of Spain",
        "lat": 10.6600,
        "lon": -61.5200,
        "landmass": "trinidad",
        "port_of": "TTO-POR",
        "source": ("Port of Spain, Golfo de Paria (10.6667N 61.5167W, artigo "
                   "da cidade); ponto no cais (10.6600N 61.5200W); port_of = "
                   "TTO-POR"),
    },
    {
        "nation": "BRB",
        "id": "BRB-BRI",
        "name": "Bridgetown",
        "lat": 13.0969,
        "lon": -59.6131,
        "landmass": "barbados",
        "port_of": "BRB-BRI",
        "source": ("Bridgetown, Deep Water Harbour (13.096944N 59.613056W, "
                   "artigo da cidade); port_of = BRB-BRI"),
    },
    {
        "nation": "SLB",
        "id": "SLB-HON",
        "name": "Honiara",
        "lat": -9.4319,
        "lon": 159.9556,
        "landmass": "solomon",
        "port_of": "SLB-HON",
        "source": ("Honiara, Point Cruz (-9.431944S 159.955556E, artigo da "
                   "cidade); port_of = SLB-HON"),
    },
    {
        "nation": "BHR",
        "id": "BHR-MAN",
        "name": "Khalifa Bin Salman",
        "lat": 26.2000,
        "lon": 50.6200,
        "landmass": "asia",
        "port_of": "BHR-MAN",
        "source": ("Khalifa Bin Salman Port (BHKBS), Hidd — 26°12'N 050°37'E "
                   "(APM Terminals, KBSP Port Information); port_of = BHR-MAN"),
    },
    {
        "nation": "QAT",
        "id": "QAT-DOH",
        "name": "Hamad Port",
        "lat": 25.0150,
        "lon": 51.6020,
        "landmass": "asia",
        "port_of": "QAT-DOH",
        "source": ("Hamad Port, Umm Al-Houl ao sul de Doha (25.015N 51.602E); "
                   "port_of = QAT-DOH"),
    },
    {
        "nation": "BRN",
        "id": "BRN-BAN",
        "name": "Muara",
        "lat": 5.0242,
        "lon": 115.0705,
        "landmass": "asia",
        "port_of": "BRN-BAN",
        "source": ("Muara Port, baía de Brunei (5.0242489N 115.0704866E, "
                   "artigo do porto); port_of = BRN-BAN"),
    },
]
