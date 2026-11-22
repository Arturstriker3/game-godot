# E1 — Dados de energia: parque, matriz nacional, potencial e acesso

Escopo: **só dado**. Nada foi ligado à simulação, ao núcleo C++, a `core/`, `game/`,
`map/` ou UI. Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE E, E1. Branch
`data/e1-energy`, worktree `G:/projetos/opengs-wt-data-e1`, base atual
`balance/global-resources` @ `0edecf64` (HEAD de dados `ea020617` + correção 2 da
re-QA t33).

O aceite do E1 é: **por nação, capacidade instalada (MW) e matriz por fonte na ordem do
dado real 2024–2026; usinas individuais atribuídas à cidade/província dona no mapa do
jogo; potencial solar/eólico e hídrico derivado por província, normalizado por potencial
técnico nacional público; perdas T&D e acesso por país; licença conferida e registrada;
lacuna declarada onde não há dado.**

## 1. Artefatos

| Arquivo | Conteúdo | Registros | SHA-256 |
|---|---|---|---|
| `data/definitions/energy/national_capacity.json` | capacidade instalada e matriz por nação (2024–2025) + desvio do parque base | 171 nações, 161 comparações (90 em 2024, 81 em 2025) | `CA882623C40E5B97960294DC615176D97AA7918380297896F2556C49437FCE24` |
| `data/definitions/energy/power_plants_base_2021_gppd.json` | parque GPPD ~2021 **bruto** (usina, fonte, MW, lat/lon) atribuído à cidade/nação do atlas | 34.932 usinas / 5.706.702,5 MW / 743 cidades / 166 nações | `CB8C0F9959B86864D786AB1FFBA9D7517A23AA75584820EEFC9E78A16B867E49` |
| `data/definitions/energy/power_plants_2026_scaled.json` | parque **escalonado** para a matriz Ember 2024–2025, com fator por nação×fonte e usinas sintéticas declaradas | 36.031 usinas (34.932 reais + 1.099 sintéticas) / 10.349.013,2 MW (10.349.005,6 MW por nação + 7,6 MW sem cidade) / 825 cidades / 176 nações | `B163A9AD7C8E304D3B9943CEB3838E5ECC01D5C5289F2950CF8CD6F562878B46` |
| `data/definitions/energy/renewable_potential.json` | solar e eólico por província/cidade (GSA/GWA) + proxy hídrico bruto (CHIRPS/CHELSA PET/ETOPO1) | 15.547 províncias / 849 cidades | `A476F01C609E96ACAD46150CE84006A176C79A547867F817AB650F16A6F3C940` |
| `data/definitions/energy/hydro_potential.json` | potencial hídrico em MW por província/cidade, normalizado pela âncora nacional | 179 nações (135 com âncora) | `106DBE4085C24EE5BEB7ADFBC5EFBDD3EAACADA0C9572E7E66A6D5D341BB7C60` |
| `data/definitions/energy/access_losses.json` | acesso à eletricidade (total/urbano/rural), perdas T&D, água segura | 217 países (176 jogáveis) | `6D69FD90CED04A341E56FE80B7CE8D6C75BFC871CB36ABD99F11565BEF4F26D5` |
| `data/definitions/energy/land_borders.json` | pares de nações com fronteira terrestre (E5) | 299 pares / 152 nações | `5465B7E5017A21C12219595E80AC657E325084DCB6235C7EE65DB7E6290F8451` |
| `data/definitions/energy/_sources.json` | registro único de fonte, URL, acesso, licença, evidência e transformação | 17 fontes (10 usadas, 1 citação, 6 recusadas) | `05FCC88BA338471A2624D8511600A96790306454AAD981FB7BA5ED8438CEB24F` |

Scripts reprodutíveis (determinísticos; 2+ execuções com SHA-256 idêntico):

```text
tools/data/energy/fetch_ember_capacity.py      cache Ember (CSV ~16 MB, não versionado)
tools/data/energy/fetch_wb_hydro_anchor.py     cache do indicador do Banco Mundial
tools/data/energy/fetch_atlas_points.py        cache GSA/GWA por província (ponto a ponto)
tools/data/energy/fetch_hydro_inputs.py        cache CHIRPS/ETOPO1 (fontes livres)
tools/data/energy/fetch_pet_chelsa.py          cache PET CHELSA v2.1 (CC0), via HTTP Range
tools/data/energy/build_national_capacity.py   -> national_capacity.json
tools/data/energy/build_power_plants_scaled.py -> power_plants_2026_scaled.json
tools/data/energy/build_hydro_national.py      -> hydro_potential.json
tools/data/energy/build_sources_registry.py    -> _sources.json
tools/build_energy_plants.py                   -> power_plants_base_2021_gppd.json
tools/build_energy_renewables.py               -> renewable_potential.json
tools/build_energy_access.py                   -> access_losses.json
tools/build_energy_borders.py                  -> land_borders.json
tools/build_energy_common.py                   geografia/atribuição comum
tools/validate_energy_data.py                  validador
```

Caches baixados ficam em `tools/data/energy/cache/` (gitignored, com manifest de URL +
SHA-256 + data de acesso). Nada acima de 20 MB é versionado. `NE_DIR` é obrigatório
quando o Natural Earth 10m não está em `tools/data/natural_earth`: não existe mais
fallback para caminho absoluto de máquina (`G:\tools\ne_extract`), e a falha é explícita.

## 2. Fontes e licenças (verificado em 2026-11-22)

