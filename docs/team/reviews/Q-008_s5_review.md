# Q-008 — Revisão do S5 completo (WorldState + SimCore, fachada fina, sem push_error no core)

- **Commits revisados:** `a62f5ae`…`7156d4f` (16 commits: S5a tipos→`ogs` + `ogs::WorldState`;
  S5b-1..6 as 16 fases do `demo_tick_core` migram para `ogs::SimCore`; S5c fachada fina;
  S5d `push_error` fora do core).
- **Pedido:** `docs/team/QUADRO.md` (@gerente-simulacao → @gerente-qualidade, fechamento do S5).
- **Veredito:** **APROVADO** (com um achado de save/load **pré-existente**, fora do escopo do S5 — ver §3).

---

## 1. Diff — é realocação, não mudança de regra

| Gate | Resultado |
| --- | --- |
| Realocação (sem mudança de regra/constante) | ✅ 11 arquivos, +7.542/−7.122 linhas; `economy_sim.cpp` cai de 12.207→6.403 linhas (~5.700 movidas para `sim_core.cpp`); constantes movidas com **valores idênticos** (spot-check de 30+ constantes: `kDemoServicePriceBandLow=0.70`, `kDemoFleetTrucksPerCapita=1/12500`, `kDemoSaveVersion=11`, etc. — todas batem com o baseline `920d50c`) |
| Gate §2.2 — 0 `godot_cpp` em `sim/` | ✅ nenhum `#include <godot_cpp...>` em `gdextension/src/sim/`; só STL e headers internos (as 6 ocorrências de "godot_cpp" são comentários "nada de godot_cpp") |
| Ordem das fases idêntica ao `demo_tick_core` original | ✅ as 16 fases do orquestrador HEAD batem 1:1 com os blocos inline do baseline: prólogo → 1 chegada → 2 reset → 2b frota → 3 labor → 4 produção → 4c raw → 5 renda → F8 serviços → 6 comércio → 7 contábil → 8 preços → 9 clamp → 9b/9b2 fiscal → 9c câmbio → 10 invariantes |
| Estado estático/global novo que quebre S16 (paralelismo) | ✅ **nenhum mutável**. Inventário completo do `sim/`: (a) `static const ModeSpec kTruck/kShip/kPlane` em `SimCore::spec()` — imutáveis; (b) `static const double a/b/c/d[]` em `normal_quantile` (`sim_labor.h`) — imutáveis; (c) `thread_local` em `profiling.cpp` (S2, pré-existente) — por-thread, é o padrão CORRETO para S16; (d) `constexpr` em `sim_constants.h`/`sim_labor.h` — tempo de compilação. O `SimCore` guarda só `WorldState& ws_` + `money_before_` + `open_prices_` (membros de instância, **recomputados no prólogo a cada tick** — ver §3) |

A migração `EconomySim::demo_*_` → `ws_.*` é renomeio mecânico; `DemoSectorAnchor` saiu do aninhamento
privado de `economy_sim.h` para `sim_types.h` (namespace `ogs`); `CMakeLists.txt` só adiciona
`src/sim/sim_core.cpp` ao build. **Nenhuma constante de balanço alterada.**

## 2. Goldens S0 + bateria + sweep (executor-flash + verificador)

**Goldens S0 (`economy_refactor_equiv.gd`): 3/3 com 0 diferenças.**

| Golden | save_sha256 | status_sha256 | Resultado |
| --- | --- | --- | --- |
| `real_60_ai` | `0f6e7870…b16a1` | `b6ecd262…6415` | ✅ 0 diffs |
| `real_60_noai` | `8dc96775…fdb0` | `3a16be88…817d` | ✅ 0 diffs |
| `demo_365_noai` | `4239fc9b…b703` | `3b73f514…21d0` | ✅ 0 diffs |

**Bateria 8/8 — todos verdes (exit 0):** `economy_integration_test`, `economy_ai_test`,
`population_demo_test`, `production_chain_test`, `investment_test`, `labor_market_test`,
`pop_needs_test`, `services_test.tscn` (via cena).

**Sweep seed-padrão 365 d — bit a bit idêntico a `docs/economy/perf/services_fix_world_sweep_v2.json`**
(variante `real`):

| Métrica | v2.json | medido | |
| --- | ---: | ---: | --- |
| falências de construções | 14 | 14 | ✅ |
| negócios de serviço | 48.077 | 48.077 | ✅ |
| falências de serviço | 13.495 | 13.495 | ✅ |
| SOL food | 0,986267218193767 | 0,986267218193767 | ✅ idêntico |
| DJI food | 0,394838716690075 | 0,394838716690075 | ✅ idêntico |

Nota: o `economy_world_sweep.gd` retorna exit 1 quando apontado para `f9_invest_world_sweep.json`
(baseline **pré-fix**), porque DJI (−7,7 p.p.) e SAH (−4,7 p.p.) estouram a tolerância de comida
vs o f9 — é a lacuna de modelo já documentada e decidida pelo dono em Q-005/Q-007, **não** uma
regressão do S5. Os números são bit a bit iguais ao `v2.json` (o baseline pós-fix correto).

## 3. Round-trip save/load — achado PRÉ-EXISTENTE (não é regressão do S5)

