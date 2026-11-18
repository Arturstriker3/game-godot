# Estágio de mina — aprofundar/expandir (D-086)

> **Estado:** mecânica implementada e testada; a **produção inicial** das minas
> com dado vem da produção real de 2024 (§7, D-086b). Os parâmetros do
> estágio — piso (5%), fatia da reserva (50%), +50% de capacidade, prazos
> (90/30 d) e curva de custo (1,25/estágio) — continuam **declarados, não
> calibrados** (decisão do dono, 2026-11-17). O aceite é de **mecânica,
> invariantes e ordem de grandeza**, não de produção real dia a dia.
> **Pendência medida (D-087, aberta):** o sweep de 365 d com a semente dá
> **37 falências** de construção contra o limite de 28 (sem ela, 20) e o
> minério fica no teto de 3× com e sem a semente — números e A/B em §7.4.
> **Escopo do núcleo:** `gdextension/src/sim/{sim_constants.h,sim_types.h,
> production.cpp,labor.cpp,construction.cpp,actions.cpp,commands.h,
> save_state.cpp}`, fachada `gdextension/src/economy_sim.{h,cpp}`, IA
> `gdextension/src/sim/ai/*` e semente `core/economy/geography_service.gd` +
> `core/economy/economy.gd`.

## 1. Desenho do dono

As minas e poços **já existentes** ficam no **último estágio** de operação,
produzindo quase nada. Para voltar a produzir é preciso **gastar dinheiro para
aprofundar/expandir** — e revitalizar um poço/mina é caro. Não há produção de
mina nova "de graça": toda mina nasce cheia no primeiro estágio, decai com a
extração e precisa de obra para abrir o estágio seguinte.

**O recurso NUNCA acaba (regra do dono, 2026-11-17).** A reserva da jazida é
**referência de profundidade** — a escala da curva de custo —, não um bloqueio:
aprofundar/expandir nunca é recusado por "reserva esgotada", a produção nunca
zera por esgotamento e nenhuma mina é encerrada por depleção. Cada estágio novo
fica **cada vez mais caro** (curva crescente e convexa, §2.4); em algum ponto é
mais barato abrir/aprofundar mina em outro lugar, o que **move a necessidade
pelo mapa** sem esgotar recursos.

## 2. Modelo

### 2.1 O que é uma "mina de estágio"

Extração **primária** (sem receita, sem insumo) de um produto que tem **jazida
rastreada** na cidade (`Deposit::tracked`): minerais, petróleo, gás, carvão,
urânio. Fazenda, algodão, madeira e indústria **não** têm estágio
(`mine_stage_factor = 1,0`). Implementado em
`SimCore::is_mine_building`/`SimCore::mine_stage_factor`
(`sim/production.cpp`).

### 2.2 Curva de declínio do estágio

Cada mina carrega, no `Building`:

| campo | significado |
| --- | --- |
| `stage` | profundidade de operação (0 = primeiro poço; `3` = último estágio das minas históricas) |
| `stage_reserve_kt` | reserva que o estágio corrente ainda pode extrair antes de cair ao piso |
| `stage_mined_kt` | o que o estágio já extraiu |

Produção relativa do estágio:

```text
fator = clamp((stage_reserve_kt − stage_mined_kt) / stage_reserve_kt, 0,05, 1,00)
```

- estágio cheio → **1,00**;
- decaimento **linear** com a reserva que ainda pode ser extraída;
- estágio esgotado (`stage_reserve_kt = 0` ou já minerado por inteiro) →
  **piso de 5%** (`kMineStageFloor`) — "quase nada", não zero: o poço continua
  existindo, com equipe mínima, até alguém pagar o aprofundamento.

O fator entra na produção efetiva (`phase_production`), na folha e no opex da
construção (`labor.cpp`, via `operating`) e dispensa o acúmulo de dias de
prejuízo (`sim_core.cpp`): mina parada no piso é **stand-by**, não falência.

**A reserva de referência da jazida escala o estágio, não o encerra.** Cada
estágio novo reivindica `kMineStageReserveShare` (= 50%) da reserva de
**referência** (`Deposit::initial_kt`) — não do saldo restante —, então a mina
sempre volta a 100% de produção ao abrir o estágio. A extração continua
debitando `Deposit::remaining_kt` (contabilidade do que já saiu) até zero e
`Deposit::extracted_kt` cresce sem limite; **ultrapassar a referência não zera
a produção, não encerra a mina e não bloqueia obra** — só encarece o próximo
aprofundamento (§2.4). A jazida nunca é publicada como esgotada
(`deposits[].depleted` = `false`; `depleted_mines` fica em 0).

### 2.3 Estado inicial (dia 0)

`GeographyService.resolve_initial_mines` acrescenta **uma mina por (cidade,
mineral com jazida atribuída pela regra A1)** — a mesma tabela que o mapa de
recursos desenha. Nenhuma jazida é inventada. Há **duas** regras, decididas
pelo dado de produção real (§7):

1. **(cidade, mineral) na semente de extração 2025** — a mina nasce
   **operando**, `"stage": 0`, com capacidade nominal = **produção real
   atribuída ÷ eficiência da jazida** (`0,5 + 0,5 × qualidade`, a mesma curva
   do núcleo), de modo que a produção **efetiva** do dia 0 seja a produção
   real atribuída. Dono: o declarado na semente (Estado/privado).
2. **sem dado** (regra histórica da decisão do dono, 2026-11-17) — a mina nasce
   com a construção **existente no último estágio**, produção no piso (5%), e
   só volta pagando o aprofundamento. Contrato: `"stage": -1` = "último
   estágio" (o número mora no núcleo: `kMineInitialStage = 3`),
   `"owner": "state"`; capacidade pela régua da mina estatal (demanda externa ×
   1,2, piso 2 t/dia, teto 10 t/dia).

