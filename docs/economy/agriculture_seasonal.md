# Parte G — agricultura sazonal (núcleo, Bloco 1)

> Decisão **D-096** (`GAME_STATE.md` §14). Dado em `docs/data/agriculture_g.md`
> (D-094). Plano: `docs/plans/perf_pop_financas_plano.md` §“PARTE G”.
> Branch `feat/g-seasonal-agriculture`, worktree `opengs-wt-g`, base
> `balance/global-resources` `6c3736fe`.
>
> O jogo começa em **2027-01-01**; o relógio do núcleo é só `WorldState::day` e o
> calendário civil é derivado de `kAgriStartYear = 2027` (mesma âncora de
> `Economy.START_UNIX`). Antes desta parte, o alimento cru era produzido
> **diariamente** pelas construções-fazenda; agora a safra chega **concentrada na
> janela de colheita** e o estoque atravessa a entressafra.

## 1. Contrato dados → núcleo

`core/economy/agriculture_service.gd` (`class_name AgricultureService`) carrega os
7 arquivos de `data/definitions/agriculture/` e declara no núcleo **antes do dia 1**
(chamado pelo autoload depois do `GeographyService.apply_to_sim`, porque a área por
cidade depende da capacidade de terra que a geografia declara):

| API (C++) | O que declara | Origem |
|---|---|---|
| `set_crop_plan(city, product, area_ha, yield_t_ha, seasons)` | área colhida, rendimento e 1–4 janelas de plantio/colheita (dia do ano + `share`) | FAOSTAT QCL (`crop_yields`) + calendário (`crop_calendar`) |
| `set_fertilizer_n(city, product, kg_ha)` | N aplicado (kg N/ha) | IFA FUBC (`fertilizer_n`) |
| `set_crop_initial_stock(city, product, t)` | estoque inicial (assign, idempotente) | USDA PSD `ending_stocks_t` ou a regra de 60 d já aplicada pelo dado (`crop_stocks`) |
| `set_crop_loss(product, loss_pct, storage_share_pct)` | perda pós-colheita (% do que entra) e a fatia de armazenagem declarada | APHLIS / FAO 2011 (`postharvest_losses`) |
| `set_shelf_life(product, days)` | prazo de prateleira dos 12 bens da cadeia (`0` = `no_data`, não decai) | FoodKeeper / FAO INPhO (`product_shelf_life`) |
| `set_storage_quality(city, q)` | **hook** de qualidade do armazém 0..1 (B5/E); sem chamada, derivada da infra | — |
| `get_agriculture_status()` | ficha por nação (estoque, cobertura, próxima colheita, produção do ano, perda do mês) + totais acumulados | — |

**Área por cidade (regra declarada).** `área_cidade = area_harvested_ha da nação ×
participação`, com `participação = capacidade de terra da cidade ÷ Σ da nação para a
cultura` — a mesma `GeographyService.city_land_capacity_t_day` que já alimenta o
`set_land_capacity`. Nação sem capacidade declarada usa a **população de simulação**
(contado no relatório do serviço em `area_basis`). O **estoque inicial** usa o mesmo
rateio (o silo fica onde está a lavoura). Participação por safra não existe nos
arquivos: **1/n** por safra (declarado).

**O que NÃO recebe plano (ausência explícita).** 62 pares nação×cultura sem
calendário utilizável (29 `no_data` + 33 `all_year`) e 215 sem área/rendimento
continuam com a **produção diária da fazenda**; nada é inventado para eles.

**Perda por produto.** O dado de perda é por **nação** e o contrato do núcleo é por
**produto**: usa-se a média ponderada pela produção nacional (`production_t`).
`storage_share_pct` só é declarada quando o dado tem (APHLIS, África).

## 2. Ordem da fase e regra da colheita

`phase_agriculture()` (`gdextension/src/sim/agriculture.cpp`) roda em `sim_day.cpp`
**depois de `phase_reset` e antes de `phase_production`** — a safra do dia já está no
estoque quando a cidade compra, e os fluxos do dia estão zerados.

1. **Plantio (uma vez por safra/ano, na janela).** Consumo de fertilizante:
   `t = n_kg_ha × área × share ÷ (kAgriFertilizerNContent × 1000)`, comprado do
   estoque do mercado da cidade (o custo entra como `input_value` das fazendas da
   cultura, na proporção da capacidade — o mesmo critério da receita). Sem estoque,
   o rendimento cai proporcionalmente até o **piso de sequeiro**
   `kAgriNoFertilizerYieldFloor` (0,35). Gás/energia é a Parte E: campo pronto
   (o N do bem é a aproximação `0,30 t N/t`).