Escrevi `tools/economy_save_load_roundtrip_test.gd` (guarda nova, no meu escopo): "salvar no dia 30,
carregar num `EconomySim` novo e rodar até 60 == rodar 60 direto", comparando SHA-256 de
`demo_to_dict()` e `get_demo_status()`.

**Resultado: o round-trip DIVERGE** já no primeiro dia após o load (`--days=2 --save-at=1` também
diverge). O caminho **direto** está intacto (save `8dc96775…fdb0` == golden `real_60_noai`).

**Determinação de pré-existência (crítica):** re-rodando o MESMO teste contra a DLL do baseline
`920d50c` (worktree isolado, release), os hashes divergentes são **bit a bit idênticos** aos do HEAD:

| caminho | `920d50c` (baseline) | `7156d4f` (HEAD) |
| --- | --- | --- |
| direto 31 d | save `5685cd27…a890` / status `4352a8a1…3a36` | idem |
| split 30+1 d | save `2b391645…1a98` / status `3a6038a2…89cd` | idem |

Ou seja: **o S5 não introduziu a divergência** — ela já existia no núcleo pré-S5. É um gap de
serialização do mundo real (`demo_to_dict`/`demo_from_dict`): algum estado usado pelo tick não
sobrevive ao round-trip. O `economy_integration_test` (que passa) só testa o cenário AGRIA/PORTUS
de 2 cidades, não o mundo real de 834 cidades.

Isso NÃO é veto do S5 (a refatoração é bit-idêntica, como provam os goldens e o próprio round-trip
reproduzir identicamente nos dois commits). É um **achado para o S6a/S6b** (save/load, o próximo
passo do Apêndice C). A nova guarda `economy_save_load_roundtrip_test.gd` fica como regressão
vermelha-consciente até o S6 corrigir o gap. Confirmei no código que `money_before_`/`open_prices_`
do `SimCore` são **recomputados no prólogo a cada tick** (não são a causa); a causa está na
serialização do `WorldState` (campo não gravado/restaurado), a ser localizado no S6.

## 4. Perf A/B release — `920d50c` vs `7156d4f`

**Método:** release (`-O3 -DNDEBUG`, clang++), DLL release copiada sobre `template_debug`
(convenção D-030/F9), `perf_day_peak --days=30 --tick-mode=worker`, headless, editor fechado,
**3 runs cada, intercalados A/B/A/B/A/B**. Mundo **bit-idêntico** nos 6 runs (858 → 1.006 → 1.169 …
construções, mesmos pedidos/comboios/entregas/decisões de IA).

| Commit (DLL release) | run | tick média | **tick p95** | tick máx |
| --- | --- | ---: | ---: | ---: |
| `920d50c` (A, baseline) | 1 | 46,39 | 48,80 | 48,91 |
| `920d50c` (A) | 3 | 45,96 | 48,84 | 48,93 |
| `920d50c` (A) | 5 | 45,89 | 48,74 | 48,79 |
| `7156d4f` (B, S5) | 2 | 45,67 | 48,81 | 48,95 |
| `7156d4f` (B) | 4 | 44,60 | 48,86 | 48,89 |
| `7156d4f` (B) | 6 | 45,69 | 48,78 | 48,85 |

- **`920d50c`:** p95 ∈ [48,74 … 48,84] ms (mediana 48,80)
- **`7156d4f`:** p95 ∈ [48,78 … 48,86] ms (mediana 48,81)
- **Delta: +0,01 ms (+0,02%)** — ruído puro (sobreposição total; amplitude intra-commit ~0,1 ms).

**Conclusão:** o S5 **não regride o tick release** (delta ≈ 0). A migração das 16 fases + helpers
para `SimCore` (chamadas de método em vez de blocos inline) é neutra em performance — consistente
com o esperado de uma realocação pura.

## 5. Veredito

**APROVADO.** O S5 é uma realocação byte-idêntica e correta:

- `sim/` 100% STL (0 `godot_cpp`), ordem das 16 fases idêntica ao original, nenhum estado
  estático/global mutável novo (só `static const` imutáveis + `thread_local` pré-existente do S2).
- Goldens S0 **3/3 0 diffs**; bateria **8/8**; sweep 365 d **bit a bit idêntico** a
  `services_fix_world_sweep_v2.json` (14 / 48.077 / 13.495 / SOL 0,986267 / DJI 0,394839).
- Perf release **não regride** (p95 ~48,8 ms nos dois commits, delta +0,01 ms = ruído).

**Achado (fora do escopo do S5, acionável no S6):** o round-trip save/load do **mundo real**
diverge no primeiro tick após o load — **pré-existente** (reproduz bit a bit no baseline `920d50c`),
portanto não é regressão do S5. Guarda nova: `tools/economy_save_load_roundtrip_test.gd`.

- **Ambiente:** release (`-O3 -DNDEBUG`), tick worker, headless, editor fechado, pt-BR.
  DLLs: `920d50c` → 1.505.792 B (SHA `ECBA0FD2…`); `7156d4f` → 1.503.232 B (SHA `4301DA7B…`).
- **DLL debug real restaurada** (`template_debug` = `B8030999…`, SHA-256 idêntico ao `.real_debug`).
- Nenhuma cena principal aberta; editor não morto.
