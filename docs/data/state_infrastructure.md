# Infraestrutura existente por estado — dado de partida da B5

> Tarefa **D-007** (dados). Insumo da fase **B5** (`docs/economy/B5_state_levers.md`,
> decisão **D-089**): a Região da B3 tem três níveis de infraestrutura — **estrada**,
> **energia/luz** e **água** — cada um `0..kInfraMaxLevel` (= 3). Na B5 todos nascem em
> **0** porque não havia dado de cobertura carregado. Este arquivo registra o dado real
> que existe hoje, **sem ligar nada ao núcleo**.

- Dataset: `data/definitions/scenario/state_infrastructure_2026.json`
- Construtor: `tools/build_state_infrastructure.py`
- Validador: `tools/validate_state_infrastructure.py`
- Branch/worktree: `data/state-infra` em `G:/projetos/opengs-wt-data-infra` (base
  `balance/global-resources` `7b0f464`). **Nada mesclado.**

## 1. O que o arquivo tem

Uma entrada por **estado** de `data/definitions/population/state_regions.json`
(4.226 ids, os mesmos — o validador compara o conjunto exato), com:

| campo (estado) | unidade | origem |
|---|---|---|
| `electricity_access_pct` / `_urban_pct` / `_rural_pct` | % da população | World Bank `EG.ELC.ACCS.ZS` / `.UR.ZS` / `.RU.ZS` |
| `electricity_year_total/urban/rural` | ano do dado | o mesmo da série |
| `water_basic_pct` / `_urban_pct` / `_rural_pct` | % da população | World Bank `SH.H2O.BASW.ZS` / `.UR.ZS` / `.RU.ZS` (espelho WDI da série JMP) |
| `water_year_total/urban/rural` | ano do dado | o mesmo da série |
| `road_quality_index` | índice 1..5 | World Bank `LP.LPI.INFR.XQ` |
| `road_year` | ano do dado | o mesmo da série |
| `granularity` | `"nation"` | ver §2 |
| `infra_levels` | `{road,power,water}` 0..3 | regra de §3 |
| `data_status` | `observed_national` \| `no_data` | ausência é explícita |
| `reason` | texto | só em `no_data`: o mesmo motivo registrado na nação |

O bloco `states` usa `data_status` `observed_national` ou `no_data`; o bloco `nations` usa
`observed` ou `no_data` (o nome muda porque no estado o valor é nacional).

O bloco `nations` guarda a **mesma** informação uma vez por país (184 entradas: as 179 de
`scenario/nations.json` mais 5 territórios que só existem em `state_regions.json`), com
`refs` (title/url/year) da série que decide cada nível. O bloco `sources` descreve cada
indicador (URL, endpoint, licença, `last_updated`).

## 2. Granularidade: **país**, não estado

Todos os 4.226 estados carregam `granularity: "nation"`: o valor é o **nacional**,
propagado a cada estado do país. Nenhum valor subnacional foi obtido, e nenhum foi
estimado a partir do nacional (nada de rateio por área ou população).

Por que não há subnacional neste arquivo — o que foi verificado nesta coleta:

- **Global Data Lab (Area Database)**: cobre regiões subnacionais, mas não expõe API
  estável (`/api/v1/...` → 404; a tabela é HTML) e o download é manual por indicador.
- **DHS StatCompiler**: tem eletricidade por região de pesquisa, mas exige registro/chave
  de API; não há extração automatizável.
- **Censos nacionais** (IBGE, Census of India, NBS China…): cada país tem um formato e um
  ano; não há tabela única.
- O repositório **não** tem extrato de vias OSM (`.osm`/`.pbf`), nem GRIP4, nem o
  *Rural Access Index*.

Consequência declarada: um estado nigeriano do interior e Lagos recebem o **mesmo** nível
(energia 0, porque o rural nacional é 23,5%). Isso é limitação de fonte, não do formato —
quando houver subnacional, basta preencher a entrada do estado e trocar `granularity` para
`"admin1"`.

