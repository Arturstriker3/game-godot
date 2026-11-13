# Dados de cultura — nações e cidades (v1.1)

Status: **dados apenas**. Nada aqui está ligado à economia, ao C++ ou à UI.
Aprovação: o usuário pediu explicitamente para não integrar agora.

## 1. Objetivo

Dar ao jogo uma camada étnico-linguística por nação e por cidade, suficiente para,
no futuro, alimentar satisfação, assimilação e ocupação sem inventar números em
tempo de execução. A v1.1 corrige dupla contagem de subgrupos, separa as
identidades nacionais hispano-americanas, atualiza cidades afetadas pela guerra
de 2022 e amplia o dado regional em regiões multiétnicas.

## 2. Arquivos e contrato

Saída em `data/definitions/culture/` (o jogo ainda não lê nenhum deles):

| arquivo | linhas | conteúdo |
| --- | --- | --- |
| `cultures.csv` | 189 | `id,name_pt,name_en,name_es,family,language,religion,parent,color` |
| `cultures.json` | 189 | mesmo conteúdo, para consumo em GDScript |
| `nation_cultures.csv` | 179 | `nation,primary_culture,accepted_cultures,composition,source,year` |
| `city_cultures.csv` | 834 | `city_id,nation,city,cultures,subcultures,source` |

Regras de leitura:

- `composition` e `cultures` são listas `cultura:percentual` separadas por `;`.
- Toda lista soma **100**. Percentuais são inteiros (maior resto).
- `outras` é o resíduo, não uma etnia: nunca aparece em `cultures.csv` nem em
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

1. **Catálogo curado** (`tools/culture_sources/cultures.csv`): 189 culturas em 42
   famílias (eslava oriental, germânica ocidental, latina ibérica, semítica árabe,
   turcomana, banta, indo-ariana, sino-tibetana, ameríndia etc.), com idioma
   principal, religião majoritária e `parent` quando for subgrupo.
2. **Composição nacional** (`tools/culture_sources/nation_cultures.csv`): uma
   linha por nação das 179 do cenário, com cultura principal, aceitas, composição,
   fonte e ano. A fonte é declarada por nação e aparece na tabela do §5.
3. **Dado regional por cidade** (`tools/culture_sources/city_overrides_*.csv`):
   319 cidades com censo/estimativa regional (Bálticos, Ucrânia, Rússia,
   Ásia Central, Cáucaso, Balcãs, Espanha, Bélgica, Canadá, China/Xinjiang/Tibete,
   Índia, Paquistão, Sudeste Asiático, Oriente Médio, América Latina, África e
   Caribe).
4. **Fallback** para as outras 515 cidades (§4).
5. `tools/build_cultures.py` gera tudo de forma determinística;
   `tools/validate_cultures.py` valida. `tools/validate_scenario_data.py` apenas
   **avisa** (não erra) se os arquivos de cultura faltarem.

### Regra 1 — exclusividade no mesmo nível

Culturas são mutuamente exclusivas no mesmo nível: um mix (nação ou cidade) não
pode conter `X` e `parent(X)`, nem avô/neto. A varredura cobre todas as 179 nações
e 834 cidades por pares pai/filho. Na v1, **3 nações** (CHN, SGP, TWN) e **29
cidades** (26 em `national_fallback`, mais Hong Kong, Macau e Singapura) tinham
par pai/filho; na v1.1 a contagem é **0** e o validador falha se um par novo for
introduzido. Os únicos pares existentes no catálogo são chineses
(`han` + `hokkien/cantones/hakka/teochew`); Singapura, Malásia, Hong Kong, Macau,
Taiwan, Guangzhou e Xiamen foram reescritos para `han` agregado com `subcultures`
detalhadas. Casos citados que não existem como culturas separadas
(árabe+beduíno, castelhano+andaluz, russo+cossaco) não têm par a corrigir.

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