2. **Colheita (só dentro da janela).** Orçamento anual = `área × rendimento`; cada
   safra leva `share`; a entrega é **cumulativa e idempotente**:
   `alvo_dia = orçamento × share × fator_rendimento × (posição+1)/dias_da_janela` e
   entrega-se `alvo − já_entregue` (por ano/safra, salvo no save). Fora da janela o
   fluxo é **zero**. A primeira vez que o núcleo vê uma janela **já em curso**
   (início do mundo em janeiro, ou load de save sem o estado), o trecho decorrido é
   marcado como entregue — o estoque carregado já o representa (nada de lote
   retroativo).
3. **Perdas.** Entrada no silo: `loss_pct × kAgriEntryLossShare (0,60)` agravada pelo
   armazém (`1 + 1,5 × (1 − q)`). Mensal: `(storage_share_pct ou loss_pct × 0,40)/12`
   com o mesmo agravamento, aplicada na virada do mês sobre o estoque das 5 culturas.
4. **Perecibilidade.** Todos os 12 bens da cadeia: **meia-vida = prazo de prateleira**
   (`kAgriShelfHalfLife` = 0,5), pré-calculada por produto. `soy`, `bran` e `feed` são
   `no_data` no arquivo e **não decaem** (soja ainda perde pela armazenagem acima).

**Sem contagem dupla.** `phase_production` **suprime** a produção diária da fazenda
quando existe plano para aquele (cidade, produto) — a capacidade dessas fazendas sai
do denominador de rateio também. **A fazenda continua como instituição**: ela recebe
a receita da venda da safra (a contabilidade reparte `revenue_by_product` pela
capacidade) e paga folha/opex, então o emprego agrícola não desaparece com a
supressão. O **autoconsumo em espécie** das famílias agrícolas passa a usar a
produção média da safra (`área × rendimento ÷ 365`) quando há plano, e não a
capacidade nominal da fazenda — senão o autoconsumo drenava um estoque que a lavoura
só repõe na colheita.

**Preço.** Nada de fórmula sazonal: a fase só mexe em VOLUME. O preço livre é o da
D-091/D-092 (`prices.cpp`); a sazonalidade emerge da oferta concentrada e do estoque
drenando. O estoque **não** entra em `consumed_total` (perda não é consumo).

## 3. Estoque inicial e conservação

Estoque inicial no dia 1 = `ending_stocks_t` observado; sem PSD (ou estoque 0 no ano),
a **regra de 60 dias do próprio dado** (`consumo × 60/365`); sem consumo declarado, o
par fica sem estoque (ausência explícita). Total declarado no mundo: **937,9 M t**.

A conta da entressafra fecha em toneladas, e o teste a verifica por nação/cultura:
`estoque inicial + chegadas físicas + colheita líquida = consumo do mercado + consumo
em espécie + perdas de estoque`. Duas medições novas existem **só para isso** (sem
efeito no jogo): `City::agri_in_kind[]` (consumo em espécie, que não entra em
`market.demand`) e `City::agri_arrived[]` (carga que **chegou** — o `imported` do
mercado conta a compra e é zerado logo depois de `phase_arrival`). A perda de ENTRADA
não entra nessa conta: ela acontece antes de a tonelada entrar no silo.

## 4. Save v24

`kSaveVersion = 24`. Bloco raiz `agriculture` (só entradas com estado):
`month_key` e, por (cidade, produto, safra), `planted_year`, `yield_scale`,
`harvest_yield_scale` (congelado no início da colheita — o plantio do ciclo seguinte
não reescreve o orçamento), `fertilizer_t`, `harvest_year`, `delivered_t`,
`loss_month_t`, `loss_month_key`. O PLANO (área, rendimento, janelas, N, perdas,
prateleira) é **definição** e viaja no bloco `definitions` do snapshot
(`crop_plans`, `crop_losses`, `shelf_life`), reaplicado pelo serviço no jogo e pelo
`definitions_from` no CLI.

Migração aditiva em degraus: **22 → 23 → 24**. A **B6 grava v23** (patrimônio da
região) e este ramo grava v24; no merge resolve-se **só a constante** — o binário v24
carrega save v23 (a chave `agriculture` ausente deixa o estado zerado: a safra em
curso no dia do load não é reentregue) e o v22 idem. Testado: round-trip v24
(bloco idêntico, primeiro dia após o load idêntico) e load de v22 (planos reaplicados
pelas definições, estado zerado, **0 salto de estoque**).

## 5. Status (`get_agriculture_status`, sob demanda)

