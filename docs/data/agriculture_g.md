# Parte G — agricultura sazonal (dados)

> Tarefa **D-094** (dados). Insumo da **Parte G** de `docs/plans/perf_pop_financas_plano.md`
> (§"PARTE G — Agricultura sazonal"). Entrega **só de dados**: nenhum código de núcleo,
> nenhum GDScript, nenhum `.tscn`. O jogo começa em **2027-01-01**; por isso o estoque
> inicial é de **fim de safra**, e ninguém nasce com silo vazio.
>
> Branch `data/g-agriculture`, worktree `G:/projetos/opengs-wt-data-agri`, base
> `balance/global-resources` `ebd24157`. Entra em `balance/global-resources` por merge `--no-ff`
> (2026-11-18); a integração no núcleo
> (produção = área × rendimento, janela de colheita, consumo do estoque, perda por
> armazenagem) é trabalho do `@gerente-simulacao`.
>
> **Numeração:** **D-094 é desta entrega** (dados da Parte G). A **B6**
> (`feat/b6-region-property`) ainda não gravou número no `GAME_STATE.md` e vinha
> planejando usar D-094; como este dado entra primeiro, **a B6 usa a próxima
> decisão livre (D-095 em diante)** — não renumerar esta.

- Pasta: `data/definitions/agriculture/`
- Construtores: `tools/build_agri_{yields,calendar,losses,fertilizer,livestock}.py`
  (todos com `--fetch`; o cache bruto `tools/_tmp_agri_raw/` **não é versionado**)
- Comum: `tools/agri_common.py`
- Validador: `tools/validate_agriculture_g.py` (**exit 0**)
- Parecer independente: `docs/team/reviews/D-007_agriculture_g_review.md`
  (**APROVADO COM RESSALVAS**, 0 divergência numérica)

## 1. Que produtos o jogo tem (e por que só estes)

Os primários de comida de `data/definitions/economy/goods.json` (`kind: raw`, `food: true`)
são cinco: **`wheat`, `rice`, `maize`, `potato`, `soy`**. Todos os arquivos usam **esses ids**
e só esses. A cadeia já existente é `wheat→flour+bran`, `rice→bagged_rice`, `maize|soy|rice|
potato|wheat→feed`, `feed→meat` (`recipes.json`); não há bem `milk`.

**Mandioca não é bem do jogo.** Como a conferência pediu NGA mandioca, o número real do
FAOSTAT fica em `crop_yields.json → non_game_reference.NGA.crops.cassava` com `in_game: false`
(item 125 "Cassava, fresh", 2024: **10.722.539 ha / 62.624.946,19 t → 5,840 t/ha**). Nada disso
alimenta o núcleo; existe só para prova de fonte.

## 2. Arquivos e cobertura

| arquivo | conteúdo | entradas | observado / regra / sem dado |
|---|---|---|---|
| `crop_calendar.json` | plantio e colheita país×cultura, 1–2 safras, hemisfério | 895 (179×5) | 274 / 592 / 29 |
| `crop_yields.json` | rendimento t/ha, área colhida ha, produção t, ano | 895 | 654 / — / 241 |
| `crop_stocks.json` | estoque de fim de safra, consumo, dias de consumo | 895 | 354 / 390 / 151 |
| `postharvest_losses.json` | perda pós-colheita % (e % da fase de armazenagem) | 895 | 86 + 677 regional / 132 |
| `fertilizer_n.json` | N aplicado kg/ha por cultura | 895 | 311 / 584 / 0 |
| `livestock_cycle.json` | carcaça bovina kg/cabeça, leite kg/cabeça/ano, ciclo | 179 nações | 175 e 171 |
| `product_shelf_life.json` | dias de prateleira dos 12 bens da cadeia | 12 produtos | 9 / — / 3 |

Todos com `_meta` (dataset, versão, gerador, `retrieved: 2026-11-18`, 179 nações, culturas),
`sources` (título, publisher, URL/DOI, ano, licença, data de acesso) e `nations`.

## 3. Fontes