| nação | principal | composição | fonte | ano |
| --- | --- | --- | --- | --- |
| Afghanistan (AFG) | pashto | pashto:45;tadjique:27;uzbeque:9;turcomeno:3;outras:16 | CIA World Factbook 2024 | 2024 |
| Angola (AGO) | ovimbundu | ovimbundu:37;mbundu:25;kikongo:13;outras:25 | CIA World Factbook 2024 | 2024 |
| Albania (ALB) | albanes | albanes:83;grego:1;outras:16 | CIA World Factbook 2024 | 2024 |
| United Arab Emirates (ARE) | arabe_peninsular | arabe_peninsular:12;hindi:30;urdu:15;punjabi:8;filipino:6;outras:29 | CIA World Factbook 2024 (residentes) | 2024 |
| Argentina (ARG) | argentino | argentino:60;italiano:20;alemao:3;arabe_levantino:3;outras:14 | INDEC 2022 (ancestralidade declarada) | 2022 |
| Armenia (ARM) | armenio | armenio:98;outras:2 | Censo 2022 (Armstat) | 2022 |
| Antigua and Barbuda (ATG) | caribenho | caribenho:91;outras:9 | CIA World Factbook 2024 | 2024 |
| Australia (AUS) | ingles | ingles:55;escoces:9;irlandes:8;italiano:3;han:5;outras:20 | Censo 2021 (ABS, ancestralidade) | 2021 |
| Austria (AUT) | alemao | alemao:80;turco:2;servio:2;croata:1;outras:15 | Statistik Austria 2023 | 2023 |
| Azerbaijan (AZE) | azeri | azeri:92;russo:3;armenio:1;outras:4 | Censo 2019 (Azstat) | 2019 |
| Burundi (BDI) | grandes_lagos | grandes_lagos:85;frances:8;outras:7 | CIA World Factbook 2024 | 2024 |
| Belgium (BEL) | flamengo | flamengo:58;frances:28;valao:5;italiano:2;outras:7 | Statbel 2023 (origem) | 2023 |
| Benin (BEN) | fon | fon:38;ioruba:15;fulani:10;outras:37 | CIA World Factbook 2024 | 2024 |
| Burkina Faso (BFA) | mossi | mossi:52;fulani:8;outras:40 | CIA World Factbook 2024 | 2024 |
| Bangladesh (BGD) | bengali | bengali:98;outras:2 | Censo 2022 (BBS) | 2022 |
| Bulgaria (BGR) | bulgaro | bulgaro:76;turco:9;romeno:3;outras:12 | Censo 2021 (NSI) | 2021 |
| Bahrain (BHR) | arabe_peninsular | arabe_peninsular:46;hindi:20;urdu:8;filipino:6;outras:20 | CIA World Factbook 2024 | 2024 |
| The Bahamas (BHS) | caribenho | caribenho:90;outras:10 | Censo 2022 | 2022 |
| Bosnia and Herzegovina (BIH) | bosnio | bosnio:50;servio:31;croata:15;outras:4 | Censo 2013 | 2013 |
| Belarus (BLR) | bielorrusso | bielorrusso:84;russo:8;polones:3;outras:5 | Censo 2019 (Belstat) | 2019 |
| Belize (BLZ) | mestico | mestico:53;crioulo:26;maia:11;outras:10 | Censo 2022 (SIB) | 2022 |
| Bolivia (BOL) | boliviano | boliviano:45;quechua:35;aimara:15;outras:5 | Censo 2012 (INE) | 2012 |
| Brazil (BRA) | brasileiro | brasileiro:70;portugues:8;italiano:6;alemao:4;japones:1;outras:11 | Censo 2022 (IBGE, cor/raça e origem) | 2022 |
| Barbados (BRB) | caribenho | caribenho:95;outras:5 | Censo 2021 | 2021 |
| Brunei (BRN) | malaio | malaio:65;han:10;outras:25 | CIA World Factbook 2024 | 2024 |
| Bhutan (BTN) | tibetano | tibetano:60;nepali:30;outras:10 | Censo 2017 | 2017 |
| Botswana (BWA) | tsuana | tsuana:79;outras:21 | Censo 2022 | 2022 |
| Central African Republic (CAF) | sango | sango:50;outras:50 | CIA World Factbook 2024 | 2024 |
| Canada (CAN) | canadense_ingles | canadense_ingles:55;quebequense:22;inuit:2;outras:21 | Censo 2021 (Statistics Canada) | 2021 |
| Switzerland (CHE) | alemao | alemao:62;frances:23;italiano:8;outras:7 | Censo 2020 (BFS) | 2020 |
| Chile (CHL) | chileno | chileno:87;mapuche:10;outras:3 | Censo 2017 (INE) | 2017 |
| People's Republic of China (CHN) | han | han:91;uigur:1;tibetano:1;mongol:1;outras:6 | Censo 2020 (NBS) | 2020 |
| Ivory Coast (CIV) | akan | akan:38;bambara:15;mossi:10;outras:37 | CIA World Factbook 2024 | 2024 |
| Cameroon (CMR) | fang | fang:20;fulani:10;hausea:5;outras:65 | CIA World Factbook 2024 | 2024 |
| Democratic Republic of the Congo (COD) | lingala | lingala:35;kikongo:20;luba:20;suaili:10;outras:15 | CIA World Factbook 2024 | 2024 |
| Republic of the Congo (COG) | kikongo | kikongo:45;lingala:30;outras:25 | CIA World Factbook 2024 | 2024 |
| Colombia (COL) | colombiano | colombiano:85;afro_latino:8;outras:7 | Censo 2018 (DANE) | 2018 |
| Costa Rica (CRI) | costa_riquenho | costa_riquenho:85;crioulo:5;outras:10 | Censo 2022 (INEC) | 2022 |
| Cuba (CUB) | cubano | cubano:80;afro_latino:15;outras:5 | Censo 2012 (ONEI) | 2012 |
| Cyprus (CYP) | grego | grego:75;turco:18;outras:7 | Censo 2021 | 2021 |
| Czech Republic (CZE) | tcheco | tcheco:88;eslovaco:3;ucraniano:2;outras:7 | Censo 2021 | 2021 |
| Germany (DEU) | alemao | alemao:80;turco:7;polones:2;russo:2;outras:9 | Destatis 2023 (origem) | 2023 |
| Djibouti (DJI) | somali | somali:60;afar:35;outras:5 | CIA World Factbook 2024 | 2024 |
| Dominica (DMA) | caribenho | caribenho:95;outras:5 | Censo 2011 | 2011 |
| Denmark (DNK) | dinamarques | dinamarques:86;turco:2;alemao:1;outras:11 | Statistics Denmark 2023 | 2023 |
| Dominican Republic (DOM) | dominicano | dominicano:75;afro_latino:15;crioulo:7;outras:3 | Censo 2022 (ONE) | 2022 |
| Algeria (DZA) | arabe_magrebino | arabe_magrebino:72;berbere:27;outras:1 | CIA World Factbook 2024 | 2024 |
| Ecuador (ECU) | equatoriano | equatoriano:80;quechua:8;afro_latino:5;outras:7 | Censo 2022 (INEC) | 2022 |
| Egypt (EGY) | arabe_egipcio | arabe_egipcio:95;outras:5 | CIA World Factbook 2024 | 2024 |
| Eritrea (ERI) | tigrinio | tigrinio:55;outras:45 | CIA World Factbook 2024 | 2024 |
| Spain (ESP) | castelhano | castelhano:70;catalao:18;galego:6;basco:2;outras:4 | Censo 2021 (INE, língua e identidade) | 2021 |
| Estonia (EST) | estoniano | estoniano:69;russo:24;ucraniano:2;bielorrusso:1;outras:4 | Censo 2021 | 2021 |
| Ethiopia (ETH) | oromo | oromo:35;amhara:30;tigrinio:6;somali:6;outras:23 | Censo 2007 (CSA) | 2007 |
| Finland (FIN) | finlandes | finlandes:90;sueco:5;russo:2;outras:3 | Statistics Finland 2023 | 2023 |
| Fiji (FJI) | fijiano | fijiano:57;hindi:37;outras:6 | Censo 2017 | 2017 |
| France (FRA) | frances | frances:75;arabe_magrebino:7;portugues:2;italiano:2;outras:14 | INSEE 2023 (origem/nacionalidade) | 2023 |
| Gabon (GAB) | fang | fang:32;kikongo:10;frances:5;outras:53 | CIA World Factbook 2024 | 2024 |
| United Kingdom (GBR) | ingles | ingles:75;escoces:8;gales:5;norte_irlandes:3;outras:9 | Censo 2021 (ONS) | 2021 |
| Georgia (GEO) | georgiano | georgiano:87;azeri:6;armenio:4;outras:3 | Censo 2014 (Geostat) | 2014 |
| Ghana (GHA) | akan | akan:47;ewe:14;mossi:6;outras:33 | Censo 2021 | 2021 |
| Guinea (GIN) | fulani | fulani:40;bambara:25;outras:35 | CIA World Factbook 2024 | 2024 |
| The Gambia (GMB) | wolof | wolof:35;fulani:25;outras:40 | Censo 2013 | 2013 |
| Guinea Bissau (GNB) | crioulo | crioulo:30;fulani:25;outras:45 | CIA World Factbook 2024 | 2024 |
| Equatorial Guinea (GNQ) | fang | fang:85;outras:15 | CIA World Factbook 2024 | 2024 |
| Greece (GRC) | grego | grego:91;albanes:4;outras:5 | Censo 2021 | 2021 |
| Grenada (GRD) | caribenho | caribenho:95;outras:5 | Censo 2011 | 2011 |
| Guatemala (GTM) | guatemalteco | guatemalteco:60;maia:40 | Censo 2018 (INE) | 2018 |
| Guyana (GUY) | caribenho | caribenho:40;hindi:30;mestico:10;outras:20 | Censo 2012 (Bureau of Statistics) | 2012 |
| Honduras (HND) | hondurenho | hondurenho:85;maia:7;crioulo:5;outras:3 | Censo 2013 (INE) | 2013 |
| Croatia (HRV) | croata | croata:90;servio:4;bosnio:2;outras:4 | Censo 2021 | 2021 |
| Haiti (HTI) | crioulo | crioulo:95;frances:3;outras:2 | CIA World Factbook 2024 | 2024 |
| Hungary (HUN) | hungaro | hungaro:85;romeno:2;outras:13 | Censo 2022 | 2022 |
| Indonesia (IDN) | javanes | javanes:40;malaio:15;sunda:10;han:3;papua:2;outras:30 | Censo 2020 (BPS) | 2020 |
| India (IND) | hindi | hindi:44;bengali:8;marata:7;telugu:7;tamil:6;outras:28 | Censo 2011 (linguística) | 2011 |
| Ireland (IRL) | irlandes | irlandes:85;ingles:12;outras:3 | Censo 2022 | 2022 |
| Iran (IRN) | persa | persa:61;azeri:16;curdo:10;outras:13 | CIA World Factbook 2024 | 2024 |
| Iraq (IRQ) | arabe_iraquiano | arabe_iraquiano:75;curdo:18;turcomeno:2;outras:5 | CIA World Factbook 2024 | 2024 |
| Iceland (ISL) | islandes | islandes:86;polones:5;outras:9 | Statistics Iceland 2023 | 2023 |
| Israel (ISR) | hebraico | hebraico:74;arabe_levantino:21;outras:5 | CBS 2023 | 2023 |
| Italy (ITA) | italiano | italiano:88;romeno:3;albanes:2;outras:7 | ISTAT 2023 | 2023 |
| Jamaica (JAM) | caribenho | caribenho:90;afro_americano:5;outras:5 | Censo 2011 | 2011 |
| Jordan (JOR) | arabe_levantino | arabe_levantino:90;curdo:3;outras:7 | CIA World Factbook 2024 | 2024 |
| Japan (JPN) | japones | japones:98;coreano:1;outras:1 | Censo 2020 | 2020 |
| Kazakhstan (KAZ) | cazaque | cazaque:69;russo:18;uzbeque:3;uigur:2;outras:8 | Censo 2021 | 2021 |
| Kenya (KEN) | kikuyu | kikuyu:17;luo:10;suaili:8;somali:5;outras:60 | Censo 2019 | 2019 |
| Kyrgyzstan (KGZ) | quirguiz | quirguiz:74;uzbeque:15;russo:5;outras:6 | Censo 2022 | 2022 |
| Cambodia (KHM) | khmer | khmer:95;vietnamita:2;outras:3 | Censo 2019 | 2019 |
| Saint Kitts and Nevis (KNA) | caribenho | caribenho:92;outras:8 | Censo 2011 | 2011 |
| South Korea (KOR) | coreano | coreano:96;han:2;outras:2 | Censo 2023 | 2023 |
| Kuwait (KWT) | arabe_peninsular | arabe_peninsular:30;hindi:25;urdu:10;arabe_egipcio:10;outras:25 | CIA World Factbook 2024 | 2024 |
| Laos (LAO) | lao | lao:65;khmer:3;outras:32 | Censo 2015 | 2015 |
| Lebanon (LBN) | arabe_levantino | arabe_levantino:80;armenio:5;outras:15 | CIA World Factbook 2024 | 2024 |
| Liberia (LBR) | kru | kru:60;crioulo:5;outras:35 | CIA World Factbook 2024 | 2024 |
| Libya (LBY) | arabe_magrebino | arabe_magrebino:80;berbere:15;outras:5 | CIA World Factbook 2024 | 2024 |
| Saint Lucia (LCA) | caribenho | caribenho:85;outras:15 | Censo 2010 | 2010 |
| Sri Lanka (LKA) | cingales | cingales:75;tamil:15;outras:10 | Censo 2012 | 2012 |
| Lesotho (LSO) | soto | soto:99;outras:1 | Censo 2016 | 2016 |
| Lithuania (LTU) | lituano | lituano:84;polones:6;russo:5;outras:5 | Censo 2021 | 2021 |
| Luxembourg (LUX) | frances | frances:50;alemao:15;portugues:15;outras:20 | STATEC 2023 | 2023 |
| Latvia (LVA) | leto | leto:62;russo:25;bielorrusso:3;ucraniano:3;outras:7 | Censo 2021 | 2021 |
| Morocco (MAR) | arabe_magrebino | arabe_magrebino:70;berbere:28;outras:2 | CIA World Factbook 2024 | 2024 |
| Moldova (MDA) | romeno | romeno:75;ucraniano:8;russo:6;outras:11 | Censo 2014 | 2014 |
| Madagascar (MDG) | malgaxe | malgaxe:95;frances:2;outras:3 | Censo 2018 | 2018 |
| Mexico (MEX) | mexicano | mexicano:78;nahua:4;maia:2;afro_latino:2;outras:14 | INEGI 2020 (autoidentificação indígena e afro; repartição por língua) | 2020 |
| North Macedonia (MKD) | macedonio | macedonio:58;albanes:25;turco:4;outras:13 | Censo 2021 | 2021 |
| Mali (MLI) | bambara | bambara:33;fulani:13;songai:6;tuaregue:2;outras:46 | CIA World Factbook 2024 | 2024 |
| Malta (MLT) | maltes | maltes:90;outras:10 | Censo 2021 | 2021 |
| Myanmar (MMR) | birmanes | birmanes:68;han:3;outras:29 | Censo 2014 | 2014 |
| Montenegro (MNE) | montenegrino | montenegrino:45;servio:29;bosnio:9;albanes:5;outras:12 | Censo 2011 | 2011 |
| Mongolia (MNG) | mongol | mongol:95;cazaque:4;outras:1 | Censo 2020 | 2020 |
| Mozambique (MOZ) | macua | macua:25;tsonga:11;outras:64 | Censo 2017 | 2017 |
| Mauritania (MRT) | arabe_magrebino | arabe_magrebino:55;fulani:15;wolof:8;tuaregue:5;outras:17 | CIA World Factbook 2024 | 2024 |
| Malawi (MWI) | chewa | chewa:35;outras:65 | Censo 2018 | 2018 |
| Malaysia (MYS) | malaio | malaio:58;han:23;tamil:7;outras:12 | Censo 2020 (DOSM) | 2020 |
| Namibia (NAM) | ovambo | ovambo:50;afrikaans:12;herero:7;outras:31 | Censo 2023 | 2023 |
| Netherlands (NED) | neerlandes | neerlandes:75;arabe_magrebino:3;turco:2;outras:20 | CBS 2023 | 2023 |
| Niger (NER) | hausea | hausea:55;songai:20;tuaregue:10;fulani:8;outras:7 | CIA World Factbook 2024 | 2024 |
| Nigeria (NGA) | hausea | hausea:30;ioruba:15;igbo:15;fulani:6;outras:34 | CIA World Factbook 2024 | 2024 |
| Nicaragua (NIC) | nicaraguense | nicaraguense:85;crioulo:5;outras:10 | CIA World Factbook 2024 | 2024 |
| Norway (NOR) | noruegues | noruegues:82;polones:2;sueco:2;outras:14 | Statistics Norway 2023 | 2023 |
| Nepal (NPL) | nepali | nepali:45;hindi:15;tibetano:8;outras:32 | Censo 2021 | 2021 |
| New Zealand (NZL) | ingles | ingles:70;maori:17;han:5;outras:8 | Censo 2023 | 2023 |
| Oman (OMN) | arabe_peninsular | arabe_peninsular:50;hindi:20;urdu:10;filipino:5;outras:15 | CIA World Factbook 2024 | 2024 |
| Pakistan (PAK) | urdu | urdu:8;punjabi:45;pashto:15;outras:32 | Censo 2023 | 2023 |
| Panama (PAN) | panamenho | panamenho:73;afro_latino:15;outras:12 | Censo 2023 (INEC) | 2023 |
| Peru (PER) | peruano | peruano:60;quechua:25;aimara:10;outras:5 | Censo 2017 (INEI) | 2017 |
| Philippines (PHL) | filipino | filipino:85;moro:5;ingles:3;han:3;outras:4 | Censo 2020 (PSA) | 2020 |
| Papua New Guinea (PNG) | papua | papua:85;melanesio:8;outras:7 | Censo 2011 | 2011 |
| Poland (POL) | polones | polones:94;ucraniano:2;bielorrusso:1;outras:3 | Censo 2021 | 2021 |
| North Korea (PRK) | coreano | coreano:99;outras:1 | CIA World Factbook 2024 | 2024 |
| Portugal (PRT) | portugues | portugues:88;brasileiro:4;ucraniano:2;outras:6 | Censo 2021 | 2021 |
| Paraguay (PRY) | paraguaio | paraguaio:60;guarani:35;outras:5 | Censo 2022 (INE) | 2022 |
| Palestine (PSX) | arabe_levantino | arabe_levantino:95;outras:5 | PCBS 2023 | 2023 |
| Qatar (QAT) | arabe_peninsular | arabe_peninsular:20;hindi:30;urdu:10;filipino:10;outras:30 | CIA World Factbook 2024 | 2024 |
| Romania (ROU) | romeno | romeno:85;hungaro:6;outras:9 | Censo 2021 | 2021 |
| Russia (RUS) | russo | russo:80;tatar:4;ucraniano:2;checheno:1;outras:13 | Censo 2021 | 2021 |
| Rwanda (RWA) | grandes_lagos | grandes_lagos:95;outras:5 | Censo 2022 | 2022 |
| Western Sahara (SAH) | arabe_magrebino | arabe_magrebino:80;tuaregue:15;outras:5 | CIA World Factbook 2024 | 2024 |
| Saudi Arabia (SAU) | arabe_peninsular | arabe_peninsular:65;hindi:10;urdu:5;arabe_egipcio:5;outras:15 | CIA World Factbook 2024 | 2024 |
| Sudan (SDN) | arabe_sudanes | arabe_sudanes:60;nuer:5;dinka:5;fulani:5;outras:25 | CIA World Factbook 2024 | 2024 |
| Senegal (SEN) | wolof | wolof:40;fulani:26;outras:34 | Censo 2019 | 2019 |
| Singapore (SGP) | han | han:74;malaio:13;tamil:9;outras:4 | Censo 2020 (SingStat) | 2020 |
| Solomon Islands (SLB) | melanesio | melanesio:90;papua:5;outras:5 | Censo 2019 | 2019 |
| Sierra Leone (SLE) | kru | kru:85;crioulo:10;outras:5 | Censo 2015 | 2015 |
| El Salvador (SLV) | salvadorenho | salvadorenho:85;maia:12;outras:3 | Censo 2019 | 2019 |
| Somaliland (SOL) | somali | somali:95;outras:5 | CIA World Factbook 2024 | 2024 |
| Somalia (SOM) | somali | somali:85;outras:15 | CIA World Factbook 2024 | 2024 |
| Serbia (SRB) | servio | servio:83;hungaro:3;bosnio:2;outras:12 | Censo 2022 | 2022 |
| South Sudan (SSD) | dinka | dinka:40;nuer:25;outras:35 | CIA World Factbook 2024 | 2024 |
| Suriname (SUR) | hindi | hindi:27;crioulo:15;javanes:14;caribenho:10;outras:34 | Censo 2012 | 2012 |
| Slovakia (SVK) | eslovaco | eslovaco:84;hungaro:8;romeno:2;outras:6 | Censo 2021 | 2021 |
| Slovenia (SVN) | esloveno | esloveno:83;croata:2;servio:2;bosnio:1;outras:12 | Censo 2021 | 2021 |
| Sweden (SWE) | sueco | sueco:80;finlandes:2;outras:18 | Statistics Sweden 2023 | 2023 |
| Eswatini (SWZ) | suazi | suazi:85;zulu:10;outras:5 | Censo 2017 | 2017 |
| Syria (SYR) | arabe_levantino | arabe_levantino:75;curdo:10;armenio:2;turcomeno:2;outras:11 | CIA World Factbook 2024 | 2024 |
| Chad (TCD) | arabe_chadiano | arabe_chadiano:30;kanuri:9;hausea:2;outras:59 | CIA World Factbook 2024 | 2024 |
| Togo (TGO) | ewe | ewe:40;outras:60 | CIA World Factbook 2024 | 2024 |
| Thailand (THA) | tailandes | tailandes:80;malaio:4;khmer:2;han:3;outras:11 | Censo 2020 | 2020 |
| Tajikistan (TJK) | tadjique | tadjique:84;uzbeque:14;outras:2 | Censo 2020 | 2020 |
| Turkmenistan (TKM) | turcomeno | turcomeno:85;uzbeque:8;russo:4;outras:3 | Censo 2012 | 2012 |
| East Timor (TLS) | timorense | timorense:90;portugues:3;malaio:2;outras:5 | Censo 2022 | 2022 |
| Trinidad and Tobago (TTO) | caribenho | caribenho:40;hindi:24;afro_americano:34;outras:2 | Censo 2011 | 2011 |
| Tunisia (TUN) | arabe_magrebino | arabe_magrebino:95;berbere:2;outras:3 | CIA World Factbook 2024 | 2024 |
| Turkey (TUR) | turco | turco:75;curdo:19;outras:6 | CIA World Factbook 2024 | 2024 |
| Taiwan (TWN) | han | han:97;outras:3 | Censo 2020 (DGBAS) | 2020 |
| Tanzania (TZA) | suaili | suaili:90;outras:10 | Censo 2022 | 2022 |
| Uganda (UGA) | ganda | ganda:18;luo:10;outras:72 | Censo 2014 | 2014 |
| Ukraine (UKR) | ucraniano | ucraniano:78;russo:17;romeno:1;hungaro:1;outras:3 | Censo 2001 | 2001 |
| Uruguay (URY) | uruguaio | uruguaio:85;italiano:5;afro_latino:4;outras:6 | Censo 2023 (INE) | 2023 |
| United States of America (USA) | anglo_americano | anglo_americano:45;afro_americano:12;hispanico:19;alemao:10;irlandes:6;outras:8 | Census ACS 2023 (ancestralidade) | 2023 |
| Uzbekistan (UZB) | uzbeque | uzbeque:83;tadjique:5;cazaque:3;tatar:2;outras:7 | Censo 2020 | 2020 |
| Saint Vincent and the Grenadines (VCT) | caribenho | caribenho:90;outras:10 | Censo 2012 | 2012 |
| Venezuela (VEN) | venezuelano | venezuelano:80;afro_latino:10;italiano:3;portugues:2;outras:5 | Censo 2011 (INE) | 2011 |
| Vietnam (VNM) | vietnamita | vietnamita:86;khmer:1;han:1;outras:12 | Censo 2019 | 2019 |
| Kosovo (XKX) | albanes | albanes:93;servio:2;bosnio:2;outras:3 | Censo 2011 (estimativa 2024) | 2011 |
| Yemen (YEM) | arabe_peninsular | arabe_peninsular:95;outras:5 | CIA World Factbook 2024 | 2024 |
| South Africa (ZAF) | zulu | zulu:23;xhosa:16;afrikaans:12;soto:10;ingles:8;outras:31 | Censo 2022 (língua) | 2022 |
| Zambia (ZMB) | bemba | bemba:21;chewa:13;outras:66 | Censo 2022 | 2022 |
| Zimbabwe (ZWE) | shona | shona:75;ndebele:18;outras:7 | Censo 2022 | 2022 |