Por nação: `storage_quality` (média das cidades), e por cultura `stock_t`,
`demand_t_day`, `days_cover`, `imported_today_t`, `exported_today_t`,
`arrived_today_t`, `in_kind_today_t`, `next_harvest_days`/`next_harvest_doy`,
`production_year_t`, `loss_month_t` (entrada + armazenagem + perecibilidade, com os
dois últimos separados), `delivered_today_t`, `harvesting`. Totais acumulados desde o
dia 0 por bem da cadeia (`perishable_t`, `storage_t`, `entry_t`) e `stock_t`.
`{}` quando não há plano declarado (mundo sintético AGRIA/PORTUS).

## 6. Aceite medido (Bloco 1)

`tools/agriculture_seasonal_test.gd` (runner SceneTree, exit ≠ 0 ao falhar) num mundo
real de 849 cidades, sem IA e sem camada urbana:

- **Janela:** 120 d a partir de 2027-01-01 — `fora_da_janela = 0`,
  `dentro_sem_entrega = 0`, 178 safras em curso no dia 1 marcadas como já colhidas
  (estoque inicial), entregas nos dois hemisférios (328 dias-norte / 419 dias-sul),
  janela × entrega idênticas na amostra (diferença máxima de 1 dia, o do dia 1).
- **Entressafra:** **0** leitura de estoque negativo; **157** pares nação×cultura
  zerados, **157 provados pelo dado** (0 inexplicados; 20 deles já depois da 1ª
  colheita): a oferta física fecha com o uso em 2% e o dia do zero é anterior à
  primeira janela de colheita.
- **Armazém (A/B no mesmo mundo):** CHN/milho, 37 cidades, 129,0 M t — perda **ruim
  5,43 M t** vs **boa 4,56 M t** em 40 d (só a qualidade muda).
- **Perecibilidade 61 d:** batata **53,8%** do estoque (meia-vida 61 d), trigo
  **13,0%** (meia-vida 365 d), soja **0** (`no_data`); taxa diária exata conferida em
  5 produtos.
- **Save:** v24 round-trip + load v22, acima.
- **Goldens do Bloco 1:** `economy_integration_test` e `economy_ai_test` verdes e
  **inalterados** (o cenário AGRIA/PORTUS não declara planos, logo não há safra
  sazonal nele). Tick do mundo real sem IA: **~24–26 ms/dia**.

## 7. Limites e lacunas (declarados)

- **Leite não existe como bem** no jogo: o dado nacional (`livestock_cycle`) fica
  exposto por `livestock_summary()` e nada é criado. A pecuária continua no fluxo
  contínuo `feed → meat`; o dado declara `seasonality: continuous` e **nenhum fator
  mensal foi inventado**; `carcass_kg_per_head` só seria usado para converter cabeça
  em tonelada (não é preciso hoje).
- **Fertilizante**: o mundo ainda não tem indústria de fertilizante (Parte E), então
  hoje a safra rende o piso de sequeiro na maior parte do mundo; o custo só aparece
  quando houver estoque do bem. A conversão N→produto é declarada (0,30).
- **Calendário**: 62 pares sem janela e 215 sem rendimento continuam com a produção
  diária legada. Soja/batata de RUS/UKR/URY/NGA/BEN seguem em regra de vizinho
  (ver D-094 §6).
- **Qualidade do armazém**: derivada da infra B5 (estrada 0,35 / energia 0,45 / água
  0,20, piso 0,15) enquanto ninguém declarar `set_storage_quality`. Não há campo
  próprio de armazém na B5; o hook existe para a B5/E.
- **Bloco 2 (aberto):** calibração de comida por país e preço sazonal em 365 d,
  bateria completa, goldens do mundo real e bench; a IA do governo **ainda** vê
  `land_capacity` das culturas com plano sazonal e pode expandir fazenda que não
  produz (o limite da IA precisa considerar o plano); a prateleira curta da carne
  (5 d) e o autoconsumo em espécie são os candidatos naturais de calibração.
  Medido no 2a (§8): o bloqueio da IA por plano piora o portão e foi revertido;
  a carne não gera perda mensurável. Ver §8.5.

## 8. Bloco 2a — mundo de 365 d com IA, portão de comida e preço

Harness de 365 d (runner SceneTree; evidência em
`docs/economy/perf/g2_world_ai_365.json`). O script do harness é reconstruído no
2b como `tools/g2_world_ai_365.gd` (o arquivo original era untracked e se perdeu
numa limpeza de temporários; o JSON abaixo é a saída que ele emitiu). Monta o
mundo pelo **caminho do autoload**: cidades + geografia real + infra B5 inicial + camada urbana +
informalidade/saúde B7 + `AgricultureService` (3.384 planos, 179 nações) + IA do
núcleo, `sim_threads=1`, 365 dias.