| chave | fonte | licença | uso |
|---|---|---|---|
| `FAOSTAT_QCL` | FAOSTAT — Crops and livestock products (bulk normalizado, 33,9 MB) | CC BY 4.0 | área colhida (5312), produção (5510), peso de carcaça (867/5417), leite (882/5417) |
| `USDA_PSD` | USDA FAS — Production, Supply and Distribution (CSV público grãos + oleaginosas) | domínio público (USDA) | **Ending Stocks** (atributo 176, 1000 MT→t) e **Domestic Consumption** (125) |
| `FAO_CROP_CALENDAR` | FAO — Crop Calendar, resumo nacional por zona agroecológica (CSV via `api.data.apps.fao.org`) | CC BY 4.0 | janelas de plantio/colheita (57 países) |
| `JRC_ASAP` | EC JRC — *Crop calendar dataset compatible with land surface phenology*, doi:10.2905/JRC.PXQH3Q0 | EU reuse notice | janelas em dekads (75 países) |
| `USDA_NASS_AH628` | USDA NASS — *Field Crops Usual Planting and Harvesting Dates* (AH-628, out/2010) | domínio público (USDA) | EUA: milho, soja, trigo, arroz e batata (5 pares) |
| `USDA_AH664` | USDA WAOB/JAWF — *Major World Crop Areas and Climatic Profiles* (AH-664, rev. set/1994) | domínio público (USDA) | calendários por país/região lidos do gráfico (28 pares): BRA, CHN, IND, CAN, MEX, ARG, FRA, DEU, POL, ITA, RUS, UKR, AUS, TUR e outros |
| `USDA_GAIN` | USDA FAS — *Global Agricultural Information Network*, relatórios nacionais citados em `evidence.report` | domínio público (USDA) | pares com texto (ARG soja, UKR, outros) |
| `FAO_GIEWS` | FAO GIEWS — *Country Briefs* (ex.: Iran, ref. 10/nov/2026) | © FAO, uso com citação | conferência textual das duas pontas onde o gráfico é regional (`IRN/wheat`) |
| `IFA_FUBC_2018` / `IFA_FUBC_HISTORIC` | IFA — *Fertilizer use by crop and country 2017-2018*, doi:10.5061/dryad.2rbnzs7qh | CC0 1.0 | `Aver_N_rate_kg_ha` por país×cultura |
| `APHLIS` | APHLIS — perdas pós-colheita por país/cultura (tabelas públicas) | citado na fonte | perda total e da fase de armazenagem (África) |
| `FAO_2011` | FAO — *Global food losses and food waste* (Gustavsson et al., 2011), Annex 4 p. 26–27 | citado na fonte | taxa regional por grupo (cereais / raízes / oleaginosas) |
| `FOODKEEPER` / `FAO_INPHO` | USDA FSIS FoodKeeper; FAO INPhO compêndios de pós-colheita | citado na fonte | dias de prateleira |

Regra de escolha entre fontes conflitantes: **dado do país > dado regional > regra declarada**.
Cada escolha fica escrita no campo `method`/`source` da própria entrada, e nenhum valor foi
inventado para preencher buraco.

## 4. Como cada número foi obtido

### 4.1 Calendário (`crop_calendar.json`)
- **Camada primária** (`tools/agri_calendar_primary.json`, **37 pares**): a janela vem de
  documento, com `evidence` = {report, quote, url, page, basis} em cada entrada. Tem prioridade
  sobre tudo e cobre **15/15 do top-15 de trigo, arroz e milho**, **10/15 da soja** e **5/15 da
  batata** (top-15 = produção do próprio `crop_yields.json`). Fontes: USDA NASS **AH-628** (EUA,
  5 pares, tabela por estado), USDA **AH-664** (28 pares, leitura do gráfico de calendário com a
  página impressa e a legenda citadas), **USDA GAIN** (4 pares, texto) e o **FAO GIEWS Country
  Brief — Iran** (10/nov/2026) no `IRN/wheat`, onde o gráfico do AH-664 é regional (Oriente
  Médio) e só a conferência textual dá as duas pontas. A camada primária **não** passa pela
  agregação: as safras entram como o documento as descreve.
- FAO: agregação das zonas agroecológicas por país/cultura — plantio do menor início ao maior
  fim, colheita idem. Blocos separados por ≥3 meses viram **duas safras**; 3+ blocos caem em
  fallback de 1 safra agregada (47 casos, `window_labels` preserva o rótulo bruto).
- JRC: dekads 1–36 convertidas para mês do primeiro dia da dekad (`source_year: null` — a fonte
  não publica ano).
- Prioridade **PRIMARY > FAO > JRC > regra**. `all_year: true` = 33 pares em que a fonte declara
  ano inteiro (sem meses inventados).
- **Regra declarada** (592 entradas): herda as janelas de um país observado **do mesmo hemisfério
  e da mesma faixa de latitude** (`agri_common.climate_band`), em dois níveis — 440 `same_region`
  (o mais próximo dentro da mesma `region` do jogo) e 152 `same_band_other_region` (outra região,
  **só** quando não há doador no próprio continente, com `cross_continent: true` e
  `justification`). Faixa de latitude vizinha (clima diferente) e hemisfério oposto **não** são
  mais aceitos: 29 pares ficam `no_data` com `reason` em pt-BR (§6.1). O doador, a distância e o
  nível ficam em `rule: {tier, donor, distance_km}`, e o build **falha** se algum par do top-15
  ficar em regra sem estar declarado em `_meta.top15_exceptions`.

