# Plano de refatoração do núcleo econômico C++ — simulação forte, modular, paralela e determinística

> **Documento de planejamento.** Nada aqui foi implementado. Nenhum arquivo de jogo, dado ou
> núcleo C++ foi alterado na elaboração deste plano, nenhuma cena foi aberta e nada foi
> commitado.
>
> Fontes de verdade usadas: `gdextension/src/economy_sim.cpp` (13.735 linhas),
> `gdextension/src/economy_sim.h` (2.178 linhas), `gdextension/CMakeLists.txt`,
> `core/economy/tick_scheduler.gd`, `core/economy/economy.gd`, `tools/perf_day_peak.gd`,
> `tools/world_scale_bench.gd`, `tools/economy_world_test.gd`, `tools/economy_tick_async_test.gd`,
> `docs/economy/perf/f9_invest_perf_release.txt`, `docs/economy/perf/f9_invest_fix_release_run1.txt`,
> `docs/economy/perf/f2_perf_release_after.txt`, `GAME_STATE.md` §13/§14/§15.
>
> **Decisões do dono respeitadas:** dividir em módulos; C++ puro no caminho quente (tipos Godot só
> na fronteira); paralelismo multi-core com resultado determinístico; SoA onde o profiler mostrar
> ganho; medição contínua por fase em debug/dev com custo zero em release; cadência escalonada só
> para processos lentos (com modo "tudo diário" para teste). **Fora de escopo agora:** mercados
> esparsos e LOD/agregação de nações.

---

## 0. Resumo executivo (o que este plano muda de fato)

1. O arquivo não é "13 mil linhas de simulação": são **~9.900 linhas de simulação** (tick, status,
   save, setores) + **~520 linhas de um modelo legado morto** (mundo de 1 cidade por nação) +
   **~630 linhas de um benchmark sintético** que duplica produção/consumo/comércio em miniatura.
   Removê-los é o passo de menor risco e maior redução de superfície.
2. O custo do tick hoje **não é matemática econômica**: `status` consome 40–50% do dia
   (`status` p95 24–27 ms de um tick p95 55–58 ms, F9), e `trade` + `arrival` somam mais 7–14 ms.
   `produced`/`income`/`prices` ficam em **0,3–1,1 ms**. Portanto: primeiro separar e medir,
   depois otimizar o que o profiler apontar — e o maior ganho de curto prazo está no
   **status**, não em SoA.
3. "Todas as nações e cidades em detalhe" **já é o estado atual** no C++: 834 cidades e 179
   nações rodam por inteiro todo dia, sem LOD (não há agregação no núcleo; a fatiagem existe
   apenas na IA em GDScript e no status). A refatoração não deve introduzir LOD: deve **abrir
   espaço** (orçamento de tick) para que o detalhe cresça por cidade (educação, demografia,
   firmas) sem estourar o frame.
4. O plano é uma **partição mecânica com verificação byte a byte** em cada passo, seguida de duas
   mudanças de desempenho **só depois** que a base está modular: paralelismo por cidade
   (determinístico por redução em ordem fixa) e cadência escalonada para processos lentos.

---

## 1. Mapa atual do arquivo

### 1.1 Forma do arquivo

| Arquivo | Linhas | Papel |
|---|---|---|
| `gdextension/src/economy_sim.cpp` | 13.735 | implementação de tudo |
| `gdextension/src/economy_sim.h` | 2.178 | tipos POD + declarações + estado |
| `gdextension/src/register_types.cpp` | ~40 | registra `EconomySim` |
| `gdextension/CMakeLists.txt` | 58 | 2 TUs compiladas (`register_types.cpp` + `economy_sim.cpp`) |
| `gdextension/build-debug`, `build-release` | — | ninja + clang++ (`CMAKE_BUILD_TYPE` = Debug / Release) |
| `gdextension/bin/*.dll` | — | `template_debug` 14,2 MB / `template_release` 1,6 MB |

Uma única classe (`EconomySim : RefCounted`, `economy_sim.h:32`) exposta ao GDScript com
~90 métodos ligados em `_bind_methods()` (`economy_sim.cpp:178–329`). Todo o estado é membro dela.

### 1.2 Blocos por responsabilidade (`economy_sim.cpp`)

Faixas verificadas por leitura do arquivo; "legado" = modelo de 1 cidade por nação, não usado
pelo jogo (ver §1.4).

