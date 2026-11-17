# A1b — limpeza dos modos legacy/benchmark e renomeação de `demo_*`

> Fase **A1b** de `docs/plans/perf_pop_financas_plano.md` (Parte A, linha "Remover os modos
> `legacy` e `benchmark`; renomear `demo_*`"). Branch `perf/a1b-cleanup`, saída de
> `balance/global-resources` (`0a27150`, HEAD do A1a). **Não muda nenhum número**: o critério é
> equivalência funcional com o A0 (`save 3e3ebbbe…1a3d0b7`, `status d79bfe32…c95ac9b`).

## 1. Inventário (feito antes de apagar)

### 1.1 O "modo legacy" (trigo, mundo de 1 cidade por nação)

O mundo legado (`initialize()` / `tick()` / `get_status()`, tipos `City`/`Shipment`) e o benchmark
sintético (`initialize_benchmark` / `run_benchmark` / `get_benchmark_status` /
`get_benchmark_city_count`, tipos `BenchCity`/`BenchMarket`/`BenchBuilding`/`BenchTradeLink`) **já
não existem no código**: foram removidos na S4 (`0469009`, decisão D-029 — "remove API morta (mundo
legado) e benchmark sintático do núcleo C++"), junto com `tools/economy_benchmark.gd`.

Prova neste HEAD (grep em `gdextension/src`, `core/`, `game/`, `map/`, `tools/`, `*.gd/*.tscn/*.ps1`):

| símbolo removido | ocorrências |
|---|---|
| `run_benchmark` | 0 |
| `initialize_benchmark` | 0 |
| `get_benchmark_status` / `get_benchmark_city_count` | 0 |
| `BenchCity` / `BenchMarket` / `BenchTradeLink` | 0 |
| `tools/economy_benchmark.gd` | arquivo inexistente |

Consumidores do legado/benchmark restantes: **nenhum** — nem no jogo, nem em teste, nem em cena.
O que restou foram cascas de dado morto (seção 2).

### 1.2 O que ainda existe com cara de "segunda face": o cenário de validação AGRIA/PORTUS

`initialize_demo()` → `ogs::initialize_demo_fixture` (`sim/scenario_fixture.cpp`: 2 cidades,
5 fazendas de trigo, frota/serviços/grupos semeados). **Não é o modo legado** e **não é usado pelo
jogo** — é o cenário de validação usado pelos testes:

- `tools/economy_integration_test.gd`, `tools/economy_ai_test.gd`, `tools/population_demo_test.gd`,
  `tools/construction_project_test.gd`, `tools/economy_city_status_test.gd`,
  `tools/economy_long_run_test.gd`, `tools/economy_refactor_equiv.gd` (`--mode=demo`),
  `tools/economy_save_load_roundtrip_test.gd` (`--mode=demo`), `tools/production_chain_test.gd`,
  `tools/fleet_fuel_test.gd`, `tools/pop_needs_test.gd`, `tools/urban_land_test.gd`,
  `tools/economy_lab.gd`.

**Decisão: fica.** Três dos testes do aceite desta fase (`economy_integration_test`,
`economy_ai_test`, `population_demo_test`) rodam sobre ele; removê-lo exigiria reinventar os mesmos
números em GDScript e violaria "não muda nenhum número". O jogo (`core/economy/economy.gd`,
`core/ui/`, `map/`) só usa `initialize_demo_world` — nenhum caminho do jogo depende do fixture.

## 2. O que saiu (código morto residual dos modos removidos)

| onde | o que | por quê |
|---|---|---|
| `sim/sim_types.h` | `enum class DemoProduct` (21 índices fixos de produto) | lista de produtos do mundo legado; a ordem hoje vem de `goods.json` e nenhum símbolo era lido |
| `sim/sim_constants.h` | `kDemoServiceMinShare`, `kDemoServiceMaxShare` | "limites do benchmark sintético" — zero usos |
| `sim/sim_constants.h` | `kDemoFreightPerTonKm`, `kDemoFreightShipPerTonKm` | frete do "mercado legado" — zero usos (o frete real está em `freight.cpp`) |
| `sim/sim_constants.h` | `kDemoLaborShare` | fatia fixa do modelo legado — zero usos (`kDemoLaborShareLow/High` são as vivas) |
| `sim/sim_constants.h` | `kDemoProductionBuffer` | regra "produzir 10% acima das vendas" do legado — zero usos |
| comentários | referências a "bench sintético", "mercado legado", "benchmark" nos pontos onde a medição é a do bench canônico | evitar que o leitor procure um modo que não existe |

Constantes de calibração **não usadas mas do modelo atual** foram deixadas de fora desta fatia (não
são legado): `kDemoTradeBufferDays` (0,0) e `kDemoUrbanLandShareOfHousePriceCap` (0,45) seguem
declaradas — a segunda documenta a ressalva R3 do D-067; remover é decisão de calibração, não de
limpeza.

Nenhum teste ou ferramenta foi removido: os que existiam só para o legado/benchmark saíram na S4
(`tools/economy_benchmark.gd`). O profiling por fase (`sim/profiling.cpp`, `OGS_PROFILE_RELEASE`)
**fica** — é o que o bench canônico (`tools/bench_canonical.ps1`) usa.

## 3. Renomeação de `demo_*` (fatia mecânica)

O sufixo "demo" vinha de quando o cenário AGRIA/PORTUS era a "demo" e o mundo real era a novidade.
Hoje `demo_*` **é o jogo**. Regra aplicada (prefixo removido, sem renomear chave de save/status):

| antes | depois |
|---|---|
| `demo_build`, `demo_invest`, `demo_set_goods`, … | `build`, `invest`, `set_goods`, … |
| `demo_set_profiling` / `demo_get_profile` | `set_profiling` / `get_profile` |
| `demo_to_dict` / `demo_from_dict` | `to_dict` / `from_dict` |
| `demo_to_snapshot` / `demo_from_snapshot(_file)` | `to_snapshot` / `from_snapshot(_file)` |
| `demo_snapshot` / `demo_restore` | `snapshot_state` / `restore_state` |
| `demo_tick_day` / `demo_tick_core` / `demo_tick_and_publish` | `tick_day` / `tick_core` / `tick_and_publish` |
| `tick_demo` (binding) | `tick_day` |
| `get_demo_status` (binding) | `get_status` |
| `initialize_demo_world` (binding) | `initialize_world` |
| `initialize_demo` (binding, fixture) / `initialize_demo_fixture` (núcleo) | `initialize_scenario` / `initialize_scenario_fixture` |
| `demo_mutex_` / `demo_lock` (local) | `mutex_` / `lock` |
| `demo_status_*` (cache) | `status_*` |
| `DemoCity`, `DemoBuilding`, `DemoMarket`, `DemoPop`, `DemoGroup`, `DemoCurrency`, `DemoTariff`, `DemoDeposit`, `DemoRecipe`, `DemoFleet`, `DemoShipment`, `DemoSnapshot`, … | `City`, `Building`, `Market`, `Pop`, `Group`, `Currency`, `Tariff`, `Deposit`, `Recipe`, `Fleet`, `Shipment`, `Snapshot`, … |
| `kDemoMaxProducts`, `kDemoEducationLevels`, `kDemoPhaseCount`, … | `kMaxProducts`, `kEducationLevels`, `kPhaseCount`, … |

**Intocado de propósito** (contrato de dado, não identificador): a chave `world_mode` do save, o
valor `"demo"` publicado em `status.mode`, as chaves do status (`money_total`, `trade_food`, …) e as
chaves de i18n `tpl.registro.demo.*`. Os goldens e saves antigos continuam válidos porque nenhuma
chave mudou.

## 4. Aceite (medido em 2026-11-17, host do A0, worktree `opengs-wt-a1b`)

Todos com a **DLL debug** construída nesta worktree (`build-debug`, Godot 4.7.2 headless).

a) **Golden A0 inalterado.** `tools/world_equivalence.ps1 -Rounds 1 -Baseline
   docs/economy/refactor/baseline/A0_60_urban.json` → shas iguais: `save 3e3ebbbe…1a3d0b7`,
   `status d79bfe32…c95ac9b`, money 2.200.539.587,40, 0 falências, 54.554 negócios de serviço.
   **0 diferenças.**