- **dono:** Estado por padrão quando não há semente. `ownership_baseline.json`
  só declara setores estatais em texto livre para 12 nações — sem estrutura por
  setor, não se inventa uma repartição.

### 2.4 Aprofundar/expandir (obra §15)

Comando da fila A3 **`deepen_mine(city_id, building_id)`** (método
`EconomySim::deepen_mine`), aplicado em `action_deepen_mine`
(`sim/actions.cpp`):

- exige mina de estágio, mina não encerrada (falência/`loss_days`) e nenhuma
  obra de aprofundamento em voo para aquela mina. **Não existe mais bloqueio
  por reserva**: o recurso nunca acaba;
- abre o estágio `stage + 1`, soma
  `capacidade × (kMineDeepenCapacityFactor − 1)` (= +50%) à capacidade e, na
  conclusão, o estágio novo reivindica
  `kMineStageReserveShare` (= 50%) da reserva de **referência** da jazida,
  voltando a 100% de produção;
- **custo** (todos os parâmetros são constantes nomeadas em
  `sim_constants.h`):

  ```text
  custo = Δcapacidade × kCapitalPerCapacity
        × depth_opex_factor(jazida) ÷ extraction_efficiency(qualidade)
        × kMineDeepenStageCostFactor ^ (estágio novo)
        × (1 + kMineDeepenExtractionCostFactor
             × min(extracted_kt / reserva_de_referência,
                   kMineDeepenExtractionCostCap))
  ```

  - `kMineDeepenStageCostFactor = 1,25`: curva **geométrica e convexa** com o
    estágio — mais fundo é sempre mais caro, e o salto entre estágios cresce;
  - `kMineDeepenExtractionCostFactor = 0,5` e
    `kMineDeepenExtractionCostCap = 4,0`: a mina que já extraiu muito (relativo
    à reserva de referência) fica até **3×** mais cara, com **teto finito** —
    nunca infinito e nunca bloqueio. É o que faz a IA preferir uma mina/jazida
    nova em outro lugar quando esta fica profunda;
  - profundidade da jazida (`depth_opex_factor`) e minério pobre
    (`extraction_efficiency`) continuam encarecendo;
- **prazo**: `kMineDeepenBaseDays + kMineDeepenDaysPerStage × estágio novo` =
  `90 + 30 × estágio` dias (declarado, não observado);
- **conclusão**: soma a capacidade, **reescala folha e opex na mesma
  proporção** (a mina reaberta volta a operar cheia) e abre o estágio novo com
  50% da reserva de referência;
- **dono paga**: Estado → orçamento da cidade; grupo F9 → caixa do grupo;
  firma privada legada → caixa da firma. O capital é reservado no início (regra
  §15) e gasto conforme a obra avança; o **saldo não gasto volta ao dono** na
  conclusão (conservação de dinheiro, `refund_project_reserved`).

### 2.5 IA (governo e investidor privado)

- **Governo** (`ai_government.cpp`): para cada mineral com jazida e margem
  positiva, se já existe mina **do Estado**, avalia o **aprofundamento** por
  retorno — `t/dia adicionadas × (preço − custo variável)`, payback da obra
  ≤ **730 dias** e custo dentro da fatia de orçamento. **Comida primeiro:**
  cidade que não cobre a cesta não desvia orçamento para obra de mina. Sem
  mina, avalia **abrir** (mesmo payback). O teto fixo de **10 t/dia**
  (`kMaxStateMineCapacity`) fica **só** para minerais de nicho — ouro, diamante,
  terras raras, tungstênio, urânio (`is_niche_mineral`); os demais são
  dimensionados por demanda externa e orçamento.
- **Escolha entre minas do mesmo mineral:** havendo mais de uma mina elegível
  na cidade, a IA compara o **custo do próximo estágio** (`AiView::b_deepen_cost`,
  calculado pelo próprio núcleo) e escolhe a **mais barata** — sem regra
  especial por reserva. Com a curva exponencial, quando a mina local fica
  profunda o custo dela passa o de uma mina/jazida nova e a necessidade
  **migra pelo mapa** naturalmente.
- **Investidor privado** (`ai_private.cpp`): quando a cidade já tem produtor do
  mineral, a ação passa a ser **aprofundar** — e a decisão usa o retorno **da
  própria obra** (`capacidade adicionada × margem ÷ custo do aprofundamento`),
  não o da planta hipotética do template; o resto do gate (margem, payback,
  caixa) é o mesmo.
- Tudo determinístico: mesma ordem de minerais, mesmo desempate, sem relógio.

## 3. Contrato de status / consultas A4

Cada construção do bloco pesado (`get_city_status`, `get_status().cities[].buildings`)
publica:

| chave | tipo | significado |
| --- | --- | --- |
| `stage` | int | estágio de operação (0 em construção sem estágio) |
| `stage_factor` | float | produção relativa do estágio (0,05..1,0; 1,0 sem estágio) |
| `stage_reserve_kt` | float | reserva que o estágio ainda pode extrair |
| `stage_mined_kt` | float | reserva já extraída pelo estágio |
| `next_deepen_cost` | float | custo do **próximo** aprofundamento (0 só quando a construção não é mina ou está encerrada) |

O bloco `deposits` da cidade segue como antes (reserva restante, qualidade,
profundidade, eficiência, fator de opex), com **`depleted` sempre `false`** e
`remaining_kt`/`extracted_kt` como contabilidade da reserva de referência. A IA
em C++ lê o mesmo estágio pelo retrato do dia
(`AiView::b_stage`/`b_stage_factor`/`b_deepen_cost`/`d_quality`).

## 4. Save

**v18** (migração de v17; a cadeia v16 → v17 → v18 roda no mesmo load):

- `buildings[].stage`, `stage_reserve_kt`, `stage_mined_kt`;
- `construction_projects[].deepen_stage` (−1 = obra normal).

