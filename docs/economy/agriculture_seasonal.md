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
