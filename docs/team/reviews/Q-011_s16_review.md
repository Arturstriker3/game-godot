# Parecer Q-011 — S16 (paralelismo determinístico por cidade)

**Veredito: APROVADO COM RESSALVA**

- Escopo: `c195d66` (scenario_fixture, fechamento do S14), `c9326dc` (pool + `phase_labor`),
  `a5e55b0` (prices/clamp + production/raw), `d9ece4a` (income), `f737878` (services),
  `d368d86` (default `sim_threads` automático) + commits de docs.
- DLL: build debug real no slot (SHA-256 `4C7556D0…`, 19.853.312 B) — restaurada ao fim.
- Ambiente: Windows, release medida com DLL `template_release` no slot `template_debug`
  (per `Q-010`), profiler em debug (`OGS_PROFILING`).

## 1. Auditoria do diff (núcleo C++)

Aprovada nos 5 itens do critério:

1. **Escrita só na própria cidade/scratch por cidade.** Cada fase paralela
   (`phase_labor`, `phase_production`/`phase_raw`, `phase_income`, `phase_services`,
   `phase_prices`/`phase_clamp`) grava apenas na cidade dona do bloco e nas suas
   construções (via `buildings_by_city`) ou em scratch indexado por cidade/produto.
   Confirmei que os helpers chamados em paralelo (`demo_labor_day`,
   `demo_pick_recipe` [const], `demo_food_basket_cost` [const], `demo_city_food_reserve`
   [const], `demo_consume_category`, `demo_services_day`, …) não escrevem em estado
   global do `WorldState` — só em `city.*`/`building.*`/`business.*` locais.
2. **Reduções seriais em ordem fixa.** `phase_production` reduz em ordem crescente de
   índice de construção (a ordem do loop serial global); `phase_raw`/`phase_income`/
   `phase_services` em ordem crescente de cidade (e setor, no caso de serviços). É
   determinismo por construção: partição estática contígua, sem work-stealing, sem
   `atomic<double>`, sem `std::execution`.
3. **Sem RNG/estado compartilhado mutável nas tarefas.** `grep` em `sim/` não achou
   `mt19937`/`rand`/`random_device` nem `static` mutável novo (só os `static const`/
   `constexpr` imutáveis e o `thread_local` pré-existente do S2 em `profiling.cpp`,
   que é por-thread e seguro). O `name_seed` dos negócios é local e fica na 2ª passada
   serial de abertura/fechamento.
4. **`thread_pool` sem deadlock na destruição/reload.** `~ThreadPool` marca `shutdown_`
   sob o mutex, faz `notify_all()` e `join()`. `parallel_for` bloqueia até `remaining_==0`;
   o pool é recriado lazy (`pool().reset(...)`) apenas a partir da thread do tick (o
   writer já segura `demo_mutex_`), nunca com worker rodando. O wake-up por geração
   (`generation_ > seen_generation`) evita perda de notificação e uso-após-free do `fn_`.
5. **Nenhum uso do pool a partir de `get_demo_status`.** `pool_` é privado de `SimCore`;
   só as 7 fases do tick chamam `pool().parallel_for`. `get_demo_status` (fachada) não
   alcança o pool. As fases globais continuam seriais: `prologue`, `arrival`, `reset`,
   `fleet`, `trade`, `accounting`, `fiscal`, `fx`, `invariants` + `status`.

## 2. Equivalência (executor-flash + conferência)

- **Goldens S0 3/3, threads=1 E threads=8, 0 diffs** (6/6):
  - `real_60_ai` → save `991ee806…`, status `b6ecd262…`
  - `real_60_noai` → save `e41dad60…`, status `3a16be88…`
  - `demo_365_noai` → save `c9449042…`, status `3b73f514…`
- **10× `threads=8`** (`real_60_noai` sem baseline): **10/10 idênticos** (1 linha
  distinta, `e41dad60…`/`3a16be88…`, money 2.200.539.587,40, falências 0) — sem data race.
- **`threads=2` e `threads=3`** (`real_60_noai`): **0 diffs** (partições diferentes idênticas).
- **Round-trip real + demo: 0 diffs** (`economy_save_load_roundtrip_test.gd`).
- **Bateria 8/8**: integration/ai/population_demo/production_chain/investment/
  labor_market/pop_needs (via `--script`) + services (via cena `services_test.tscn`).
- Verificador-flash implícito: confiro os hashes reportados contra os `baseline/*.json`
  lidos diretamente — batem exato; exit codes 0.

