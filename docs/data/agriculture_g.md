# Parte G — agricultura sazonal (dados)

> Tarefa **D-094** (dados). Insumo da **Parte G** de `docs/plans/perf_pop_financas_plano.md`
> (§"PARTE G — Agricultura sazonal"). Entrega **só de dados**: nenhum código de núcleo,
> nenhum GDScript, nenhum `.tscn`. O jogo começa em **2027-01-01**; por isso o estoque
> inicial é de **fim de safra**, e ninguém nasce com silo vazio.
>
> Branch `data/g-agriculture`, worktree `G:/projetos/opengs-wt-data-agri`, base
> `balance/global-resources` `ebd24157`. **Nada mesclado.** A integração no núcleo
> (produção = área × rendimento, janela de colheita, consumo do estoque, perda por
> armazenagem) é trabalho do `@gerente-simulacao`.

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
| `crop_calendar.json` | plantio e colheita país×cultura, 1–2 safras, hemisfério | 895 (179×5) | 237 / 658 / 0 |
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
| `IFA_FUBC_2018` / `IFA_FUBC_HISTORIC` | IFA — *Fertilizer use by crop and country 2017-2018*, doi:10.5061/dryad.2rbnzs7qh | CC0 1.0 | `Aver_N_rate_kg_ha` por país×cultura |
| `APHLIS` | APHLIS — perdas pós-colheita por país/cultura (tabelas públicas) | citado na fonte | perda total e da fase de armazenagem (África) |
| `FAO_2011` | FAO — *Global food losses and food waste* (Gustavsson et al., 2011), Annex 4 p. 26–27 | citado na fonte | taxa regional por grupo (cereais / raízes / oleaginosas) |
| `FOODKEEPER` / `FAO_INPHO` | USDA FSIS FoodKeeper; FAO INPhO compêndios de pós-colheita | citado na fonte | dias de prateleira |

Regra de escolha entre fontes conflitantes: **dado do país > dado regional > regra declarada**.
Cada escolha fica escrita no campo `method`/`source` da própria entrada, e nenhum valor foi
inventado para preencher buraco.

## 4. Como cada número foi obtido

### 4.1 Calendário (`crop_calendar.json`)
- FAO: agregação das zonas agroecológicas por país/cultura — plantio do menor início ao maior
  fim, colheita idem. Blocos separados por ≥3 meses viram **duas safras**; 3+ blocos caem em
  fallback de 1 safra agregada (47 casos, `window_labels` preserva o rótulo bruto).
- JRC: dekads 1–36 convertidas para mês do primeiro dia da dekad (`source_year: null` — a fonte
  não publica ano).
- Prioridade FAO > JRC > regra. `all_year: true` = 33 pares em que a fonte declara ano inteiro
  (sem meses inventados).
- **Regra declarada** (658 entradas): herda as janelas de um país observado, em três níveis —
  350 `same_region` (mesma faixa de latitude e mesma região do jogo), 263 `same_band_nearest`
  e 45 `adjacent_band` (faixa vizinha, para os países frios e NZL). O doador, a distância e o
  nível ficam em `rule: {tier, donor, distance_km}`.

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

Calendário da amostra: **EGY trigo é observado** (FAO: plantio 15–30/nov, colheita 25/abr–25/mai).
As demais caem em regra: BRA soja ← BOL (`same_region`, 2.137 km, duas safras: jun/out–nov e
nov–jan/mar–mai), IND arroz e trigo ← MMR, CHN arroz ← BGD, USA milho ← PRK (10.726 km),
UKR trigo ← AZE.

## 6. Limitações declaradas (não são erros; são o buraco que a fonte deixa)

1. **Calendário: 73,5% é herdado.** FAO e JRC não publicam calendário nacional para USA, BRA,
   CHN, IND, RUS, UKR e a maior parte da Europa. O USDA IPAD publica os calendários apenas como
   **imagem** (chart), não como dado legível por máquina — por isso não entrou. Consequência
   visível: `USA maize ← PRK 10.726 km`, `UKR wheat ← AZE 1.689 km`. O doador sempre está no
   JSON; o núcleo deve tratar `rule_neighbor` como aproximação, não como observação.
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