Métrica do portão = a do M2/B5: **média dos últimos 30 dias de
`p_consumed / p_need` por nação < 0,975** (`docs/economy/M2_marco.md`).

**A/B de controle no mesmo HEAD** (`--agriculture=0`, mesmo harness e caminho):

| mundo | nações < 0,975 | mediana | média | falências | drift | hash |
|---|---|---|---|---|---|---|
| **com agricultura (publicado)** | **105**/179 | 0,9556 | 0,8972 | 322 | +0,002581 | `4c80cd36…` |
| sem agricultura (mesmo HEAD) | 149/179 | 0,6836 | 0,6550 | 4 | +0,001810 | `a333f1c9…` |
| base documentada (B5, antes da G) | ~148/179 | 0,6836 | 0,6495–0,6550 | 4 | ≤0,0018 | — |

O portão **melhora** com a G (149 → 105): o estoque inicial (937,9 M t, §3) é o
colchão do primeiro ano. **Não é produção do ano** — ver a perda por prateleira
abaixo. A comparabilidade com a base é de caminho idêntico (mesmo harness).

### 8.1 Invariantes, dinheiro e falências

- **Violações de invariante: 0**, dias travados: 0, dia final 365.
- **Dinheiro conservado**: drift **+0,002581 XGS** (limite 0,01), caixa final
  ~6,5 bi.
- **Falências de construção: 322** (limite do sweep: 28) — **todas são obras
  abertas pela IA durante o ano**. Nenhuma das construções do dia 0 fecha
  (`missing` = 0 no save final), e o mundo termina com 3.862 construções
  (≈3.000 abertas em 365 d; 748 são fazendas de cultura).
- **Causa medida**: a G derruba o preço de ~1,5–1,9× base (mundo sem agricultura,
  escasso) para ~1,0× (mundo com os estoques herdados). O gate de retorno da IA
  é de margem corrente; com o preço em cima do custo, ~10% das obras novas não
  pagam folha/opex em 60 d e fecham. Não é regra da G — é o encontro do
  `kLossDaysBeforeClose` com o novo nível de preço.
- **Não calibrado neste bloco** (decisão em §8.5): mexer no gate de retorno da IA
  ou no `kLossDaysBeforeClose` é mudança de núcleo fora do escopo da agricultura;
  fica como item do 2b com o número acima.

### 8.2 Portão de comida — as 15 piores nações

Cobertura em dias de consumo = estoque final ÷ demanda do dia; "produção do ano" =
o que entrou no mercado no ano (colheita entregue + produção diária das culturas
sem plano); "importado" = compras do ano; armazém = qualidade derivada da infra
B5 (0..1, −1 = nação sem plano).

| # | nação | satisf. | tr/ar/mi/ba/so (d) | produção do ano | importado | armazém | causa dominante |
|---|---|---|---|---|---|---|---|
| 1 | SLB | 0,191 | 0/0/0/−/0 | 893 t | 10 kt | 0,61 | estoque inicial curto |
| 2 | HTI | 0,193 | 0/0/0/−/0 | 135 kt | 279 kt | 0,26 | armazém ruim |
| 3 | PSX | 0,211 | 0/−/0/9/0 | 37 kt | 160 kt | 0,80 | estoque inicial curto |
| 4 | DJI | 0,313 | 0/0/0/−/0 | 6 t | 64 kt | 0,60 | estoque inicial curto |
| 5 | SOM | 0,341 | 0/0/0/−/0 | 25 kt | 310 kt | 0,31 | importação insuficiente |
| 6 | COD | 0,370 | 0/0/0/0,5/0 | 1,5 Mt | 3,3 Mt | 0,33 | armazém ruim |
| 7 | SAH | 0,442 | 0/−/0/−/0 | 0 | 4 kt | — | sem rendimento |
| 8 | COG | 0,476 | 0/1/0/0/0,3 | 120 kt | 394 kt | 0,33 | estoque inicial curto |
| 9 | MLI | 0,523 | 0/190/372/−/11 | 2,2 Mt | 432 kt | 0,52 | acesso/renda |
| 10 | SOL | 0,630 | 0/0/0/−/0 | 50 kt | 169 kt | — | sem rendimento |
| 11 | JPN | 0,634 | 12/80/15/25/10 | 4,5 Mt | 5,0 Mt | 0,92 | perda de estoque |
| 12 | ITA | 0,735 | 468/521/374/68/417 | 9,0 Mt | 782 kt | 0,99 | acesso/renda |
| 13 | AUT | 0,741 | 683/−/1290/251/884 | 2,5 Mt | 108 kt | 1,00 | acesso/renda |
| 14 | NED | 0,742 | 52/−/14/56/− | 2,8 Mt | 637 kt | 1,00 | perda de estoque |
| 15 | GMB | 0,743 | 0/0/0,4/−/1,7 | 32 kt | 79 kt | 0,59 | perda de estoque |

