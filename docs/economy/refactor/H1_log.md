# H1 — Modularização da fachada (log dos passos)

Plano: `H_modularizacao_escala_precos.md` §4. Regra de ouro: **refatoração pura antes de
qualquer número mudar**. Cada passo é um commit próprio, com build e portão de bit-identidade;
qualquer diferença de hash = passo revertido, não aceito com tolerância.

## H1a — `edge/definitions_loader` (carregador de definições)

**Escopo:** mover para `gdextension/src/edge/definitions_loader.{h,cpp}` os cinco métodos da
fachada que carregam dado bruto no `WorldState`. Os corpos foram movidos **verbatim** (mesmas
linhas, mesma ordem de iteração, mesmos nomes de campo); só o TU de definição mudou.

| Método | Linhas em `economy_sim.cpp` (antes) | Linhas movidas |
| --- | --- | --- |
| `EconomySim::set_currencies` | 936-1074 | 139 |
| `EconomySim::set_goods` | 1077-1173 | 97 |
| `EconomySim::set_recipes` | 4944-5060 | 117 |
| `EconomySim::seed_industry` | 5062-5298 | 237 |
| `EconomySim::set_needs` | 5300-5521 | 222 |
| **Total** | | **812** |

**Antes/depois:**

- `gdextension/src/economy_sim.cpp`: **12.497 → 11.680 linhas** (−817: 812 de corpo + 5 linhas em
  branco separadoras). Nada mais mudou no arquivo: `git diff` é só remoção.
- `gdextension/src/edge/definitions_loader.cpp`: **845 linhas** (31 de preâmbulo + 812 movidas + 2
  de fecho).
- `gdextension/src/edge/definitions_loader.h`: âncora do módulo (documentação; as declarações
  continuam em `economy_sim.h`, porque os métodos são de `EconomySim`).
- `gdextension/CMakeLists.txt`: `src/edge/definitions_loader.cpp` no alvo `economy_gdextension`.

**Por que continuam métodos de `EconomySim`:** os cinco escrevem direto em `ws_`/`sim_core_` sob o
`mutex_` recursivo da fachada; extrair para funções livres exigiria abrir membros privados e
mexer em contrato — o plano H1a pede só a mudança de arquivo.

**Dependências:** o único helper do namespace anônimo de `economy_sim.cpp` usado pelos corpos
movidos é `safe_nonneg` (8 usos). Foi duplicado em `definitions_loader.cpp` com o mesmo corpo
(inline, namespace anônimo): mesma semântica, mesmo resultado. `economy_sim.cpp` mantém o seu
(usado por `parse_region` e outros). Os lambdas `recipe_for`/`seed_final_chain` são locais de
`seed_industry` e viajaram junto.

**Nota de encoding:** os comentários de `economy_sim.cpp` já estavam com mojibake UTF-8
pré-existente (não introduzido aqui). A extração preservou os bytes: `definitions_loader.cpp`
carrega o mesmo texto dos comentários, sem reescrita.

**Harness:** `tools/c1_world_ai_365.gd:1813` — `"..." + "..." % [...]` formatava só o segundo
trecho (`%` liga mais que `+`), então o primeiro saía com `%d` literais. Passou a
`("..." + "...") % [...]`; mudança de harness, não de simulação.

## Portão de bit-identidade (H1a)

Build: DLL debug `cmake --build gdextension/build-debug -j2` (clang 23.1.5, godot-cpp 4.7) e
`sim_unit_tests` (`build-unit-tests`, `-j2`).

| Checagem | Resultado |
| --- | --- |
| `sim_unit_tests` | **62/62** casos, 451 asserções, exit 0 |
| 365 d **ON** (`c1_world_ai_365 --days=365`, cérebro ligado) | `trajectory_sha256` **67d950b7…** = baseline G1/D-131 (falências 0, violações 0, comida 93/179, drift 0,013175) |
| 365 d **OFF** (`OGS_C1_AI_BRAIN=0`) | `trajectory_sha256` **49c797ef…** = baseline D-126 (falências 0, violações 0, comida 96/179, drift 0,016524) |
| `tools/run_tests.ps1 -Tier pr -Base main` | ver seção "Bateria" abaixo |