| Fonte | Uso | Licença | Situação |
|---|---|---|---|
| **GPPD v1.3.0** (WRI, 2021) | usinas individuais | **CC BY 4.0** | usada |
| **Ember — Yearly Electricity Data** (release global, 2026) | capacidade e matriz nacional 2024–2025 | **CC BY 4.0** | usada |
| **Banco Mundial Data360 — `WB_CLEAR_HP_TECHNFEASIBLE_PT`** (2021) | âncora do potencial hídrico | **CC BY 4.0** | usada |
| **Banco Mundial WDI** via Data360 (acesso, perdas T&D; água = JMP) | acesso e perdas | **CC BY 4.0** | usada |
| **Global Solar Atlas 2.x** (Solargis/ESMAP/Banco Mundial) | GHI e PVOUT (anual + mensal) | **CC BY 4.0** | usada |
| **Global Wind Atlas 3/4** (DTU/ESMAP/Banco Mundial) | vento/densidade a 100 m (Weibull) | **CC BY 4.0** | usada |
| **CHIRPS v3.0** (CHC/UCSB, 2025) | precipitação anual do proxy hídrico | **domínio público / CC BY 4.0** | usada |
| **CHELSA v2.1 — PET** (WSL, ERA5/Penman-Monteith, 1981–2010) | PET anual do runoff | **CC0 1.0** (domínio público; uso comercial livre) | usada |
| **ETOPO1** (NOAA/NCEI) | relevo do proxy hídrico | **domínio público** | usada |
| **Natural Earth 10m** (rios, admin) | rios do proxy hídrico, divisas | **domínio público** | usada |
| **WorldClim 2.1** | — | uso acadêmico/não comercial; redistribuição e uso comercial proibidos sem permissão | **recusada** — saiu do E1 (D-115c) |
| **CGIAR-CSI Global-PET v3.1** (Zomer & Trabucco) | PET (versão anterior desta rodada) | página do CGIAR-CSI: “provided for **non-commercial use**”; o produto é **derivado do WorldClim 2.1** (o figshare rotula CC BY 4.0, mas não é o termo do produto) | **recusada** — re-QA t33; trocada pela CHELSA |
| **Our World in Data — Energy** | tentativa de separar óleo | CC BY 4.0, mas só geração (`oil_electricity_twh`), não capacidade | **recusada** — lacuna declarada |
| **EIA International Energy Data** (bulk INTL) | tentativa de separar óleo | domínio público, mas só capacidade fóssil agregada e geração de petróleo | **recusada** — lacuna declarada |
| **IRENA ELECSTAT** | comparação de capacidade | termos IRENA art. 4.1 (**não comercial**) | **recusada** |
| **Global Energy Monitor (trackers)** | usinas | CC BY 4.0, mas o download exige atestar “não uso comercial” | **recusada** |
| **IJHD — Hydropower Essentials** | número global de potencial (16.400 TWh/ano) | citação de fato, com atribuição | só citação |

**Decisão D-115(c):** o WorldClim **não** é mais usado no E1. Solar/eólico vêm do
GSA/GWA (CC BY 4.0); o hídrico usa CHIRPS (domínio público/CC BY 4.0), PET CHELSA
v2.1 (CC0 1.0), ETOPO1 (domínio público) e rios Natural Earth. A dependência
**pré-existente** de `data/definitions/natural/province_nature.json` no WorldClim
(clima do jogo) é dívida separada, registrada no §15 — não foi mexida nesta fase.
`validate_energy_data.py --require-commercial` **passa** no estado atual.

**Correção 2 (re-QA t33):** a rodada anterior usava o **CGIAR-CSI Global-PET v3.1**
com a evidência errada ("CC BY 4.0, derivado do WorldClim bruto"). A página oficial do
CGIAR-CSI diz que Global-PET/Global-Aridity são **"provided for non-commercial use"** e
**"both modeled using the data available from WorldClim Global Climate Data"**
(`https://csidotinfo.wordpress.com/data/global-aridity-and-pet-database/`, verificado
em 2026-11-22). A fonte foi **recusada** e trocada pela **CHELSA v2.1 PET**
(`https://chelsa-climate.org/datasets/chelsa_climatologies/`, verificado em
2026-11-22): “License: **Creative Commons Zero - No Rights Reserved (CC0 1.0)**”,
Penman-Monteith com forçante **ERA5** (não é derivado do WorldClim). A forçante é a
declarada na **especificação técnica do CHELSA V2.1**
(`https://chelsa-climate.org/models/chelsa`): o modelo faz downscaling de campos de
reanálise e a precipitação não corrigida (`prec`) e o vento (`sfcWE`) vêm do **ERA5**; o
artigo de forçante é Karger et al. (2023), *CHELSA-W5E5: daily 1 km meteorological forcing
data for climate impact studies*, Earth Syst. Sci. Data 15, 2445–2464,
`https://doi.org/10.5194/essd-15-2445-2023`. Verificado em 2026-11-22. O validador ganhou
uma **lista de bloqueio explícita** (`NON_COMMERCIAL_SOURCE_IDS`) e varredura de texto
de licença, além de `--selftest-commercial`, que prova que uma fonte não comercial em
uso reprova mesmo com `commercial_ok: true` no registro.

**Decisão D-115(d):** a Ember não separa óleo (`Other fossil`); a tentativa com OWID e
EIA não encontrou capacidade instalada de óleo por país em nenhuma das duas (OWID só
publica geração; o EIA só tem capacidade fóssil agregada e geração de petróleo). `oil`
fica `null` em `national_capacity.json` e o parque de óleo do GPPD fica **não
escalonado** em `power_plants_2026_scaled.json`, com o motivo declarado.

## 3. Matriz nacional 2024–2025 (Ember)

Regra declarada (correção 2, re-QA t33): para cada país, vale o **último ano entre 2024
e 2025 em que a Ember publica as 9 fontes de capacidade** (Hydro, Solar, Wind, Coal, Gas,
Nuclear, Bioenergy, Other fossil, Other renewables) — **mesmo sem a linha
`Total generation`**. Nesse caso o total é a soma exata das fontes
(`total_source: sum_sources`). Nenhum ano é interpolado nem extrapolado e
`year_reason` declara por país como o ano foi escolhido:

| `year_reason` | Significado |
|---|---|
| `all_9_sources_and_total` | 9 fontes + `Total generation` (total da Ember) |
| `all_9_sources_no_total` | 9 fontes, sem `Total generation` (total = soma das fontes) |
| `total_only_partial_sources` | nenhum ano com 9 fontes; usa o último com total > 0 e lista `sources_missing` |
| `partial_sources_any_positive` | só há capacidade positiva esparsa (zero puro não conta, ex.: KGZ 2025) |