A B2 ocupa o **v17** (`pop.strata` + `rent_income_next`); as minas entram no
**v18** porque a branch da B3 em andamento também reivindicava um schema novo e
o merge foi feito com a B2 já na base (`f271a4a`). A migração é aditiva e olha
só as chaves que faltam, então um save **v16** sobe pela B2 (semeadura dos
quintis no fim do load) e em seguida cai no último estágio de mina; um save
**v17** só faz a segunda etapa. Save v17 e anteriores **não** têm as chaves de
estágio: toda construção de extração entra no **último estágio** com o estágio
esgotado (produção no piso) — a regra histórica. A partir da Parte 2 deste doc, o
mundo NOVO não nasce todo assim: a semeadura inicial escolhe o estágio pela
produção real de 2025 (§7). Nenhum outro campo muda de significado.
`building.depleted` continua no save por compatibilidade, mas é **ignorado**
(force `false` ao carregar): a depleção deixou de existir.

## 5. Testes

- `tools/mine_stage_test.gd`:
  - (a) aprofundar **nunca** é bloqueado, mesmo depois de extrair além da
    reserva de referência (a produção no piso continua e o próximo estágio só
    fica mais caro), e a jazida não é publicada como esgotada;
  - (b) o custo do estágio n+1 é maior que o do estágio n para vários n e o
    crescimento é **convexo** (Δ crescente), com o prazo crescendo 30 d/estágio;
  - (c) com duas minas do mesmo mineral e custos de próximo estágio diferentes,
    a IA (governo) aprofunda **a mais barata** — a ordem das construções no
    mundo é a adversa (a cara primeiro);
  - (d) dinheiro conservado; save v17 round-trip preserva estágio e custo;
    migração v16 cai no último estágio (piso);
  - (e) **semente pela produção real**: três pares reais (CAN-EDM/petróleo,
    CHN-XIAN/carvão, AUS-PER/bauxita) nascem no estágio 0 com capacidade
    `produção atribuída ÷ eficiência` (conferida contra a qualidade da jazida e
    o dono da semente), uma jazida sem dado nasce no último estágio, e no
    núcleo a mina `stage = 0` abre a reserva do estágio no primeiro
    `set_deposit` e produz 100% da capacidade no dia 0, enquanto a `stage = -1`
    fica no piso de 5%.
- Bateria: `economy_integration_test`, `economy_ai_test`,
  `population_demo_test`, `economy_light_signals_test`, `services_test`,
  `city_gov_test`, `construction_queue_test`, `urban_land_test`,
  `economy_save_load_v11_compat_test`.
- Mundo real 365 d com IA (`economy_world_test --ai=1 --urban=1 --threads=1`):
  falências **8** (limite 28 da bateria de sanidade), drift **0,0002**, nenhuma
  violação de invariante e **sem colapso de preço de minério** (preço no dia 365
  = ferro **1,84×**, cobre **1,68×**, ouro **1,67×** e bauxita **1,55×** o base).
- CLI × Godot com IA, 10 d, snapshots **byte-idênticos** (SHA-256
  `F03B6549F03281876DDAE37ADD8D3F034F1561C7B4062BF6C1A328CC3D8FF490`).

## 6. O que **não** está fechado (calibração futura)

- Quantidades: a **capacidade inicial** das minas com dado está ancorada na
  produção real de 2024 (§7); o que segue declarado é o estágio em si — fração
  da reserva de referência por estágio (0,5), fator de capacidade (+50%),
  prazos (90/30 dias), piso (5%) e fator geométrico de custo (1,25/estágio) com
  a modulação por extração (0,5 com teto 4,0). Também declarado: a repartição
  Estado/privado da semente (28 países em petróleo/gás + carvão CHN/IND) e a
  exclusão dos 208 pares sem depósito mapeado. O laboratório é o lugar de
  calibrá-los.
- A semente fixa o **dia 0**; a partir do dia 1 a produção de cada mina segue a
  demanda do mercado (`sold_prev`), então a participação de um país pode cair
  sem que a capacidade tenha mudado (ver cobre IDN no dia 30, §7.3). Calibrar
  a demanda/série é trabalho de outra frente, não da semente.
- A reserva real por estágio não usa ainda a profundidade para limitar quanto se
  pode descer (a reserva de referência é a escala; a profundidade só encarece).
- A UI (ficha da mina) ainda não mostra o botão de aprofundar; o contrato de
  status já publica custo e estágio.
- **Aberto (D-087):** com a semente, o sweep de 365 d dá **37 falências** de
  construção (limite do dono 28; sem a semente, 20) e o minério fica no **teto
  de 3×** do núcleo com e sem ela (§7.4). Os dois medem o mesmo ciclo — mina
  semeada grande, IA aprofundando **51.946** vezes em 365 d e preço preso no
  teto — e ficam para a próxima fatia de calibração; nada foi isentado.

## 7. Semente pela produção real (D-086b)

### 7.1 De onde vem cada número

Dois arquivos versionados, trazidos do commit `5ee6211` (branch de origem da
semente):

| arquivo | conteúdo |
| --- | --- |
| `data/definitions/natural/extraction_production_2024.json` | produção **real 2024 por país** e a citação de cada fonte |
| `data/definitions/natural/extraction_2025.json` | auditoria da atribuição: por país, produção real, participação mundial, t/dia do jogo, dono, cidades e capacidade de cada uma; lista `unallocated` |

O gerador é `tools/build_extraction_seed.py` (presente no commit `5ee6211`,
não versionado neste branch); ele lê `deposits.json`, `cities.json` e
`ai_templates.cpp` e não usa rede.

**Produção real (2024):**