| # | Bloco / responsabilidade | Linhas | Observação |
|---|---|---|---|
| 1 | Prólogo: includes, `safe_*`, constantes globais, `kDemoSaveVersion = 11` (109), ctor/dtor | 1–177 | constantes de duas eras misturadas |
| 2 | `_bind_methods` — API GDScript | 178–329 | inclui `demo_tick_core()` **exposto** (linha 186) |
| 3 | **Legado**: `ModeSpec`, `initialize`, `tick`, `get_status` | 331–559 | `tick()` (406–532) é o mundo antigo |
| 4 | **Legado**: dispatch/rotas/frete + serializadores `city_to_dict`…`order_to_dict` | 560–926 | usa `City`/`Port`/`TradeOrder`; só `Leg`, `spec`, `distance_km`, `make_leg` são compartilhados com o demo |
| 5 | Constantes de economia real: frota (F4), portos, cobertura de estoque, preços-base | 928–1320 | `demo_base_price` (1314) |
| 6 | **Câmbio, tarifas, competitividade** (F5): moedas, lookup, `demo_tick_fx`, `demo_set_currencies` | 1321–1816 | inclui migração de índice de produto (1243–1288) |
| 7 | Catálogo de **bens** (`demo_set_goods`) | 1727–1816 | dentro do bloco 6 na contagem de linhas |
| 8 | **Fixture AGRIA/PORTUS** (`initialize_demo`) + `initialize_demo_world` | 1817–2093 | fixture usada por 8 ferramentas |
| 9 | Índices por cidade, `demo_money_total`, `demo_snapshot`/`demo_restore`, `demo_check_invariants` | 2094–2288 | retrato completo do mundo por tick |
| 10 | **Comércio de alimento** (`demo_trade_food`) | 2289–2992 | 704 linhas; matching global + frete + portos |
| 11 | **Comércio de bens industriais** (`demo_trade_goods`) | 2993–3485 | 493 linhas; espelha o anterior |
| 12 | Agregações nacionais + **frota/frete** (`demo_tick_fleet`, `demo_leg_unit_freight`, `demo_fleet_buy`) | 3486–4077 | frota por nação em `unordered_map` + ordem explícita |
| 13 | **Portos**: capacidade, expansão, fila, demurrage | 4077–4288 | 1,6% do tick |
| 14 | Entrada do tick + perfil existente + `demo_warm_status` | 4289–4362 | `tick_demo`→`demo_tick_and_publish`→`demo_tick_core` |
| 15 | **`demo_tick_core`** — o dia inteiro em uma função | 4364–5744 | **1.618 linhas**; detalhe em §1.3 |
| 16 | `DemoStatusKeys` (chaves pré-convertidas) | 5746–5981 | 236 linhas de construtor |
| 17 | **Status por cidade** (`demo_fill_city_status`) | 5982–6770 | **789 linhas** em uma função |
| 18 | **Status agregado** (`get_demo_status` + totais nacionais) | 6771–7209 | 439 linhas |
| 19 | Choque de preço, índices de produto, cesta de comida, nome de produto | 7210–7261 | |
| 20 | **Receitas industriais** (catálogo, `demo_pick_recipe`) | 7262–7391 | |
| 21 | Bootstrap industrial (`demo_seed_industry`) | 7392–7591 | moinho+padaria de referência |
| 22 | **Necessidades** (`needs.json`) e custo de cesta por nível | 7592–7876 | |
| 23 | **Consumo**: cesta alimentar, categoria, autoconsumo, consumo do governo | 7877–8180 | |
| 24 | Setters do mundo real: aptidão, jazida, demanda externa, índice de renda, TFP, Gini | 8181–8346 | |
| 25 | **Trabalho/educação/renda**: `sector_employment` espelhado, estratos, classes (Gini), perfis | 8347–9011 | tabela compilada de 09 dados (§8784) |
| 26 | **`demo_labor_day`** | 9012–9251 | mercado de trabalho do dia |
| 27 | **Serviços**: catálogo (20 setores), mercados, negócios, `demo_services_day` | 9252–10226 | 975 linhas |
| 28 | **Salários** (`demo_update_wages`) | 10227–10339 | |
| 29 | Investimento/construção por cidade (`demo_invest`, `demo_build`) | 10340–10544 | |
| 30 | **Grupos/propriedade/IDE/estatais** (F9) | 10545–11648 | 1.104 linhas |
| 31 | Sinais de investimento + `demo_spend_government` | 11649–11703 | |
| 32 | **Save** (`demo_to_dict`) | 11704–12211 | 508 linhas |
| 33 | **Load + migrações** (`demo_from_dict`) | 12212–13103 | **892 linhas** |
| 34 | **Benchmark sintético** (`initialize_benchmark`, `benchmark_*`, `run_benchmark`) | 13104–13734 | 631 linhas, modelo próprio |
| 35 | Fecho do namespace | 13735 | |

### 1.3 Dentro de `demo_tick_core` (4364–5744) — o "dia em uma função"

| Fase | Linhas | O que faz | Tipo de dependência |
|---|---|---|---|
| bootstrap | 4378–4391 | seed de indústria, snapshot para rollback, `money_before` | global |
| 0. manutenção | 4392–4431 | `day_++`, obras de porto, remove falidas/esgotadas, reindex, preços de abertura | global |
| 1. chegada | 4433–4452 | `demo_in_transit_` → estoque de destino, libera veículos | por cidade destino + frota nacional |
| 2. reset | 4454–4549 | zera fluxos do dia, `demo_init_pop_needs`, fluxos de serviço, preço-base | global (arrays por produto) |
| 2b. frota | 4551–4554 | `demo_tick_fleet` (mensal, `day_ % 30`, 3748) | por nação |
| 3. trabalho | 4555–4604 | custo de opex por profundidade + `demo_labor_day` por cidade | **por cidade** |
| 4. produção | 4604–4765 | capacidade por cidade/produto (uma passada), extração, receitas, esgotamento | **por cidade** |
| 4c. brutos | 4765–4821 | demanda externa com teto por cidade/produto | por cidade/produto |
| 5. renda | 4823–5315 | orçamento, renda externa, cesta, subsídio, consumo alimentar, classes, necessidades, consumo do governo | **por cidade** |
| 5b. serviços | 5316–5326 | `demo_services_day` por cidade | por cidade + vetor global de negócios |
| 6. comércio | 5328–5335 | `demo_trade_food` + `demo_trade_goods` | **global** |
| 7. contabilidade | 5337–5571 | receita por construção, lucro, JV, dividendos, falência, `demo_update_wages`, folha de serviços | por cidade + grupos/nacionais |
| 8. preços | 5573–5691 | regra de preço (estoque/oferta/demanda/custo/CIF) + escassez | **por cidade/produto** |
| 9. clamp | 5693–5714 | banda de ±5% sobre o preço de abertura | **por cidade/produto** |
| 9b. fiscal | 5716–5724 | `demo_national_fiscal_transfer` + `demo_settle_groups` | **por nação/grupo** |
| 9c. câmbio | 5726–5731 | `demo_tick_fx` (balança comercial → float) | **global** |
| 10. invariantes | 5733–5743 | `demo_check_invariants` + rollback por `demo_restore` | **global** |
| (fora) status | 4295–4306 | `get_demo_status()` depois do dia | global, Godot |

Regras de ordem que **não podem** ser quebradas (documentadas no próprio código): limite de
variação de preço aplicado uma vez no fim (4421–4431 + 5693); fechamento de firmas no início do
dia para o PIB não as incluir (4397–4416); comércio antes de contabilidade; câmbio depois da
contabilidade (5719–5728).

### 1.4 Duplicações e código legado removível