### 4.2 Rendimento e área (`crop_yields.json`)
FAOSTAT QCL, último ano ≤2024 com área > 0: `yield_t_ha = production_t / area_harvested_ha`
(derivado, 3 casas). 175/179 nações casadas; 4 sem correspondência declarada (SAH, SOL, TWN,
XKX). 6 nações ficam 5×`no_data` porque o FAOSTAT não tem nenhuma das cinco culturas (GNQ, SAH,
SGP, SOL, TWN, XKX). Áreas reais: CAN não tem arroz e IDN não tem trigo no QCL (alerta do build,
ausência real).

### 4.3 Estoque inicial (`crop_stocks.json`)
USDA PSD `Ending Stocks` do último marketing year ≤2024 (354 entradas, 115 de trigo, 114 de
milho, 65 de arroz, 60 de soja; PSD não cobre batata). Sem PSD — ou com estoque 0 no ano —
**regra declarada de 60 dias de consumo**: `estoque = consumo × 60/365`, com consumo do PSD
quando existe ou produção FAOSTAT como proxy (declarado em `consumption_basis`). 390 entradas de
regra, 151 sem dado. `crop_stocks._meta.rule_days_default = 60`.

### 4.4 Perdas pós-colheita (`postharvest_losses.json`)
86 entradas com medição do próprio país (APHLIS, 40 países africanos + storage share), 677 com
taxa **regional** FAO 2011 (`regional_observed`, não é medição nacional) e 132 por mediana
regional/global (`regional_default`). Batata e soja não existem no APHLIS: 100% FAO 2011/regra.

### 4.5 Fertilizante (`fertilizer_n.json`)
IFA FUBC: `Aver_N_rate_kg_ha` (média sobre a área total da cultura) do ano mais recente ≤2019;
311 observados (220 do levantamento 2017/18, 91 de relatórios históricos 1984–2001, rotulados
`IFA_FUBC_HISTORIC`) e 584 por mediana da mesma `Region_IFA` e cultura. Sanidade contra o
Banco Mundial `AG.CON.FERT.ZS` (todos os nutrientes por ha arável) fica em
`_meta.crosscheck_world_bank` — só ordem de grandeza, não substitui o dado.

### 4.6 Pecuária e prateleira
Carcaça bovina e leite por animal vêm do QCL (kg/cabeça e kg/cabeça/ano) por nação. O ciclo é
**contínuo** (`seasonality: "continuous"`, `monthly_factor: null`): não há série mensal publicada
por país, e inventar uma seria violar a regra de ouro. Idade de abate (17 meses), lactação (305 d)
e intervalo entre partos (12 meses) vêm de páginas FAO citadas, verificadas por frase no build.
Prateleira: 9 valores com fonte (grãos 365 d em ambiente seco; milho 274; batata 61; farinha 365;
pão 18; arroz ensacado 730; carne 5 d refrigerada; enlatado 1825) e 3 `no_data` (soja, farelo,
ração) — sem fonte acessível, ficou nulo em vez de estimado.

## 5. Amostra (conferida pelo verificador contra o cache bruto)

| país/cultura | rendimento t/ha | área ha (ano) | estoque t (método) | N kg/ha |
|---|---|---|---|---|
| BRA soja | 3,147 | 45.906.902 (2024) | 37.688.000 (PSD, 221 d) | 15,7 |
| BRA milho | 5,426 | 21.186.816 (2024) | 10.604.000 (PSD, 42 d) | 68,5 |
| IND arroz | 4,313 | 50.511.930 (2024) | 48.000.000 (PSD, 144 d) | 135,4 |
| IND trigo | 3,559 | 31.832.884 (2024) | 11.800.000 (PSD, 40 d) | 112,3 |
| CHN arroz | 7,155 | 29.006.900 (2024) | 104.500.000 (PSD, 263 d) | 177,6 |
| CHN trigo | 5,940 | 23.587.400 (2024) | 127.775.000 (PSD, 311 d) | 212,1 |
| USA milho | 11,257 | 33.547.180 (2024) | 39.404.000 (PSD, 46 d) | 157,6 |
| USA soja | 3,413 | 34.823.570 (2024) | 8.840.000 (PSD, 47 d) | 5,5 |
| NGA milho | 2,030 | 5.500.000 (2024) | 331.000 (PSD, 10 d) | 9,2 |
| UKR trigo | 4,595 | 4.883.800 (2024) | 726.000 (PSD, 31 d) | 103,8 |
| EGY trigo | 6,778 | 1.386.892 (2024) | 2.911.000 (PSD, 53 d) | 180,4 |