| mineral | fonte | unidade bruta | conversão usada |
| --- | --- | --- | --- |
| petróleo | Energy Institute Statistical Review 2025 (dados 2024), via OWID (CC BY 4.0) | TWh | `kt = TWh × 86,0` (1 t = 41,868 GJ) |
| gás | idem | TWh | `kt = TWh × 70,5` (1 t = 51,0 GJ, gás seco) |
| carvão | idem | TWh | `kt = TWh × 180,0` (1 t = 20,0 GJ, carvão térmico médio) |
| ferro | USGS Mineral Commodity Summaries 2025 — Iron Ore (2024e) | milhões de t (usable ore) | `kt = Mt × 1000` |
| cobre | USGS MCS 2025 — Copper (2024e) | kt (conteúdo de cobre) | direto |
| bauxita | USGS MCS 2025 — Bauxite and Alumina (2024e) | milhares de t secas | direto |

As conversões TWh→kt são **declaradas** (poder calorífico médio), não medição
por país; a confiança do arquivo diz isso explicitamente
(`published_aggregate`).

**Alvo do jogo:** a produção real define a **participação** de cada país, não a
escala absoluta. A escala é `sold/dia` do mercado no **dia 365 do baseline
`090dbd1`** (IA + urbano, `tools/diag_extraction_market.gd`), que é o quanto o
modelo consome:

| mineral | alvo mundial (t/dia) |
| --- | --- |
| petróleo | 2.616,5 |
| gás | 1.196,3 |
| carvão | 258,5 |
| ferro | 18.278,9 |
| cobre | 3.726,3 |
| bauxita | 6.506,7 |

Regra de atribuição, por país e mineral:

```text
produção_jogo(pais) = (real_2024(pais) / real_2024_mundial) × alvo_mundial
```

O total do país é dividido entre as **cidades donas das jazidas reais** pela
regra A1 do `GeographyService.assign_deposits` (cidade da mesma nação mais
próxima do ponto do depósito, na mesma massa de terra), ponderada pela
**reserva recuperável** de cada cidade; o resíduo do rateio volta para a maior
cidade; cotas abaixo de 0,5 t/dia são dobradas na maior cidade (sem cidade com
cota viável, o país não gera construção). Nenhuma jazida é criada: onde não há
depósito mapeado, não há mina.

**O que a semente não cobre (declarado, não corrigido):**

- as tabelas do USGS listam os produtores principais, não o mundo inteiro:
  cobertura de 97,6% do ferro, 86,7% do cobre e 98,8% da bauxita (petróleo,
  gás e carvão vêm do EI com 100%);
- **208 pares (país, mineral) não têm depósito/cidade dona no cenário** e
  ficam em `unallocated`: petróleo 71 (241,6 t/dia), gás 83 (380,7), carvão 52
  (21,1), bauxita 2 (144,6). Exemplos de peso: gás CHN (71,6 t/dia), bauxita
  SAU (83,9), petróleo QAT (43,3). Eles mantêm a regra histórica (mina no
  último estágio, piso), então o jogo **subestima** esses produtores — é a
  alternativa honesta a inventar uma jazida.
- **7 pares têm cota mas nenhuma cidade** e, no arquivo, aparecem com
  `game_t_day > 0` e `cities: []` **fora** de `unallocated` (carvão BRA, GBR,
  IRN, MEX, SRB; petróleo FRA, SUR) — todos abaixo de 0,5 t/dia, somando
  ~1,36 t/dia (0,005% do mundo). Como a cota abaixo do mínimo não gera
  construção, o efeito no jogo é nulo; é inconsistência de **contabilidade do
  arquivo** (o gerador não os moveu para `unallocated`), registrada aqui em vez
  de reescrever o JSON à mão sem o gerador versionado (§7.1, primeiro
  parágrafo).

No núcleo, a capacidade nominal da construção é
`produção atribuída ÷ (0,5 + 0,5 × qualidade)`, porque a produção efetiva é
`capacidade × eficiência × fator do estágio × mão de obra`. O dono vem da
tabela declarada no gerador: Estado para petróleo/gás em 28 países produtores
estatais e carvão CHN/IND; privado nos demais. Essa repartição é **premissa
declarada**, não um dado por empresa.

### 7.2 Por que o estágio inicial é 0

O modelo **não relaciona o número do estágio à capacidade nem à profundidade
da jazida**. O fator de produção é `1,0` em qualquer estágio recém-aberto
(`stage_reserve_kt > 0`, `stage_mined_kt = 0`); o número do estágio só desloca
a escada do **próximo** aprofundamento (`1,25^(estágio+1)` no custo e
`90 + 30 × (estágio+1)` dias) e a fatia de 50% da reserva de referência. A
escala de produção vem da **capacidade**, e é ela que a semente calibra.

Portanto: mina com dado nasce no **estágio 0** (primeiro poço, operando) e a
produção do dia 0 é a produção real atribuída; mina sem dado nasce no **último
estágio** (`3`) com o estágio esgotado (piso de 5%). Se um modelo futuro ligar
estágio a profundidade/capacidade, a semente deve escolher o estágio por essa
relação — hoje não existe, e inventar limiares seria número sem fonte.

Consequência documentada: o próximo aprofundamento de uma mina semeada é o
primeiro degrau da escada (mais barato e mais curto que o de uma mina
histórica), mas o custo continua proporcional à capacidade
(`Δcapacidade × 2.000 × depth ÷ qualidade`), então o grande produtor paga
proporcionalmente mais para crescer.

### 7.3 Medição: participação real × simulada

Medido no worktree `opengs-wt-mine` com o mundo real (834 cidades, IA e urbano
ligados como no `world_equivalence`) em **18/11/2026**, DLL debug do próprio
worktree. A produção por construção **não** sai em `get_status()` (S7-c: o
status diário não publica construções); a medição usa `get_city_status` por
cidade e soma `capacidade × utilização`, que é exatamente a saída do dia.
"dia 0" = retrato depois do 1º tick; "dia 30" = depois de 30 ticks.