| Item | Onde | Tamanho | Uso hoje | Ação |
|---|---|---|---|---|
| **Modelo legado** `initialize`/`tick`/`get_status` + `City`/`Port`/`Shipment`/`TradeOrder`, `cities_`/`ports_`/`in_transit_`, serializadores `city_to_dict`/`port_to_dict`/`nation_to_dict`/`order_to_dict`, `dispatch_trade`, `build_route`, `build_overseas_route`, `nearest_port`, `find_city`, `in_transit_total`, `spec`-only paths | cpp 331–559, 560–926 (parte), h 538–667, 1967–1972 | ~520 linhas cpp + ~130 h | **nenhuma**: grep em todos os `.gd`/`.tscn` do projeto não encontra `sim.initialize(`, `sim.tick()` nem `get_status()` (só o watchdog do MCP, outro sistema) | **remover** (S4) |
| **Benchmark sintético** `BenchCity`/`BenchMarket`/`BenchBuilding`/`BenchTradeLink`, `initialize_benchmark`, `benchmark_*`, `run_benchmark`, `get_benchmark_status`, `get_benchmark_city_count` | cpp 13104–13734, h 1513–1584, 1952–1965, 2147–2171 | ~631 linhas cpp + ~115 h | só `tools/economy_benchmark.gd` | **remover** (S4) — `tools/world_scale_bench.gd` já mede o sim real (tick_demo + fases + determinismo síncrono × assíncrono) |
| **Fixture AGRIA/PORTUS** (`initialize_demo`, cenário de 2 cidades) | cpp 1817–1941 | ~125 linhas | 8 ferramentas de teste (`economy_integration_test`, `economy_ai_test`, `economy_long_run_test`, `population_demo_test`, `production_chain_test`, `fleet_fuel_test`, `economy_lab`) | **manter**, mover para `scenario_fixture.cpp` (S14) |
| **Caminho "compat" sem Gini** dentro do consumo (classes espelham estratos) | cpp 4924–4935, 5258–5266, 8480 | ~80 linhas | saves antigos e cenário sintético | **manter** (é migração real), marcar `legacy_compat` |
| **Rollback por snapshot completo** (`demo_snapshot`/`demo_restore` a cada tick) | cpp 2161–2218, 4388–4389 | ~60 linhas | rede de segurança dos invariantes | manter; custo já mitigado por buffer persistente (comentário 4386–4387) |
| `demo_tick_core` ligado ao GDScript | cpp 186 | 1 linha | ninguém chama | **desligar** (deixar só `demo_tick_and_publish`) |
| `UtilityFunctions::push_error` dentro do tick | cpp 5738 | 4 linhas | log de invariante | **mover** para a fronteira (S5) |

Resultado da limpeza: **~1.151 linhas cpp + ~240 h** a menos, e duas semânticas concorrentes
(mundo legado, benchmark) que hoje dão sinal falso de desempenho e de comportamento.

### 1.5 Perfil medido (base para priorizar)

F9, release, 834 cidades, 30 dias, `perf_day_peak --tick-mode=worker --profile=1`
(`docs/economy/perf/f9_invest_perf_release.txt` e `..._run1.txt`):

| Fase | ms/dia (faixa observada) | Fração do tick |
|---|---|---|
| `status` (total) | 23,4–28,3 — `status_cities` 17,9–24,6, `status_orders` 3,2–4,4, `status_nations` 1,9–2,5 | **40–50%** |
| `trade` | 2,6–8,3 (cresce com nº de construções) | 5–15% |
| `arrival` | 3,8–5,8 | 7–10% |
| `income` | 0,7–1,1 | ~2% |
| `production` | 0,3–0,5 | ~1% |
| `prices` | 0,3 | ~1% |
| `labor` + `services` + `accounting` + `reset` + `fiscal` + `fx` + `invariants` (resíduo) | ~7–10 (derivado de tick − fases impressas) | ~15–20% |

Portões oficiais: **tick p95 ≤ 50 ms**, **`status` p95 ≤ 30 ms**, **frame de pico ≤ 16 ms**
(F9: tick 55,19–58,22 **não atende**; status 25,58–27,11 e frame 13,47–13,97 atendem). O tick
acompanha o número de construções (858 → 1.930 em 8 dias, ~7–9 µs por construção/tick) —
**a refatoração não muda esse acoplamento**, mas a modularização e o paralelismo dão margem.

---

## 2. Estrutura-alvo de módulos

### 2.1 Árvore proposta

```
gdextension/src/
  economy_sim.cpp|h            # fachada GDExtension atual (fica fina, mantém nome/API)
  register_types.cpp
  sim/
    sim_types.h                # POD+enums+constantes: sem Godot, sem estado
    sim_constants.h            # números de balanceamento (extraídos do prólogo)
    sim_params.h               # struct SimParams (todo demo_set_balance + knobs F5–F9)
    profiling.h | profiling.cpp# macro de escopo, fases, contadores, JSON/Tracy
    thread_pool.h | thread_pool.cpp
    world_state.h | world_state.cpp   # WorldState: cidades, construções, índices, buffers
    market_book.h | market_book.cpp   # colunas cidade×bem (SoA) + views
    market_book_aos.h          # adaptador AoS enquanto a migração não termina
    freight.h | freight.cpp    # spec(modo), distância, frete unitário, demanda de rota
    production.h | production.cpp
    labor.h | labor.cpp
    consumption.h | consumption.cpp
    services.h | services.cpp
    trade.h | trade.cpp
    logistics.h | logistics.cpp
    fx.h | fx.cpp
    investment.h | investment.cpp
    fiscal.h | fiscal.cpp
    prices.h | prices.cpp
    invariants.h | invariants.cpp
    scenario_fixture.h | scenario_fixture.cpp   # AGRIA/PORTUS (teste)
    sim_core.h | sim_core.cpp  # orquestrador das fases (substitui demo_tick_core)
  edge/
    binding.cpp                # EconomySim : RefCounted; _bind_methods; delega ao SimCore
    status_publish.cpp         # get_demo_status / fill_city_status / keys  (Dictionary)
    save_load.cpp              # to_dict / from_dict / migrações               (Dictionary)
    params_godot.cpp           # demo_set_* que recebem Variant/Array/Dictionary
```

Tamanhos são **estimativas** de destino (partição, não crescimento líquido): simulação ~6.800,
fronteira ~3.200, tipos/estado ~1.500, profiler/pool ~400.

### 2.2 Fronteiras

| Regra | Detalhe | Como verificar |
|---|---|---|
| `sim/` não inclui `godot_cpp` | nenhum `#include <godot_cpp/...>`; nada de `String`/`Dictionary`/`Array`/`Variant`/`push_error` | gate no CI: `grep -l godot_cpp sim/*` deve sair vazio |
| Godot só em `edge/` e no `economy_sim.*` | `binding.cpp`, `status_publish.cpp`, `save_load.cpp`, `params_godot.cpp` | idem |
| `sim/` não conhece cena/autoload/IA | paralelismo próprio, sem `WorkerThreadPool` | revisão + grep |
| `world_state` não conhece fases | só dados e índices; nenhuma regra econômica | revisão |
| uma direção de dependência | `sim_core → fases → world_state/market_book → sim_types`; `edge → sim_core` | ver §2.3 |
| erro sem exceção | `invariants` devolve `std::vector<std::string>`; `binding` loga | `push_error` só em `edge/` |

