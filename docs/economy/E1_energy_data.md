# E1 — Dados de energia: parque, matriz nacional, potencial e acesso

Escopo: **só dado**. Nada foi ligado à simulação, ao núcleo C++, a `core/`, `game/`,
`map/` ou UI. Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE E, E1. Branch
`data/e1-energy`, worktree `G:/projetos/opengs-wt-data-e1`, base atual
`balance/global-resources` @ `0edecf64` (HEAD de dados `72357b37` + esta rodada de
correções).

O aceite do E1 é: **por nação, capacidade instalada (MW) e matriz por fonte na ordem do
dado real 2024–2026; usinas individuais atribuídas à cidade/província dona no mapa do
jogo; potencial solar/eólico e hídrico derivado por província, normalizado por potencial
técnico nacional público; perdas T&D e acesso por país; licença conferida e registrada;
lacuna declarada onde não há dado.**

## 1. Artefatos

| Arquivo | Conteúdo | Registros | SHA-256 |
|---|---|---|---|
| `data/definitions/energy/national_capacity.json` | capacidade instalada e matriz por nação (2024–2025) + desvio do parque base | 171 nações, 161 comparações | `EC9CF51764B17458A4640B538A8A1D13C38E1CBCE4C4550695AD6FD101D0D1EF` |
| `data/definitions/energy/power_plants_base_2021_gppd.json` | parque GPPD ~2021 **bruto** (usina, fonte, MW, lat/lon) atribuído à cidade/nação do atlas | 34.932 usinas / 5.706.702,5 MW / 743 cidades / 166 nações | `CB8C0F9959B86864D786AB1FFBA9D7517A23AA75584820EEFC9E78A16B867E49` |
| `data/definitions/energy/power_plants_2026_scaled.json` | parque **escalonado** para a matriz Ember 2024–2025, com fator por nação×fonte e usinas sintéticas declaradas | 36.034 usinas (34.932 reais + 1.102 sintéticas) / 10.327.743,2 MW (10.327.735,6 MW por nação + 7,6 MW sem cidade) / 825 cidades / 176 nações | `A094300E0B76DC6208CCAF35899578080ED488655E5D3309A63063C364D70D8F` |
| `data/definitions/energy/renewable_potential.json` | solar e eólico por província/cidade (GSA/GWA) + proxy hídrico bruto (CHIRPS/PET/ETOPO1) | 15.547 províncias / 849 cidades | `47D49953A1933208F6656ECB0CA3DCCD24501E11FDFA284D304E3FDD9BFE0D86` |
| `data/definitions/energy/hydro_potential.json` | potencial hídrico em MW por província/cidade, normalizado pela âncora nacional | 179 nações (135 com âncora) | `D3F284DF15D8DB5BB75B01ACC83FA9B90029693CD5110FB808AE1C27B1C6B9CD` |
| `data/definitions/energy/access_losses.json` | acesso à eletricidade (total/urbano/rural), perdas T&D, água segura | 217 países (173 jogáveis) | `DEDF622E4B564468FC58315E366A017BAA37A7CF4E6CA38414F1853BE791AA07` |
| `data/definitions/energy/land_borders.json` | pares de nações com fronteira terrestre (E5) | 299 pares / 152 nações | `5465B7E5017A21C12219595E80AC657E325084DCB6235C7EE65DB7E6290F8451` |
| `data/definitions/energy/_sources.json` | registro único de fonte, URL, acesso, licença, evidência e transformação | 16 fontes (10 usadas, 1 citação, 5 recusadas) | `99736E618E3C2CB7829FE1CF66141AF25403580B4FD329D11DFCD2F0A059EAFC` |

Scripts reprodutíveis (determinísticos; 2+ execuções com SHA-256 idêntico):