- **447** minas semeadas em operação (todos os 447 pares da semente com
  depósito A1; **0** pares sem jazida);
- **1.020** minas publicadas no total; **573** no piso — exatamente as
  históricas, nenhuma semeada caiu ao piso até o dia 30;
- **418/447** construções semeadas produziram no dia 0 dentro de **1%** do
  alvo atribuído; **427/447** dentro de **5%**. Os 29 restantes são limitados
  por **mão de obra da cidade** no dia 0 (a mesma razão aparece em todos os
  minerais da mesma cidade), não por erro de capacidade: piores casos
  AUS-KAR ferro `0,190`, AUS-WEI bauxita `0,202`, CAN-KUU ferro `0,469` e
  AUS-BRO ferro `0,591` do alvo.

Totais mundiais (t/dia):

| mineral | alvo do jogo | atribuído à semente | sim dia 0 | sim dia 30 |
| --- | --- | --- | --- | --- |
| petróleo | 2.616,5 | 2.374,1 | 2.350,5 | 2.372,3 |
| gás | 1.196,3 | 815,6 | 786,4 | 785,3 |
| carvão | 258,5 | 236,8 | 242,1 | 240,8 |
| ferro | 18.278,9 | 17.832,9 | 15.444,0 | 15.139,9 |
| cobre | 3.726,3 | 3.230,5 | 3.281,8 | 3.194,4 |
| bauxita | 6.506,7 | 6.285,5 | 5.993,5 | 5.922,2 |

Leitura: o dia 0 fecha com o **atribuído** (diferença só de mão de obra, ≤3,6%
no mundo); o dia 30 já não mede a semente, e sim a demanda — a produção segue
`sold_prev` do mercado, então países cuja demanda interna/exportação é menor
que a cota recuam (ex.: cobre IDN 178,2 → 93,9 t/dia; ferro AUS 4.418 → 4.077).
A diferença entre alvo e atribuído é a cobertura da fonte (ferro/cobre/bauxita)
e os pares sem depósito mapeado (§7.1).

#### Petróleo — 10 maiores produtores reais (2024)

| # | país | real kt/ano | share real | atribuído t/d | sim d0 t/d | share d0 | d0/atrib. | sim d30 t/d | share d30 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | USA | 863.056,7 | 18,93% | 495,32 | 470,81 | 20,03% | 0,951 | 495,32 | 20,88% |
| 2 | RUS | 526.681,7 | 11,55% | 302,27 | 302,27 | 12,86% | 1,000 | 302,27 | 12,74% |
| 3 | SAU | 510.255,2 | 11,19% | 292,84 | 292,84 | 12,46% | 1,000 | 292,84 | 12,34% |
| 4 | CAN | 290.217,8 | 6,37% | 166,56 | 166,56 | 7,09% | 1,000 | 166,56 | 7,02% |
| 5 | IRN | 236.877,5 | 5,20% | 135,95 | 135,95 | 5,78% | 1,000 | 135,95 | 5,73% |
| 6 | CHN | 212.929,3 | 4,67% | 122,20 | 122,20 | 5,20% | 1,000 | 122,20 | 5,15% |
| 7 | IRQ | 210.694,4 | 4,62% | 120,92 | 120,92 | 5,14% | 1,000 | 120,04 | 5,06% |
| 8 | BRA | 181.705,3 | 3,99% | 104,28 | 104,28 | 4,44% | 1,000 | 104,28 | 4,40% |
| 9 | ARE | 179.782,6 | 3,94% | 103,18 | 103,18 | 4,39% | 1,000 | 103,18 | 4,35% |
| 10 | KWT | 131.981,7 | 2,89% | 75,75 | 75,75 | 3,22% | 1,000 | 75,75 | 3,19% |

#### Gás natural — 10 maiores produtores reais (2024)

| # | país | real kt/ano | share real | atribuído t/d | sim d0 t/d | share d0 | d0/atrib. | sim d30 t/d | share d30 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | USA | 727.464,5 | 24,86% | 297,35 | 297,35 | 37,81% | 1,000 | 297,35 | 37,87% |
| 2 | RUS | 444.054,0 | 15,17% | 181,51 | 181,51 | 23,08% | 1,000 | 181,51 | 23,11% |
| 3 | IRN | 184.853,4 | 6,32% | 75,56 | 75,56 | 9,61% | 1,000 | 75,56 | 9,62% |
| 4 | CHN | 175.114,7 | 5,98% | 0,00 | 0,00 | 0,00% | — | 0,00 | 0,00% |
| 5 | CAN | 140.626,9 | 4,80% | 57,48 | 57,48 | 7,31% | 1,000 | 57,48 | 7,32% |
| 6 | QAT | 126.690,9 | 4,33% | 51,78 | 51,78 | 6,59% | 1,000 | 51,78 | 6,59% |
| 7 | AUS | 107.366,6 | 3,67% | 43,89 | 13,36 | 1,70% | 0,304 | 12,34 | 1,57% |
| 8 | NOR | 87.603,7 | 2,99% | 35,81 | 35,81 | 4,55% | 1,000 | 35,81 | 4,56% |
| 9 | SAU | 87.115,7 | 2,98% | 0,00 | 0,00 | 0,00% | — | 0,00 | 0,00% |
| 10 | DZA | 70.635,6 | 2,41% | 28,87 | 28,87 | 3,67% | 1,000 | 28,87 | 3,68% |

CHN e SAU não têm depósito de gás mapeado no cenário (§7.1); AUS é limitado por
mão de obra no dia 0.

#### Carvão — 10 maiores produtores reais (2024)