Artefatos: `G:\opengs_tmp\h1_wt_365_on.json` e `h1_wt_365_off.json` (fora do repo).

## Bateria

`powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -Tier pr -Base main`
(seleção por diff: `gdextension/src/**` → conjunto amplo de núcleo + sentinelas `pr`).

- **42/42 OK**, 0 falha, 0 crash, 0 timeout, 0 pulado — run `20261125_201647`
  (`G:\opengs_tmp\opengs_test_run_20261125_201647\summary.json`), 20:16→21:28.
- Inclui `sim_unit_tests` (CLI, 0,28 s), a rodada c1 de **90 d** (`c1_world_ai_90`, 225,65 s) e
  `determinism_cross_process` (65,8 s).
- O runner ficou ~45 min na fila do lock global porque outro agente (worktree `opengs-wt-g1b`)
  usava o lock; sem conflito, só espera.

## H1b — `edge/status_city` (ficha e colunas de cidade)

**Escopo:** mover para `gdextension/src/edge/status_city.{h,cpp}` o corpo de
`EconomySim::fill_city_status` — o preenchimento do `StatusCitySlot` (dicionário da ficha da
cidade e as colunas do painel) a partir do `WorldState`. Movido **verbatim**: mesmas linhas,
mesma ordem de escrita e de espelho incremental (`sync_string`, `sync_scalar`, `sync_pop`,
`sync_double`), mesmos nomes de campo.

| Método | Linhas em `economy_sim.cpp` (antes) | Linhas movidas |
| --- | --- | --- |
| `EconomySim::fill_city_status` | 2295-3245 | 951 |
| **Total** | | **951** |

**Antes/depois:**

- `gdextension/src/economy_sim.cpp`: **11.680 → 10.728 linhas** (−952: 951 de corpo + 1 linha
  em branco separadora). `git diff --numstat` = `0 952`: só remoção, zero inserção.
- `gdextension/src/edge/status_city.cpp`: **972 linhas** (19 de preâmbulo + 951 movidas + 1 em
  branco + 1 de fecho). SHA-256 do bloco no TU novo é igual ao do bloco removido.
- `gdextension/src/edge/status_city.h`: âncora do módulo (documentação; a declaração continua em
  `economy_sim.h`, porque o método é de `EconomySim`).
- `gdextension/CMakeLists.txt`: `src/edge/status_city.cpp` no alvo `economy_gdextension`.

**Por que continua método de `EconomySim`:** o corpo lê `ws_`, `sim_core_` e `status_keys_` da
fachada; extrair para função livre exigiria abrir membros privados — o passo pede só a mudança
de arquivo.

**Dependências:** nenhum helper do namespace anônimo de `economy_sim.cpp` é usado pelo corpo. Os
lambdas `sync_string`, `sync_scalar`, `sync_pop`, `sync_double` e o `put` de `income_strata` são
locais e viajaram junto. `fx_rate`, `currency_code`, `product_name`, `city_scalars`, `owner_kind`,
`nation_health_access`, `extraction_efficiency` e `depth_opex_factor` são membros declarados em
`economy_sim.h`; `SimCore::quintile_gini`/`mine_stage_factor`/`is_mine_building` e
`sim_core_.strata_gini`/`mine_deepen_cost` são do núcleo. Portanto, sem duplicação de helper
(≠ do H1a, que duplicou `safe_nonneg`).

**Nota de encoding:** o corpo (comentários em mojibake pré-existente incluídos) foi copiado byte
a byte; nenhum texto foi reescrito.

## Portão de bit-identidade (H1b)

Build: DLL debug `cmake --build gdextension/build-debug -j2` e `sim_unit_tests`
(`build-unit-tests`, `-j2`), clang 23.1.5, godot-cpp 4.7.