```text
tools/data/energy/fetch_ember_capacity.py      cache Ember (CSV ~16 MB, não versionado)
tools/data/energy/fetch_wb_hydro_anchor.py     cache do indicador do Banco Mundial
tools/data/energy/fetch_atlas_points.py        cache GSA/GWA por província (ponto a ponto)
tools/data/energy/fetch_hydro_inputs.py        cache CHIRPS/ETOPO1/PET (fontes livres)
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
SHA-256 + data de acesso). Nada acima de 20 MB é versionado. `NE_DIR` é descoberto por
`NE_DIR`, `tools/data/natural_earth` ou o caminho legado, com falha explícita se ausente
(sem caminho absoluto silencioso).

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
| **CGIAR-CSI PET v3.1** | PET anual do runoff | **CC BY 4.0** | usada |
| **ETOPO1** (NOAA/NCEI) | relevo do proxy hídrico | **domínio público** | usada |
| **Natural Earth 10m** (rios, admin) | rios do proxy hídrico, divisas | **domínio público** | usada |
| **WorldClim 2.1** | — | uso acadêmico/não comercial; redistribuição e uso comercial proibidos sem permissão | **recusada** — saiu do E1 (D-115c) |
| **Our World in Data — Energy** | tentativa de separar óleo | CC BY 4.0, mas só geração (`oil_electricity_twh`), não capacidade | **recusada** — lacuna declarada |
| **EIA International Energy Data** (bulk INTL) | tentativa de separar óleo | domínio público, mas só capacidade fóssil agregada e geração de petróleo | **recusada** — lacuna declarada |
| **IRENA ELECSTAT** | comparação de capacidade | termos IRENA art. 4.1 (**não comercial**) | **recusada** |
| **Global Energy Monitor (trackers)** | usinas | CC BY 4.0, mas o download exige atestar “não uso comercial” | **recusada** |
| **IJHD — Hydropower Essentials** | número global de potencial (16.400 TWh/ano) | citação de fato, com atribuição | só citação |

**Decisão D-115(c):** o WorldClim **não** é mais usado no E1. Solar/eólico vêm do
GSA/GWA (CC BY 4.0); o hídrico usa CHIRPS (domínio público/CC BY 4.0), PET CGIAR-CSI
v3.1 (CC BY 4.0), ETOPO1 (domínio público) e rios Natural Earth. A dependência
**pré-existente** de `data/definitions/natural/province_nature.json` no WorldClim
(clima do jogo) é dívida separada, registrada no §15 — não foi mexida nesta fase.
`validate_energy_data.py --require-commercial` **passa** no estado atual.

**Decisão D-115(d):** a Ember não separa óleo (`Other fossil`); a tentativa com OWID e
EIA não encontrou capacidade instalada de óleo por país em nenhuma das duas (OWID só
publica geração; o EIA só tem capacidade fóssil agregada e geração de petróleo). `oil`
fica `null` em `national_capacity.json` e o parque de óleo do GPPD fica **não
escalonado** em `power_plants_2026_scaled.json`, com o motivo declarado.

## 3. Matriz nacional 2024–2025 (Ember)

Regra: para cada país, a linha `Capacity (GW)` do **último ano com capacidade positiva
entre 2024 e 2025** (nunca interpola, nunca extrapola). Ano com só zeros não conta
(ex.: KGZ 2025 trazia apenas `Other renewables 0.0`; vale 2024). `year` fica explícito
por país: **79 países em 2025, 92 em 2024**.

Mapeamento para as 9 fontes do jogo: `Hydro→hydro`, `Solar→solar`, `Wind→wind`,
`Coal→coal`, `Gas→gas`, `Nuclear→nuclear`, `Bioenergy→biomass`,
`Other fossil + Other renewables → other`. **Óleo não é separado pela Ember** (vem dentro
de `Other fossil`), então `oil: null` em todos os países, com
`other_breakdown.other_fossil_mw` e o motivo em `meta.gaps.oil` — a tentativa com
OWID/EIA está no §2. **Nenhum número foi inventado.**

Mundo (soma dos 171 países com dado): **10.033,4 GW**, 0 divergência entre a soma das
fontes e o total da Ember (tolerância 0,5 MW; `check.sum_matches_total` verdadeiro em
171/171).

| Fonte | Mundo (GW) |
|---|---|
| solar | 2.348,7 |
| carvão | 2.214,5 |
| gás | 2.058,5 |
| hídrica | 1.286,1 |
| eólica | 1.283,6 |
| nuclear | 393,1 |
| outras | 319,5 |
| biomassa | 129,4 |
| óleo | **lacuna** (dentro de “outras”) |

### 10 maiores por MW (cap = 2024–2025)

| # | País | Ano | Total GW | hídrica | solar | eólica | carvão | gás | nuclear | biomassa | outras |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | CHN | 2025 | 3.726,5 | 382 | 1.202 | 641 | 1.243 | 161 | 62 | 19 | 17 |
| 2 | USA | 2025 | 1.333,2 | 84 | 212 | 159 | 189 | 550 | 97 | 10 | 33 |
| 3 | IND | 2025 | 555,5 | 49 | 136 | 55 | 251 | 27 | 9 | 11 | 19 |
| 4 | JPN | 2025 | 322,9 | 28 | 92 | 6 | 55 | 82 | 33 | 6 | 20 |
| 5 | BRA | 2025 | 260,6 | 110 | 65 | 35 | 3 | 19 | 2 | 18 | 8 |
| 6 | DEU | 2024 | 248,2 | 6 | 91 | 73 | 32 | 32 | 0 | 9 | 5 |
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
- **Conflitos de localização (300 usinas / 61.945,7 MW)** classificados por causa:

  | Causa | Usinas | MW | Regra aplicada |
  |---|---:|---:|---|
  | `border_pixel` | 148 | 38.968,5 | fica na cidade mais próxima da nação do atlas |
  | `deleted_territory` | 125 | 2.016,1 | território do GPPD que não existe no jogo; fica na cidade do atlas |
  | `occupied` | 27 | 20.961,0 | território disputado/ocupado; vale o controlador do atlas |
  | `gppd_coord_error` | 5 | 1.472,2 | 1 corrigido com evidência + 4 descartados (abaixo) |

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
  `scaling_note: retired_by_ember_target_zero`.
- **Óleo:** alvo `null` (Ember não separa); o parque de óleo do GPPD fica **não
  escalonado** e o motivo é declarado. Nação sem matriz Ember (8 lacunas): parque fica
  não escalonado e declarado.

**Resultado:** 36.034 usinas (34.932 reais + 1.102 sintéticas), **10.327.743,2 MW**
(10.327.735,6 MW por nação + 7,6 MW sem cidade), 825 cidades, 176 nações. **1.584 checagens nação×fonte, 0 falhas** (≤0,01 MW).
O total fica **294,4 GW acima** da Ember: **261,9 GW** de óleo não separado + **32,7 GW**
das 8 nações sem matriz Ember (com sobreposição de óleo dentro das lacunas). Isso é
lacuna declarada, não erro de soma.

### 10 maiores após o escalonamento vs Ember

| # | País | Escalonado MW | Ember MW | Δ MW | Leitura |
|---|---|---|---:|---:|---|
| 1 | CHN | 3.727.839,0 | 3.726.510,0 | +1.329,0 | óleo não separado |
| 2 | USA | 1.370.393,4 | 1.333.250,0 | +37.143,4 | óleo não separado |
| 3 | IND | 557.140,8 | 555.460,0 | +1.680,8 | óleo não separado |
| 4 | JPN | 367.030,2 | 322.910,0 | +44.120,2 | óleo não separado |
| 5 | BRA | 268.923,6 | 260.640,0 | +8.283,6 | óleo não separado |
| 6 | DEU | 251.031,2 | 248.180,0 | +2.851,2 | óleo não separado |
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
- **PET** = Global Aridity and PET Database v3.1 (CGIAR-CSI, CC BY 4.0), PET anual
  30 arc-sec, no centróide; `runoff = clamp((precip − PET)/precip, 0, 1)`.
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
  **44 sem** (`national_anchor_missing`) — lacuna declarada, não zero inventado. EGY e NAM
  têm âncora mas o proxy é 0 em todas as províncias (`distribution_missing`).
- Mundo declarado: **4.642 GW** (soma das 171 âncoras disponíveis), coerente com a âncora
  global de 16.400 TWh/ano (a 0,40 de fator de capacidade ≈ 16,3 PWh/ano).
- O proxy global antigo (16.400 TWh/ano) continua como `normalized: global_anchor` em
  `renewable_potential.json`; a normalização nacional do E1 substitui essa escala em
  `hydro_potential.json`.

## 8. Acesso, perdas e água (E5/E7)

- Ano mais recente não vazio por país/indicador; ausente = `null`, sem interpolação.
- Cobertura entre 217 países: acesso à eletricidade 216, urbano/rural 214, **perdas T&D
  150**, água segura 151 (urbano 104, rural 85). 173 das 179 nações jogáveis têm algum
  dado; 6 não (`PSX, SAH, SOL, SSD, TWN, XKX`).
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
493422 checagens | 0 falhas | 1 avisos
```