| # | país | real kt/ano | share real | atribuído t/d | sim d0 t/d | share d0 | d0/atrib. | sim d30 t/d | share d30 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | CHN | 4.664.926,8 | 51,65% | 133,51 | 134,36 | 55,49% | 1,006 | 134,36 | 55,79% |
| 2 | IND | 898.231,7 | 9,94% | 25,71 | 25,71 | 10,62% | 1,000 | 25,36 | 10,53% |
| 3 | IDN | 848.548,8 | 9,39% | 24,29 | 24,29 | 10,03% | 1,000 | 24,29 | 10,08% |
| 4 | AUS | 585.449,9 | 6,48% | 16,76 | 16,76 | 6,92% | 1,000 | 16,76 | 6,96% |
| 5 | USA | 530.155,8 | 5,87% | 15,17 | 16,82 | 6,95% | 1,109 | 15,87 | 6,59% |
| 6 | RUS | 455.970,1 | 5,05% | 13,05 | 13,05 | 5,39% | 1,000 | 13,05 | 5,42% |
| 7 | ZAF | 279.950,7 | 3,10% | 0,00 | 0,00 | 0,00% | — | 0,00 | 0,00% |
| 8 | MNG | 101.479,9 | 1,12% | 0,00 | 0,00 | 0,00% | — | 0,00 | 0,00% |
| 9 | KAZ | 95.921,5 | 1,06% | 2,75 | 2,74 | 1,13% | 1,000 | 2,74 | 1,14% |
| 10 | COL | 82.709,2 | 0,92% | 2,37 | 2,37 | 0,98% | 1,000 | 2,37 | 0,98% |

ZAF e MNG não têm depósito de carvão mapeado (§7.1). USA no dia 0 ficou 10,9%
acima porque a razão trabalhadores/vagas da construção passou de 1,0 nesse dia
(arredondamento de vagas), não por capacidade extra; os demais desvios acima de
1,000 são da mesma ordem (≤0,9%).

#### Ferro — 10 maiores produtores reais (2024)

| # | país | real kt/ano | share real | atribuído t/d | sim d0 t/d | share d0 | d0/atrib. | sim d30 t/d | share d30 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | AUS | 930.000,0 | 38,13% | 6.799,75 | 4.418,16 | 28,61% | 0,650 | 4.077,45 | 26,93% |
| 2 | BRA | 440.000,0 | 18,04% | 3.217,09 | 3.217,09 | 20,83% | 1,000 | 3.217,07 | 21,25% |
| 3 | CHN | 270.000,0 | 11,07% | 1.974,12 | 1.974,12 | 12,78% | 1,000 | 1.974,11 | 13,04% |
| 4 | IND | 270.000,0 | 11,07% | 1.974,12 | 1.974,12 | 12,78% | 1,000 | 1.947,42 | 12,86% |
| 5 | RUS | 91.000,0 | 3,73% | 665,35 | 665,35 | 4,31% | 1,000 | 665,35 | 4,39% |
| 6 | IRN | 90.000,0 | 3,69% | 658,04 | 658,04 | 4,26% | 1,000 | 658,03 | 4,35% |
| 7 | ZAF | 66.000,0 | 2,71% | 482,56 | 482,56 | 3,12% | 1,000 | 481,56 | 3,18% |
| 8 | CAN | 54.000,0 | 2,21% | 394,82 | 329,62 | 2,13% | 0,835 | 378,28 | 2,50% |
| 9 | USA | 48.000,0 | 1,97% | 350,95 | 334,46 | 2,17% | 0,953 | 352,64 | 2,33% |
| 10 | UKR | 42.000,0 | 1,72% | 307,09 | 307,09 | 1,99% | 1,000 | 307,08 | 2,03% |

AUS é o caso de mão de obra (AUS-KAR, a maior mina, produz 19% do alvo no dia 0
e puxa a média do país); CAN por CAN-KUU (47%).

#### Cobre — 10 maiores produtores reais (2024)

| # | país | real kt/ano | share real | atribuído t/d | sim d0 t/d | share d0 | d0/atrib. | sim d30 t/d | share d30 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | CHL | 5.300,0 | 26,58% | 858,67 | 859,12 | 26,18% | 1,001 | 858,94 | 26,89% |
| 2 | COD | 3.300,0 | 16,55% | 534,64 | 523,80 | 15,96% | 0,980 | 517,06 | 16,19% |
| 3 | PER | 2.600,0 | 13,04% | 421,23 | 421,23 | 12,84% | 1,000 | 421,12 | 13,18% |
| 4 | CHN | 1.800,0 | 9,03% | 291,62 | 291,62 | 8,89% | 1,000 | 291,50 | 9,13% |
| 5 | IDN | 1.100,0 | 5,52% | 178,21 | 178,21 | 5,43% | 1,000 | 93,90 | 2,94% |
| 6 | USA | 1.100,0 | 5,52% | 178,21 | 178,72 | 5,45% | 1,003 | 179,27 | 5,61% |
| 7 | RUS | 930,0 | 4,66% | 150,67 | 150,99 | 4,60% | 1,002 | 150,97 | 4,73% |
| 8 | AUS | 800,0 | 4,01% | 129,61 | 112,62 | 3,43% | 0,869 | 118,56 | 3,71% |
| 9 | KAZ | 740,0 | 3,71% | 119,89 | 119,89 | 3,65% | 1,000 | 119,81 | 3,75% |
| 10 | MEX | 700,0 | 3,51% | 113,41 | 113,41 | 3,46% | 1,000 | 113,39 | 3,55% |

IDN no dia 30 cai à metade porque a demanda absorvida é menor, não porque a
semente mudou.

#### Bauxita — 10 maiores produtores reais (2024)