| Checagem | Resultado |
| --- | --- |
| `sim_unit_tests` | **62/62** casos, 451 asserções, exit 0 |
| 365 d **ON** (`c1_world_ai_365 --days=365`, cérebro ligado) | `trajectory_sha256` **67d950b7…** = baseline G1/D-131 (0 falências, 0 violações, comida 93/179, drift 0,013175) |
| 365 d **OFF** (`OGS_C1_AI_BRAIN=0`) | `trajectory_sha256` **49c797ef…** = baseline D-126 (0 falências, 0 violações, comida 96/179, drift 0,016524) |
| JSON completo × artefato do H1a | **idêntico** nas duas rodadas (só `wall_s` muda) |
| `tools/run_tests.ps1 -Tier pr -Base main` | ver "Bateria (H1b)" |

Artefatos: `G:\opengs_tmp\h1b_365_on.json` e `h1b_365_off.json`.

## Bateria (H1b)

`powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -Tier pr -Base main`
(seleção por diff: `gdextension/src/**` → conjunto amplo de núcleo + sentinelas `pr`).

- **42/42 OK**, 0 falha, 0 crash, 0 timeout, 0 pulado — run `20261125_223800`
  (`G:\opengs_tmp\opengs_test_run_20261125_223800\summary.json`), 22:38→23:39.
- Inclui `sim_unit_tests` (CLI, 0,85 s), a rodada c1 de **90 d** (`c1_world_ai_90`) e
  `determinism_cross_process`; fecha também a bateria do H1a (mesmo conjunto, 42/42).
- O runner aguardou entre testes processos Godot de outra tarefa (`PID 2124`, `29168`, `19464`);
  sem conflito, só espera.

## H1d — `edge/status_cache` (sinais, portos, ordens, câmbio, tarifas e `get_status`)

**Escopo:** mover para `gdextension/src/edge/status_cache.{h,cpp}` os seis métodos da fachada que
montam o cache leve e o dicionário de status do dia: `fill_light_signals` (sinais por cidade lidos
pela IA), `build_ports`, `build_orders`, `build_currencies`, `build_tariffs` e `get_status`. Movidos
**verbatim**: mesmas linhas, mesma ordem de escrita, mesma ordem de chaves e mesmos nomes de campo.

| Método | Linhas em `economy_sim.cpp` (antes) | Linhas movidas |
| --- | --- | --- |
| `EconomySim::fill_light_signals` | 2295-2518 | 224 |
| `EconomySim::build_ports` | 2526-2553 | 28 |
| `EconomySim::build_orders` | 2555-2598 | 44 |
| `EconomySim::build_currencies` | 2600-2614 | 15 |
| `EconomySim::build_tariffs` | 2616-2628 | 13 |
| `EconomySim::get_status` | 3005-3268 | 264 |
| **Total** | | **588** |

**Antes/depois:**

- `gdextension/src/economy_sim.cpp`: **10.728 → 10.127 linhas** (−601: 588 de corpo + o bloco de
  comentário da A4 e as linhas em branco separadoras). `git diff --numstat` = `0 601`: só remoção,
  zero inserção.
- `gdextension/src/edge/status_cache.cpp`: **624 linhas** (24 de preâmbulo + 334 do bloco
  sinais→tarifas + 1 em branco + 264 de `get_status` + 1 de fecho).
- `gdextension/src/edge/status_cache.h`: âncora do módulo (documentação; as declarações continuam em
  `economy_sim.h`, porque os métodos são de `EconomySim`).
- `gdextension/CMakeLists.txt`: `src/edge/status_cache.cpp` no alvo `economy_gdextension`.

**SHA-256 dos blocos (byte-exato):** o bloco movido foi extraído por bytes e o TU novo carrega o
mesmo texto — `fe6487d9…` (sinais, portos, ordens, câmbio e tarifas, 334 linhas) e `c10530af…`
(`get_status`, 264 linhas). Nada foi reescrito; o mojibake pré-existente dos comentários viajou junto.

