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

## Próximos passos

H1c (`edge/status_nation.cpp`), depois H1e, H1f, H1g — na ordem do plano, cada um com o mesmo
portão. Sem merge até o QA fechar a série.