Detalhe por cultura das seis piores (estoque, demanda, cobertura, colheita
entregue no ano, importado; "não" = par sem plano e o motivo do dado):

| nação | cultura | plano | estoque | demanda (t/d) | cobertura | produção do ano | importado |
|---|---|---|---|---|---|---|---|
| SLB | trigo | não (rendimento) | 0 | 132,7 | 0 d | 0 | 4 kt |
| SLB | arroz | sim | 0 | 8,0 | 0 d | 893 t | 10 t |
| SLB | milho | não (rendimento) | 0 | 5,8 | 0 d | 0 | 527 t |
| SLB | soja | não (rendimento) | 0 | 65,4 | 0 d | 0 | 5 kt |
| HTI | trigo | não (rendimento) | 0 | 381,9 | 0 d | 0 | 0 |
| HTI | arroz | sim | 0 | 139,6 | 0 d | 43 kt | 0 |
| HTI | milho | sim | 0 | 313,7 | 0 d | 57 kt | 65 kt |
| HTI | soja | não (rendimento) | 0 | 742,3 | 0 d | 36 kt | 215 kt |
| PSX | trigo | sim | 0 | 378,7 | 0 d | 11 kt | 86 kt |
| PSX | milho | não (rendimento) | 0 | 219,9 | 0 d | 0 | 60 kt |
| PSX | batata | sim | 1 kt | 153,7 | 9,2 d | 26 kt | 0 |
| DJI | trigo | não (rendimento) | 0 | 160,3 | 0 d | 0 | 53 kt |
| DJI | milho | sim | 0 | 19,5 | 0 d | 6 t | 7 kt |
| SOM | trigo | sim | 33 t | 1.743,8 | 0 d | 464 t | 141 kt |
| SOM | milho | sim | 154 t | 1.088,0 | 0,1 d | 22 kt | 147 kt |
| COD | arroz | sim | 0 | 2.774,6 | 0 d | 613 kt | 268 kt |
| COD | milho | sim | 2 kt | 11.457,8 | 0,1 d | 770 kt | 2,3 Mt |

Regra da causa dominante (declarada, todos os escores ficam no JSON): cada
sinal vale uma fração da cesta não atendida (`1 − satisf.`) —
`sem_calendario`/`sem_rendimento` = fatia da demanda em culturas que o dado
deixou sem plano; `estoque_inicial_curto` = `1 − dias_de_estoque_inicial/30`
(no dia 1); `armazem_ruim` = perda anualizada ÷ demanda anual (saturada em 5%),
e vira **`perda_de_estoque`** quando a qualidade do armazém ≥ 0,55 (a perda é da
prateleira, não da infraestrutura — ITA/AUT têm 468/683 dias de trigo e 0,99 de
armazém); `importacao_insuficiente` = `1 − importado/consumo anual`. Por último,
país com **≥ 180 dias de estoque em casa** é marcado **`acesso/renda`**: há
comida no país e a fome é de acesso (preço/renda), que a ficha agronômica não
mede — MLI (230 d) e ITA (413 d) são esse caso.

**O que o portão NÃO prova.** Há comida no agregado em vários dos 15 (ITA, AUT,
MLI, NED) e a população ainda come < 97,5% da necessidade: é a mesma dívida de
preço/renda do M2 #10, agora com estoque. Não se inventa causa que o dado não
tem.

### 8.3 Perdas acumuladas no ano (só a G)

Efeito da prateleira (`storage_days` do dado): meia-vida = prazo de prateleira.
Totais de perecibilidade em 365 d: **milho 285,6 Mt** (274 d), **trigo 247,9 Mt**
(365 d), **batata 190,5 Mt** (61 d), **arroz 133,7 Mt** (365 d); perda de
armazém +25,8 Mt de trigo e 20,5 Mt de milho; perda de entrada 25,0/23,0 Mt.
Estoque final: trigo 400,2 Mt, milho 392,2 Mt, soja 345,2 Mt, arroz 235,5 Mt,
batata 19,3 Mt.