**Conclusão: o paralelismo é byte-idêntico e determinístico.**

## 3. Perf (A/B release + profiler debug)

**A/B release 3× intercalados** (`perf_day_peak --days=30 --tick-mode=worker`,
DLL release no slot; tick p95 regime):

| | run 1 | run 2 | run 3 | mediana |
|---|---|---|---|---|
| `threads=1` | 48,74 | 49,32 | 48,96 | **48,96 ms** |
| `threads=8` | 48,86 | 48,82 | 48,84 | **48,84 ms** |

Ganho medido: **≈0,1 ms (dentro do ruído)**. O `threads=8` reproduz o do gerente
(48,82→48,84), mas o `threads=1` **não reproduz** os 56 ms alegados (aqui 48,96 ms);
o "56→49 (13%)" é artefato de contenção do baseline serial (o próprio dado do gerente
tem espalhamento 54,9–60,9 ms). Em máquina limpa, a meta **tick p95 ≤50 ms já é atingida
mesmo com `threads=1`** (48,96 ms) — o paralelismo não é o que "fecha" a meta.

**Profiler (debug, `threads=8`, 30 d síncrono — tick 130,82 ms média / 144,91 p95):**

| fase | média (ms) | share | natureza |
|---|---|---|---|
| status | 39,91 | 30,5% | **serial** (publicação Godot, S7) |
| trade | 31,19 | 23,8% | **serial** |
| arrival | 29,65 | 22,7% | **serial** |
| services | 6,43 | 4,9% | paralela |
| raw | 6,15 | 4,7% | paralela |
| production | 5,08 | 3,9% | paralela |
| income | 4,58 | 3,5% | paralela |
| accounting | 2,74 | 2,1% | serial |
| reset | 1,47 | 1,1% | serial |
| invariants | 0,82 | — | serial |
| prices | 0,77 | — | paralela |
| fx | 0,75 | — | serial |
| clamp | 0,53 | — | paralela |
| labor | 0,40 | — | paralela (fleet embutido) |
| fiscal | 0,36 | — | serial |

- **~81,7% do tick continua serial** (status 39,9 + trade 31,2 + arrival 29,7 +
  accounting/reset/fx/invariants/fiscal ≈ 106,9 ms). As fases paralelas somam **≈23,9 ms
  (18,3%)** — e, em release, essas laçadas por cidade são *memory-bound* e já quase
  gratuitas sob `-O2` (auto-vetorização), por isso o ganho do pool é ~0,1 ms e não os
  ~7 ms da regra-de-três debug→release.
- `fleet` não tem lap próprio (fica embutido em `labor`, que totaliza 0,40 ms → frota é
  mensal e desprezível); `fiscal` (0,36 ms) idem. As **reduções seriais** das fases
  paralelas são pequenas e estão embutidas no tempo de cada fase.

**Próximo gargalo serial (guia para S15/S17):** dentro do tick C++ é **`trade`
(31,2 ms, 23,8%)**, seguido de **`arrival` (29,7 ms, 22,7%)**. `status` (39,9 ms) é o
maior absoluto, mas é **publicação Godot (`status_cities` 31,2 ms), não tick** — é a
alavanca do S7, fora do S16. Ou seja: **mais paralelismo por cidade não move mais o
ponteiro**; o próximo passo é atacar `trade`/`arrival` (S15 SoA / S17 cadência).

## 4. Conclusão

- **Correção aprovada sem ressalva**: paralelismo byte-idêntico, determinístico,
  sem estado compartilhado/RNG, pool sem deadlock, `get_demo_status` fora do pool.
- **Ressalvas (não bloqueiam)**:
  1. A alegação "13% mais rápido (56→49)" **não reproduz** em máquina limpa — o ganho
     real é ~0,1 ms e a meta ≤50 ms já é atingida com `threads=1` (48,96 ms). O baseline
     serial de 56 ms do gerente estava contaminado por contenção.
  2. ~82% do tick é serial; o paralelismo cobre só ~18%. O gargalo seguinte é `trade`
     (31,2 ms) e `arrival` (29,7 ms) — não há mais o que paralelizar por cidade com
     retorno relevante; direcionar para S15 (SoA) / S17 (cadência) e para o `status`
     (S7, publicação).

DLL debug real restaurada (19.853.312 B, `4C7556D0…`). Nenhuma cena principal aberta;
editor e godot-editor-mcp não foram tocados.
