# Dados de cultura — nações e cidades (v1.2)

Status: **dados apenas**. Nada aqui está ligado à economia, ao C++ ou à UI.
Aprovação: o usuário pediu explicitamente para não integrar agora.

## 1. Objetivo

Dar ao jogo uma camada **cultural e linguística** por nação e por cidade,
suficiente para, no futuro, alimentar satisfação, assimilação e ocupação sem
inventar números em tempo de execução.

**Regra de modelagem:** cultura é **identidade nacional/cultural** — o povo a que
a pessoa pertence (brasileiro, americano, mexicano, francês, catalão, curdo,
hauçá…) — **nunca raça, cor ou categoria censitária de "cor/raça"**. A v1.2
substitui as categorias raciais que existiam na v1.1 (`afro_latino`, `mestico`,
`afro_americano`, `anglo_americano`, `hispanico`) pela identidade nacional do
país, dá a cada nação uma cultura principal com nome de gentílico, troca `hindi`
por `hindustani` e adota as grandes identidades linguísticas da Índia.

Histórico: a v1 corrigiu a cobertura inicial; a v1.1 corrigiu dupla contagem de
subgrupos, separou as identidades nacionais hispano-americanas, atualizou cidades
afetadas pela guerra de 2022 e ampliou o dado regional em regiões multiétnicas.

## 2. Arquivos e contrato

Saída em `data/definitions/culture/` (o jogo ainda não lê nenhum deles):

| arquivo | linhas | conteúdo |
| --- | --- | --- |
| `cultures.csv` | 208 | `id,name_pt,name_en,name_es,family,language,religion,parent,color` |
| `cultures.json` | 208 | mesmo conteúdo, para consumo em GDScript |
| `nation_cultures.csv` | 179 | `nation,primary_culture,accepted_cultures,composition,source,year` |
| `city_cultures.csv` | 834 | `city_id,nation,city,cultures,subcultures,source` |

O esquema é **o mesmo da v1.1** (colunas atuais + `parent`/`subcultures`): só o
conteúdo mudou. Quem lê esses arquivos em paralelo não precisa adaptar código.

Regras de leitura:

- `composition` e `cultures` são listas `cultura:percentual` separadas por `;`.
- Toda lista soma **100**. Percentuais são inteiros (maior resto).
- `outras` é o resíduo, **não uma etnia**: nunca aparece em `cultures.csv` nem em
  `accepted_cultures`. Em `city_overrides_*.csv` ele pode ser declarado
  explicitamente para fechar a soma; nesse caso o build não acrescenta resíduo.
- `accepted_cultures` é lista separada por `;`, sem a cultura principal.
- `parent` (catálogo) marca subgrupo de outra cultura: `hokkien`, `cantones`,
  `hakka` e `teochew` têm `parent=han`. Nenhum mix pode conter uma cultura e seu
  ancestral (regra 1 do §3).
- `subcultures` (cidade, opcional) detalha subgrupos da cultura ancestral em
  percentual **da cidade**, no formato `hokkien:24;cantones:13`. O subgrupo
  precisa ter `parent` presente no mix e fração ≤ à do ancestral; não entra na
  soma de 100 nem conta como cultura nomeada.
- Em `city_cultures.csv`, `source = national_fallback` significa **sem dado
  regional**: a mistura é a composição nacional ajustada (ver §4). Qualquer outro
  valor é a fonte regional do dado curado; `estimate_post2022` marca estimativa
  pós-invasão da Ucrânia (ver §8).
- Religião é informativa. Não há regra de jogo baseada nela nesta versão.

## 3. Método

1. **Catálogo curado** (`tools/culture_sources/cultures.csv`): 208 culturas em 45
   famílias (eslava oriental, germânica ocidental, latina ibérica, semítica árabe,
   turcomana, banta, indo-ariana, dravidiana, sino-tibetana, caribenha,
   hispano-americana, anglo-oceânica, ameríndia etc.), com idioma principal,
   religião majoritária e `parent` quando for subgrupo.
2. **Composição nacional** (`tools/culture_sources/nation_cultures.csv`): uma
   linha por nação das 179 do cenário, com cultura principal, aceitas, composição,
   fonte e ano. A fonte é declarada por nação e aparece na tabela do §5.
3. **Dado regional por cidade** (`tools/culture_sources/city_overrides_*.csv`):
   321 cidades com censo/estimativa regional (Bálticos, Ucrânia, Rússia,
   Ásia Central, Cáucaso, Balcãs, Espanha, Bélgica, Canadá, China/Xinjiang/Tibete,
   Índia, Paquistão, Sudeste Asiático, Oriente Médio, América Latina, África,
   Caribe, Londres, Nova York e Alasca).
4. **Fallback** para as outras 513 cidades (§4).
5. `tools/build_cultures.py` gera tudo de forma determinística;
   `tools/validate_cultures.py` valida (inclusive a lista negada de categorias
   raciais e a existência de cultura principal nacional). `tools/validate_scenario_data.py`
   apenas **avisa** (não erra) se os arquivos de cultura faltarem.

### Regra 1 — exclusividade no mesmo nível

Culturas são mutuamente exclusivas no mesmo nível: um mix (nação ou cidade) não
pode conter `X` e `parent(X)`, nem avô/neto. A varredura cobre todas as 179 nações
e 834 cidades por pares pai/filho. Na v1, **3 nações** (CHN, SGP, TWN) e **29
cidades** (26 em `national_fallback`, mais Hong Kong, Macau e Singapura) tinham
par pai/filho; desde a v1.1 a contagem é **0** e o validador falha se um par novo
for introduzido. Os únicos pares existentes no catálogo são chineses
(`han` + `hokkien/cantones/hakka/teochew`); Singapura, Malásia, Hong Kong, Macau,
Taiwan, Guangzhou e Xiamen usam `han` agregado com `subcultures` detalhadas.