Checagens: licença completa (nome, `commercial_ok`, data, evidência e `accessed_at_utc`)
para toda fonte usada e todo arquivo consumidor existente; matriz com as 9 fontes, ano na
janela, soma das fontes = total (≤0,5 MW) e óleo como lacuna; cobertura 171 + 8 = 179;
usina base com `gppd_country`, `nation` = cidade do atlas, causa de conflito válida e
`location_conflict` = província; descartadas ausentes; **Σ by_city = Σ by_nation**
(≤0,01 MW) no base e no escalonado; escalonado com `data_year: 2026`, alvo 2024–2025,
1.584 checagens nação×fonte (≤0,01 MW), óleo não escalonado e sintéticas com cidade/peso;
solar/eólico em faixa física, série solar mensal de 12 meses, vento sem série mensal
declarado; hídrico com soma das províncias = âncora nacional (≤0,01 MW), frações = 1 e
`no_data` sem zero inventado; acesso 0..100; fronteiras sem par repetido/consigo mesma.
`--require-commercial` **passa** (nenhuma fonte restrita em uso). Saídas byte-idênticas em
execuções repetidas.

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
- **`NE_DIR`:** descoberto por `NE_DIR`/`tools/data/natural_earth`/caminho legado, com
  falha explícita; documentado no §1 e no cabeçalho do builder.
- **`accessed_at_utc`:** presente em todas as fontes usadas do `_sources.json`.
- **Créditos:** dívida registrada no `_sources.json` e no §15 — a tela de créditos precisa
  citar Ember, WRI, Banco Mundial, GSA/GWA, CHIRPS, CGIAR-CSI, ETOPO1 e Natural Earth
  antes do E2 ligar o dado.
- **Números do doc:** conferidos contra os arquivos gerados (contagens, MW, cobertura,
  conflitos e hashes) na rodada final de builders 2× + validador.

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
   (GPPD), Banco Mundial (WDI + âncora hídrica), GSA/GWA, CHIRPS, CGIAR-CSI PET, ETOPO1 e
   Natural Earth** antes do E2 popular a rede com o parque escalonado. Registro:
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
