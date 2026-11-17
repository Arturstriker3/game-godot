# B3 — Região da cidade = estados inteiros (admin-1 do Natural Earth)

> Status: **dado gerado e validado, NÃO ligado na simulação**. Substitui a regra
> "província → cidade mais próxima da mesma nação" (`hinterland_2026.json`) quando a
> B3 entrar no núcleo. Decisão do dono em `docs/plans/perf_pop_financas_plano.md`
> (§B3, opção 1: estados inteiros).
> Data: 2026-11-17 · branch `data/b3-states` · worktree `opengs-wt-data-states`.

## 1. O que mudou

| | regra antiga (`hinterland_2026.json`) | regra nova (`state_regions.json`) |
|---|---|---|
| unidade da região | província admin-1 avulsa | **estado inteiro** (admin-1 do Natural Earth) |
| fronteira | distância ao centroide da cidade | divisa real do estado |
| nome da região | não tem | tem (`states[].name` = nome real do admin-1) |
| estados sem cidade | — | anexados a um vizinho com cidade |
| estados com 2+ cidades | — | estado vai para a maior; as outras ficam com as províncias do estado mais próximas |

Saída: `data/definitions/population/state_regions.json`
(sha256 `2e375cfb6d1bbd3235bc684cf9e3a76f312e257437feb1d2035949ae901a4746`,
idêntico em duas execuções independentes).

Contrato por registro:

```text
states[state_id] = { name, nation, provinces[], area_km2, population_2026,
                     city_id, attach_rule }
cities[city_id]  = { states[], population_region, urban_population,
                     region_population }   # region = population_region - urban, piso 0
```