O estoque herdado de 937,9 M t não sobrevive ao ano: a prateleira remove ~857 Mt.
O preço plano de 2027 (§8.4) é o primeiro ano do colchão — não uma propriedade
permanente.

**Carne (5 d):** perda acumulada **0 t** (perecível e armazém). O mercado de
carne não acumula estoque no dia (produção e venda diárias), então a prateleira
de 5 d não tem o que decair. **Nada calibrado**: não há perda absurda a corrigir.

### 8.4 Preço sazonal (sem fórmula sazonal)

Amostra mensal (30 d), razão ao preço-base `goods.json`, ponderada pela
quantidade vendida (`m_sold`), agregada por hemisfério (amostra pedida: norte
USA/CHN/IND/FRA; sul BRA/ARG/AUS/ZAF). A única fase que mexe em preço é
`prices.cpp` (estoque + oferta vs demanda); a G mexe só em volume.

| hemisfério | cultura | jan→dez | mínimo | máximo |
|---|---|---|---|---|
| norte | trigo | 1,04 1,05 1,04 1,03 1,04 1,04 1,03 1,03 1,04 1,03 1,04 1,04 | out 1,03 | fev 1,05 |
| norte | arroz | 1,03 1,01 1,02 1,02 1,02 1,02 1,03 1,02 1,02 1,02 1,02 1,02 | fev 1,01 | jan 1,03 |
| norte | milho | 1,00 1,00 1,00 1,00 1,00 1,00 1,01 1,00 1,00 1,01 1,01 1,00 | fev 1,00 | out 1,01 |
| norte | soja | 1,03 1,05 1,14 1,06 1,07 1,01 1,01 1,02 1,03 1,04 1,04 1,03 | jun 1,01 | mar 1,14 |
| sul | trigo | 1,07 1,09 1,09 1,08 1,09 1,09 1,13 1,08 **1,86** 1,11 1,09 1,08 | jan 1,07 | set 1,86 |
| sul | arroz | 1,01 1,02 1,02 1,02 1,01 1,00 1,02 1,00 1,00 1,03 1,02 1,02 | ago 1,00 | out 1,03 |
| sul | milho | 1,00 (todo o ano) | jul 1,00 | dez 1,01 |
| sul | soja | 1,04 1,03 1,03 1,04 1,04 1,03 1,03 1,04 1,04 1,03 1,04 1,03 | jun 1,03 | ago 1,04 |

**Leitura honesta: no mundo publicado a sazonalidade de preço NÃO aparece** — 3
das 4 culturas ficam dentro de ±5% da base o ano inteiro. O estoque herdado
cobre o consumo do ano, então a janela de colheita quase não move a oferta
disponível. A única exceção é o **trigo do sul em setembro (1,86)**, puxado por
AUS 1,94 — antes da colheita australiana (out–dez): é escassez de entressafra
num mercado fino, não um ciclo geral. Nada foi forçado.

**Sonda que isola o mecanismo** (`--no-initial-stock=1`: mesmos planos, sem o
estoque do dia 1; hash `b456b534…`): aí a sazonalidade emerge do volume —

| caso | janela de colheita (dado) | preço antes | preço na janela | Δ |
|---|---|---|---|---|
| IND trigo | mar–abr | 1,61 (fev) | 1,03 (mar) | **−36%** |
| FRA trigo | jul–ago | 2,81 (jun) | 1,08 (jul) | **−62%** |
| BRA soja | mar–mai | 1,72 (jan) | 1,07 (fev) → 1,05 (mar–dez) | **−38%** |
| trigo sul (AUS/ZAF, colheita out–dez) | out–dez | 1,00 (mar–jun) | 1,21 (jul) | **+21%** na entressafra |
| ARG soja | abr–jul | 1,00 (mar) | 1,04 (nov–dez) | **+4%** na entressafra |

Nenhuma fórmula mensal existe no núcleo: a queda é a oferta concentrada em
`phase_agriculture` encontrando `prices.cpp`. A sonda é diagnóstico, **não** o
mundo publicado.

### 8.5 Calibração: o que foi tentado e o que ficou

1. **IA/governo × fazenda de cultura com plano — tentado, medido e revertido.**
   Implementado (view com o sinal `crop_planned` do núcleo + bloqueio do
   candidato de fazenda no governo, no privado e nos grupos). Números: portão
   **121**/179 (vs 105 sem a regra), falências 300 (vs 322), hash `81da6d92…`.
   A premissa "a fazenda não produz" não vale para o modelo: a construção com
   plano **recebe a receita da safra** pela capacidade (`phase_accounting`
   reparte `revenue_by_product`) e paga folha local — bloquear corta renda de
   cidades finas e piora o portão. **Código revertido** (o diff não toca
   `gdextension/`); a medição fica registrada aqui e no `ai1/ai2` de `perf/`.