b) **Threads 1 × 8.** Mesmos dois shas nas duas execuções; só `meta.threads` difere.
c) **Bateria.** `economy_integration_test`, `economy_ai_test`, `population_demo_test`,
   `construction_project_test`, `housing_market_test` e `city_gov_test` → **exit 0**.
   `economy_save_load_roundtrip_test --days=60 --save-at=30` → **0 diffs** (mesmo save e status
   hash nos dois caminhos).
d) **CLI = Godot sem IA.** Snapshot exportado (54.875.755 B, 834 cidades, 846 construções, 113
   produtos), replay do Godot 60 d e `opengs_sim_cli --days=60` → arquivos **byte-idênticos**
   (`sha256 79c7897a415c6c593230c64a2adda908bf97046d0dcfdccc50f253ccacbf8feb`, o mesmo do A1a) e
   comparador `world_equivalence_compare.py` com tolerância 0 → **0 diferenças**.
e) **Builds.** Debug (Ninja, clang do llvm-mingw) e release de medição
   (`build-profile-release`, `-DCMAKE_BUILD_TYPE=Release -DOGS_PROFILE_RELEASE=ON
   -DGODOTCPP_TARGET=template_release`) **sem warning novo**; as duas DLLs ficam em `bin/`
   (`template_debug` e `template_release`) e o slot debug foi conferido depois do build release.
f) **Bench canônico (`tools/bench_canonical.ps1`, N=3, editor fechado, release de medição).**
   Tick de parede **36,78 ms** (mediana) contra **35,98 ms** do A1a — **+2,2%**, dentro do limite
   de 3%; dia completo **155,92 ms** contra **153,82 ms** (**+1,4%**); variação entre rodadas
   **1,46%** (tick) e 0,64% (dia total) ≤ 3%; `status` 15,67 ms. A primeira tentativa foi
   descartada: outra frente rodava um `Godot --headless` de 365 d em paralelo
   (`opengs-wt-recal-iso2`) e o próprio script avisa que a medição sai ruidosa (deu 38,07 ms /
   4,64% de variação). A renomeação é **mecânica** (diff simétrico: 2.567+/2.567− no C++ e
   949+/949− nos chamadores).

`grep` final: `\bdemo_` em `gdextension/src` = **0**; em `.gd`/`.tscn` de `core|game|map|tools` =
**0**; `run_benchmark|initialize_benchmark|BenchCity|get_benchmark_*` = **0**. Restam só
`"DEMO"` (id da nação do cenário de validação), `status.mode == "demo"` e as chaves i18n
`tpl.registro.demo.*` — todos dado, não identificador.