Exemplo do efeito: **DEU 2025** tem as 9 fontes e **não** tem `Total generation`;
antes caía para 2024 (248,18 GW), agora vale 2025 com **268,38 GW** (soma das fontes).

Mapeamento para as 9 fontes do jogo: `Hydro→hydro`, `Solar→solar`, `Wind→wind`,
`Coal→coal`, `Gas→gas`, `Nuclear→nuclear`, `Bioenergy→biomass`,
`Other fossil + Other renewables → other`. **Óleo não é separado pela Ember** (vem dentro
de `Other fossil`), então `oil: null` em todos os países, com
`other_breakdown.other_fossil_mw` e o motivo em `meta.gaps.oil` — a tentativa com
OWID/EIA está no §2. **Nenhum número foi inventado.**

Mundo (soma dos 171 países com dado): **10.054,6 GW**, 0 divergência entre a soma das
fontes e o total (`check.sum_matches_total` verdadeiro em 171/171, tolerância 0,5 MW).
Distribuição dos anos: **81 países em 2025, 90 em 2024**; `year_reason`:
`all_9_sources_and_total` 32, `all_9_sources_no_total` 2, `total_only_partial_sources` 135,
`partial_sources_any_positive` 2. A maioria fica em 2024 porque a Ember 2025 ainda não
publicou alguma linha de fonte do país (e não há publicação anterior = 0 que autorize
assumir zero); nesses casos o total é a linha `Total generation` de 2024.

**Janela da prova de zero (re-QA t39):** `sources_absent_as_zero` vale **só dentro da
janela 2024–2025**. A fonte ausente no ano escolhido é lida como zero apenas porque a
**última publicação anterior** daquela fonte no dataset da Ember é `0.0` — e essa
publicação pode ser de um ano **anterior à janela** (ex.: nuclear alemã ausente em 2025 e
0 desde 2023). É uma comprovação de zero **na janela**, não prova histórica de que a fonte
nunca existiu; nenhum ano antes de 2024 vira matriz e a ausência **sem** publicação
anterior = 0 continua `null` (§5, `national_target_missing`), nunca zero inventado.

| Fonte | Mundo (GW) |
|---|---|
| solar | 2.364,3 |
| carvão | 2.214,1 |
| gás | 2.058,6 |
| eólica | 1.289,0 |
| hídrica | 1.286,5 |
| nuclear | 393,1 |
| outras | 319,5 |
| biomassa | 129,6 |
| óleo | **lacuna** (dentro de “outras”) |

### 10 maiores por MW (cap = 2024–2025)

| # | País | Ano | Total GW | hídrica | solar | eólica | carvão | gás | nuclear | biomassa | outras |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | CHN | 2025 | 3.726,5 | 382 | 1.202 | 641 | 1.243 | 161 | 62 | 19 | 17 |
| 2 | USA | 2025 | 1.333,2 | 84 | 212 | 159 | 189 | 550 | 97 | 10 | 33 |
| 3 | IND | 2025 | 555,5 | 49 | 136 | 55 | 251 | 27 | 9 | 11 | 19 |
| 4 | JPN | 2025 | 322,9 | 28 | 92 | 6 | 55 | 82 | 33 | 6 | 20 |
| 5 | DEU | 2025 | 268,4 | 6 | 106 | 78 | 32 | 32 | 0 | 9 | 5 |
| 6 | BRA | 2025 | 260,6 | 110 | 65 | 35 | 3 | 19 | 2 | 18 | 8 |
| 7 | RUS | 2025 | 237,5 | 52 | 2 | 3 | 37 | 113 | 29 | 0 | 1 |
| 8 | FRA | 2025 | 167,6 | 24 | 31 | 26 | 2 | 18 | 61 | 2 | 4 |
| 9 | KOR | 2025 | 158,9 | 2 | 30 | 2 | 42 | 50 | 26 | 3 | 3 |
| 10 | CAN | 2025 | 156,0 | 84 | 6 | 18 | 3 | 27 | 13 | 3 | 2 |

### Cobertura e lacunas (179 nações do jogo)

- **171 com matriz 2024–2025**; **8 sem dado** na janela: `CAF, DMA, LKA, PSX, SAH, SOL,
  TLS, UKR`. Em CAF/DMA/LKA/UKR a Ember publica o país mas **sem capacidade** em 2024/2025
  (a Ucrânia tem série interrompida); PSX/SAH/SOL/TLS não existem como país na Ember. O
  último recurso (nada de valor inventado) é a lacuna declarada em
  `meta.gaps.nations_without_ember`.
- Ano-base 2024 em 92 países é **dado real**, não estimativa: a Ember ainda não publicou
  2025.
- Ember tem 209 áreas; 38 não são nação do jogo (agregados, territórios).

## 4. Parque base individual (GPPD, ~2021) — D-115(a)

- **34.932 usinas / 5.706.702,5 MW**; 34.930 com cidade, 34.930 com nação; **743 cidades**
  em 166 nações. Cada registro é `method: gppd_2021_unadjusted` — **nada extrapolado para
  2026**; `meta.data_year = 2021`.
- **Regra (a):** `nation` é a nação da **cidade/província do atlas** (controlador no mapa
  do jogo), não a nação do GPPD. O ISO3 bruto do GPPD fica em `gppd_country`; o código
  traduzido para o jogo fica em `gppd_country_game` (aliases `PSE→PSX`, `GUF→FRA`). O
  total por nação é a soma exata das cidades do atlas; usina sem cidade (as 2 estações
  da Antártida, 7,6 MW) fica fora do agregado e é declarada em
  `meta.counts.mw_without_city`.