**Por que continuam métodos de `EconomySim`:** os corpos leem `ws_`, `sim_core_`, `status_keys_`,
`status_live_`/`status_built_day_` e os dois slots de `StatusCache` da fachada, e prendem o `mutex_`
recursivo; extrair para funções livres exigiria abrir membros privados — o passo pede só a mudança de
arquivo.

**Dependências:** nenhum helper do namespace anônimo de `economy_sim.cpp` é usado. Tudo o que os
corpos chamam é membro (`fill_leg_status`, `compute_in_transit`, `fill_city_status`,
`capture_nation_labor`, `build_nations`, `money_total`, `product_name`, `fx_rate`) ou vem de headers
do núcleo (`sim/balance.h`, `sim/stock.h`, `sim/profiling.h`, `sim/sim_constants.h`) — sem duplicação
de helper.

## Portão de bit-identidade (H1d)

Build: DLL debug `cmake --build gdextension/build-debug -j2` e `sim_unit_tests`
(`build-unit-tests`), clang 23.1.5, godot-cpp 4.7.

| Checagem | Resultado |
| --- | --- |
| `sim_unit_tests` | **62/62** casos, 451 asserções, exit 0 |
| 365 d **ON** (`c1_world_ai_365 --days=365`) | `trajectory_sha256` **67d950b7…** = baseline G1/D-131 (0 falências, 0 violações, comida 93/179, drift 0,013175) |
| 365 d **OFF** (`OGS_C1_AI_BRAIN=0`) | `trajectory_sha256` **49c797ef…** = baseline D-126 (0 falências, 0 violações, comida 96/179, drift 0,016524) |
| `tools/run_tests.ps1 -Tier pr -Base main` | ver "Bateria (H1d)" |

Artefatos: `G:\opengs_tmp\h1d_365_on.json` e `h1d_365_off.json`.

## Bateria (H1d)

`powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -Tier pr -Base main`
(seleção por diff: `gdextension/src/**` → conjunto amplo de núcleo + sentinelas `pr`).

- **42/42 OK**, 0 falha, 0 crash, 0 timeout, 0 pulado — run `20261126_002522`
  (`G:\opengs_tmp\opengs_test_run_20261126_002522\summary.json`).
- Inclui `sim_unit_tests` (CLI), `status_query_test` (as consultas que leem `get_status`), a rodada
  c1 de **90 d** (`c1_world_ai_90`) e `determinism_cross_process`.
- O runner esperou Godot de teste de outra tarefa antes de `c1_balance_test` e de
  `c2_central_bank_test`; sem conflito, só espera.

## H1c — `edge/status_nation` (agregado por nação e trabalho)

**Escopo:** mover para `gdextension/src/edge/status_nation.{h,cpp}` o corpo de
`EconomySim::capture_nation_labor` (retrato semanal do trabalho), o lookup privado
`EconomySim::find_labor_snapshot` (usado só por `build_nations`) e o corpo de
`EconomySim::build_nations` (bloco `nations` do status: totais por nação, frota, mercado de
trabalho e Gini de riqueza). Movidos **verbatim**: mesmas linhas, mesma ordem de escrita e de
chaves, mesmos nomes de campo.

| Método | Linhas em `economy_sim.cpp` (antes) | Linhas movidas |
| --- | --- | --- |
| `EconomySim::capture_nation_labor` | 2295-2348 | 54 |
| `EconomySim::find_labor_snapshot` | 2350-2358 | 9 |
| `EconomySim::build_nations` | 2360-2668 | 309 |
| **Total (bloco contíguo)** | 2295-2668 | **374** |

**Antes/depois:**

- `gdextension/src/economy_sim.cpp`: **10.127 → 9.752 linhas** (−375: 374 de corpo + 1 linha em
  branco separadora). `git diff --numstat` = `0 375`: só remoção, zero inserção.
- `gdextension/src/edge/status_nation.cpp`: **397 linhas** (21 de preâmbulo + 374 movidas + 1 em
  branco + 1 de fecho).
- `gdextension/src/edge/status_nation.h`: âncora do módulo (documentação; as declarações continuam em
  `economy_sim.h`, porque os métodos são de `EconomySim`).