### 2.3 Grafo de dependências

```
sim_types.h ──<(POD puro)
   ├── profiling.h ──► sim_types.h (Phase enum)
   ├── world_state.h ──► sim_types.h
   │     ├── market_book.h ──► world_state.h
   │     ├── freight.h ──► sim_types.h            (compartilhado por trade/logistics/investment)
   │     ├── production  ──► world_state, market_book, profiling
   │     ├── labor       ──► world_state, profiling
   │     ├── consumption ──► world_state, market_book, profiling
   │     ├── services    ──► world_state, profiling
   │     ├── trade       ──► world_state, market_book, freight, profiling
   │     ├── logistics   ──► world_state, freight, profiling
   │     ├── fx          ──► world_state
   │     ├── prices      ──► world_state, market_book
   │     ├── fiscal      ──► world_state
   │     ├── investment  ──► world_state, production (leitura), fiscal (leitura)
   │     ├── invariants  ──► world_state
   │     └── sim_core    ──► todas as fases   (ordem do dia mora AQUI, não nas fases)
   └── scenario_fixture ──► world_state

edge/binding.cpp ─────► sim_core, status_publish, save_load, params_godot
edge/status_publish ──► world_state (leitura)
edge/save_load ───────► world_state (leitura/escrita) + versão do schema
```

Regra de ouro: **a ordem do dia fica em `sim_core.cpp`**, visível numa função de ~120 linhas com
uma linha por fase. Nenhuma fase chama outra fase.

### 2.4 Contratos que não mudam

- API GDScript: todos os métodos atuais (menos os 9 mortos de S4) continuam com o mesmo nome,
  ordem de argumentos e formato de retorno.
- Save: `version = 11` e o schema atual; migrações existentes preservadas. Só sobe de versão
  quando um layout de dado mudar de verdade (S15/S17).
- Status: mesmas chaves (`DemoStatusKeys`), mesmo double buffer, mesma semântica "snapshot
  válido até o próximo dia".
- Modelo de concorrência externa: **um escritor** (thread do `WorkerThreadPool` via
  `tick_scheduler.gd`), mutex recursivo como rede de segurança (`economy_sim.h:2145`).

---

## 3. Sequência de refatoração (passos pequenos e verificáveis)

### 3.1 Critérios de aceite comuns a **todos** os passos

1. Compila em **debug e release** (`cmake --build gdextension/build-debug`, idem `build-release`;
   ninja + clang++ já configurados).
2. Bateria mínima verde: `economy_integration_test`, `economy_ai_test`, `population_demo_test`,
   `economy_tick_async_test`, `economy_world_test --days=60 --ai=1`.
3. **Equivalência byte a byte** com o commit anterior em 60 dias com IA:
   `tools/economy_refactor_equiv.gd` (criado em S0) roda o cenário real, 60 dias com IA, e grava
   um JSON canônico com (a) `sim.demo_to_dict()` integral, (b) `sim.get_demo_status()` integral,
   (c) `demo_get_profile()`; `JSON.stringify(..., "", true)` (chaves ordenadas) e hash
   `var_to_bytes(...).hash()` comparados ao golden. **0 diferenças** é o padrão; qualquer
   diferença reprova o passo.
4. Passos de desempenho (S15–S17) medem com `perf_day_peak --days=10 --tick-mode=worker
   --profile=1`, **3 execuções**, e registram em `docs/economy/perf/refactor_stepNN.txt`.
5. Cada passo: **≤ 6 arquivos tocados, ≤ 800 linhas movidas, ≤ 3 builds** (cabe em uma tarefa
   de agente com limite de passos). Se estourar, o passo é dividido.

### 3.2 Passos