## 6. Cores

As cores existem para um futuro modo de mapa por cultura. São geradas em OKLCH e
escolhidas por família para garantir **ΔE\*ab ≥ 25** entre quaisquer duas culturas
da mesma família (o pior caso medido é 30,5, na família banta com 23 culturas;
hispano-americana, com 21, fica em 32,6; ΔE00 correspondente é menor e é impresso
no build apenas como diagnóstico). A cor é reprodutível: mesma curadoria, mesmas
cores.

## 7. Cobertura e números da v1.1

| métrica | v1 | v1.1 |
| --- | --- | --- |
| culturas | 158 | 189 |
| famílias | 41 | 42 (nova: hispano-americana) |
| nações cobertas | 179 | 179 (100%) |
| cidades cobertas | 834 | 834 (100%) |
| cidades com dado regional | 224 | 319 |
| `national_fallback` | 610 | 515 |
| cidades com `subcultures` | 0 | 10 |

- `validate_cultures.py`: 0 erros, 0 avisos.
- `validate_scenario_data.py`: 0 erros, 0 avisos (seção de cultura com 4 arquivos).
- Novas culturas: 18 identidades nacionais hispano-americanas + `afro_latino` e
  `mestico`; `mapuche`, `nahua`, `sunda`, `batak`, `minangkabau`, `bugis`, `moro`,
  `balochi`, `tartaro_crimeia`; e os subgrupos chineses `hakka` e `teochew`
  (`parent=han`).