## 3. Regra de conversão (proposta, declarada no JSON)

`kInfraMaxLevel = 3` (`gdextension/src/sim/sim_constants.h:215`). O validador **lê a
constante do header** e falha se ela divergir de `conversion_rule.infra_max_level`.

**Energia** ← `electricity_access_rural_pct` (fallback `electricity_access_pct` se o rural
não existir):

| valor | nível | cobertura usada pela B5 (`nível/3`) |
|---|---|---|
| ausente | 0 | 0 — ausência explícita, **não** cobertura zero |
| `< 50%` | 0 | 0 |
| `50–74,99%` | 1 | 0,333 |
| `75–94,99%` | 2 | 0,667 |
| `≥ 95%` | 3 | 1 |

**Água** ← `water_basic_rural_pct`, mesmos cortes. **Estrada** ← `road_quality_index`
(LPI 1..5): `< 2,0` → 0; `2,0–2,99` → 1; `3,0–3,99` → 2; `≥ 4,0` → 3.

Por que **rural** nas duas de acesso: o nível da B5 é o pior caso da região — energia e
água chegam primeiro à cidade, e é o rural que sustenta o trabalho primário e o êxodo. A
média nacional esconderia justamente o gargalo que o lever modela (ver
`infra_productivity_multiplier`/`infra_exodus_multiplier` em `gdextension/src/sim/region.cpp:176-203`).

Por que esses cortes: 95% é o patamar de **acesso quase universal** da agenda SDG
7.1/6.1 (o resto é a última milha); 75% e 50% dividem a escala em três quartos, metade.
O LPI usa quartis da escala nativa 1..5 (2, 3, 4) — o índice já é uma escala cardinal
declarada, e 3,0 é o ponto "adequado" da própria escala.

Distribuição resultante (4.226 estados / população de `state_regions.json`):

| nível | energia (estados / % pop) | água (estados / % pop) | estrada (estados / % pop) |
|---|---|---|---|
| 0 | 712 / 13,6% | 289 / 5,8% | 393 / 4,1% |
| 1 | 116 / 3,2% | 551 / 11,1% | 1.751 / 36,5% |
| 2 | 386 / 9,1% | 1.207 / 56,4% | 1.808 / 37,3% |
| 3 | 3.012 / 74,2% | 2.179 / 26,7% | 274 / 22,1% |

**Isto ainda não está ligado ao núcleo.** Carregar esses níveis muda o resultado (com
estrada 3 a penalidade de acesso de 30% some; com energia/água 3 o êxodo cai ao piso).
A B5 §8 já registra a pendência "recalibrar a penalidade de acesso se o dado de cobertura
inicial for carregado" — a decisão de ligar, e com que constantes, é da simulação, não
deste dado.

## 4. Fontes

| métrica | indicador | publicador | licença | nações do jogo | último ano |
|---|---|---|---|---|---|
| Eletricidade (total / urbano / rural) | `EG.ELC.ACCS.ZS` / `.UR.ZS` / `.RU.ZS` | World Bank WDI (SE4ALL) | CC BY 4.0 | 175 / 173 / 173 | 2024 (171 nações); LBY e MRT em 2012 |
| Água básica (total / urbano / rural) | `SH.H2O.BASW.ZS` / `.UR.ZS` / `.RU.ZS` | World Bank WDI (espelho do JMP) | CC BY 4.0 | 174 / 160 / 158 | 2024 (152 no rural) |
| Estrada (qualidade) | `LP.LPI.INFR.XQ` | World Bank — Logistics Performance Index | CC BY 4.0 | 163 | 2022 (136), 2018 (23), 2016 (3), 2014 (1) |

- `last_updated` da API World Bank no momento da coleta: **2026-11-11**; coleta em
  **2026-11-18**.