| Passo | Objetivo | Arquivos | Saída verificável |
|---|---|---|---|
| **S0** | **Harness de equivalência e goldens** (não move código) | `tools/economy_refactor_equiv.gd|.tscn`, `docs/economy/refactor/baseline/*.json` | 3 goldens reprodutíveis: AGRIA 365 d sem IA, mundo real 60 d com IA, mundo real 60 d sem IA; duas execuções seguidas dão hash idêntico |
| **S1** | Extrair `sim/sim_types.h` + `sim/sim_constants.h` (POD, enums, constantes do prólogo) | 2 novos + `economy_sim.h` | build 2×; golden 0; `static_assert` de `offsetof(DemoMarket, stock)` preservado (h:732) |
| **S2** | `sim/profiling.h|.cpp`: macro `OGS_SIM_ZONE`, fases, contadores, export JSON; portar o `lap()` de `demo_tick_core` (4370–4376) para a macro | 3 novos + `economy_sim.cpp` | golden 0; `demo_get_profile()` devolve as mesmas 15 fases + 4 sub-fases; release: macro vazia (conferir símbolo ausente no `.obj`) |
| **S3** | Split **mecânico** do `.cpp` em 3 TUs (sem mudar símbolo): `legacy_world.cpp`, `demo_setup.cpp`, `demo_tick.cpp`; CMake atualizado | 3 novos + CMake | build 2×; golden 0; registrar tempo de build antes/depois |
| **S4** | **Remover legado + benchmark** (§1.4): métodos, tipos, membros, bindings, `tools/economy_benchmark.gd`; `demo_tick_core` deixa de ser ligado | ~10 | build 2×; golden 0; bateria mínima; `grep` provando que nada referencia o removido; registrar a remoção de API em `GAME_STATE` §14 |
| **S5** | `sim_core` + `world_state`: classe `ogs::SimCore` recebe o estado; `EconomySim` vira fachada; `invariants` devolve strings (sem `push_error` no core); **a ordem do dia vira uma função de ~120 linhas** com uma chamada por fase (fases ainda no arquivo) | 4 | golden 0; build 2×; nenhuma regra alterada |
| **S6a** | `edge/save_load.cpp`: mover `demo_to_dict` (508 l.) | 2 | golden 0; `economy_integration_test` (schema v11) |
| **S6b** | `edge/save_load.cpp`: mover `demo_from_dict` + migrações (892 l.) | 2 | golden 0; migração v1…v11 exercitada pelo teste |
| **S7** | `edge/status_publish.cpp`: mover `DemoStatusKeys`, slots, `fill_city_status`, `get_demo_status` + sub-perfis | 3 | golden 0 (status); `status` p95 medido e registrado |
| **S8** | `sim/logistics.cpp` (+`freight.cpp`): chegada, frota, portos, frete, demanda de rota | 4 | golden 0; `fleet_fuel_test`, `port_capacity_test`, `convoy_lifecycle_test` |
| **S9** | `sim/trade.cpp`: `demo_trade_food` + `demo_trade_goods` unificados por um núcleo de matching parametrizado **sem mudar ordem** | 3 | golden 0; `economy_world_test --days=60 --ai=1`; fase `trade` registrada |
| **S10** | `sim/production.cpp` + `sim/prices.cpp`: capacidade/extração/receitas/brutos; regra de preço/clamp/choque | 4 | golden 0; `production_chain_test` |
| **S11** | `sim/consumption.cpp`: needs, cestas, alimento, categorias, governo, duráveis | 3 | golden 0; `pop_needs_test`, `population_demo_test` |
| **S12** | `sim/labor.cpp`: `demo_labor_day`, educação, estratos, classes, salários, âncoras setoriais | 3 | golden 0; `labor_market_test`, `economy_ai_test` |
| **S13** | `sim/services.cpp` | 2 | golden 0; `services_test` |
| **S14** | `sim/investment.cpp` + `sim/fiscal.cpp` + `sim/fx.cpp` + `scenario_fixture.cpp` | 6 | golden 0; `investment_test`, `currency_trade_test` |
| **S15** | **SoA** (`market_book`) nas fases que o profiler apontar (§5) | 4 | A/B: ganho ≥ 15% na fase alvo **e** ≥ 1,5 ms/dia, senão reverter; golden 0; memória medida |
| **S16** | **Paralelismo** com flag `sim_threads` (default 1) | 4 | golden 0 com N = 1, 2, 4, 8 (byte a byte); `economy_tick_async_test`; perf 3×; só então default = `hw-1` |
| **S17** | **Cadência** + modo "tudo diário" (§6) | 4 | 365 d `daily` × `layered` dentro dos limites ratificados; `economy_long_run_test`; `world_sweep 365` |
| **S18** | Fechamento: remover flags, atualizar `GAME_STATE` §13/§14, doc de módulos | 3 | bateria completa + perf 3× |

### 3.3 Ordem de ganho esperado (para o dono decidir até onde ir)

| Depois de | Ganho | Certeza |
|---|---|---|
| S3–S4 | **-1.151 linhas**, ~-8% de tempo de compilação, zero risco econômico | alta |
| S5–S14 | base modular; nenhum ganho de tempo por si só (meta: **mesmo tick**) | alta |
| S7 + otimização de status | maior alavanca única: status é 40–50% do tick | média |
| S15 (SoA) | 0–15% do tick, **só** se o profiler mandar | média |
| S16 (paralelismo) | 40–60% do tick com 4–8 threads nas fases por cidade | média |
| S17 (cadência) | 5–15% do tick + espaço para detalhe novo | média |

---

## 4. Plano de paralelismo

### 4.1 Classificação das fases

| Fase | Paralelizável | Unidade | Por quê / cuidado |
|---|---|---|---|
| reset | **sim** | cidade | arrays globais por produto (`demo_produced_today_`) → acumular local e reduzir em ordem |
| labor | **sim** | cidade | escreve só na própria cidade; lê política nacional (imutável no dia) |
| production | **sim** | cidade | `demo_produced_today_`, `demo_consumed_total_` são globais → buffers locais + redução fixa |
| raw/external | **sim** | cidade×produto | totais globais → redução fixa |
| income | **sim** | cidade | lê preços locais; não toca dinheiro global |
| services | **parcial** | cidade, com partição do vetor global de negócios | `demo_service_businesses_` é vetor global com índices por cidade (`demo_service_by_city_`); abertura/fechamento precisam de seção crítica curta ou segunda passada serial; exportação paga `foreign_cash` global → redução |
| prices / clamp | **sim** | cidade×produto | totalmente independente |
| contabilidade (receita/lucro por firma) | **sim** | cidade | grupos/nacionais/fluxos são globais → passada serial separada |
| arrival | **parcial** | cidade destino | agrupar cargas por destino; liberação de frota é por nação → redução por nação |
| **trade** | **não** (v1) | — | a ordem depende do ranking global `gap/need` (2377) e da oferta global por porto/frota; paralelizar só o *scoring* de candidatos (leitura) |
| fiscal | **por nação** | nação | equalização é intra-nação (4083–4105); nações independentes |
| settle de grupos | **não** | — | caixa do grupo é global e a ordem de crédito importa |
| fx | **não** | — | agrega balança global por moeda |
| invariantes | **não** | — | somas globais (dinheiro) precisam de uma ordem única |
| status | **não** (mantém no writer) | — | cria `Dictionary`; nenhum Variant pode cruzar thread |

### 4.2 Pool próprio × `WorkerThreadPool` do Godot — decisão: **pool próprio**

Motivos (verificados no código):
1. `sim/` tem de ser C++ puro (§2.2). `WorkerThreadPool` é API do engine e exigiria
   `#include <godot_cpp/...>` dentro do núcleo — contraria a decisão do dono.
2. O dia **já** roda em uma tarefa do `WorkerThreadPool` (`core/economy/tick_scheduler.gd:44`).
   Aninhar tarefas do engine dentro dela cria dependência do tamanho do pool do engine e disputa
   com main thread (mapa, comboios, IA fatiada a 1,5 ms/frame).
3. Precisamos de controle explícito: número de threads, partição estática (determinismo), e
   desligar por flag/comparativo.

Especificação de `sim/thread_pool`:
- `N = clamp(hardware_concurrency() - 1, 1, 8)`, sobrescrito por `OGS_SIM_THREADS`.
- threads persistentes, criadas na primeira fase paralela (custo de carga, não de dia).
- `parallel_for_cities(begin, end, chunk, fn)`: **partição estática** por cidade (nunca
  work-stealing, nunca `std::execution` — MinGW/libstdc++ não garante backend paralelo).