- Regiões que ganharam dado regional nesta versão: Balcãs (Sérvia, Bósnia,
  Macedônia do Norte, Kosovo, Montenegro, Croácia, Eslovênia), Cáucaso (Batumi,
  Ganja), Ásia Central (Cazaquistão, Uzbequistão, Tajiquistão, Turcomenistão),
  Romênia, Bulgária, Eslováquia, Bielorrússia, Crimeia/Donbass/Zaporíjia/Kherson,
  China/Xinjiang/Tibete/Taiwan/Hong Kong/Macau, Malásia, Indonésia, Filipinas,
  Nepal, Butão, Mianmar, Tailândia, Baluchistão, Nigéria, Etiópia, África do Sul,
  Quênia, Marrocos, Argélia, Tunísia, Egito, Sudão e América Latina.

## 8. Limitações declaradas

- **Não é censo**: composições nacionais são agregados publicados, arredondados
  para percentuais inteiros. Servem para simulação, não para afirmação factual.
- **América Hispânica**: cada país tem identidade própria na família
  `hispano-americana` (`mexicano`, `argentino`, `colombiano`, `peruano`,
  `boliviano`, `chileno`, `paraguaio`, `uruguaio`, `cubano`, `dominicano`,
  `porto_riquenho`, `guatemalteco`, `hondurenho`, `salvadorenho`, `nicaraguense`,
  `costa_riquenho`, `panamenho`, `venezuelano`, `equatoriano`). `castelhano`
  designa apenas a identidade da Espanha (e imigrantes espanhóis, como em
  Andorra e nas Malvinas). Povos indígenas permanecem explícitos onde o censo dá
  percentual (`quechua`, `aimara`, `guarani`, `maia`, `mapuche`, `nahua`) e
  afrodescendentes/mestiços usam `afro_latino`/`mestico` conforme o censo de cada
  país (categorias censitárias, não etnias). Guiana e Belize usam `mestico` porque
  a fonte publica "mestizo/mixed", não uma nacionalidade espanhola.
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
  Rico (agora `porto_riquenho`, não `hispanico`), Liechtenstein, Mônaco e San
  Marino.
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
