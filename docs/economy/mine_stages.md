# Estágio de mina — aprofundar/expandir (D-083)

> **Estado:** mecânica implementada e testada; **quantidades não calibradas**
> (decisão do dono, 2026-11-17). O aceite desta fase é de **mecânica e
> invariantes**, não de produção real.
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
recursos desenha. Nenhuma jazida é inventada.

- contrato de entrada: `"stage": -1` = "último estágio" (o número mora no
  núcleo: `kMineInitialStage = 3`), `"owner": "state"`;
- capacidade nominal: a mesma régua da mina estatal (demanda externa × 1,2,
  piso 2 t/dia, teto 10 t/dia). A produção efetiva fica no piso do último
  estágio;
- **dono:** Estado por padrão. `ownership_baseline.json` só declara setores
  estatais em texto livre para 12 nações — sem estrutura por setor, não se
  inventa uma repartição.

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

**v17** (migração de v16) — **o formato não muda nesta revisão**:

- `buildings[].stage`, `stage_reserve_kt`, `stage_mined_kt`;
- `construction_projects[].deepen_stage` (−1 = obra normal).

Save v16 e anteriores **não** têm as chaves: toda construção de extração entra
no **último estágio** com o estágio esgotado (produção no piso) — exatamente o
mundo que a decisão do dono descreve. Nenhum outro campo muda de significado.
`building.depleted` continua no save por compatibilidade, mas é **ignorado**
(force `false` ao carregar): a depleção deixou de existir.

> **Nota de merge:** a B2 (`feat/b2-strata`) também usa o save v17. A
> renumeração/rebase para v18 fica para o merge — não foi feita aqui.

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
    migração v16 cai no último estágio (piso).
- Bateria: `economy_integration_test`, `economy_ai_test`,
  `population_demo_test`, `economy_light_signals_test`, `services_test`,
  `city_gov_test`, `construction_queue_test`, `urban_land_test`,
  `economy_save_load_v11_compat_test`.
- Mundo real 365 d com IA (`economy_world_test --ai=1 --urban=1 --threads=1`):
  falências **8** (limite 28, igual ao baseline D-083), drift **0,0002**, nenhuma
  violação de invariante e **sem colapso de preço de minério** (preço no dia 365
  = ferro **1,84×**, cobre **1,68×**, ouro **1,67×** e bauxita **1,55×** o base).
- CLI × Godot com IA, 10 d, snapshots **byte-idênticos** (SHA-256
  `F03B6549F03281876DDAE37ADD8D3F034F1561C7B4062BF6C1A328CC3D8FF490`).

## 6. O que **não** está fechado (calibração futura)

- Quantidades: capacidade das minas históricas, fração da reserva de referência
  por estágio (0,5), fator de capacidade (+50%), prazos (90/30 dias), piso (5%),
  fator geométrico de custo (1,25/estágio) e modulação por extração
  (0,5 com teto 4,0) são **parâmetros declarados**, não medições — o laboratório
  é o lugar de calibrá-los.
- A reserva real por estágio não usa ainda a profundidade para limitar quanto se
  pode descer (a reserva de referência é a escala; a profundidade só encarece).
- A UI (ficha da mina) ainda não mostra o botão de aprofundar; o contrato de
  status já publica custo e estágio.