- `gdextension/CMakeLists.txt`: `src/edge/status_nation.cpp` no alvo `economy_gdextension`.

**SHA-256 dos blocos (byte-exato):** o bloco contíguo movido foi extraído por bytes e o TU novo
carrega o mesmo texto — bloco total (`capture_nation_labor` + `find_labor_snapshot` +
`build_nations`, 374 linhas) `256526cc…`; por função: `27411739…` (54), `23c3e1b7…` (9) e
`4ecf55f6…` (309). Nada foi reescrito; o mojibake pré-existente dos comentários viajou junto.

**Por que continuam métodos de `EconomySim`:** os corpos leem `ws_`, `sim_core_`, `status_keys_`,
`labor_snapshot_`/`labor_snapshot_day_` e os slots de `StatusCache` da fachada; extrair para funções
livres exigiria abrir membros privados — o passo pede só a mudança de arquivo.

**Dependências:** nenhum helper do namespace anônimo de `economy_sim.cpp` é usado. Tudo o que os
corpos chamam é membro (`effective_productivity`, `find_labor_snapshot`) ou vem de headers do núcleo
(`sim/sim_types.h` para `kEducationLevels`/`kIncomeLevels`/`kIncomeStrata`, via `economy_sim.h`;
`SimCore::quintile_gini` em `sim/sim_core.h`) — sem duplicação de helper. `find_labor_snapshot`
viajou junto porque é o único usuário do retrato e só `build_nations` o chama (mesmo arquivo, mesmo
comportamento).

## Portão de bit-identidade (H1c)

Build: DLL debug `cmake --build gdextension/build-debug -j2` e `sim_unit_tests`
(`build-unit-tests`, `-j2`), clang 23.1.5, godot-cpp 4.7.

| Checagem | Resultado |
| --- | --- |
| `sim_unit_tests` | **62/62** casos, 451 asserções, exit 0 |
| 365 d **ON** (`h1c_365_on`, `--ai-brain=1`) | `trajectory_sha256` **67d950b7…** = baseline G1/D-131 (0 falências, 0 violações, comida 93/179, drift 0,013175) |
| 365 d **OFF** (`h1c_365_off`, `--ai-brain=0`) | `trajectory_sha256` **49c797ef…** = baseline D-126 (0 falências, 0 violações, comida 96/179, drift 0,016524) |
| JSON completo × artefato do H1d | **idêntico** nas duas rodadas (só `wall_s` muda) |
| `tools/run_tests.ps1 -Tier pr -Base main` | ver "Bateria (H1c)" |

Artefatos: `G:\opengs_tmp\h1c_365_on.json` e `h1c_365_off.json`.

## Bateria (H1c)

`powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -Tier pr -Base main`
(seleção por diff: `gdextension/src/**` → conjunto amplo de núcleo + sentinelas `pr`).

- **42/42 OK**, 0 falha, 0 crash, 0 timeout, 0 pulado — run `20261126_010457`
  (`G:\opengs_tmp\opengs_test_run_20261126_010457\summary.json`).
- Inclui `sim_unit_tests` (CLI), `status_query_test`, `labor_market_test`, a rodada c1 de **90 d**
  (`c1_world_ai_90`, 226,57 s) e `determinism_cross_process`.
- As rodadas 365 d ON/OFF do portão foram executadas pelo mesmo runner em invocação própria
  (mapa temporário com as duas no tier `pr`, pedido `-List economy_integration_test`), para
  respeitar o lock global: run `20261126_014618`, **4/4 OK** (365 ON 848,97 s; 365 OFF 385,76 s).
- O runner ficou na fila do lock global enquanto o worktree `opengs-wt-g1b` rodava testes; sem
  conflito, só espera.

## Sincronização com o `main` (G2) e baseline recapturado

Antes do H1e o worktree foi sincronizado com o `main` de **128ae77b** (merge do G2) em
**2d1418e5**. Três conflitos, todos resolvidos por composição:

- `GAME_STATE.md`: linhas aditivas dos dois lados (H1a/H1b/H1d/H1c do ramo + G1 parte 1 e G2
  do `main`).
- `tools/c1_world_ai_365.gd`: o comentário da precedência `%` × `+` (correção de harness do H1a)
  ficou; o `main` não alterou o trecho.
- `gdextension/src/economy_sim.cpp`: a fachada ficou com o lado H1 (funções movidas ausentes) e
  os blocos que o G2 acrescentou **dentro de funções já movidas** foram portados verbatim para o
  módulo dono: o bloco §2.6 de `seed_industry` (1.569 bytes, SHA-256 `73de193d…`) e a função nova
  `set_bootstrap_industry` (1.976 bytes, `51c75b29…`) para `edge/definitions_loader.cpp`; o
  diagnóstico `bootstrap` de `get_status` (373 bytes, `0edbd62e…`) para `edge/status_cache.cpp`.
  Os blocos são byte-idênticos aos do `main`; os corpos movidos, comparados sem esses blocos, são
  byte-idênticos aos dos TUs (`seed_industry` e `get_status`). As demais mudanças do G2 na
  fachada (includes, `_bind_methods`, `tick_and_publish`, `get_profile`, `get_day_summary`,
  save/load) entraram por auto-merge.

Build debug + `sim_unit_tests`: **74/74** casos, 754 asserções. Baseline recapturado no worktree
pós-merge, **sem** mudança do H1e (DLL debug, `--days=365`, `threads=1`):

| Rodada | SHA-256 | Falências | Comida (<0,975) | Drift |
| --- | --- | --- | --- | --- |
| **ON** (`--bootstrap=1 --ai-brain=1`) | `b4a4ab38…` | 2 | 95/179 (mediana 0,970682) | 0,014103 |
| **OFF** (`--bootstrap=0 --ai-brain=0`) | `ded6f54b…` | 0 | 97/179 (mediana 0,968398) | 0,016673 |

Os dois shas são **exatamente** os do G2 (`b4a4ab38…`/`ded6f54b…`): o merge + a porta dos
blocos do G2 não mexeram no mundo. Artefatos: `G:\opengs_tmp\h1e_base_365_on.json` e
`h1e_base_365_off.json` (runner `20261126_033550`, 2/2 OK; ON 786,77 s e OFF 383,88 s).

## H1e — `edge/commands_bridge` (fila A3 + despachante)

**Escopo:** mover para `gdextension/src/edge/commands_bridge.{h,cpp}` a ponte de comandos: os
conversores `command_to_dict`/`command_from_dict` (namespace anônimo do TU), `dispatch_command`,
`apply_pending_locked`, `apply_commands`, `pending_command_count`, `get_command_log`,
`clear_command_log`, `command_log_truncated`, `replay_commands` e `EconomySim::apply_one_locked`
(o `switch` de tipos de comando que chama os `apply_*`). Movidos **verbatim**: mesmas linhas,
mesma ordem de aplicação e de chaves, mesmos nomes de campo.

| Bloco | Linhas em `economy_sim.cpp` (antes) | Linhas movidas |
| --- | --- | --- |
| Fila, log e replay (contíguo, com os conversores) | 1627-1834 | 208 |
| `EconomySim::apply_one_locked` (cabeçalho + corpo) | 6177-6180 + 6190-6459 | 4 + 270 |
| **Total** | | **482** |

**Antes/depois:**

- `gdextension/src/economy_sim.cpp`: **9.808 → 9.322 linhas** (−486: 482 de corpo + 4 linhas em
  branco separadoras). `git diff --numstat` = `0 486`: só remoção, zero inserção. O `to_std`
  continua na fachada (a superfície que enfileira — `invest`, `build`, … — segue lá).
- `gdextension/src/edge/commands_bridge.cpp`: **519 linhas** (33 de preâmbulo + 208 da fila + 1 em
  branco + 4 do cabeçalho do despachante + 1 em branco + 270 do corpo + 1 em branco + 1 de fecho).