`state_id` = `adm1_code` do Natural Earth (único, estável). A parte ocupada pela Rússia
de um admin-1 dividido recebe sufixo `@RUS` (3 casos: Donets'k, Kherson, Zaporizhzhya).

## 2. Como foi construído (`tools/build_state_regions.py`)

1. **Província → admin-1 pai**, reconstruído com as três provas do pipeline
   (`tools/build_atlas_layers.py`): (a) id ≤ 4.596 é o próprio id da fonte; (b) bloco de
   ocupação RUS logo depois do último admin-1; (c) filha da subdivisão = nome `"<pai> N"`
   com pai único de mesmo nome/nação, cerca monotônica de ids e, nos 44 casos ambíguos,
   ponto-no-polígono do admin-1 da fonte. O resultado é validado pela propriedade do
   pipeline (filhas alocadas em ordem crescente de pai) — 15.547/15.547 províncias.
2. **População**: mesmo raster do hinterland (WorldPop Global2 R2025A, 2026, 1 km,
   sha256 `61c487e7…`), somado por província, × fator da nação (WorldPop → `nations.json`)
   e apportionado em inteiros por maior resto **por província**, de modo que estado,
   cidade e nação somem exatamente o mesmo total.
3. **Área**: soma de `natural/province_areas.json` (área real do raster por província).
4. **Adjacência**: pares de pixels vizinhos de estados diferentes em
   `world_province.png`, restritos a terra navegável (`world_sdf.png` × `nav_mask.png`).
   Sem a máscara, o preenchimento do mar com a província de terra mais próxima criava
   divisas no oceano (Santa Catarina aparecia vizinha do Rio de Janeiro).
   Os **48 admin-1 sem nenhum pixel** no raster (Westminster, Čair de Skopje, municípios
   de San Marino/Malta/Eslovênia) usam os 4 estados de mesma nação com centroide mais
   próximo.
5. **Anexação**: BFS multi-fonte anel a anel. Um estado sem cidade junta-se ao vizinho que
   já pertence a uma cidade; quando há mais de um no mesmo anel vence a **cidade de maior
   população** (`cities.json`). A população comparada é a da cidade, não a do estado: o
   admin-1 minúsculo (Westminster tem 12 km²) fica com população 0 no apportion e
   decidiria o anexo por acaso. Sem caminho até cidade alguma, cai para o estado com
   cidade de centroide mais próximo.
6. **Estado com 2+ cidades**: o estado vai inteiro para a cidade de maior população; cada
   outra cidade fica com as províncias do estado das quais é a mais próxima (Voronoi
   dentro do estado, empate para a maior cidade). A cidade principal nunca perde a
   província que a contém.

## 3. Números e invariantes

| item | valor |
|---|---|
| estados | 4.226 (4.223 admin-1 presentes no atlas + 3 partes ocupadas RUS) |
| cidades | 834 |
| províncias | 15.547 (todas em exatamente 1 estado) |
| soma dos estados | 8.203.501.062 |
| soma das regiões das cidades | 8.203.501.062 |
| `nations.json` somado | 8.203.501.062 |
| urbano (`cities.json`) | 2.110.546.632 |
| interior (região − urbano) | 6.143.747.129 |
| área total dos estados | 145,7 M km² |

As 179 nações fecham exatamente o total de `nations.json`; `region_population` é igual a
`max(0, population_region − urban_population)` em 100% das cidades.

## 4. Comparação com a regra anterior

**Top 20 regiões (nova regra), com o valor anterior:**

| cidade | país | antes | depois | Δ% |
|---|---|---:|---:|---:|
| Kolkata | IND | 130 710 619 | 241 558 646 | +84,8% |
| Guangzhou | CHN | 104 379 181 | 238 067 643 | +128,1% |
| Xi'an | CHN | 86 895 831 | 203 898 126 | +134,6% |
| Lucknow | IND | 108 544 665 | 169 921 943 | +56,5% |
| **Patna** | IND | **226 162 854** | **133 266 004** | **−41,1%** |
| Chengdu | CHN | 59 125 061 | 127 492 265 | +115,6% |
| Bengaluru | IND | 65 293 411 | 109 150 261 | +67,2% |
| Kanpur | IND | 58 157 725 | 99 389 888 | +70,9% |
| Beijing | CHN | 76 295 642 | 96 057 294 | +25,9% |
| Jaipur | IND | 60 777 115 | 85 047 304 | +39,9% |
| Nanjing | CHN | 61 686 971 | 83 849 199 | +35,9% |
| Manila | PHL | 62 046 030 | 70 066 820 | +12,9% |
| Bhopal | IND | 26 703 637 | 65 846 885 | +146,6% |
| Hangzhou | CHN | 61 063 261 | 63 600 946 | +4,2% |
| Jinan | CHN | 136 091 156 | 62 679 732 | −53,9% |
| Dongguan | CHN | 46 741 372 | 61 312 202 | +31,2% |
| Hefei | CHN | 61 165 454 | 60 695 899 | −0,8% |
| Wuhan | CHN | 152 628 770 | 57 554 997 | −62,3% |
| New Delhi | IND | 112 547 743 | 57 077 126 | −49,3% |
| Shenyang | CHN | 39 901 076 | 52 223 497 | +30,9% |

**Patna**: 226,2 M → 133,3 M (−41,1%). A região agora é exatamente o estado do **Bihar**
(133.266.004), o valor real do estado — a distorção de 226 M sumiu.

**Magnitude da mudança:** 372 de 834 cidades (44,6%) mudam a região em mais de 20%;
358 delas com |Δ| > 100 mil. Para comparação: o plano registrava 7 aglomerações com
região menor que o urbano na regra antiga; hoje são 17 (§8).

**Maiores variações absolutas:**

| cidade | país | antes | depois | Δ | Δ% |
|---|---|---:|---:|---:|---:|
| Guangzhou | CHN | 104 379 181 | 238 067 643 | +133 688 462 | +128,1% |
| Xi'an | CHN | 86 895 831 | 203 898 126 | +117 002 295 | +134,6% |
| Kolkata | IND | 130 710 619 | 241 558 646 | +110 848 027 | +84,8% |
| Wuhan | CHN | 152 628 770 | 57 554 997 | −95 073 773 | −62,3% |
| Patna | IND | 226 162 854 | 133 266 004 | −92 896 850 | −41,1% |
| Jinan | CHN | 136 091 156 | 62 679 732 | −73 411 424 | −53,9% |
| Chengdu | CHN | 59 125 061 | 127 492 265 | +68 367 204 | +115,6% |
| Lucknow | IND | 108 544 665 | 169 921 943 | +61 377 278 | +56,5% |
| Chongqing | CHN | 91 897 319 | 31 845 824 | −60 051 495 | −65,3% |
| New Delhi | IND | 112 547 743 | 57 077 126 | −55 470 617 | −49,3% |
| Imphal | IND | 55 858 248 | 7 144 519 | −48 713 729 | −87,2% |
| Bengaluru | IND | 65 293 411 | 109 150 261 | +43 856 850 | +67,2% |

**Por nação grande** (estados = admin-1 do país; multi-cidade = estados com 2+ cidades;
`>20%` = cidades cuja região mudou mais de 20%):

| país | estados | cidades | multi-cidade | >20% | regiao antes | regiao depois |
|---|---:|---:|---:|---:|---:|---:|
| IND | 35 | 25 | 5 | 19 | 1 463 865 525 | 1 463 865 525 |
| BRA | 27 | 25 | 3 | 15 | 212 812 405 | 212 812 405 |
| USA | 53 | 33 | 5 | 15 | 341 784 857 | 341 784 857 |
| CHN | 47 | 28 | 7 | 22 | 1 406 585 000 | 1 406 585 000 |
| NGA | 37 | 13 | 0 | 9 | 237 527 782 | 237 527 782 |
| RUS | 90 | 45 | 6 | 20 | 143 513 328 | 143 513 328 |
| IDN | 33 | 19 | 3 | 8 | 285 721 236 | 285 721 236 |
| PAK | 8 | 14 | 3 | 8 | 255 219 554 | 255 219 554 |
| FRA | 101 | 15 | 0 | 3 | 68 720 337 | 68 720 337 |
| DEU | 16 | 12 | 3 | 7 | 83 491 249 | 83 491 249 |

O total por nação **não muda** (é a mesma população, redistribuída); muda a região de cada
cidade. Na Nigéria não há nenhum estado com 2+ cidades; na França também não (os 101
admin-1 franceses anexam a uma das 15 cidades).

## 5. Estados com 2+ cidades

**67 estados têm 2 ou mais cidades**; em **65** as províncias ficam divididas entre 2+
cidades (nos outros 2 a cidade secundária não conseguiu nenhuma província — ver §7).
O estado fica registrado em `cities[].states` de todas as cidades que possuem alguma
província dele; **somar `states` de duas cidades conta o estado duas vezes** — o número
correto por cidade é `population_region`.

| estado | país | população do estado | cidades |
|---|---|---:|---|
| Uttar Pradesh | IND | 249 170 583 | Kanpur, Lucknow |
| Maharashtra | IND | 132 138 148 | Mumbai, Nagpur, Nashik, Pune |
| Punjab | PAK | 130 530 345 | Arifwala, Bahawalpur, Faisalabad, Gujranwala, Lahore, Multan, Sargodha |
| Guangdong | CHN | 126 219 136 | Dongguan, Guangzhou |
| Shandong | CHN | 99 855 099 | Jinan, Weifang |
| Madhya Pradesh | IND | 92 909 705 | Bhopal, Indore |
| Tamil Nadu | IND | 81 465 287 | Chennai, Coimbatore, Kallakurichi, Madurai |
| Gujarat | IND | 75 249 736 | Ahmedabad, Surat, Vadodara |
| Dhaka | BGD | 61 218 643 | Dhaka, Nagar Naluākot, Nāgarpur |
| Sind | PAK | 57 781 852 | Karachi, Hyderabad |
| Oromiya | ETH | 56 306 635 | Addis Ababa, Nazrēt |
| Jawa Barat | IDN | 47 722 089 | Bandung, Tasikmalaya |
| São Paulo | BRA | 47 082 522 | São Paulo, Campinas |
| California | USA | 43 621 446 | Los Angeles, San Jose |
| Jawa Timur | IDN | 42 682 110 | Surabaya, Malang |

Exemplo: São Paulo (estado) tem 47,1 M; a capital fica com 27,4 M e Campinas com 19,7 M
(as províncias do interior mais próximas de Campinas). Maharashtra: Mumbai, Pune, Nagpur
e Nashik dividem 132,1 M.

## 6. Estados sem cidade no roster

**3.497 estados** não têm nenhuma cidade do roster e foram anexados:

| regra | estados |
|---|---:|
| `neighbor_largest_population` (BFS por anel; maior cidade no mesmo anel) | 3.426 |
| `nearest_centroid` (sem caminho até cidade; centroide mais próximo) | 65 |
| `no_city_in_nation` (nação sem cidade alguma — territórios de população 0) | 6 |

Os 20 maiores anexados (população do estado → cidade que o recebeu):

| estado | país | população | cidade |
|---|---|---:|---|
| Henan | CHN | 98 807 834 | Xi'an |
| Hebei | CHN | 73 973 601 | Beijing |
| Hunan | CHN | 65 364 287 | Guangzhou |
| Guangxi | CHN | 49 792 110 | Guangzhou |
| Odisha | IND | 48 251 532 | Kolkata |
| Jiangxi | CHN | 44 779 744 | Guangzhou |
| Jharkhand | IND | 42 050 057 | Kolkata |
| Guizhou | CHN | 38 625 398 | Chengdu |
| Kerala | IND | 37 552 378 | Bengaluru |
| Assam | IND | 37 535 208 | Kolkata |
| Shanxi | CHN | 34 668 260 | Xi'an |
| Haryana | IND | 33 801 450 | New Delhi |
| Chhattisgarh | IND | 31 993 104 | Mumbai |
| Inner Mongol | CHN | 23 897 150 | Xi'an |
| Jilin | CHN | 23 873 808 | Shenyang |
| México | MEX | 17 968 864 | Mexico City |
| Rift Valley | KEN | 15 143 445 | Nairobi |
| Jammu and Kashmir | IND | 14 537 086 | Ludhiana |
| Bandundu | COD | 13 950 300 | Kinshasa |
| Banten | IDN | 13 938 773 | Jakarta |

A lista completa fica no próprio JSON (`states[].attach_rule` e `states[].city_id`).
Onde há muitas unidades pequenas, o anexo reflete a geografia: em GBR (218 admin-1 para
11 cidades), SVN (191), LVA (117), ITA (109) e FRA (101) quase todo estado anexa.

## 7. Efeito em TERRA cultivável

Área cultivável por cidade = Σ `area_km2` × `cultivable` (`province_nature.json`) das
províncias que a cidade possui. A área **total por nação não muda** (é a mesma terra,
redistribuída); o que muda é quem fica com ela.

| cidade | país | antes km² | depois km² | Δ | Δ% |
|---|---|---:|---:|---:|---:|
| Chicago | USA | 206 659 | 439 099 | +232 440 | +112,5% |
| Nashville | USA | 344 405 | 134 127 | −210 278 | −61,1% |
| Guangzhou | CHN | 133 572 | 325 230 | +191 658 | +143,5% |
| Belém | BRA | 165 949 | 350 148 | +184 199 | +111,0% |
| Montréal | CAN | 47 970 | 211 687 | +163 718 | +341,3% |
| Birobidzhan | RUS | 165 323 | 11 683 | −153 640 | −92,9% |
| Xi'an | CHN | 101 308 | 253 319 | +152 012 | +150,0% |
| Nikolayevsk-on-Amure | RUS | 69 886 | 215 704 | +145 818 | +208,6% |
| Miami | USA | 20 808 | 157 449 | +136 641 | +656,7% |
| Kolkata | IND | 56 254 | 187 106 | +130 852 | +232,6% |

Totais por nação grande (km² cultiváveis): IND 827.771, BRA 2.317.360, USA 2.576.143,
CHN 1.644.932, NGA 199.687, RUS 2.006.532, IDN 436.387, PAK 41.220, FRA 263.074,
DEU 103.426 — **idênticos antes e depois**. As 44 províncias sem aptidão agrícola
(ilhas minúsculas) entram com 0.

## 8. Casos especiais e ressalvas

1. **Frente RUS × UKR**: Luhansk inteira é RUS; Donets'k, Kherson e Zaporizhzhya são
   divididas. A parte ocupada vira um estado próprio (`UKR-…@RUS`), com a mesma geometria
   do admin-1 de origem. A cidade decide o lado pela nação: Mariupol (RUS) fica no estado
   ocupado; Kramatorsk (UKR) no estado ucraniano.
2. **Cidades com região 0**: Miyakojima (JPN) e Ponce (USA) — são a segunda cidade de um
   estado com uma única província (Okinawa, Porto Rico); a principal ficou com ela.
3. **17 cidades com região menor que o urbano** (50,8 M de excesso; piso 0 no interior):
   Shanghai (−18,0 M), Buenos Aires (−12,2 M), Osaka (−7,0 M), Hong Kong (−3,4 M),
   Jakarta (−2,5 M). A aglomeração do roster é maior que o estado inteiro — ressalva do
   plano que agora atinge 17 cidades (antes eram 7). Recomendação: opção 3 (capitais
   extras para estados grandes sem cidade) se o dono quiser corrigir.
4. **Regiões gigantes** (Guangzhou 238 M, Kolkata 242 M, Xi'an 204 M): vêm do anexo em
   cadeia. O plano já previa essa possibilidade e deixa a opção 3 como fatia futura.
5. **Admin-1 minúsculo**: GBR (218 admin-1, boroughs) e MKD (84 municípios) fazem a
   "capital do estado" ser Westminster (12 km²) e Čair (5 km²) — Londres e Skopje ficam
   com o borough/município e absorvem os vizinhos por BFS (Londres: 14,4 M). 48 admin-1
   não têm nenhum pixel no raster e por isso usam a vizinhança por centroide.
6. **Hong Kong**: a cidade do roster fica na ilha; 9 distritos de Kowloon/New Territories
   anexam a Guangzhou porque a fronteira terrestre deles é com Guangdong e o porto não é
   fronteira. Efeito da regra "maior cidade vizinha", registrado e não corrigido à mão.
7. **Sede da cidade pela geometria da fonte**: para cidades costeiras o pixel do raster
   pode cair na província vizinha (Nova York → Nova Jersey). A sede vem do polígono
   admin-1 do Natural Earth; por isso New York City é a cidade do estado de New York,
   San Juan de Porto Rico e Naha de Okinawa.
8. **57 estados com população 0**: territórios desabitados (Antártida, Clipperton, Coral
   Sea) e micro-unidades sem célula WorldPop; ficam com `population_2026 = 0` e cidade
   anexada ou `null` (os 6 de `no_city_in_nation`). Dois deles (`ATA+99?`, `RUS+99?`) vêm
   com **nome vazio na própria fonte** — o campo fica vazio em vez de inventar um nome.
9. **Splits**: em 65 estados as províncias ficam em 2+ cidades; `states[].city_id` é a
   cidade principal, mas ela pode possuir só parte do estado. Para somar população, use
   `cities[].population_region`.

## 9. Reprodutibilidade

```powershell
# TIFF WorldPop 2026 (1 km) — fora do git (290 MB)
python tools/build_state_regions.py `
  --tif G:/projetos/opengs-wt-data-b3/tools/data/worldpop/global_pop_2026_CN_1km_R2025A_v1.tif `
  --compare
```

- Fonte: WorldPop Global2 R2025A v1, `global_pop_2026_CN_1km_R2025A_v1.tif`,
  CC BY 4.0, DOI 10.5258/SOTON/WP00845; admin-1: Natural Earth 10m
  (`ne_10m_admin_1_states_provinces`).
- Determinístico: duas execuções produziram o mesmo sha256
  (`2e375cfb…`); não há data de execução no JSON.
- Verificação independente: `verificador-flash` conferiu BRA, IND e USA à mão
  (estrutura, nomes, somas, cidades, adjacência e Patna) — **APROVADO com
  ressalvas R1–R7**, parecer em `docs/team/reviews/Q-018_b3_state_regions_review.md`.
- **Nada disto está ligado à simulação**: é insumo da B3. Nenhuma mudança de
  `core/`, `game/`, `map/` ou do núcleo C++.
