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

## 4. Aceite (medido em 2026-11-17, host do A0)

Ver `GAME_STATE.md` §14 (D-069) para os números desta fatia.