2. **Prateleira da carne (5 d) — nada a calibrar.** Perda 0 t no ano (§8.3).
3. **Sazonalidade de preço — nada forçado.** Nenhuma fórmula entrou; a
   diferença entre publicado × sonda é o estoque inicial, não o preço.

### 8.6 Reprodutibilidade

- Duas rodadas idênticas do mundo publicado fecham o **mesmo hash**
  `4c80cd365aceb9bc15ed69c616bdb77fbe8330f8e3aeac4c24f9af301996ac51`
  (sha256 de: dia, dinheiro, falências, comida por nação, série mensal de preço,
  totais da agricultura e estoque final por nação×cultura). A rodada anterior à
  correção do rótulo de mês fechou `21c2064d…` nas duas execuções — mesmos
  números, só a etiqueta de mês do último ponto mudou.
- A/B sem agricultura: `a333f1c9…`, também repetido.
- `sim_threads=1`; nenhuma variante cambial entra (o mundo publicado é `peg`).
- Sonda float/`--fx` **não** foi rodada neste bloco (fica para o 2b).

### 8.7 Bloco 2b (aberto)

- **Falências da IA (322, teto 28)**: gate de retorno × preço pós-estoque; é a
  maior dívida medida deste bloco. Precisa de decisão de núcleo (não é G).
- **Goldens do mundo real**: `economy_integration_test`/`economy_ai_test` não
  veem plano sazonal (AGRIA/PORTUS); falta um golden do mundo real com a G.
- **CLI × Godot** e bateria completa (sweep de 5 variantes com `--agriculture=1`).
- **Sonda float** e bench de release com a G.
- **Autoconsumo em espécie** por nação (hoje só o piso F9 + média da safra).
- **Dado `no_data` do trigo/arroz**: a meia-vida de 365 d remove ~250/134 Mt no
  primeiro ano; revisar se a regra de meia-vida é a leitura certa da prateleira
  (decisão de dado/regra, não de calibração silenciosa).

## 9. Calendário gravado na região — conquista não troca a safra (D-098)

Decisão do dono: o calendário de plantio/colheita é **gravado na região no
início do jogo**, herdado da nação **original** (a do `cities.json`), e a
produção usa esse calendário **para sempre**. Trocar o dono da região não muda a
época da safra.

**O que já garantia isso e o que faltava.** O núcleo nunca olhou a nação para
colher: `phase_agriculture` lê só o plano (cidade, produto, janelas em dia do
ano, área, rendimento) e o plano viaja no save em `definitions.crop_plans`, com
o estado da safra no bloco `agriculture` (v24). O furo era a **redeclaração**:
`AgricultureService.apply_to_sim` derivava o plano de `city.nation`. Um save
carregado e redeclarado depois de uma conquista trocaria a janela pelo
calendário do dono novo.

**Como ficou.** Três peças, todas em GDScript (o núcleo não muda):

1. `AgricultureService.calendar_nations(cities)` devolve
   `{ city_id: nação_original }`. O autoload `Economy` monta isso **uma vez**
   em `_ready` e guarda em `Economy.calendar_nations`;
2. `apply_to_sim(..., {"calendar_nations": ...})` agrupa e deriva os planos
   pela nação do calendário, não pela dona atual (e reporta
   `inherited_calendar_cities`);
3. `apply_to_sim(..., {"only_missing": true})` — o caminho de load,
   `Economy.refresh_agriculture()` — **não redeclara** par (cidade, cultura) que
   já veio no save (`skipped_existing`) e não semeia estoque inicial
   (`initial_stock: false`).

O calendário efetivo que fica no save é o próprio plano: as janelas em dia do
ano dentro de `definitions.crop_plans`. Um load não as reescreve.

**Teste** `tools/agri_calendar_owner_test.gd` (sem IA, mundo real de 849
cidades):

| passo | resultado |
|---|---|
| plano inicial de BRA-BEL, soja | colheita **[3, 4, 5]** (dado BRA) |
| calendário do dado | BRA soja [3,4,5] × USA soja [9,10,11] |
| conquista simulada **no save** (cidade `BRA`→`USA`, snapshot recarregado) | janela **[3, 4, 5]** (inalterada) |
| reaplicação no load com guarda (`calendar_nations` + `only_missing`) | **3.384** planos pulados, janela inalterada, 0 t de estoque ressemeado |
| reaplicação **sem** a guarda (dono novo) | janela vira **[9, 10, 11]** — a regressão que a decisão fecha |
| 200 d no mundo conquistado | soja do dono novo só colhe em **[3, 4, 5]** |