- desligado automaticamente quando `cities < 256` ou `cities × products < 50k` (mundo de teste
  AGRIA/PORTUS e bench pequenos continuam seriais → golden estável).
- `sim_threads = 1` é o default até S16 passar; a flag existe para A/B.

### 4.3 Redução determinística

Regra: **threads nunca somam em acumulador compartilhado**. Cada thread escreve em
`scratch[city_index]` (ou `scratch[chunk]` de tamanho fixo), e uma passada serial soma em ordem
**crescente de índice de cidade e de produto** — a mesma ordem do código atual. Consequência:
o resultado independe de N e é bit-idêntico ao serial (soma de `double` é comutativa mas não
associativa; a ordem fixa preserva a associação).

Casos com acumulador global hoje (todos viram redução): `demo_produced_today_`,
`demo_consumed_today_`, `demo_produced_`, `demo_consumed_total_`, `demo_transport_cash_`,
`demo_foreign_cash_`, `demo_construction_cash_`, `demo_bankruptcies_`, `demo_depleted_mines_`,
`demo_service_bankruptcies_`, e os totais nacionais do status.

### 4.4 Buffers por thread e corrida de cache

- `PhaseScratch` por thread, alocado uma vez (cidade × produto quando necessário), com
  **padding de 64 bytes** para evitar false sharing nas colunas.
- Nenhuma escrita em cidade que não seja a "dona" do chunk: produção/consumo **só** no próprio
  índice; leitura de outras cidades é permitida apenas via `const WorldState&` e apenas para
  campos que não mudam na fase (aptidão, jazida, população, receita) — lista branca documentada
  em `production.h`.
- Serviços: `std::mutex` (ou `std::atomic_flag` + lista de pendências) **só** para
  abrir/fechar negócio; a demanda por cidade não toca o vetor global.

### 4.5 Riscos de corrida (e mitigação)

| Risco | Onde | Mitigação |
|---|---|---|
| acumulador global somado por várias threads | §4.3 | redução em ordem fixa; proibido `std::atomic<double>` para dinheiro |
| cidade lendo campo que outra thread escreve | production × trade, income × services | fases com escrita cruzada continuam seriais (trade, fiscal, fx, invariantes) |
| vetor global de negócios de serviço | `demo_service_businesses_` | seção crítica curta + ordem por `business_id` |
| iteração de `unordered_map` decidindo ordem | `demo_settle_groups` (11023, 11051, 11067), `demo_fdi_lookup_`… | proibir iteração de map em regra econômica; quando precisar, iterar `demo_group_order_`/ordem explícita |
| mutex do tick x threads internas | `demo_mutex_` (h:2145) | pool só é usado **dentro** de `demo_tick_core` (já sob o mutex); nunca a partir de `get_demo_status` |
| thread presa em `wait()` no encerramento | `tick_scheduler.gd` (histórico 0xC0000005) | pool com `join()` no destrutor, antes de soltar o `sim` |

---

## 5. Onde SoA vale e onde não

### 5.1 Vale (laços produto × cidade)

| Dado | Hoje | Coluna SoA | Quem usa |
|---|---|---|---|
| estoque, preço, desejado, demanda, vendido, vendido_ontem, escassez, excesso | `DemoCity::markets[160]` (h:1256) | 8 colunas | `prices`, `clamp`, `trade` (ofertas), status por produto |
| `import_price`, `imported`, `exported`, `import_value`, `export_value` | arrays dentro de `DemoCity` (h:1236–1244) | 5 colunas | preço/CIF, contabilidade, status |
| capacidade e receita por produto | `demo_capacity_total_`/`demo_revenue_` equivalentes | 2 colunas | produção, status |
| aptidão e demanda externa | `DemoCity::suitability[160]`, `external_demand[160]` | 2 colunas | produção/brutos, status |

Memória: 160 × 834 = 133.440 slots × 8 B = **1,07 MB por coluna**; 13 colunas ≈ **13,9 MB**
(com 114 bens ativos, ≈ 11,8 MB). Ordem de grandeza aceitável frente aos 542,8 MB atuais.

Ganho esperado: os laços que hoje saltam `sizeof(DemoCity)` (~27 KB) entre cidades passam a
varrer memória contígua — `trade` (construção de ofertas), `clamp` e a agregação nacional do
status. **Só vale se o profiler mostrar ≥15% da fase e ≥1,5 ms/dia** (§3.2, S15).

### 5.2 Não vale

| Dado | Por quê |
|---|---|
| campos escalares por cidade (população, caixa, orçamento, pop agregada) | já contíguos em `std::vector<DemoCity>`; laço cidade-major é amigo do cache |
| `demo_buildings_` (vetor de construções heterogêneas) | ordem e agrupamento por cidade importam; poucas dezenas por cidade; converter quebraria `demo_build`/save sem ganho |
| negócios de serviço (esparsos: ~0–20 por cidade) | índice esparso; AoS é o formato certo |
| grupos (198), moedas (136), tarifas, frotas (179 nações) | pequenos; dominados por lookup de string |
| snapshot/rollback | precisa copiar o estado inteiro; coluna ou linha dá o mesmo volume |
| status | é custo de `Dictionary`/`String`, não de layout |

### 5.3 Migração segura

1. Introduzir `market_book` como **view** sobre as colunas, com a mesma leitura/escrita AoS
   disponível (`market_book_aos.h`) — passo mecânico, golden 0.
2. Migrar **uma fase por vez** (trade → clamp → prices → status), com equivalente bit-exato
   esperado, porque a ordem aritmética por cidade/produto não muda.
3. Save e snapshot passam a ler do `market_book`; `demo_to_dict` continua com o mesmo JSON
   (a transposição é interna).

---

## 6. Cadência escalonada

### 6.1 Estado real hoje

| Processo | Cadência atual | Linha |
|---|---|---|
| educação (mistura por nível) | **estática** (vem do perfil da cidade; não evolui) | 8574–8610 |
| demografia | **não existe** (população fixa; `sim_population` constante) | — |
| salários | **diária**, com rigidez `demo_wage_rigidity_` (default 0,01) | 10227–10339 |
| decisões de grupos (IA) | 30 dias escalonados (GDScript, `group_investor.gd`) | — |
| settle de grupos (dinheiro) | **diária** | 11018–11101 |
| transferência fiscal nacional | **diária** | 4078–4106 |
| manutenção/compra de frota | mensal, `day_ % 30 == 0` (único caso de cadência no C++) | 3748 |
| expansão de portos | diária (verifica obras vencidas) | 4193–4207 |
| abertura/fechamento de negócios de serviço | diária (é sinal de mercado) | 9610–10066 |