### Regra 2 — identidade nacional/cultural, não raça

O validador tem uma **lista negada** (por id e por termo em qualquer nome i18n):
`afro_latino`, `mestico`, `afro_americano`, `anglo_americano`, `hispanico`, além
de `branco`, `negro`, `pardo`, `afro*`, `mestiç*`, `racial`, `raça`. Se qualquer
uma reaparecer no catálogo, na composição nacional ou nas aceitas, a validação
falha. O mesmo vale para a cultura principal de cada nação, que precisa existir,
estar na composição com **fração ≥ 5%** e ser a maior cultura nomeada (exceções
documentadas: PAK, ARE e QAT — ver §5).

O que mudou na v1.2:

| antes (v1.1) | agora (v1.2) | onde |
| --- | --- | --- |
| `anglo_americano` + `afro_americano` + `hispanico` | `americano` (identidade nacional) + `mexicano_americano`, `porto_riquenho`, `cubano` (identidades culturais com origem própria) | USA, JAM, TTO, BHS |
| `afro_latino` | identidade nacional do país (`colombiano`, `cubano`, `dominicano`, `equatoriano`, `mexicano`, `panamenho`, `uruguaio`, `venezuelano`) | COL, CUB, DOM, ECU, MEX, PAN, URY, VEN |
| `mestico` | `belizense`, `guianense` (nacional) | BLZ, GUY |
| `caribenho` como principal de um Estado | gentílico do país (`jamaicano`, `trinitario`, `bahamense`, `barbadense`, `antiguano`, `dominiquense`, `granadino`, `sancristovense`, `santa_luciense`, `sao_vicentino`) | Caribe |
| `crioulo` como principal do Haiti | `haitiano` | HTI |
| `hindi` | `hindustani` (hindi-urdu) + `canares`, `malaiala` | IND, NPL, FJI, Guiana, Golfo, Paquistão |
| — | `australiano`, `neozelandes`, `aborigene`, `havaiano`, `nativo_americano`, `indigena_brasileiro`, `luxemburgues` | AUS, NZL, USA, BRA, LUX |