| # | país | real kt/ano | share real | atribuído t/d | sim d0 t/d | share d0 | d0/atrib. | sim d30 t/d | share d30 |
|---|---|---|---|---|---|---|---|---|---|
| 1 | GIN | 130.000,0 | 29,23% | 1.879,71 | 1.862,34 | 31,07% | 0,991 | 1.832,74 | 30,95% |
| 2 | AUS | 100.000,0 | 22,49% | 1.445,93 | 1.110,98 | 18,54% | 0,768 | 1.075,18 | 18,16% |
| 3 | CHN | 93.000,0 | 20,91% | 1.344,72 | 1.344,72 | 22,44% | 1,000 | 1.344,69 | 22,71% |
| 4 | BRA | 33.000,0 | 7,42% | 477,16 | 477,16 | 7,96% | 1,000 | 477,15 | 8,06% |
| 5 | IDN | 32.000,0 | 7,20% | 462,70 | 462,70 | 7,72% | 1,000 | 462,67 | 7,81% |
| 6 | IND | 25.000,0 | 5,62% | 361,48 | 361,48 | 6,03% | 1,000 | 356,59 | 6,02% |
| 7 | RUS | 6.300,0 | 1,42% | 91,09 | 91,48 | 1,53% | 1,004 | 91,48 | 1,54% |
| 8 | JAM | 6.100,0 | 1,37% | 88,20 | 88,20 | 1,47% | 1,000 | 87,50 | 1,48% |
| 9 | SAU | 5.800,0 | 1,30% | 0,00 | 0,00 | 0,00% | — | 0,00 | 0,00% |
| 10 | KAZ | 4.900,0 | 1,10% | 70,85 | 70,85 | 1,18% | 1,000 | 70,85 | 1,20% |

SAU não tem depósito de bauxita mapeado (§7.1). AUS-WEI produz 20% do alvo no
dia 0 (mão de obra), o que derruba a participação do país.

> **Honestidade do dado:** os números de "sim d0/d30" são produção do modelo
> (capacidade × utilização), não uma série histórica; "real" é produção de
> 2024 publicada (USGS/EI) e "atribuído" é a conversão declarada de §7.1. As
> diferenças de participação no dia 30 são em grande parte demanda do modelo, e
> não erro de semeadura — por isso a tabela traz as duas colunas.

### 7.4 Efeito nos goldens, no CLI e no mundo de 365 d

**Goldens A0/S0 (regerados em 18/11/2026, DLL debug deste worktree).** A
diferença acumulada tem **duas** causas, e as duas já reprovavam os goldens
antigos: o fix B2 (`ec73719`, que corrigiu a classe publicada e a proxy dos
quintis **sem** regerar os goldens na época) e esta semente (`1288d4c`).

| golden | save_sha256 | status_sha256 | antes (save / status) |
| --- | --- | --- | --- |
| `A0_60_urban` (60 d, IA + urbano) | `700c8481…` | `357755a3…` | `84d28700…` / `5f91d311…` |
| `real_60_ai` (S0, 60 d, IA) | `2cfabec1…` | `96b9489c…` | `d64df2a1…` / `fc560221…` |
| `real_60_noai` (S0, 60 d, sem IA) | `1739e53e…` | `f2c81576…` | `7070ff67…` / `bb88d8bb…` |
| `demo_365_noai` (AGRIA/PORTUS) | `daf195c5…` | `489422ae…` | `a9dbd376…` / `ab317791…` |

Determinismo: o A0 com duas rodadas idênticas e os três S0 com uma segunda
rodada `--baseline` (0 diferenças nos quatro). O `demo_365_noai` muda **só**
pelo fix B2: o harness demo monta AGRIA/PORTUS com `initialize_scenario` e não
chama `resolve_initial_mines`, então a semente não o alcança.

**CLI × Godot com IA (10 d, 834 cidades, 3.186 construções):** os snapshots
finais são **byte-idênticos** (SHA-256 `B2983EEBD3B6…`, antes `F03B6549F032…` —
muda porque as 447 minas agora nascem operando).

**365 d real + IA (`--ai=1 --urban=1 --threads=1`):** drift de dinheiro
**0,0003 XGS** e comida por nação **dentro** do portão de 30 d; **falências 37
(limite 28) — REPROVADO**. O A/B com `extraction_2025.json` ausente (renomeado
durante a corrida e restaurado) dá **20 falências**: o resto do branch responde
pela subida 8 (D-086, antes do fix B2) → 20, e a semente por **+17**. Evidência:
`docs/economy/perf/mine_seed_world_ai_365.json` (com) e
`docs/economy/perf/mine_seed_world_ai_365_noseed.json` (sem). Não há isenção: o
gate do dono não fecha com a semente como está.

**Preço de minério no dia 365** (média ponderada pela quantidade vendida das
834 cidades, contra o `base_price` do catálogo; medido com
`tools/ore_price_365.gd`, que reproduz a tabela — no dia 1 ele dá 1,05× em
todos e os volumes batem com a coluna "sim dia 0" de §7.3):

| mineral | com a semente | sem a semente |
| --- | --- | --- |
| petróleo | 3,000× | 3,000× |
| gás | 3,000× | 3,000× |
| carvão | 3,000× | 3,000× |
| ferro | 3,000× | 3,000× |
| cobre | 2,754× | 2,679× |
| bauxita | 3,000× | 3,000× |

Leitura: **não é colapso de preço, é o contrário** — os minerais estão no
**teto de 3×** do núcleo **com e sem** a semente, então o teto **não** é efeito
desta fase (a D-086 media 1,84× o ferro antes do fix B2; a mudança veio com o
resto do branch, sem isenção nova). O que a semente muda é o **volume**: com
ela o ferro vendido vai a **20.215 t/dia** contra 10.939 e a bauxita a **9.348**
contra 5.637, com o preço já preso no teto. Aumentar a oferta não derruba o
preço enquanto o teto estiver saturado: isso é a frente de preços, não a
semente.

### 7.5 Correção da t18: régua da mina e folha pelo uso (D-086b)