Ou seja: **não há infraestrutura de cadência** — há um `% 30` solto. Parte do plano é criar o
escalonador e usá-lo só onde o processo é comprovadamente lento.

### 6.2 Cadência proposta (a ratificar)

| Processo | Hoje | Proposta | Justificativa |
|---|---|---|---|
| educação (coortes, quando dinâmica existir) | estática | avanço de coorte em bloco anual (`day % 365`), com fração diária opcional | formação é anual por natureza |
| demografia (quando existir) | — | anual, com incremento pré-calculado por dia | nascimentos/mortes são anuais |
| salários | diário | **semanal** (`day % 7`), com passo equivalente (`1-(1-r)^7`) | resposta semanal é suficiente; reduz 6/7 do custo |
| transferência fiscal | diária | **mensal** (`day % 30`) | equalização é fluxo lento |
| settle de grupos | diária | **manter diária** | é dinheiro; atrasar quebra a conservação percebida |
| ranking de grupos | sob demanda | cache por dia, recalculado só se `dirty` | puro custo de status |
| manutenção de frota | mensal | formalizar no escalonador (mesmo passo) | já é o comportamento |
| expansão de portos | diária | **event-driven**: fila de conclusão `{port, day}` | sem varredura diária |

Contrato do escalonador (em `sim_core`):

```
CadenceTable { process, period_days, phase_offset, last_run_day }
bool due(process, day) { return (day + phase_offset) % period_days == 0; }
```
`phase_offset` espalha os processos entre os dias do período (nunca todos no mesmo dia).

### 6.3 Modo "tudo diário"

`SimParams::cadence_mode = Layered | Daily`:
- `Layered`: cadências da tabela ratificada.
- `Daily`: **todo** processo roda todo dia (equivalente ao comportamento atual para os que já são
  diários) — usado por testes, `world_sweep`, `long_run` e pelo harness de equivalência.

GDScript: `sim.demo_set_cadence("daily"|"layered")`; harness: `--cadence=daily`.
Default em produção: `Layered` **só depois** de S17 passar; até lá, `Daily` é o default.

### 6.4 Critério de equivalência (365 dias, `Daily` × `Layered`)

Proposta de limites máximos (a ratificar com o dono):

| Métrica | Limite |
|---|---|
| `money_total` | drift exato **0,0000** (conservação) |
| violações de invariante | **0** |
| satisfação alimentar por nação | nenhuma nação pior > **1,0 p.p.**; média ≥ **−0,3 p.p.**; máximo |Δ| ≤ **1,5 p.p.** |
| falências de construção | |Δ| ≤ max(10%, 3) e total ≤ **28** |
| desemprego (8 nações jogáveis) | |Δ| ≤ **0,5 p.p.** |
| PIB real por nação | |Δ| ≤ **2%** |
| educação por nível (quando dinâmica) | |Δ| ≤ **0,2 p.p.** |
| estrutura do status | mesmas chaves e tipos |

Se um processo estourar o limite, ele **volta a diário** — a cadência é rejeitada, não
recalibrada, até haver evidência de que o desvio vem de outra camada.

---

## 7. Profiler por fase (debug/dev, custo zero em release)

### 7.1 Macro de escopo

`sim/profiling.h`:

```cpp
#if defined(OGS_PROFILING)
  #define OGS_SIM_ZONE(phase) ::ogs::profile::Zone _ogs_zone_##__LINE__(phase)
  #define OGS_SIM_COUNTER(name, value) ::ogs::profile::counter(name, value)
#else
  #define OGS_SIM_ZONE(phase) ((void)0)
  #define OGS_SIM_COUNTER(name, value) ((void)0)
#endif
```

- `OGS_PROFILING` **não** é definido no build release (`gdextension/build-release`): nenhum
  `steady_clock::now()`, nenhum branch, nenhum símbolo — custo **zero**, verificável por `nm`/dump
  de símbolos do `.obj`.
- `build-debug` define `OGS_PROFILING`. O toggle em runtime continua existindo
  (`demo_set_profiling(true|false)`, cpp 4313) para não pagar nem o `clock` quando desligado.
- Em release, `demo_get_profile()` devolve `{}` (mesmo contrato de hoje com profiling desligado).

### 7.2 Fases e contadores

- Manter as 15 fases atuais (`kDemoPhase*`, h:1271–1277) e as 4 sub-fases de status
  (`status_cities`, `status_orders`, `status_nations`, `status_misc`) — o harness já as lê
  (`tools/perf_day_peak.gd`, `tools/real_tick_profile.gd`).
- Adicionar sub-fases onde a decisão de otimização precisa de detalhe:
  `production.capacity`, `production.recipes`, `production.extraction`, `consumption.food`,
  `consumption.needs`, `services.demand`, `services.businesses`, `trade.offers`, `trade.matching`,
  `logistics.fleet`, `logistics.ports`, `prices.rule`, `prices.clamp`.
- Contadores (não-temporais): `cities`, `buildings`, `active_products`, `offers_scored`,
  `offers_used`, `legs_built`, `shipments`, `pool_jobs`, `pool_wait_us`, `status_writes`,
  `status_skips`. São eles que explicam o crescimento do tick (858 → 1.930 construções), coisa
  que o tempo sozinho não explica.

### 7.3 Export

- `demo_get_profile()` mantém o formato atual (usado por `perf_day_peak`, `real_tick_profile`).
- Novo `sim.demo_profile_json()` (fronteira) e `--profile=json` nos harnesses para gravar
  `docs/economy/perf/*.json` comparável entre passos.
- Agregação por execução: média, p50, p95, p99, máx por fase (hoje só a última amostra é exposta).

### 7.4 Tracy (opcional)

`-DOGS_TRACY=ON` + `TracyZoneScopedN` no `Zone`. Sem Tracy, o mesmo dado sai em JSON. Tracy não
entra no fluxo padrão nem no build do jogador.

### 7.5 O que se mede em cada passo