- **Conflitos de localização (300 usinas / 61.945,7 MW = 148 + 125 + 27)** classificados
  por causa. O `location_conflict` só existe para usina que **continua** no parque:

  | Causa | Usinas | MW | Regra aplicada |
  |---|---:|---:|---|
  | `border_pixel` | 148 | 38.968,5 | fica na cidade mais próxima da nação do atlas |
  | `deleted_territory` | 125 | 2.016,1 | território do GPPD que não existe no jogo; fica na cidade do atlas |
  | `occupied` | 27 | 20.961,0 | território disputado/ocupado; vale o controlador do atlas |
  | **total contado** | **300** | **61.945,7** | `meta.location_conflicts.count` |

  **Fora do count:** `gppd_coord_error` — **5 usinas / 1.472,2 MW**. O erro de
  coordenada/país não é um conflito de atribuição: a usina é **corrigida com evidência**
  (1 caso, sai da lista de conflitos) ou **descartada** (4 casos, saem do parque) e por
  isso não entra em `meta.location_conflicts.count` nem em `by_cause`. O detalhe fica em
  `meta.location_conflicts.coord_error` (`corrected: 1`, `discarded: 4`),
  `meta.coord_fixes` e `meta.discarded`. Somar 5 ao count daria 305 e quebraria a
  identidade com as usinas do arquivo.

  Os 10 pares mais frequentes: PRT→ESP 36, FRA→MDG 25, MUS→MDG 23, UKR→RUS 21,
  FRA→DMA 20, CAN→USA 10, USA→CAN 8, FRA→LCA 8, DEU→CHE 6, ESP→MAR 6.
- **Correção comprovada:** `WRI1006467` (ESCOMBRERAS GRUPO 1) tinha lat 7,5732 em Gana;
  o complexo correto é lat 37,5732, mesmo lon (-0,9392), mesmo dono (GDF SUEZ CARTAGENA
  ENERGIA) e mesmo ano (2006) da usina irmã `WRI1006415` (CTCC ESCOMBRERAS), em ESP-VAL.
  Corrigido para 37,5732; a usina passa a ESP e deixa de ser conflito.
- **Descartes (4 usinas / 272,95 MW):** `GARONA VERDA` (ESP, lat 0,7046/lon 42,8025 na
  Somália), `P.E. FERREIRA 2` (ESP, lat -2,9997/lon 37,2174 na Tanzânia/Quênia),
  `P.E. FOLCH II` (ESP, lat -0,2108/lon 40,4682 no Quênia/Somália) e `Outside the
  Northeast` (NLD, lat 38,0211/lon -87,5172 em Indiana/EUA). Nenhuma tem usina irmã ou
  fonte para corrigir; foram **descartadas**, nunca realocadas por palpite. O detalhe
  fica em `meta.discarded`.
- **Invariante QA:** `Σ by_city[nação] == by_nation[nação]` (≤0,01 MW) — diferença
  **0,000000 MW** no arquivo base; `Σ usinas = by_nation + mw_without_city`.
- **GPPD × Ember (161 nações):** mediana **−45,1 %** (base ~2021 abaixo da matriz
  2024–2025); por fonte: carvão −1,4 %, nuclear −0,6 %, hídrica −19,4 %, gás −41,8 %,
  solar −97,2 %, eólica −95,3 %, biomassa/outras −100 %. Positivos: LUX +145,9 %,
  XKX +104,0 %, PRY +95,2 %, MDG +79,9 %, ATG +39,1 %. Leitura: o GPPD não serve como
  total de MW de 2026; serve como nome, fonte, coordenada e “quem existe”.

## 5. Parque escalonado 2026 — D-115(b)

`power_plants_2026_scaled.json` (`data_year: 2026`, `capacity_target_years: [2024, 2025]`,
`base_reference_year: 2021`) aplica **fator declarado por nação×fonte**:

```text
fator = alvo_Ember_mw / base_GPPD_mw          (em mili-MW)
capacity_mw = round(base_mili * fator)        (resto na maior usina do grupo)
```

- Fonte **sem usina no GPPD** mas com MW na Ember: o alvo é distribuído entre as cidades
  da nação por **maior resto**, com peso declarado — solar pelo GHI anual GSA, eólico pelo
  vento GWA a 100 m, hídrico pelo potencial hídrico, demais pela **população** (indústria
  por cidade não existe como dado; declarado). Cada cidade recebe uma usina
  `synthetic: true` com `method: ember_2024_2025_city_allocation`.
- Fonte com alvo **zero** na Ember (ex.: nuclear alemã, carvão britânico): fator 0
  declarado; a usina permanece como registro com 0 MW e
  `scaling_note: retired_by_ember_target_zero` (o zero é o **alvo da janela 2024–2025**,
  não prova de que a usina nunca operou; o MW do GPPD ~2021 fica preservado em
  `capacity_mw_gppd`).
- **Óleo:** alvo `null` (Ember não separa); o parque de óleo do GPPD fica **não
  escalonado** e o motivo é declarado. Nação sem matriz Ember (8 lacunas): parque fica
  não escalonado e declarado.
- **Sem alvo nacional (`national_target_missing`):** **109 usinas / 32.493,5 MW** ficam
  **não escalonadas** e cada uma leva `scaling_note: national_target_missing`
  (`meta.gaps.national_target_missing`, com `plants`, `mw` e `by_nation`). São dois
  grupos: (i) as 104 usinas das 8 nações sem matriz 2024–2025 (CAF, DMA, LKA, PSX, SAH,
  SOL, TLS, UKR — UKR sozinha tem 41 usinas/28.886,5 MW); (ii) **5 usinas / 978,1 MW em
  país com matriz mas sem a fonte no agregado** — CYP (gás, 868,0 MW), MDG (gás, 29,6 MW
  em 2 usinas), MWI (gás, 15,5 MW) e NIC (gás, 65,0 MW). A QA (t33) citou essa lacuna
  como 6 usinas/979 MW: o **MW confere (978,1)** e o arquivo tem **5** plantas nesses
  quatro países; a fonte ausente é sempre **gás** (a Ember não publica a linha, e não há
  publicação anterior = 0 que autorize assumir zero — por isso não vira 0,0).
- **Usinas reais escalonadas levam `method`:** `gppd_2021_scaled_to_ember` quando o fator
  foi aplicado e `gppd_2021_unadjusted` quando ficaram como no GPPD (óleo, sem alvo
  nacional); o validador exige o campo e a coerência com `scaling_method`.