- **Água**: a série JMP também é publicada pelo GHO/WHO (`WSH_WATER_BASIC`), mas sob
  **CC BY-NC-SA 3.0 IGO** (não comercial). O arquivo usa o **espelho WDI**, CC BY 4.0, com
  os mesmos números: a conferência comparou **13.864** pares país/ano/dimensão e achou
  **0 diferenças acima de 0,01** (delta máximo 5,0e-10). O GHO fica registrado em
  `sources` como `cross_check_nao_usado_no_build`.
- **Ano por dimensão**: total, urbano e rural podem vir de anos diferentes (o ano mais
  recente com valor não nulo, janela 2010–2025, é escolhido por série). Cada campo tem o
  seu `year_*`, e o `refs` cita a série que **decide** o nível (rural, senão total).

### O que **não** foi usado, e por quê

- **`IS.ROD.DNST.K2`, `IS.ROD.TOTL.KM`, `IS.ROD.PAVE.ZS`** (densidade e extensão de
  estrada): **descontinuados**. A API devolve 180 linhas sem país e `lastupdated`
  **2013-02-22**; nenhum valor utilizável. Nada foi derivado deles.
- **Rural Access Index** (World Bank/TRL, 2018): não está no endpoint WDI e não há
  download automatizável verificado nesta coleta. Fica como fonte futura — é a métrica
  ideal ("% da população rural a 2 km de estrada transitável"), porque casaria com os
  mesmos cortes de percentual usados em energia e água.
- **OSM**: o repositório não tem extrato de vias (as camadas de `map/map_data/atlas` são
  raster de províncias/área, não malha viária).
- **GRIP4** (Meijer et al., 2018): só existe como vetor global de 2,3 GB por região;
  não há tabela de comprimento por país.

## 5. Cobertura

Duas bases diferentes, sempre nomeadas: o bloco `nations` tem **184** entradas (as **179**
nações de `scenario/nations.json` + **5** territórios que só existem em
`state_regions.json`); o bloco `states` tem **4.226** ids.

- **Estados** (base 4.226 de `state_regions.json`): **4.169 `observed_national`** e
  **57 `no_data`** (todos com percentuais `null`, níveis 0 e `reason` registrado — o
  mesmo texto da nação).
- **Nações** (base 184 no bloco `nations`, das quais 179 são nações do jogo): **175 com
  eletricidade, 174 com água, 163 com estrada**.
  - Sem nenhuma observação (base 184): **9** — as 4 nações do jogo `SAH` (Saara
    Ocidental), `SOL` (Somalilândia), `TWN` (Taiwan) e `XKX` (Kosovo), que não estão nas
    séries, mais os 5 territórios que só existem em `state_regions.json` (`ATA`
    Antártida, `CLP` Clipperton, `CSI` Ilhas do Mar de Coral, `IOA` Ilha Christmas, `KAS`
    Caxemira).
  - Sem estrada (base 179 nações do jogo): **16** = **12** pequenas/insulares com água/luz
    e sem rodada do LPI + as **4** do jogo sem nada (`SAH`, `SOL`, `TWN`, `XKX`).
  - Sem água, além das 9 sem nada (base 184): `NIC` (Nicarágua) não tem observação na
    série usada.
- **Fallback total em vez de rural** (base 184): **2** nações na energia (`GRD`, `PRK`) e
  **16** na água (BHR, BHS, BRB, BRN, GRD, ITA, JPN, KNA, KOR, KWT, LBN, LBY, QAT, SGP,
  SVN, VEN) — em geral Estados pequenos/urbanizados, onde a série rural não é publicada
  separada.

### Exemplos (valores nacionais, propagados aos estados)

| país | eletricidade rural (ano) | água rural (ano) | LPI (ano) | níveis road/power/water |
|---|---|---|---|---|
| Brasil `BRA` | 99,2% (2024) | 100,0% (2024) | 3,2 (2022) | 2 / 3 / 3 |
| Índia `IND` | 99,9% (2024) | 94,91% (2024) | 3,2 (2022) | 2 / 3 / 2 |
| Nigéria `NGA` | 23,5% (2024) | 66,55% (2024) | 2,4 (2022) | 1 / 0 / 1 |
| Alemanha `DEU` | 100,0% (2024) | 100,0% (2024) | 4,3 (2022) | 3 / 3 / 3 |
| China `CHN` | 100,0% (2024) | 93,83% (2024) | 4,0 (2022) | 3 / 3 / 2 |