Calendário da amostra: **todos observados** — USA milho (AH-628: plantio abr–jun, colheita
set–nov), USA soja e USA trigo (AH-628), BRA soja (AH-664 p. 80: plantio out–dez, colheita
mar–mai), IND arroz (AH-664: kharif jun–ago/out–nov e rabi nov–jan/mar–abr), CHN trigo, RUS
trigo, AUS trigo, FRA trigo (AH-664), ARG soja (USDA GAIN: 1ª out–dez/abr–mai, 2ª dez–jan/mai–jul)
e EGY trigo (FAO: plantio 15–30/nov, colheita 25/abr–25/mai). Nenhum caso da amostra cai em regra
de vizinho — era exatamente esse o defeito corrigido (`USA milho ← PRK` não existe mais).

## 6. Limitações declaradas (não são erros; são o buraco que a fonte deixa)

1. **Calendário: 66,1% é herdado** (era 73,5%). O top-15 de trigo, arroz e milho é 100%
   observado; falta batata (10 dos 15 pares) e soja de RUS/UKR/URY/NGA/BEN — nenhum documento
   acessível declara **as duas pontas** da janela para eles: o **USDA IPAD** (fonte canônica de
   crop calendar) respondeu **HTTP 503** durante toda a coleta (reconferido em 18/nov/2026), o
   AH-664 não tem capítulo de batata, e ABARES/CPRI-ICAR e os relatórios GAIN de batata da UE
   não trazem meses por país. Esses 15 pares estão declarados em `_meta.top15_exceptions` e
   seguem em regra estrita (mesmo hemisfério **e** mesma faixa). A regra ficou mais estreita:
   faixa vizinha e hemisfério oposto saíram, então **29 pares** (nórdicos/bálticos em batata,
   arroz e soja; NZL em tudo; RUS em arroz/batata/soja) passaram de "herdado" para `no_data` com
   motivo — é ausência honesta, não erro. `USA maize ← PRK 10.726 km`, que aparecia no GAME_STATE,
   **não existe mais**.
2. **Janelas agregadas largas** quando a fonte tem 3+ safras (BGD Aman/Aus/Boro, MMR): 47 pares
   ficaram em 1 safra agregada; 185 safras têm colheita agregada começando antes do fim do
   plantio agregado (efeito de misturar zonas). `note` marca os casos.
3. **Estoque:** 26 entradas observadas têm marketing year <2015 (séries de país congeladas no
   PSD, que passou a reportar a UE como agregado). Está visível no `marketing_year`.
4. **Perdas:** 75,6% é taxa regional FAO 2011 (cereais/raízes/oleaginosas), não medição do país;
   APHLIS cobre só grãos e só África. Em 2 casos o `storage_share_pct` é maior que o `loss_pct`
   (ZAF e ZMB, trigo) — quirk da fonte, não foi "corrigido".
5. **Fertilizante:** 91 observações vêm de relatórios 1984–2001; a batata usa o grupo
   "Roots & tubers" (26 países só têm a série "Potato" antiga e caíram na regra).
6. **Prateleira:** soja, farelo e ração sem fonte → `null`. `wheat→flour` dá razão 1,0 porque é
   o que as duas fontes dizem.
7. **Batata** tem rendimento real acima de 30 t/ha (até 51,3 t/ha na NZL); o validador usa teto
   de 60 t/ha para ela e avisa entre 30 e 60. Cereais e soja ficam abaixo de 30 com folga.

## 7. O que o núcleo precisa fazer com isso (contrato de leitura)

- **Produção** = `area_harvested_ha` (ou a área plantada da região, B3/B6) × `yield_t_ha`.
  Só chega ao estoque dentro da janela de `harvest` da safra; fora dela, o fluxo é 0.
- **Estoque** começa em `ending_stocks_t`, drenado pelo consumo diário; o preço sobe na
  entressafra e cai na colheita (Parte C precifica; aqui só o volume).
- **Perda** = `loss_pct` aplicado ao que entra no silo, agravada quando a infra B5 de armazém é
  baixa (o dado separa `storage_share_pct` onde existe).
- **Fertilizante** = `n_kg_ha` × área → demanda de `fertilizer` (gás + fosfato, Parte E).
- **Pecuária** = fluxo contínuo `feed→meat`; `carcass_kg_per_head` converte cabeça em tonelada;
  `product_shelf_life.storage_days` define quanto tempo cada bem aguenta em estoque.

## 8. Reproduzir

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"
& $py tools/build_agri_yields.py --fetch
& $py tools/build_agri_calendar.py --fetch
& $py tools/build_agri_losses.py --fetch
& $py tools/build_agri_fertilizer.py --fetch
& $py tools/build_agri_livestock.py --fetch
& $py tools/validate_agriculture_g.py     # EXIT=0
```

Sem `--fetch`, os construtores leem `tools/_tmp_agri_raw/` (cache apagado ao fim da tarefa;
`--fetch` reconstrói tudo). Cada build é determinístico: duas execuções produzem bytes idênticos.