`agriculture_seasonal_test` continua verde (3.384 planos, estoque inicial
937,9 M t, 157/157 zerados provados, save v24/v22).

## 10. Bloco 2b — harness de 365 d reconstruído e o que ele reconfirma

O script do 2a era untracked e se perdeu numa limpeza de temporários; ele foi
reescrito como `tools/g2_world_ai_365.gd` (evidência da nova rodada em
`docs/economy/perf/g2_world_ai_365_rerun.json`). Mesmo caminho do autoload
(geografia, infra B5, urbano, B7, `AgricultureService` com o calendário herdado,
IA, `threads=1`), 365 d:

| medida | 2a (publicado) | 2b (reconstruído) |
|---|---|---|
| dia final | 365 | **365** |
| violações / dias travados | 0 / 0 | **0 / 0** |
| falências | 322 | **322** |
| drift de dinheiro | +0,002581 | **+0,002581** |
| portão de comida (< 0,975) | 105/179 | **105/179** |
| mediana / mínima | 0,9556 / SLB 0,1912 | **0,9556 / SLB 0,1912** |
| hash da trajetória | `4c80cd36…` | `6c45e927…` (entrada do hash diferente) |

Todo número publicado se repete; o hash muda porque a reconstrução calcula
sha256 de uma série menor (dia, dinheiro, falências) — a série original também
incluía comida por nação, preço e estoque. Duas rodadas seguidas dão o mesmo
`6c45e927…` (determinístico).

**Causa dominante das 15 piores** (regra do §8.2, agora com os escores medidos —
estoque do dia 1, importação do ano, perda ÷ demanda e a fatia de demanda sem
plano): SLB/DJI/SAH/SOL `sem_rendimento`; HTI/PSX/SOM/COD/COG/NED/GMB/JPN
`estoque_inicial_curto`; MLI/ITA/AUT `acesso/renda`. Só SLB troca de rótulo em
relação ao 2a (os dois escores empatam em ~0,78).

**Estoque no dia da amostra** (Mt; dia 30 = jan, dia 60 = mar — fevereiro não
cai no passo de 30 d):

| dia | trigo | arroz | milho | soja | batata |
|---|---|---|---|---|---|
| 30 | 229,0 | 172,0 | 273,3 | 122,9 | 71,4 |
| 90 | 231,8 | 158,0 | 266,0 | 168,2 | 85,6 |
| 150 | 307,5 | 169,3 | 285,4 | 289,2 | 49,7 |
| 210 | 437,5 | 200,1 | 275,1 | 300,9 | 32,9 |
| 270 | 504,3 | 216,5 | 343,1 | 313,4 | 24,9 |
| 330 | 437,7 | 237,8 | 423,6 | 346,1 | 21,4 |
| 360 | 407,2 | 236,5 | 399,0 | 345,9 | 19,3 |
| 365 | 400,2 | 235,5 | 392,2 | 345,2 | 19,3 |

A soja sobe de 123 para 346 Mt entre jan e nov (colheita brasileira/argentina
mar–jul + a americana set–nov): o volume se concentra e o estoque guarda o ano.
A batata cai de 71 para 19 Mt (meia-vida 61 d). O estoque final bate com o §8.3.

**Preço por hemisfério** (razão à base, ponderada pela venda; amostra
USA/CHN/IND/FRA e BRA/ARG/AUS/ZAF, série mensal): nenhuma cultura sai de
**±8%** da base no ano. Norte: trigo 1,02–1,04, arroz 1,04–1,06, milho 1,00,
soja 1,02–1,04, batata 1,00. Sul: trigo 1,04–1,08, arroz 1,00–1,05, milho 1,00,
soja 1,00–1,03, batata 1,00. **A sazonalidade de preço não aparece no mundo
publicado** — a leitura do §8.4 se confirma. O 1,86 do trigo do sul em setembro
do 2a era um mercado fino (cidade-sonda específica), não um ciclo: a mesma
cultura dá 1,04–1,08 aqui. A prova do mecanismo continua sendo a sonda sem
estoque inicial (`g2_price_probe_no_initial_stock.json`).

**Falências (322 × teto 28)** seguem abertas: o harness confirma o número e a
causa já medida no 2a (preço ~1,0× base com o estoque herdado × gate de retorno
de 60 d da IA). Não é regra da G; precisa de decisão de núcleo.