## 6. Lacunas e pendências

1. **Subnacional**: nenhum. É a maior lacuna — o nível é o mesmo no país inteiro. Fonte
   candidata: DHS (eletricidade por região de pesquisa), Global Data Lab, censos
   nacionais; todas exigem extração manual por país/rodada.
2. **Estrada é qualidade percebida, não densidade.** O LPI mede infraestrutura de
   comércio e transporte (portos, ferrovia, estradas, TIC) por enquete de operadores.
   Serve como proxy declarado; **não** é km/km² nem "% a 2 km de estrada". Se o Rural
   Access Index entrar, ele substitui o LPI na estrada com os mesmos cortes de percentual.
3. **Idade do dado**: LBY e MRT usam eletricidade rural de **2012**; ARG, ERI, GNQ e VCT
   usam água rural de 2016–2018; 24 nações usam LPI de 2014–2018. O ano está em cada
   campo; nada foi extrapolado.
4. **Uso no jogo**: não ligado. Ligar exige decisão de calibração (penalidade de acesso,
   ganho de produtividade) e regeneração de goldens — é da simulação.
5. O cache bruto da coleta (`tools/_tmp_state_infra_raw/`) **não** é versionado: é
   intermediário e reconstruível por `--fetch` (§7). Sem ele, o construtor falha
   listando o arquivo que falta.

## 7. Como regenerar

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python3.exe"
& $py tools/build_state_infrastructure.py --fetch   # baixa do World Bank o que faltar no cache
& $py tools/build_state_infrastructure.py           # reconstrói do cache (determinístico)
& $py tools/build_state_infrastructure.py --fetch --force-fetch   # rebaixa os 7
& $py tools/build_state_infrastructure.py --raw-dir "$env:TEMP\ogs_infra_raw"  # outro cache
& $py tools/validate_state_infrastructure.py        # exit 0 = OK
```

- **Endpoint**: `https://api.worldbank.org/v2/country/all/indicator/{INDICATOR}?format=json&per_page=20000`,
  um arquivo `wb_{INDICATOR}.json` por indicador; os **7** são `EG.ELC.ACCS.ZS/.UR.ZS/.RU.ZS`,
  `SH.H2O.BASW.ZS/.UR.ZS/.RU.ZS` e `LP.LPI.INFR.XQ`. World Bank WDI, **CC BY 4.0**;
  coleta em **2026-11-18**.
- **Cache**: `--raw-dir` (padrão `tools/_tmp_state_infra_raw/`) é **intermediário e não
  versionado**; `--fetch` baixa só o que falta, `--force-fetch` rebaixa os 7. Sem
  `--fetch`, o construtor lê apenas o cache e falha listando o arquivo que falta.
- A API devolve a série histórica completa (desde 1960); o build usa só a janela
  **2010–2025** (ano mais recente com valor por série). Cada download tem timeout de
  120 s e 1 retry; o transporte pede gzip. Se a API responder `meta.pages > 1`, o
  construtor falha com mensagem clara em vez de paginar.
- O GHO/WHO (`WSH_WATER_BASIC`, CC BY-NC-SA) **não** é baixado nem usado no build, e as
  séries `IS.ROD.*` descontinuadas ficam de fora.

Saída determinística: duas execuções produzem o mesmo SHA-256
(`b139ea921652fdc691be835b00d635089c336988486bfecfaa2e58782c31b586`).
O validador checa: ids iguais aos de `state_regions.json` (4.226), faixas 0–100% e 1–5,
níveis recalculados pela regra declarada, `kInfraMaxLevel` do header, estado == nação sem
drift, `coverage` recontada, `reason` em todo `no_data` (e ausente no observado) e
`refs`/`sources` presentes.