**Resultado:** 36.031 usinas (34.932 reais + 1.099 sintéticas), **10.349.013,2 MW**
(10.349.005,6 MW por nação + **7,6 MW em 2 usinas sem cidade**, que ficam fora de
`by_nation`), 825 cidades, 176 nações. **1.584 checagens nação×fonte, 0 falhas** (≤0,01 MW).
Contra a Ember (**10.054.640,0 MW** nas 171 nações com matriz), `by_nation` fica
**294,4 GW acima**, e a aritmética fecha em três parcelas declaradas (re-QA t39):

| Parcela | MW | Leitura |
|---|---:|---|
| Óleo do GPPD **fora** das nações sem matriz | 260.659,3 | dos **261.878,7 MW** de óleo total, **1.219,4 MW** estão dentro das 5 nações sem matriz; a Ember não separa óleo (`Other fossil`) |
| 5 nações com usina e **sem** matriz Ember (CAF, DMA, LKA, PSX, UKR) | 32.734,8 | as outras 3 nações da lacuna (SAH, SOL, TLS) não têm usina no GPPD |
| `national_target_missing` **em nação com matriz**: 5 usinas de gás em CYP, MDG, MWI e NIC | 978,1 | a Ember não publica a linha de gás; sem publicação anterior = 0, fica não escalonado em vez de virar 0,0 |
| **Soma** | **294.372,2** | **294,4 GW** (a diferença de ~6,6 MW para o total medido é arredondamento de 3 casas por usina) |

Os **7,6 MW** das 2 usinas sem cidade **não entram** nessa conta, porque não estão em
`by_nation`; somados, dão o total bruto **10.349.013,2 MW**. Isso é lacuna declarada, não
erro de soma.

### 10 maiores após o escalonamento vs Ember

| # | País | Escalonado MW | Ember MW | Δ MW | Leitura |
|---|---|---|---:|---:|---|
| 1 | CHN | 3.727.839,0 | 3.726.510,0 | +1.329,0 | óleo não separado |
| 2 | USA | 1.370.393,4 | 1.333.250,0 | +37.143,4 | óleo não separado |
| 3 | IND | 557.140,8 | 555.460,0 | +1.680,8 | óleo não separado |
| 4 | JPN | 367.030,2 | 322.910,0 | +44.120,2 | óleo não separado |
| 5 | DEU | 271.231,2 | 268.380,0 | +2.851,2 | óleo não separado (alvo agora é 2025) |
| 6 | BRA | 268.923,6 | 260.640,0 | +8.283,6 | óleo não separado |
| 7 | RUS | 237.569,1 | 237.470,0 | +99,1 | óleo não separado |
| 8 | FRA | 172.028,0 | 167.600,0 | +4.428,0 | óleo não separado |
| 9 | KOR | 163.535,0 | 158.880,0 | +4.655,0 | óleo não separado |
| 10 | CAN | 158.334,0 | 155.990,0 | +2.344,0 | óleo não separado |

As fontes que a Ember separa fecham **exatamente** no alvo (0,00 MW); a diferença de cada
linha é só o óleo do GPPD, que a Ember agrega em `Other fossil`.

## 6. Potencial solar e eólico por província — D-115(c)

- **Global Solar Atlas 2.x** (Solargis/ESMAP/Banco Mundial, CC BY 4.0): GHI e PVOUT
  anuais + **série mensal**, amostrados no centróide da província. A série mensal é
  convertida para kWh/m²/dia (GHI) e kWh/kWp/dia (PVOUT) dividindo pelos dias do mês;
  a média anual usa o total anual / 365,25.
- **Global Wind Atlas 3/4** (DTU/ESMAP/Banco Mundial, CC BY 4.0): clima generalizado de
  Weibull a 100 m; a média e a densidade de potência usam a classe de rugosidade
  **0,100 m** (campo aberto/pastagem), declarada; a densidade é corrigida pela densidade
  do ar na altitude média da província (ETOPO1, atmosfera padrão).
- **O GWA não publica série mensal**: o vento fica com média anual e
  `monthly_available: false`, `monthly_m_s: null` — lacuna declarada, sem inventar
  sazonalidade. O E2 deve usar a média anual no fator de capacidade eólico.
- Por cidade: média ponderada pela **área da província × fração da cidade**, com a
  série de 12 meses do GSA (GHI/PVOUT) preservada.
- **Cobertura da coleta:** **12.298** províncias com ponto medido completo na API
  GSA/GWA; **2.965** usam o ponto medido mais próximo da **mesma nação** (raio 12°),
  declarado em `sample_method: nearest_cached_province`, `sample_source_pid` e
  `sample_distance_km`; **284** ficam sem amostra (no_data) porque não há ponto medido
  dentro do raio. **705** pontos do cache ficaram com erro de API depois das tentativas.
  O fallback evita buraco, mas não substitui o ponto local — é uma interpolação espacial
  declarada.
- Sanidade (nação → solar kWh/m²/dia · vento m/s a 100 m): entra no validador por faixa
  física (0 < solar < 9; 0 < vento < 20); amostras conhecidas: BRA ~5,0 · ~5,5;
  DEU ~3,0 · ~6,0; USA ~4,5 · ~6,5; NOR ~2,4 · ~7,0 (valores de referência do atlas, não
  calibração).
- Limites declarados: GSA/GWA são climatologias; o vento é média anual (sem
  sazonalidade) e a amostra no centróide não captura micro-sítio. A licença é CC BY 4.0
  (comercial), com créditos obrigatórios (§15).

## 7. Potencial hídrico — D-115(c), sem WorldClim

O proxy bruto por província é
`raw = precip/1000 × área × desnível/1000 × runoff × river_factor`, com `confidence: low`:

- **precip_mm** = CHIRPS v3.0 anual 2025 (domínio público/CC BY 4.0), 0,05°, no centróide
  da província. CHIRPS cobre **60°N–60°S**; as **1.746 províncias** fora dessa faixa ficam
  `no_data` (lacuna declarada), **sem** preencher com WorldClim.