| Tipo de passo | Métricas obrigatórias |
|---|---|
| S1–S14 (movimento) | build time (debug/release) + **golden 0** |
| S7 (status) | `status` p95 + `status_cities` p95 |
| S15 (SoA) | ms da fase alvo + memória estática |
| S16 (paralelismo) | tick p95 por N (1/2/4/8) + `pool_wait_us` |
| S17 (cadência) | tick p95 + equivalência 365 d |

---

## 8. Riscos e como medir o ganho

### 8.1 Riscos

| Risco | Prob. | Impacto | Mitigação verificável |
|---|---|---|---|
| **determinismo** quebrado por ordem de soma | alta | alto (save/status divergem, testes vermelhos) | redução em ordem fixa (§4.3); proibido `atomic<double>`; golden diff em S0/S16 com N=1,2,4,8 |
| **determinismo** por iteração de `unordered_map` | média | alto | `demo_settle_groups` (11023/11051/11067) e afins migram para ordem explícita antes de S16; revisão proíbe iteração de map em regra |
| `std::sort` instável em empates de oferta | média | médio | auditados: 2652, 3217 (candidatos por índice), 1666 (taxas), 8831, 10686 — todos ordenam índices/números, não `double` de preço; manter e documentar |
| **saves** quebrados | média | alto | S6a/S6b com golden 0; versão só sobe com mudança real de layout; migração testada por `economy_integration_test` |
| **perf** regride no split | média | médio | cada passo mede tick/status; S15 tem critério de reversão |
| **compilação** fica lenta | média | médio | `world_state.h` estável, sem templates no header, forward declarations, 20 TUs em paralelo (ninja), opcional PCH de `<vector>/<string>/<unordered_map>` |
| **memória** (SoA + buffers de thread) | baixa | médio | medir antes/depois; SoA só com ganho comprovado (13,9 MB máx) |
| mutação GDScript durante o tick | baixa | alto | manter mutex + regra de um escritor; workers só existem dentro do tick |
| tempo/escopo | alta | médio | 19 passos, cada um com aceite; S15–S17 são opcionais e independentes |

### 8.2 Portões de desempenho (mantidos)

| Portão | Alvo | Hoje (F9) |
|---|---|---|
| tick p95 | ≤ **50 ms** | 55,19–58,22 (**não atende**) |
| `status` p95 | ≤ **30 ms** | 25,58–27,11 (atende) |
| frame de pico (virada/pump) | ≤ **16 ms** | 13,47–13,97 (atende) |
| `money_total` | drift 0,0000 | 0,0000 |
| violações de invariante | 0 | 0 |

Nota honesta: o tick não fecha 50 ms **principalmente** porque o mundo cresce de 858 para 1.930
construções nos primeiros 8 dias (F9 §7). Modularização não muda isso; **paralelismo (S16)** e
**status (S7)** mudam. Medir com dois mundos: tamanho inicial (dias 1–4) e saturado (≥ dia 8).

### 8.3 Medição de ganho por etapa

Para cada passo que mexe em desempenho, registrar em `docs/economy/perf/refactor_stepNN.txt`:

```
comando: godot --headless --path . res://tools/perf_day_peak.tscn -- \
         --days=10 --tick-mode=worker --profile=1
execuções: 3  |  máquina: (igual à do F9)
tick:    média / p95 / máx     (dias 1-4) e (dias 8-10)
status:  p50 / p95 / máx  + status_cities
fase alvo: antes → depois (ms e % do tick)
memória: MB estáticos
golden:  hash  |  diferenças = 0
```

---

## Apêndice A — comandos de verificação

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"

# build (os dois diretórios já existem, ninja + clang++)
cmake --build gdextension/build-debug
cmake --build gdextension/build-release

# equivalência (harness criado em S0)
& $godot --headless --path . --script res://tools/economy_refactor_equiv.gd -- --days=60 --ai=1 --out=docs/economy/refactor/stepNN.json --baseline=docs/economy/refactor/baseline/real_60_ai.json

# bateria rápida
& $godot --headless --path . --script res://tools/economy_integration_test.gd
& $godot --headless --path . --script res://tools/economy_ai_test.gd
& $godot --headless --path . --script res://tools/population_demo_test.gd
& $godot --headless --path . --script res://tools/economy_tick_async_test.gd

# mundo real
& $godot --headless --path . --script res://tools/economy_world_test.gd -- --days=60 --ai=1

# perfil do dia (3 execuções)
& $godot --headless --path . res://tools/perf_day_peak.tscn -- --days=10 --tick-mode=worker --profile=1

# longo (só S17)
& $godot --headless --path . --script res://tools/economy_long_run_test.gd
```

## Apêndice B — decisões que precisam do dono

1. **Ratificar os limites de equivalência da cadência** (§6.4) — hoje são proposta.
2. **Aceitar a remoção de API morta** (`initialize`, `tick`, `get_status`,
   `initialize_benchmark`, `run_benchmark`, `get_benchmark_status`, `get_benchmark_city_count`)
   e de `tools/economy_benchmark.gd`.
3. **Definir o teto de threads** (`hw-1` até 8 é a proposta) e se o jogo pode usar todos os
   núcleos ou deve deixar 2 livres para o engine.
4. **Dizer se S15 (SoA) e S17 (cadência) entram no mesmo ciclo** ou ficam para depois do
   fechamento de S16 — são independentes e reversíveis.
5. **Confirmar que o detalhe novo** (educação dinâmica, demografia, mais firmas por cidade) é a
   próxima fase após S18 — o plano abre o orçamento, mas não o preenche.

## Apêndice C — decisões do head (Claude Opus, 2026-11-15; o dono pode revogar)
1. Limites de equivalência da cadência (§6.4): aceitos como proposta inicial; reavaliar com o sweep de 365 d.
2. Remoção da API morta (initialize/tick/get_status/benchmark*) e de tools/economy_benchmark.gd: APROVADA (o bench oficial roda o mundo real).
3. Teto de threads: min(núcleos − 2, 8) — deixa 2 núcleos para o engine/render.
4. S15 (SoA) e S17 (cadência) só depois de S16 (paralelismo) fechado e medido.
5. Detalhe novo (educação dinâmica, demografia, mais firmas) é a fase seguinte a S18.
Ordem de execução: S0 harness → S4 remoção → otimização do STATUS (é 40–50% do tick) → S5…S14 módulos → S16 → S15/S17 → S18. "Obras como projeto físico" (§15 do plano de cadeias) entra logo após os módulos de produção/investimento existirem (S8–S12), para não conflitar com a partição.