**Minorias reais mantidas** (identidade própria, com fonte): catalães, bascos,
galegos, quebequenses, curdos, uigures, tibetanos, tártaros (da Crimeia
inclusive), tâmeis do Sri Lanka, iorubás/hauçás/igbos, berberes/amazigues,
inuit-iúpik, maoris, povos maias, quíchuas, aimarás, guaranis, mapuches, nahuas,
moro, papuas e os subgrupos chineses. **Imigrantes** só entram quando o grupo é
grande e documentado: turcos na Alemanha, magrebinos na França, sul-asiáticos no
Golfo e em Londres, chineses no Sudeste Asiático, hispânicos em Nova York/Miami,
nordestinos de origem italiana/alemã/japonesa em São Paulo e Porto Alegre.

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"
& $py tools/build_cultures.py
& $py tools/validate_cultures.py
& $py tools/validate_scenario_data.py
```


## 4. Fallback nacional ajustado

Sem dado regional, a cidade parte da composição nacional e é **achatada** para
refletir que capitais e metrópoles são mais diversas:

```
peso k = f(população) + 0,08 se capital, limitado a 0,30
f(população) = 0,22 (>= 5 mi) | 0,14 (>= 1 mi) | 0,08 (>= 250 mil) | 0
share' = share^(1 - k), renormalizado para 100
```

Depois disso ficam no máximo **5 culturas nomeadas + `outras`** (o resto é somado
em `outras`). O ajuste é declarado, determinístico e não cria cultura que a nação
não tenha. Ele **não é dado real** — por isso `source=national_fallback`.

## 5. Fontes por nação

Fonte e ano são os declarados na curadoria. Onde o país não publica censo étnico,
a linha usa a estimativa de língua/nacionalidade mais próxima e isso está explícito
no texto da fonte; onde nem isso existe, a composição é a melhor agregação
publicada e o ano indica o censo-base.

A coluna **principal** é sempre uma identidade nacional/cultural (gentílico quando
o país tem uma identidade nacional consolidada: `brasileiro`, `americano`,
`mexicano`, `canadense_ingles`, `luxemburgues`, `haitiano`, `jamaicano`…). Em
Estados multiétnicos sem identidade nacional majoritária na fonte (Nigéria, Angola,
Quênia, África do Sul…), a principal é **o maior grupo cultural publicado** — é o
que a fonte mede, não um gentílico inventado. Exceções declaradas à regra "a
principal é a maior cultura nomeada": `PAK` (urdu é a identidade nacional e a
maior língua materna é o punjabi), `ARE` e `QAT` (a identidade nacional dos
cidadãos é menor que o conjunto de residentes estrangeiros).

| nação | principal | composição | fonte | ano |
| --- | --- | --- | --- | --- |
| AFG | pashto | pashto:45;tadjique:27;uzbeque:9;turcomeno:3;outras:16 | CIA World Factbook 2024 | 2024 |
| AGO | ovimbundu | ovimbundu:37;mbundu:25;kikongo:13;outras:25 | CIA World Factbook 2024 | 2024 |
| ALB | albanes | albanes:83;grego:1;outras:16 | CIA World Factbook 2024 | 2024 |
| ARE | arabe_peninsular | arabe_peninsular:12;hindustani:30;urdu:15;punjabi:8;filipino:6;outras:29 | CIA World Factbook 2024 (residentes) | 2024 |
| ARG | argentino | argentino:60;italiano:20;alemao:3;arabe_levantino:3;outras:14 | INDEC 2022 (ancestralidade declarada) | 2022 |
| ARM | armenio | armenio:98;outras:2 | Censo 2022 (Armstat) | 2022 |
| ATG | antiguano | antiguano:91;outras:9 | CIA World Factbook 2024 (nacionalidade) | 2024 |
| AUS | australiano | australiano:60;irlandes:9;escoces:8;han:5;italiano:3;aborigene:3;outras:12 | Censo 2021 (ABS, ancestralidade) | 2021 |
| AUT | alemao | alemao:80;turco:2;servio:2;croata:1;outras:15 | Statistik Austria 2023 | 2023 |
| AZE | azeri | azeri:92;russo:3;armenio:1;outras:4 | Censo 2019 (Azstat) | 2019 |
| BDI | grandes_lagos | grandes_lagos:85;frances:8;outras:7 | CIA World Factbook 2024 | 2024 |
| BEL | flamengo | flamengo:58;frances:28;valao:5;italiano:2;outras:7 | Statbel 2023 (origem) | 2023 |
| BEN | fon | fon:38;ioruba:15;fulani:10;outras:37 | CIA World Factbook 2024 | 2024 |
| BFA | mossi | mossi:52;fulani:8;outras:40 | CIA World Factbook 2024 | 2024 |
| BGD | bengali | bengali:98;outras:2 | Censo 2022 (BBS) | 2022 |
| BGR | bulgaro | bulgaro:76;turco:9;romeno:3;outras:12 | Censo 2021 (NSI) | 2021 |
| BHR | arabe_peninsular | arabe_peninsular:46;hindustani:20;urdu:8;filipino:6;outras:20 | CIA World Factbook 2024 | 2024 |
| BHS | bahamense | bahamense:95;outras:5 | Censo 2022 (nacionalidade) | 2022 |
| BIH | bosnio | bosnio:50;servio:31;croata:15;outras:4 | Censo 2013 | 2013 |
| BLR | bielorrusso | bielorrusso:84;russo:8;polones:3;outras:5 | Censo 2019 (Belstat) | 2019 |
| BLZ | belizense | belizense:53;crioulo:26;maia:11;outras:10 | Censo 2022 (SIB) | 2022 |
| BOL | boliviano | boliviano:45;quechua:35;aimara:15;outras:5 | Censo 2012 (INE) | 2012 |
| BRA | brasileiro | brasileiro:96;indigena_brasileiro:1;portugues:1;italiano:1;outras:1 | Censo 2022 (IBGE, povos indígenas) + dados de nacionalidade | 2022 |
| BRB | barbadense | barbadense:95;outras:5 | Censo 2021 (nacionalidade) | 2021 |
| BRN | malaio | malaio:65;han:10;outras:25 | CIA World Factbook 2024 | 2024 |
| BTN | tibetano | tibetano:60;nepali:30;outras:10 | Censo 2017 | 2017 |
| BWA | tsuana | tsuana:79;outras:21 | Censo 2022 | 2022 |
| CAF | sango | sango:50;outras:50 | CIA World Factbook 2024 | 2024 |
| CAN | canadense_ingles | canadense_ingles:55;quebequense:22;inuit:2;outras:21 | Censo 2021 (Statistics Canada) | 2021 |
| CHE | alemao | alemao:62;frances:23;italiano:8;outras:7 | Censo 2020 (BFS) | 2020 |
| CHL | chileno | chileno:87;mapuche:10;outras:3 | Censo 2017 (INE) | 2017 |
| CHN | han | han:91;uigur:1;tibetano:1;mongol:1;outras:6 | Censo 2020 (NBS) | 2020 |
| CIV | akan | akan:38;bambara:15;mossi:10;outras:37 | CIA World Factbook 2024 | 2024 |
| CMR | fang | fang:20;fulani:10;hausea:5;outras:65 | CIA World Factbook 2024 | 2024 |
| COD | lingala | lingala:35;kikongo:20;luba:20;suaili:10;outras:15 | CIA World Factbook 2024 | 2024 |
| COG | kikongo | kikongo:45;lingala:30;outras:25 | CIA World Factbook 2024 | 2024 |
| COL | colombiano | colombiano:93;outras:7 | Censo 2018 (DANE, identidade nacional) | 2018 |
| CRI | costa_riquenho | costa_riquenho:85;crioulo:5;outras:10 | Censo 2022 (INEC) | 2022 |
| CUB | cubano | cubano:95;outras:5 | Censo 2012 (ONEI, nacionalidade) | 2012 |
| CYP | grego | grego:75;turco:18;outras:7 | Censo 2021 | 2021 |
| CZE | tcheco | tcheco:88;eslovaco:3;ucraniano:2;outras:7 | Censo 2021 | 2021 |
| DEU | alemao | alemao:80;turco:7;polones:2;russo:2;outras:9 | Destatis 2023 (origem) | 2023 |
| DJI | somali | somali:60;afar:35;outras:5 | CIA World Factbook 2024 | 2024 |
| DMA | dominiquense | dominiquense:95;outras:5 | Censo 2011 (nacionalidade) | 2011 |
| DNK | dinamarques | dinamarques:86;turco:2;alemao:1;outras:11 | Statistics Denmark 2023 | 2023 |
| DOM | dominicano | dominicano:90;haitiano:7;outras:3 | Censo 2022 (ONE) | 2022 |
| DZA | arabe_magrebino | arabe_magrebino:72;berbere:27;outras:1 | CIA World Factbook 2024 | 2024 |
| ECU | equatoriano | equatoriano:85;quechua:8;outras:7 | Censo 2022 (INEC) | 2022 |
| EGY | arabe_egipcio | arabe_egipcio:95;outras:5 | CIA World Factbook 2024 | 2024 |
| ERI | tigrinio | tigrinio:55;outras:45 | CIA World Factbook 2024 | 2024 |
| ESP | castelhano | castelhano:70;catalao:18;galego:6;basco:2;outras:4 | Censo 2021 (INE, língua e identidade) | 2021 |
| EST | estoniano | estoniano:69;russo:24;ucraniano:2;bielorrusso:1;outras:4 | Censo 2021 | 2021 |
| ETH | oromo | oromo:35;amhara:30;tigrinio:6;somali:6;outras:23 | Censo 2007 (CSA) | 2007 |
| FIN | finlandes | finlandes:90;sueco:5;russo:2;outras:3 | Statistics Finland 2023 | 2023 |
| FJI | fijiano | fijiano:57;hindustani:37;outras:6 | Censo 2017 | 2017 |
| FRA | frances | frances:75;arabe_magrebino:7;portugues:2;italiano:2;outras:14 | INSEE 2023 (origem/nacionalidade) | 2023 |
| GAB | fang | fang:32;kikongo:10;frances:5;outras:53 | CIA World Factbook 2024 | 2024 |
| GBR | ingles | ingles:75;escoces:8;gales:5;norte_irlandes:3;outras:9 | Censo 2021 (ONS) | 2021 |
| GEO | georgiano | georgiano:87;azeri:6;armenio:4;outras:3 | Censo 2014 (Geostat) | 2014 |
| GHA | akan | akan:47;ewe:14;mossi:6;outras:33 | Censo 2021 | 2021 |
| GIN | fulani | fulani:40;bambara:25;outras:35 | CIA World Factbook 2024 | 2024 |
| GMB | wolof | wolof:35;fulani:25;outras:40 | Censo 2013 | 2013 |
| GNB | crioulo | crioulo:30;fulani:25;outras:45 | CIA World Factbook 2024 | 2024 |
| GNQ | fang | fang:85;outras:15 | CIA World Factbook 2024 | 2024 |
| GRC | grego | grego:91;albanes:4;outras:5 | Censo 2021 | 2021 |
| GRD | granadino | granadino:95;outras:5 | Censo 2011 (nacionalidade) | 2011 |
| GTM | guatemalteco | guatemalteco:60;maia:40 | Censo 2018 (INE) | 2018 |
| GUY | guianense | guianense:50;hindustani:40;outras:10 | Censo 2012 (Bureau of Statistics) | 2012 |
| HND | hondurenho | hondurenho:85;maia:7;crioulo:5;outras:3 | Censo 2013 (INE) | 2013 |
| HRV | croata | croata:90;servio:4;bosnio:2;outras:4 | Censo 2021 | 2021 |
| HTI | haitiano | haitiano:95;frances:3;outras:2 | CIA World Factbook 2024 | 2024 |
| HUN | hungaro | hungaro:85;romeno:2;outras:13 | Censo 2022 | 2022 |
| IDN | javanes | javanes:40;malaio:15;sunda:10;han:3;papua:2;outras:30 | Censo 2020 (BPS) | 2020 |
| IND | hindustani | hindustani:48;bengali:8;marata:7;telugu:7;tamil:6;gujarati:5;canares:4;malaiala:3;punjabi:3;outras:9 | Censo 2011 (linguística) | 2011 |
| IRL | irlandes | irlandes:85;ingles:12;outras:3 | Censo 2022 | 2022 |
| IRN | persa | persa:61;azeri:16;curdo:10;outras:13 | CIA World Factbook 2024 | 2024 |
| IRQ | arabe_iraquiano | arabe_iraquiano:75;curdo:18;turcomeno:2;outras:5 | CIA World Factbook 2024 | 2024 |
| ISL | islandes | islandes:86;polones:5;outras:9 | Statistics Iceland 2023 | 2023 |
| ISR | hebraico | hebraico:74;arabe_levantino:21;outras:5 | CBS 2023 | 2023 |
| ITA | italiano | italiano:88;romeno:3;albanes:2;outras:7 | ISTAT 2023 | 2023 |
| JAM | jamaicano | jamaicano:95;outras:5 | Censo 2011 (nacionalidade) | 2011 |
| JOR | arabe_levantino | arabe_levantino:90;curdo:3;outras:7 | CIA World Factbook 2024 | 2024 |
| JPN | japones | japones:98;coreano:1;outras:1 | Censo 2020 | 2020 |
| KAZ | cazaque | cazaque:69;russo:18;uzbeque:3;uigur:2;outras:8 | Censo 2021 | 2021 |
| KEN | kikuyu | kikuyu:17;luo:10;suaili:8;somali:5;outras:60 | Censo 2019 | 2019 |
| KGZ | quirguiz | quirguiz:74;uzbeque:15;russo:5;outras:6 | Censo 2022 | 2022 |
| KHM | khmer | khmer:95;vietnamita:2;outras:3 | Censo 2019 | 2019 |
| KNA | sancristovense | sancristovense:92;outras:8 | Censo 2011 (nacionalidade) | 2011 |
| KOR | coreano | coreano:96;han:2;outras:2 | Censo 2023 | 2023 |
| KWT | arabe_peninsular | arabe_peninsular:30;hindustani:25;urdu:10;arabe_egipcio:10;outras:25 | CIA World Factbook 2024 | 2024 |
| LAO | lao | lao:65;khmer:3;outras:32 | Censo 2015 | 2015 |
| LBN | arabe_levantino | arabe_levantino:80;armenio:5;outras:15 | CIA World Factbook 2024 | 2024 |
| LBR | kru | kru:60;crioulo:5;outras:35 | CIA World Factbook 2024 | 2024 |
| LBY | arabe_magrebino | arabe_magrebino:80;berbere:15;outras:5 | CIA World Factbook 2024 | 2024 |
| LCA | santa_luciense | santa_luciense:95;outras:5 | Censo 2010 (nacionalidade) | 2010 |
| LKA | cingales | cingales:75;tamil:15;outras:10 | Censo 2012 | 2012 |
| LSO | soto | soto:99;outras:1 | Censo 2016 | 2016 |
| LTU | lituano | lituano:84;polones:6;russo:5;outras:5 | Censo 2021 | 2021 |
| LUX | luxemburgues | luxemburgues:53;portugues:15;frances:7;italiano:4;alemao:2;outras:19 | STATEC 2023 (nacionalidade) | 2023 |
| LVA | leto | leto:62;russo:25;bielorrusso:3;ucraniano:3;outras:7 | Censo 2021 | 2021 |
| MAR | arabe_magrebino | arabe_magrebino:70;berbere:28;outras:2 | CIA World Factbook 2024 | 2024 |
| MDA | romeno | romeno:75;ucraniano:8;russo:6;outras:11 | Censo 2014 | 2014 |
| MDG | malgaxe | malgaxe:95;frances:2;outras:3 | Censo 2018 | 2018 |
| MEX | mexicano | mexicano:80;nahua:4;maia:2;outras:14 | INEGI 2020 (autoidentificação indígena e língua) | 2020 |
| MKD | macedonio | macedonio:58;albanes:25;turco:4;outras:13 | Censo 2021 | 2021 |
| MLI | bambara | bambara:33;fulani:13;songai:6;tuaregue:2;outras:46 | CIA World Factbook 2024 | 2024 |
| MLT | maltes | maltes:90;outras:10 | Censo 2021 | 2021 |
| MMR | birmanes | birmanes:68;han:3;outras:29 | Censo 2014 | 2014 |
| MNE | montenegrino | montenegrino:45;servio:29;bosnio:9;albanes:5;outras:12 | Censo 2011 | 2011 |
| MNG | mongol | mongol:95;cazaque:4;outras:1 | Censo 2020 | 2020 |
| MOZ | macua | macua:25;tsonga:11;outras:64 | Censo 2017 | 2017 |
| MRT | arabe_magrebino | arabe_magrebino:55;fulani:15;wolof:8;tuaregue:5;outras:17 | CIA World Factbook 2024 | 2024 |
| MWI | chewa | chewa:35;outras:65 | Censo 2018 | 2018 |
| MYS | malaio | malaio:58;han:23;tamil:7;outras:12 | Censo 2020 (DOSM) | 2020 |
| NAM | ovambo | ovambo:50;afrikaans:12;herero:7;outras:31 | Censo 2023 | 2023 |
| NED | neerlandes | neerlandes:75;arabe_magrebino:3;turco:2;outras:20 | CBS 2023 | 2023 |
| NER | hausea | hausea:55;songai:20;tuaregue:10;fulani:8;outras:7 | CIA World Factbook 2024 | 2024 |
| NGA | hausea | hausea:30;ioruba:15;igbo:15;fulani:6;outras:34 | CIA World Factbook 2024 | 2024 |
| NIC | nicaraguense | nicaraguense:85;crioulo:5;outras:10 | CIA World Factbook 2024 | 2024 |
| NOR | noruegues | noruegues:82;polones:2;sueco:2;outras:14 | Statistics Norway 2023 | 2023 |
| NPL | nepali | nepali:45;hindustani:15;tibetano:8;outras:32 | Censo 2021 | 2021 |
| NZL | neozelandes | neozelandes:70;maori:17;han:5;outras:8 | Censo 2023 (nacionalidade) | 2023 |
| OMN | arabe_peninsular | arabe_peninsular:50;hindustani:20;urdu:10;filipino:5;outras:15 | CIA World Factbook 2024 | 2024 |
| PAK | urdu | urdu:8;punjabi:45;pashto:15;outras:32 | Censo 2023 | 2023 |
| PAN | panamenho | panamenho:88;outras:12 | Censo 2023 (INEC, identidade nacional) | 2023 |
| PER | peruano | peruano:60;quechua:25;aimara:10;outras:5 | Censo 2017 (INEI) | 2017 |
| PHL | filipino | filipino:85;moro:5;ingles:3;han:3;outras:4 | Censo 2020 (PSA) | 2020 |
| PNG | papua | papua:85;melanesio:8;outras:7 | Censo 2011 | 2011 |
| POL | polones | polones:94;ucraniano:2;bielorrusso:1;outras:3 | Censo 2021 | 2021 |
| PRK | coreano | coreano:99;outras:1 | CIA World Factbook 2024 | 2024 |
| PRT | portugues | portugues:88;brasileiro:4;ucraniano:2;outras:6 | Censo 2021 | 2021 |
| PRY | paraguaio | paraguaio:60;guarani:35;outras:5 | Censo 2022 (INE) | 2022 |
| PSX | arabe_levantino | arabe_levantino:95;outras:5 | PCBS 2023 | 2023 |
| QAT | arabe_peninsular | arabe_peninsular:20;hindustani:30;urdu:10;filipino:10;outras:30 | CIA World Factbook 2024 | 2024 |
| ROU | romeno | romeno:85;hungaro:6;outras:9 | Censo 2021 | 2021 |
| RUS | russo | russo:80;tatar:4;ucraniano:2;checheno:1;outras:13 | Censo 2021 | 2021 |
| RWA | grandes_lagos | grandes_lagos:95;outras:5 | Censo 2022 | 2022 |
| SAH | arabe_magrebino | arabe_magrebino:80;tuaregue:15;outras:5 | CIA World Factbook 2024 | 2024 |
| SAU | arabe_peninsular | arabe_peninsular:65;hindustani:10;urdu:5;arabe_egipcio:5;outras:15 | CIA World Factbook 2024 | 2024 |
| SDN | arabe_sudanes | arabe_sudanes:60;nuer:5;dinka:5;fulani:5;outras:25 | CIA World Factbook 2024 | 2024 |
| SEN | wolof | wolof:40;fulani:26;outras:34 | Censo 2019 | 2019 |
| SGP | han | han:74;malaio:13;tamil:9;outras:4 | Censo 2020 (SingStat) | 2020 |
| SLB | melanesio | melanesio:90;papua:5;outras:5 | Censo 2019 | 2019 |
| SLE | kru | kru:85;crioulo:10;outras:5 | Censo 2015 | 2015 |
| SLV | salvadorenho | salvadorenho:85;maia:12;outras:3 | Censo 2019 | 2019 |
| SOL | somali | somali:95;outras:5 | CIA World Factbook 2024 | 2024 |
| SOM | somali | somali:85;outras:15 | CIA World Factbook 2024 | 2024 |
| SRB | servio | servio:83;hungaro:3;bosnio:2;outras:12 | Censo 2022 | 2022 |
| SSD | dinka | dinka:40;nuer:25;outras:35 | CIA World Factbook 2024 | 2024 |
| SUR | hindustani | hindustani:27;crioulo:15;javanes:14;caribenho:10;outras:34 | Censo 2012 (nacionalidade) | 2012 |
| SVK | eslovaco | eslovaco:84;hungaro:8;romeno:2;outras:6 | Censo 2021 | 2021 |
| SVN | esloveno | esloveno:83;croata:2;servio:2;bosnio:1;outras:12 | Censo 2021 | 2021 |
| SWE | sueco | sueco:80;finlandes:2;outras:18 | Statistics Sweden 2023 | 2023 |
| SWZ | suazi | suazi:85;zulu:10;outras:5 | Censo 2017 | 2017 |
| SYR | arabe_levantino | arabe_levantino:75;curdo:10;armenio:2;turcomeno:2;outras:11 | CIA World Factbook 2024 | 2024 |
| TCD | arabe_chadiano | arabe_chadiano:30;kanuri:9;hausea:2;outras:59 | CIA World Factbook 2024 | 2024 |
| TGO | ewe | ewe:40;outras:60 | CIA World Factbook 2024 | 2024 |
| THA | tailandes | tailandes:80;malaio:4;khmer:2;han:3;outras:11 | Censo 2020 | 2020 |
| TJK | tadjique | tadjique:84;uzbeque:14;outras:2 | Censo 2020 | 2020 |
| TKM | turcomeno | turcomeno:85;uzbeque:8;russo:4;outras:3 | Censo 2012 | 2012 |
| TLS | timorense | timorense:90;portugues:3;malaio:2;outras:5 | Censo 2022 | 2022 |
| TTO | trinitario | trinitario:56;hindustani:35;outras:9 | Censo 2011 (origem/identidade) | 2011 |
| TUN | arabe_magrebino | arabe_magrebino:95;berbere:2;outras:3 | CIA World Factbook 2024 | 2024 |
| TUR | turco | turco:75;curdo:19;outras:6 | CIA World Factbook 2024 | 2024 |
| TWN | han | han:97;outras:3 | Censo 2020 (DGBAS) | 2020 |
| TZA | suaili | suaili:90;outras:10 | Censo 2022 | 2022 |
| UGA | ganda | ganda:18;luo:10;outras:72 | Censo 2014 | 2014 |
| UKR | ucraniano | ucraniano:78;russo:17;romeno:1;hungaro:1;outras:3 | Censo 2001 | 2001 |
| URY | uruguaio | uruguaio:89;italiano:5;outras:6 | Censo 2023 (INE) | 2023 |
| USA | americano | americano:77;mexicano_americano:11;han:2;filipino:2;porto_riquenho:2;cubano:1;nativo_americano:1;outras:4 | ACS 2023 (origem e identidade; recategorizado por cultura) | 2023 |
| UZB | uzbeque | uzbeque:83;tadjique:5;cazaque:3;tatar:2;outras:7 | Censo 2020 | 2020 |
| VCT | sao_vicentino | sao_vicentino:90;outras:10 | Censo 2012 (nacionalidade) | 2012 |
| VEN | venezuelano | venezuelano:90;italiano:3;portugues:2;outras:5 | Censo 2011 (INE) | 2011 |
| VNM | vietnamita | vietnamita:86;khmer:1;han:1;outras:12 | Censo 2019 | 2019 |
| XKX | albanes | albanes:93;servio:2;bosnio:2;outras:3 | Censo 2011 (estimativa 2024) | 2011 |
| YEM | arabe_peninsular | arabe_peninsular:95;outras:5 | CIA World Factbook 2024 | 2024 |
| ZAF | zulu | zulu:23;xhosa:16;afrikaans:12;soto:10;ingles:8;outras:31 | Censo 2022 (língua) | 2022 |
| ZMB | bemba | bemba:21;chewa:13;outras:66 | Censo 2022 | 2022 |
| ZWE | shona | shona:75;ndebele:18;outras:7 | Censo 2022 | 2022 |

## 6. Cores

As cores existem para um futuro modo de mapa por cultura. São geradas em OKLCH e
escolhidas por família para garantir **ΔE\*ab ≥ 25** entre quaisquer duas culturas
da mesma família (o pior caso medido é 30,5, na família banta com 23 culturas;
hispano-americana, com 20, fica em 33,3 e caribenha, com 14, em 38,9; ΔE00
correspondente é menor e é impresso no build apenas como diagnóstico). A cor é
reprodutível: mesma curadoria, mesmas cores.

## 7. Cobertura e números da v1.2

| métrica | v1.1 | v1.2 |
| --- | --- | --- |
| culturas | 189 | 208 |
| famílias | 42 | 45 (novas: anglo-oceânica, aborígene, polinésia) |
| nações cobertas | 179 (100%) | 179 (100%) |
| cidades cobertas | 834 (100%) | 834 (100%) |
| cidades com dado regional | 319 | 321 |
| `national_fallback` | 515 | 513 |
| cidades com `subcultures` | 10 | 10 |
| categorias raciais no catálogo | 5 | **0** |
| nações sem cultura principal nacional | 0 | **0** |
| culturas sem uso | 0 | **0** |

- `validate_cultures.py`: 0 erros, 0 avisos (lista negada ligada).
- `validate_scenario_data.py`: 0 erros, 0 avisos (seção de cultura com 4 arquivos).
- **Removidas (5):** `afro_latino`, `mestico`, `afro_americano`, `anglo_americano`,
  `hispanico`.
- **Adicionadas (24):** `americano`, `mexicano_americano`, `belizense`,
  `guianense`, `haitiano`, `jamaicano`, `trinitario`, `bahamense`, `barbadense`,
  `antiguano`, `dominiquense`, `granadino`, `sancristovense`, `santa_luciense`,
  `sao_vicentino`, `canares`, `malaiala`, `australiano`, `neozelandes`,
  `aborigene`, `havaiano`, `nativo_americano`, `indigena_brasileiro`,
  `luxemburgues`.
- **Renomeadas:** `hindi` → `hindustani` (hindi-urdu); `inuit` → **Inuit-iúpik**
  (mesmo id, para cobrir iúpiat/iúpik do Alasca), com nomes i18n atualizados.
- **Nações reescritas (31):** USA, BRA, MEX, COL, CRI, CUB, DOM, ECU, PAN, URY,
  VEN, BLZ, GUY, HTI, JAM, TTO, BHS, ATG, BRB, DMA, GRD, KNA, LCA, VCT, IND, AUS,
  NZL, LUX, SUR, NPL, FJI. As aceitas foram reajustadas para não apontar para
  cultura removida.
- **Dado regional novo:** Londres (`GBR-LDN`, 5 grupos nomeados + `outras`),
  Nova York/Los Angeles/Miami/Chicago/Houston/San Antonio/Honolulu/Anchorage e
  Bethel (Alasca) reescritos por identidade cultural; São Paulo, Rio e Porto
  Alegre sem categoria racial; Porto Rico sem `afro_latino`; Aruba e Curaçao com
  os hispanofalantes não identificados em `outras`.

## 8. Limitações declaradas

- **Não é censo**: composições nacionais são agregados publicados, arredondados
  para percentuais inteiros. Servem para simulação, não para afirmação factual.
- **América Latina e Caribe**: cada país tem identidade própria na família
  `hispano-americana` (`mexicano`, `argentino`, `colombiano`, `peruano`,
  `boliviano`, `chileno`, `paraguaio`, `uruguaio`, `cubano`, `dominicano`,
  `porto_riquenho`, `guatemalteco`, `hondurenho`, `salvadorenho`, `nicaraguense`,
  `costa_riquenho`, `panamenho`, `venezuelano`, `equatoriano`) ou na família
  `caribenha` (`haitiano`, `jamaicano`, `trinitario`, `bahamense`, `barbadense`,
  `antiguano`, `dominiquense`, `granadino`, `sancristovense`, `santa_luciense`,
  `sao_vicentino`, `guianense`, `belizense`). `castelhano` designa apenas a
  identidade da Espanha (e imigrantes espanhóis, como em Andorra e nas Malvinas).
  Povos indígenas permanecem explícitos onde o censo dá percentual (`quechua`,
  `aimara`, `guarani`, `maia`, `mapuche`, `nahua`). **Afrodescendentes e mestiços
  não são culturas neste modelo**: o pertencimento por cor/raça foi absorvido pela
  identidade nacional do país (`colombiano`, `cubano`, `belizense`…). Belize e
  Guiana usavam `mestico` porque a fonte publica "mestizo/mixed"; agora usam a
  identidade nacional, preservando o percentual publicado.
- **EUA**: `americano` é a identidade nacional e **não é categoria racial** —
  cobre a maioria de origem europeia, africana ou mista (o antigo `afro_americano`
  foi absorvido). As minorias nomeadas são identidades culturais com origem/língua
  própria: `mexicano_americano` (chicano/tejano), `porto_riquenho`, `cubano`,
  `dominicano`, `haitiano`, `han`, `filipino`, `vietnamita`, `coreano`, `japones`,
  `havaiano`, `inuit` (iúpiat/iúpik no Alasca) e `nativo_americano`. A composição
  nacional reparte os 19% de origem hispânica só nas identidades com fonte
  (`mexicano_americano`, `porto_riquenho`, `cubano`); o restante fica em `outras`.
- **Composições nacionais que não são gentílico**: em Estados multiétnicos sem
  identidade nacional majoritária publicada (Nigéria, Angola, Quênia, Zâmbia,
  África do Sul, Etiópia…), a principal é o maior grupo cultural da fonte — não há
  gentílico nacional medido. `SUR` usa `hindustani` (maior grupo do censo de 2012)
  e `PAK` usa `urdu` (identidade nacional e língua franca), ambos documentados na
  tabela do §5. `ARE`/`QAT` têm a identidade nacional dos cidadãos menor que o
  conjunto de residentes estrangeiros.
- **Índia**: a fonte é linguística, então a cultura principal é `hindustani`
  (hindi-urdu) e as demais grandes línguas entram nomeadas (`bengali`, `marata`,
  `telugu`, `tamil`, `gujarati`, `canares`, `malaiala`, `punjabi`); não existe uma
  cultura nacional "indiana" na fonte.
- **Fallback amplifica minorias pequenas**: a fórmula do §4 eleva frações
  pequenas (`share^(1-k)`), então `indigena_brasileiro:1` na composição nacional
  aparece como ~3-4% em cidade `national_fallback`. É efeito declarado do ajuste
  sintético, não dado real.
- **Reexpressões que preservam o percentual publicado** (nenhum número novo):
  Belize (53/26/11) e Trinidad e Tobago (56/35/9) apenas trocaram o rótulo
  racial/censitário pela identidade nacional/creole; Guiana usa `hindustani:40`
  porque o censo de 2012 publica 39,8% "East Indian"; a Austrália soma
  "australiano" (29,9%) e "inglês" (33,0%) em `australiano:60` e mantém os 3,2%
  aborígenes nomeados; o Brasil usa `brasileiro:96` (identidade nacional) e só
  minoria com percentual publicado (`indigena_brasileiro:1`, Censo 2022).
- **Donetsk, Mariupol, Sebastopol, Zaporíjia e Kherson** (`source=estimate_post2022`):
  não há censo utilizável depois de 2022. Os valores são **estimativas
  declaradas**, derivadas de censos pré-2022 (2001 na Ucrânia; 2014 na Crimeia),
  da mudança de cidadania/documentação sob ocupação e do deslocamento registrado
  por ONU/OCHA e IOM. A incerteza é alta e a composição real é desconhecida;
  Sebastopol usa 81/14/1 (russos/ucranianos/tártaros) como aproximação da
  estrutura de 2014 redistribuída. Luhansk não está entre as 834 cidades, então
  não há linha para ela.
- **Caxemira**: Srinagar não está entre as 834 cidades da base e nenhuma cidade
  do cenário cai na Caxemira (Índia ou Paquistão); **não foi criada cidade** e
  não há dado regional a aplicar. Simferopol também não existe na base — a única
  cidade da Crimeia é Sebastopol (`RUS-SEV`), que recebeu o mix regional.
  Bilbao (País Basco) e Bolzano (Tirol do Sul) também não estão na base.
- **Ucrânia**: último censo publicado é 2001; a composição nacional reflete isso.
- **Conflitos recentes** (Síria, Sudão, Iêmen, Líbia): estimativas anteriores ou
  contemporâneas ao conflito, sem correção de deslocamento (exceto as cinco
  cidades marcadas `estimate_post2022`).
- **Estimativas regionais**: onde o país não publica recorte municipal recente, a
  fonte diz "estimativa regional" e o ano base está no texto (ex.: Nigéria usa
  censo 2006, Somália/Etiópia usam censos antigos). Não são projeções do jogo.
- **Territórios dependentes** herdam a metrópole quando não há override; há
  overrides para Aruba, Curaçao, Groenlândia, Malvinas, Nova Caledônia, Porto
  Rico (`porto_riquenho`, sem categoria racial), Liechtenstein, Mônaco e San
  Marino. Em Aruba e Curaçao os hispanofalantes de origem não identificada ficam
  em `outras` (não há uma nacionalidade publicada para eles).
- **`outras`** agrega minorias não catalogadas; não é uma cultura.
- **África subsaariana**: vários países não têm censo étnico recente; a fonte
  indica "estimativa regional" e a composição deve ser lida como aproximação.
- **Nada disso está no save**: não há campo de cultura em save/load nesta versão.

## 9. Uso futuro (não implementado)

- **Satisfação**: penalidade por distância cultural entre a mistura da cidade e a
  cultura principal da nação; culturas aceitas reduzem a penalidade.
- **Assimilação**: deriva lenta e determinística das frações em direção à cultura
  principal quando a cidade é aceita e a satisfação é alta, conservando a soma 100.
- **Ocupação**: a cultura do ocupante entra como aceita e ganha pressão; frações
  não aceitas geram resistência. Exige regra explícita antes de qualquer código.
- **Modo de mapa**: cor por cultura principal da cidade, usando as cores do
  catálogo (garantia de ΔE dentro da família).

Qualquer uma dessas integrações muda contrato de dados e exige plano de migração,
teste headless e revisão do `qa-docs`.