- `gdextension/src/edge/commands_bridge.h`: âncora do módulo (documentação; as declarações
  continuam em `economy_sim.h`, porque os métodos são de `EconomySim`).
- `gdextension/CMakeLists.txt`: `src/edge/commands_bridge.cpp` no alvo `economy_gdextension`.

**SHA-256 dos blocos (byte-exato):** `e1a06cfb…` (fila/log/replay, 208 linhas), `9a92a71e…`
(cabeçalho + `apply_one_locked`, 274 linhas) e `63080795…` (só a função, 270 linhas). Nada foi
reescrito; o mojibake pré-existente dos comentários viajou junto.

**Por que continuam métodos de `EconomySim`:** os corpos leem `ws_`, `commands_`, `mutex_`,
`tick_in_progress_`, `tick_thread_` e `current_day_` da fachada; extrair para funções livres
exigiria abrir membros privados — o passo pede só a mudança de arquivo.

**Dependências:** `current_thread_hash` é o único helper do namespace anônimo de
`economy_sim.cpp` usado pelo bloco movido (`dispatch_command`); foi duplicado inline no TU novo
com o mesmo corpo (como o `safe_nonneg` do H1a) e o original fica (usado também no
`tick_thread_.store`). O novo TU inclui `sim/bank.h`, `sim/central_bank.h` e `sim/crisis.h`
(usados pelo despachante); `ogs::Command` e os slots vêm de `sim/commands.h` via
`economy_sim.h`.

## Portão de bit-identidade (H1e)

Build: DLL debug (`cmake --build gdextension/build-debug`) e `sim_unit_tests`
(`build-unit-tests`), clang 23.1.5, godot-cpp 4.7.

| Checagem | Resultado |
| --- | --- |
| `sim_unit_tests` | **74/74** casos, 754 asserções, exit 0 |
| 365 d **ON** pós-merge (baseline) | `trajectory_sha256` **b4a4ab38…** (2 falências, comida 95/179, drift 0,014103) |
| 365 d **ON** com o H1e | **b4a4ab38…** = baseline, bit a bit |
| 365 d **OFF** pós-merge (baseline) | `trajectory_sha256` **ded6f54b…** (0 falências, comida 97/179, drift 0,016673) |
| 365 d **OFF** com o H1e | **ded6f54b…** = baseline, bit a bit |
| JSON completo × baseline | **idêntico** nas duas rodadas (só `wall_s`/metadados de tempo mudam) |
| `tools/run_tests.ps1 -Tier pr -Base main` | ver "Bateria (H1e)" |

Artefatos: `G:\opengs_tmp\h1e_gate_365_on.json` e `h1e_gate_365_off.json`
(runner `20261126_035753`).

## Bateria (H1e)

`powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 -Tier pr -Base main`
(seleção por diff: `gdextension/src/**` → conjunto amplo de núcleo + sentinelas `pr`), com o
mapa temporário do portão (cópia do `tools/test_impact_map.json` + as duas rodadas 365 no tier
`pr`, mesmo mecanismo do H1c).

- **44/44 OK**, 0 falha, 0 crash, 0 timeout, 0 pulado — run `20261126_035753`
  (`G:\opengs_tmp\opengs_test_run_20261126_035753\summary.json`).
- Inclui `sim_unit_tests` (CLI, 0,28 s), 39 testes Godot (de 21,8 s a 294,7 s — `c2b2_fx_stress`,
  `housing_market`, `services_business_audit`), a rodada c1 de **90 d** (`c1_world_ai_90`,
  184,37 s), `determinism_cross_process` (64,7 s) e as duas rodadas **365 d do portão**
  (ON 682,37 s; OFF 390,9 s).
- O runner esperou o lock global do worktree `opengs-wt-g3` antes de começar; sem conflito, só
  espera.

## Próximos passos

H1e (`edge/commands_bridge`) fechado; seguem H1f e H1g — na ordem do plano, cada um com o mesmo
portão. Sem merge até o QA fechar a série.