- **PET** = **CHELSA-climatologies v2.1** (WSL, **CC0 1.0**, Penman-Monteith,
  climatologia 1981–2010, forçante ERA5 declarada na especificação técnica do CHELSA V2.1 —
  `https://chelsa-climate.org/models/chelsa` — e em Karger et al. 2023, ESSD 15,
  2445–2464, `https://doi.org/10.5194/essd-15-2445-2023`), PET anual no centróide;
  `runoff = clamp((precip − PET)/precip, 0, 1)`. O COG de 30 arc-sec é lido por
  **HTTP Range no overview 1/8 (0,0667° ≈ 7,4 km)** — `fetch_pet_chelsa.py` soma os 12
  meses e grava `chelsa_pet_annual_1981-2010_1of8.tif` (mm/ano) no cache. Resolução de
  7,4 km é declarada; CHIRPS é 0,05° (≈5,5 km).
- **desnível** = max−min da altitude **ETOPO1** (1 arc-min, domínio público) nos pixels de
  terra da província, na grade 4096×2048 do atlas.
- **river_factor** = cobertura de rio Natural Earth 10m a ≤50 km, piso 0,02.
- **Normalização nacional** (em `build_hydro_national.py`):
  `potencial_nacional_mw = hidro_instalada_2021_mw / (developed_pct_2021/100)`;
  `potencial_província_mw = potencial_nacional_mw × raw_província / Σ raw_nação`.
  `developed_pct_2021` = Banco Mundial Data360 `WB_CLEAR_HP_TECHNFEASIBLE_PT` (CC BY 4.0);
  `hidro_instalada_2021` = Ember (CC BY 4.0). O denominador não é a mesma publicação do
  Banco Mundial (Atlas Hidrelétrico 2021), então o valor carrega esse desvio — por isso a
  confiança é `low` em toda nação, nunca `high`.
- A soma das províncias de cada país é **igual** à âncora nacional (resto de arredondamento
  na província de maior `raw`); as frações somam 1. **135 das 179 nações** com âncora;
  **44 sem** (`national_anchor_missing`) — lacuna declarada, não zero inventado.
- **`distribution_missing` (25 nações):** âncora existe, mas o proxy é **0 em todas as
  províncias** (PET ≥ precipitação em todas as amostras), então nenhuma província recebe
  MW em vez de inventar distribuição: AFG, ARM, BFA, DMA, DZA, EGY, FIN, HUN, IRN, IRQ,
  ISL, KGZ, LBN, MAR, MDA, MLI, NAM, SDN, SYR, TGO, TJK, TUN, URY, UZB, ZMB. Outras 61
  nações têm âncora e proxy positivo.
- **Efeito da troca de PET (correção 2):** com a CHELSA a média global do PET amostrado é
  1.399 mm/ano (mediana 1.432; máx 3.441) contra 1.441/1.339/3.616 do CGIAR, e o runoff
  fica zero em **71,2 %** das províncias (antes 66,9 %). A lista de `distribution_missing`
  muda na margem: **AZE e PAK passam a ter distribuição** e **DMA e URY entram** na lacuna
  (25 → 25 nações: duas saem, duas entram). O proxy é grosseiro por construção
  (`confidence: low`) e sensível ao limiar de umidade — nenhuma dessas nações recebe
  número inventado.
- Mundo declarado: **4.642 GW** (soma das 171 âncoras disponíveis), coerente com a âncora
  global de 16.400 TWh/ano (a 0,40 de fator de capacidade ≈ 16,3 PWh/ano).
- O proxy global antigo (16.400 TWh/ano) continua como `normalized: global_anchor` em
  `renewable_potential.json`; a normalização nacional do E1 substitui essa escala em
  `hydro_potential.json`.

## 8. Acesso, perdas e água (E5/E7)

- Ano mais recente não vazio por país/indicador; ausente = `null`, sem interpolação.
- Cobertura entre 217 países: acesso à eletricidade 216, urbano/rural 214, **perdas T&D
  150**, água segura 151 (urbano 104, rural 85). **176 das 179 nações jogáveis** têm
  registro no WDI; 3 não (`SAH, SOL, TWN` — o WDI não publica linha para Saara Ocidental,
  Somalilândia nem Taiwan).
- **Ligação de código (correção 2):** o WDI publica `SSD`, `XKX` e `PSE`; o jogo usa
  `SSD`, `XKX` e `PSX`. A ligação é pelo id direto, pelo `NATION_CODE_OVERRIDES` do jogo
  (ex.: `NED→NLD`) e pelo alias explícito `PSE→PSX`. Antes, os três ficavam de fora
  porque o validador comparava `nation_code(tag)` (que devolve `SDS`/`KOS`) com a chave do
  WDI. SSD passou a ter acesso 5,4 %, XKX 100 %, PSX 100 %.
- Amostra (acesso total/urb/rural · perdas · água): BRA 99,8/99,9/99,2 · 14,9 % · 88,6;
  NGA 62,5/85,5/23,5 · 14,1 % · 29,9; NOR 100/100/100 · 4,6 % · 99,1; CHN 100/100/100 ·
  3,1 % · (só urbana 96,4).
- Perdas T&D são a referência do piso técnico do E5 (o piso nunca chega a zero).

## 9. Fronteiras terrestres (E5)

Derivadas do raster de donos com máscara de terra, 4-vizinhança, comprimento com cosseno
da latitude, pares < 30 km descartados: 299 pares entre 152 nações. Controles negativos
passaram (FRA-GBR, DNK-SWE, IDN-SGP, RUS-USA, CHN-TWN, ESP-MAR, ITA-TUN ausentes);
positivos coerentes (CHN 14 vizinhos, BRA 10, DEU 9, FRA 8, NOR 3, USA 2). A **lista de
pares** é o dado; o km é ±20 % (CAN-USA 9.841 km vs ~8.900 oficial).

## 10. Validação

