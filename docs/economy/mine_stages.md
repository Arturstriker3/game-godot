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

**A reserva da jazida continua sendo a única fonte física.** Cada tonelada
extraída debita `Deposit::remaining_kt` **e** `stage_mined_kt`; o estágio só
distribui o ritmo no tempo. Reserva zerada → mina encerrada (`depleted`), sem
falência, e nenhum aprofundamento é aceito.

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
`EconomySim::deepen_mine`), aplicado em
`action_deepen_mine` (`sim/actions.cpp`):

- exige mina de estágio, **reserva restante > 0**, mina não encerrada e nenhuma
  obra de aprofundamento em voo para aquela mina;
- abre o estágio `stage + 1`, soma
  `capacidade × (kMineDeepenCapacityFactor − 1)` (= +50%) à capacidade e, na
  conclusão, o estágio novo reivindica
  `kMineStageReserveShare` (= 50%) da reserva **restante** da jazida, voltando a
  100% de produção enquanto ela durar;
- **custo**: `capacidade nova × kCapitalPerCapacity × depth_opex_factor(jazida)
  ÷ extraction_efficiency(qualidade) × (1 + 0,35 × estágio novo)`. Mais fundo
  (profundidade) e minério mais pobre (qualidade) encarecem; o 2º
  aprofundamento custa 1,35× o 1º;
- **prazo**: `90 + 30 × estágio novo` dias (declarado, não observado);
- **conclusão**: soma a capacidade, **reescala folha e opex na mesma
  proporção** (a mina reaberta volta a operar cheia) e abre o estágio novo com
  50% da reserva restante;
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
| `next_deepen_cost` | float | custo do **próximo** aprofundamento (0 = indisponível: sem reserva/mine encerrada) |

O bloco `deposits` da cidade segue como antes (reserva restante, qualidade,
profundidade, eficiência, fator de opex). A IA em C++ lê o mesmo estágio pelo
retrato do dia (`AiView::b_stage`/`b_stage_factor`/`d_quality`).

## 4. Save

**v17** (migração de v16):

- `buildings[].stage`, `stage_reserve_kt`, `stage_mined_kt`;
- `construction_projects[].deepen_stage` (−1 = obra normal).

Save v16 e anteriores **não** têm as chaves: toda construção de extração entra
no **último estágio** com o estágio esgotado (produção no piso) — exatamente o
mundo que a decisão do dono descreve. Nenhum outro campo muda de significado.

## 5. Testes

- `tools/mine_stage_test.gd` (novo):
  - (a) mina inicial no último estágio produz no piso (0,5 t/dia de 10 t/dia);
  - (b) aprofundar custa o valor publicado, leva `90 + 30 × estágio` dias,
    devolve a produção (capacidade 10 → 15 t/dia) e o orçamento do dono é
    debitado;
  - (c) o 2º aprofundamento custa mais que o 1º e demora mais;
  - (d) reserva esgotada bloqueia (`"reserva esgotada"`);
  - (e) dinheiro conservado do início ao fim;
  - save v17 (round-trip) e migração de v16 para o último estágio.
- Bateria existente: `economy_integration_test` (schema 17),
  `economy_ai_test`, `population_demo_test`, `economy_light_signals_test`.
- `tools/economy_world_test.gd` passa a semear as minas iniciais (mesmo caminho
  do autoload), então o sweep de 365 d exercita a mecânica.

## 6. O que **não** está fechado (calibração futura)

- Quantidades: capacidade das minas históricas, fração da reserva por estágio
  (0,5), fator de capacidade (+50%), prazos (90/30 dias), piso (5%) e
  crescimento de custo (0,35/estágio) são **parâmetros declarados**, não
  medições — o laboratório é o lugar de calibrá-los.
- Reserva real por estágio não usa ainda a profundidade para limitar quanto se
  pode descer (a reserva é a mesma; a profundidade só encarece).
- A UI (ficha da mina) ainda não mostra o botão de aprofundar; o contrato de
  status já publica custo e estágio.