A §7.4 fechou o mundo com **37 falências** (limite 28) e atribuiu **+17** à
semente. A t18 separou o sintoma em duas causas no mesmo lugar — a mina
semeada — e corrigiu as duas.

**Diagnóstico (365 d real + IA, mesmo mundo, A/B só da semente).**

- **37 = 20 AUS + 17** no resto do mundo; sem a semente, **20** (1 AUS + 19).
- Por causa: com a semente **23 salário/folha** + 13 demanda/ociosidade + 1
  demanda sem receita; sem ela, 17 demanda/ociosidade + 3 salário/folha.
- **15 minas semeadas** fecharam, todas na AUS e todas por **salário/folha**;
  no fechamento, as que ainda operavam rodavam a **0,136–0,182** de utilização
  (~0,15) e as paradas a ~0.

Duas falhas compunham o quadro:

1. **Régua errada.** A firma semeada herdava a régua da fazenda
   (`7,5 empregos/t`, salário `2,0`, opex `2,5`) em vez da régua da mina da
   tabela que a própria IA usa (`core/economy/ai/industry_templates.gd`):
   ferro `3,35`/`1,0`, bauxita `2,0`/`1,0`, carvão `1,5`/`1,0`, gás `0,6`/`1,5`.
   Em folha por t/dia isso é `7,5×2,0 = 15` contra `3,35`, `2,0`, `1,5` e `0,9`
   — **4,5× no ferro, 7,5× na bauxita, 10× no carvão e 16,7× no gás**.
2. **Folha sem a venda.** Folha e opex seguiam só o fator de **estágio**
   (D-086): uma mina no estágio 0 pagava a equipe cheia mesmo operando a 15%
   da capacidade. A produção já era limitada pela venda; o custo não.

**Correção (arquivos e regra).**

- `core/economy/geography_service.gd`: a mina semeada leva `jobs_per_ton`,
  `wage` e `opex_per_ton` da tabela da mina; sem entrada na tabela, cai no
  caminho antigo. Novo parâmetro `use_seed: bool = true` em
  `resolve_initial_mines` para o A/B (diagnóstico só; o jogo usa o padrão).
- `gdextension/src/economy_sim.cpp`: `initialize_world` usa os três campos
  quando presentes; ausentes, `kJobsPerTon`/`kFarmWage`/`kOpexPerTon` como
  antes (fazendas e minas históricas intactas).
- `gdextension/src/sim/labor.cpp`: a taxa de operação da mina passa a
  `min(fator de estágio, last_utilization)`, com o piso `kMineStageFloor` —
  nos três pontos (folha, opex por cidade e `labor_day`).

**Resultado (365 d real + IA, `--ai=1 --urban=1 --threads=1`).** **24
falências** (5 na AUS) contra limite 28; **2 semeadas** (AUS-BRO bauxita a
0,144 e AUS-WEI bauxita a 0,156, ambas por folha). Sem a semente, o mesmo mundo
dá **17**, então a semente agora custa **+7** em vez de +17. As 2 semeadas que
ainda fecham são o resíduo para a calibração; não são escondidas nem isentadas.
Evidência: `tools/_tmp_diag365_fix2.json` (com) e
`tools/_tmp_diag365_fix2_noseed.json` (sem) — artefatos locais da t18.

**Goldens (regerados neste worktree).** A correção muda o mundo já no dia 1
(a mina nasce com outra folha), então os dois goldens que contêm semente mudam:

| golden | save_sha256 | status_sha256 | §7.4 (antes) |
| --- | --- | --- | --- |
| `A0_60_urban` (60 d, IA + urbano) | `c395f293.` | `bccab3eb.` | `700c8481.` / `357755a3.` |
| `real_60_ai` (S0, 60 d, IA) | `3ea1f783.` | `e6026b4d.` | `2cfabec1.` / `96b9489c.` |

**Na base (`balance/global-resources`, merge `4ac1d12`).** Os goldens foram
regerados aqui, onde a B3 já estava: `A0_60_urban` `save 50373b42.` /
`status 769fd73f.` (antes `ff6855a0.` / `3b8749b6.`) e `real_60_ai`
`55d1f8ad.` / `53c6ffcd.` (antes `da665be8.` / `652f965d.`), duas rodadas
idênticas em cada; `real_60_noai` e `demo_365_noai` saíram byte-idênticos aos
da base. CLI × Godot 10 d (IA, `threads=1`, 849 cidades): **byte-idênticos**
(`BED6288E.`). 365 d real + IA na base: **0 falências** (teto 28), drift
≈ **−0,001** XGS e comida na dívida da B3 — **141 nações** abaixo de 0,975 na
média de 30 d (pior **0,000**), sem isenção nova.

**Preço de minério — questão de mercado para a calibração.** O teto de 3× da
§7.4 não é efeito da semente. A pressão de preço (`prices.cpp`) é
`(desired − latent) / max(stock + supply, 0,001)` e a produção
(`production.cpp`) soma `desired` no alvo a plena capacidade da usina, antes de
saber se há estoque/insumo. No A0 do dia 60, a soma de `m_desired` do **ferro**
nas 834 cidades é **20.821.206,7 t/dia** contra ~17,8 mil t/dia de capacidade
de mina: a escassez satura o clamp de 3× com **qualquer** oferta de mina, e por
isso o ferro fica em 3,000× com e sem a semente. Corrigir isso é mudar a regra
de demanda desejada/pressão (contar só o que a usina consegue comprar e
processar, ou pressionar por vendas), não a semeadura — fica registrado para a
fase de calibração.

**Sobre a lista de 8 do `77a452f`.** Aquele diagnóstico listou 8 minas; não foi
possível reproduzir a lista com a API antiga, então ela é dispensada. O A/B por
`use_seed` agora mede 15 minas semeadas falidas, todas na AUS.