```text
python tools/validate_energy_data.py
564101 checagens | 0 falhas | 1 avisos

python tools/validate_energy_data.py --require-commercial
564101 checagens | 0 falhas | 1 avisos

python tools/validate_energy_data.py --selftest-commercial
SELFTEST OK: fake_noncommercial: texto da licença com marcador restritivo (non-commercial)
```

Checagens: licença completa (nome, `commercial_ok`, data, evidência e `accessed_at_utc`)
para toda fonte usada e todo arquivo consumidor existente, **lista de bloqueio explícita**
de fontes não comerciais + marcadores de texto (um `commercial_ok: true` mentiroso não
passa) e prova negativa em `--selftest-commercial`; matriz com as 9 fontes, ano na janela,
`year_reason` válido/coerente (`sources_missing ⊆ sources_absent_as_zero`, `sum_sources`
quando não há `Total generation`), soma das fontes = total (≤0,5 MW), DEU 2025 ≈ 268.380 MW
e óleo como lacuna; cobertura 171 + 8 = 179; usina base com `gppd_country`, `nation` =
cidade do atlas, causa de conflito válida, `gppd_coord_error` fora do `count` e
`location_conflict` = província; descartadas ausentes; **Σ by_city = Σ by_nation**
(≤0,01 MW) no base e no escalonado; escalonado com `data_year: 2026`, alvo 2024–2025,
1.584 checagens nação×fonte (≤0,01 MW), `method` em toda usina real coerente com
`scaling_method`, `national_target_missing` batendo com as usinas, óleo não escalonado e
sintéticas com cidade/peso; solar/eólico em faixa física, série solar mensal de 12 meses,
vento sem série mensal declarado; hídrico com PET **CHELSA (CC0)** e sem CGIAR, soma das
províncias = âncora nacional (≤0,01 MW), frações = 1 e `no_data` sem zero inventado;
acesso 0..100 com **SSD/XKX/PSX ligados** ao WDI; fronteiras sem par repetido/consigo
mesma. **Prova negativa da guarda comercial:** marcando o CGIAR como `used` (mesmo com
`commercial_ok: true`), `--require-commercial` reprova com 2 falhas (lista de bloqueio +
“ainda consta como fonte usada”, exit 1); restaurado, volta a 0 falhas. Saídas
byte-idênticas em execuções repetidas (builders 2×, SHA-256 igual nos 6 arquivos).

## 11. Lacunas e o que fica para E2

1. **Óleo** não separado: Ember agrega em `Other fossil`; OWID/EIA não publicam capacidade
   de óleo por país. `oil: null` e parque de óleo não escalonado, declarados.
2. **8 nações sem matriz 2024–2025** e **44 sem âncora hídrica** — lacunas por fonte
   ausente, não por falha de coleta.
3. **CHIRPS >60°N/60°S** — 1.746 províncias sem precipitação; proxy hídrico `no_data` sem
   usar WorldClim. Se o E2 precisar de potencial nessas províncias, a fonte livre a
   resolver (ERA5/GPCP) é decisão de outra rodada.
4. **GWA sem série mensal** — vento com média anual; o E2 deve declarar o fator de
   capacidade eólico sem sazonalidade.
5. **Parque 2021 vs 2026** — o escalonamento é uma transformação declarada; a base é o
   GPPD ~2021, não usinas 2024–2026. Nada foi extrapolado silenciosamente.
6. **`location_conflict`** — a usina fica no controlador do atlas (regra a); a causa fica
   registrada para E2/E5.
7. **Geração por fonte** (TWh) está disponível na mesma base da Ember e ainda não foi
   usada — candidata natural ao fator de capacidade do E2.
8. **Água** é só distribuição (E7); não há hidrologia nesta fase.
9. **Sem alvo nacional (`national_target_missing`, §5):** 109 usinas / 32.493,5 MW ficam
   com o MW do GPPD (não escalonado) — 104 em nações sem matriz Ember e **5 em CYP, MDG,
   MWI e NIC** (gás: 978,1 MW) que têm matriz mas não a linha de gás. Não foi inventado
   zero para essas fontes.
10. **Petróleo/óleo e gás não publicados** são a mesma família de lacuna: a Ember agrega
    óleo em `Other fossil` e às vezes não publica uma fonte; o E2 precisa tratar
    `null` como “sem alvo”, não como zero.
11. **PET em 0,0667°:** a CHELSA é 30 arc-sec e foi lida no overview 1/8 (≈7,4 km) para
    não baixar 12 COGs de ~400 MB; se o E2 precisar de resolução maior (micro-sítio de
    bacia), o nível 1/4 está disponível no mesmo COG.

## 12. Achado de qualidade fora do escopo

`data/definitions/natural/province_areas.json` tem células degeneradas (ex.: província
14087 “Tasmania 1” com 1.781.428 km²; AUS soma 12,1 M km² contra 7,7 M reais). Os
agregados de energia **não** usam esse arquivo: a área vem do raster do atlas (total
145,9 M km², coerente com a terra emersa). Vale corrigir
`tools/compute_province_areas.py` na frente do mapa.

## 13. Correções da QA (t25)

- **Achado 1 (nação da usina):** resolvido pela opção (b) da decisão do dono — `nation` é
  a nação do atlas e o total por nação é a soma das cidades do atlas; `gppd_country`
  preserva o GPPD. Invariante `Σ by_city = Σ by_nation` no validador.
- **Classificação de conflitos:** `gppd_coord_error` (corrigir/discartar),
  `deleted_territory` (cidade do atlas, declarando), `border_pixel` (cidade mais próxima
  da nação do atlas), `occupied` (regra a). Cada usina leva `location_cause`.
- **ESCOMBRERAS:** lat 7,5732 → 37,5732 com evidência da usina irmã; os 4 casos sem
  evidência foram descartados (não realocados).
- **Licença no meta:** o GPPD virou objeto `{name: CC BY 4.0, url, commercial_ok}` no
  `meta.park_source`, em vez de string solta.
- **`NE_DIR`:** lido de `NE_DIR`/`tools/data/natural_earth`, com falha explícita; o
  fallback absoluto `G:\tools\ne_extract` foi removido (correção 2). Documentado no §1 e
  no cabeçalho do builder.
- **`accessed_at_utc`:** presente em todas as fontes usadas do `_sources.json`.
- **Créditos:** dívida registrada no `_sources.json` e no §15 — a tela de créditos precisa
  citar Ember, WRI, Banco Mundial, GSA/GWA, CHIRPS, CHELSA, ETOPO1 e Natural Earth
  antes do E2 ligar o dado.
- **Números do doc:** conferidos contra os arquivos gerados (contagens, MW, cobertura,
  conflitos e hashes) na rodada final de builders 2× + validador.

## 13b. Correções da re-QA t33 (correção 2)

- **Licença do PET (BLOQUEADOR):** o CGIAR-CSI Global-PET v3.1 foi **recusado**. A
  evidência anterior estava invertida: o produto é **derivado do WorldClim 2.1** e a
  página oficial do CGIAR-CSI diz “provided for **non-commercial use**”. Entrou a
  **CHELSA v2.1 PET** (WSL), **CC0 1.0**, Penman-Monteith com forçante **ERA5**
  (licença e forçante verificadas na página do dataset em 2026-11-22). O proxy hídrico
  (runoff/`raw_index`) foi refeito: PET média 1.399 mm/ano (mediana 1.432; máx 3.441),
  contra 1.441/1.339/3.616 do CGIAR — mesma ordem de grandeza, distribuição um pouco
  mais seca (runoff zero em 71,2 % das províncias contra 66,9 %).
- **Guarda comercial independente do registro:** `NON_COMMERCIAL_SOURCE_IDS`
  (`worldclim_21`, `cgiar_pet_v3_1`, `irena_elecstat`, `gem_trackers`) + marcadores de
  texto de licença + `--selftest-commercial`. Prova negativa: marcando o CGIAR como
  `used` (mesmo com `commercial_ok: true`), `--require-commercial` reprova
  (“id na lista de bloqueio” + “ainda consta como fonte usada”, exit 1); restaurado,
  volta a passar.
- **Regra do ano (Ember):** último ano com as 9 fontes de capacidade cobertas, mesmo
  sem `Total generation`, com `year_reason` e `sources_missing`/`sources_absent_as_zero`
  por país (§3). DEU 2024 → **2025 (268.380,0 MW)**; o parque escalonado da Alemanha vai
  a 271.231,2 MW (268.380,0 + 2.851,2 de óleo não separado).
- **Doc §4:** a tabela fecha em 300 usinas/61.945,7 MW e os **5 `gppd_coord_error`
  (1.472,2 MW)** ficam explicitamente **fora do count** (1 corrigido + 4 descartados).
- **Docstrings e caminhos:** o nome antigo `power_plants_2026.json` saiu das docstrings
  (agora `power_plants_base_2021_gppd.json`/`power_plants_2026_scaled.json`); o
  `NE_DIR` não tem mais fallback absoluto (`G:\tools\ne_extract`) — exige `NE_DIR` ou o
  conjunto em `tools/data/natural_earth/`.
- **`method` nas usinas reais** e **`national_target_missing`** declarados (§5/§11).
- **WDI → tags do jogo:** `SSD`, `XKX` e `PSE` passaram a ligar em `SSD`, `XKX` e `PSX`
  (alias `PSE→PSX` declarado em `meta.iso3_aliases`), levando a cobertura de 173 para
  **176** das 179 nações (§8).

## 14. Escalonamento: leitura dos 10 maiores

A tabela do §5 mostra que as fontes separadas pela Ember fecham no alvo e a diferença é
só o óleo. Para E2, a leitura prática é: **use o total por nação×fonte de
`national_capacity.json` como teto** e o parque escalonado como inventário de usinas
(nome, cidade, fonte, MW). O óleo do GPPD é a única fonte que não fecha com a Ember; se o
E2 quiser fechar o total, precisa de uma fonte de capacidade de óleo (lacuna §11.1) ou
tratar o óleo como `other` (decisão de design, não de dado).

## 15. Dívidas e créditos

1. **WorldClim no clima do jogo (pré-existente, ABERTO):** o E1 não usa mais WorldClim,
   mas `data/definitions/natural/province_nature.json` (clima do jogo) ainda usa WorldClim
   2.1, cuja página `worldclim.org/about.html` declara uso **acadêmico/não comercial** e
   proíbe redistribuição sem permissão escrita — não é CC BY. Antes de release comercial:
   permissão escrita de `info@worldclim.org` ou troca do clima por fonte livre. Esta dívida
   é **do clima**, não do E1, e não foi mexida nesta rodada.
2. **Créditos das fontes (ABERTO):** a tela de créditos precisa citar **Ember, WRI
   (GPPD), Banco Mundial (WDI + âncora hídrica), GSA/GWA, CHIRPS, CHELSA PET (CC0),
   ETOPO1 e Natural Earth** antes do E2 popular a rede com o parque escalonado. Registro:
   `_sources.json › meta.credits_debt`.
3. **Óleo (ABERTO):** sem fonte licenciada de capacidade instalada de óleo por país;
   `oil: null` e parque de óleo não escalonado. OWID/EIA foram tentados e recusados com
   motivo (§2).
4. **CHIRPS >60°N/60°S (ABERTO):** 1.746 províncias sem precipitação; proxy hídrico
   `no_data`. Resolver com fonte livre (ERA5/GPCP) se o E2 precisar dessas províncias.
5. **GWA sem sazonalidade (DECLARADO):** vento com média anual; o fator de capacidade
   eólico do E2 não terá série mensal.
6. **`province_areas.json` (fora do escopo):** células degeneradas; energia não usa esse
   arquivo, mas o mapa ainda pode usar. Ver §12.
7. **Cobertura GSA/GWA (ABERTO):** 12.298/15.547 províncias com ponto medido completo;
   2.965 usam o vizinho medido mais próximo da mesma nação (raio 12°), declarado; 284
   ficam no_data. Reprocessar quando a API estabilizar para reduzir a interpolação.
