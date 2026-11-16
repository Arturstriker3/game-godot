# Q-010 — Revisão do S8–S14 (partição do `SimCore` em 11 módulos por domínio)

- **Commits revisados:** `f8e24ef` (S8 logística) · `a6db16f` (S9 trade) · `0ad39be` (S10 produção+preços) ·
  `afa18f4` (S11 consumo) · `56933dd` (S12 trabalho) · `0beae62` (S13 serviços) · `10d78a6` (S14
  investimento+fiscal+câmbio) · `e2f6b30` (docs) — o `SimCore` de ~5.800 linhas é particionado em 11 módulos
  (`freight.cpp`, `logistics.cpp`, `trade.cpp`, `production.cpp`, `prices.cpp`, `consumption.cpp`,
  `labor.cpp`, `services.cpp`, `fiscal.cpp`, `fx.cpp`, `investment.cpp`) em `gdextension/src/sim/`.
- **Pedido:** `docs/team/QUADRO.md` (@gerente-simulacao → @gerente-qualidade, revisão S-001/S8–S14).
- **Veredito:** **APROVADO** (realocação pura, byte-idêntica; o tick 152 ms do gerente é a DLL debug, não regressão).

---

## 1. Diff — realocação pura, 0 `godot_cpp`, sem estado mutável novo, sem ciclo

| Gate | Resultado |
| --- | --- |
| Realocação **sem mudança de regra/constante** | ✅ **PROVA AUTOMÁTICA:** extraí as linhas de código (sem comentário/blank/`#include`) de `sim_core.cpp` em `ac3bcad` (4.761 linhas) e de `sim_core.cpp`+11 módulos em `e2f6b30` (4.785 linhas). **0 linhas removidas/alteradas**; as 24 linhas "novas" são só boilerplate de namespace: `namespace ogs {` ×11 + `}` ×12 + `namespace {` ×1. Ou seja, cada linha de código moveu-se **byte a byte**; só mudou o arquivo que a contém e a adição de `namespace ogs`. |
| 0 `godot_cpp` em `sim/` (gate §2.2) | ✅ nenhum `#include <godot_cpp…>` em `gdextension/src/sim/` (as 6 ocorrências da string são comentários "nada de godot_cpp"). |
| Nenhum estado static/global **mutável** novo (S16 paralelismo) | ✅ os únicos `static`/`thread_local` encontrados são **imutáveis** ou **pré-existentes**: `static const double a/b/c/d[]` (`sim_labor.h:92-104`, coefs de normal_cdf/normal_quantile); `static constexpr` (`sim_types.h:129-159`); `static const Row kRows[]` (`labor.cpp:93`); `static const ModeSpec kTruck/kShip/kPlane` (`freight.cpp:47-53`); `static` **métodos-membros** de `SimCore` (`sim_core.h:86-166`); e o `thread_local` do profiler do S2 (`profiling.cpp:23-25`, **pré-existente**, não tocado nesta cadeia). **Nada de estado mutável novo** — S16 seguro. |
| Dependências entre módulos **sem ciclo** | ✅ grafo de includes é um DAG estrito (ver §1.1). |

### 1.1 Grafo de includes (texto)

```text
sim_types.h        (STL only: <map>,<set>,<string>,<unordered_map>,<vector>,<cstddef>)
     ▲
     │
world_state.h      (#include "sim_types.h")
     ▲
     │
sim_core.h         (#include "sim_labor.h", #include "world_state.h")
     ▲              ── e também:
     │               sim_labor.h   → <algorithm>,<cmath> (STL only)
     │               sim_constants.h → (header-only, sem #include)
     │               profiling.h   → <chrono>,<vector> (STL only)
     │
┌────┴──────┬─────────────┬──────────────┬───────────────────┬────── … (11 módulos + sim_core.cpp)
│ freight   │ logistics   │ trade        │ production/prices │ consumption/labor/services/fiscal/fx/investment
└───────────┴─────────────┴──────────────┴───────────────────┴──────
```

Cada um dos 11 módulos `.cpp` (e `sim_core.cpp`) inclui exatamente o mesmo conjunto:
`sim_core.h`, `profiling.h`, `sim_constants.h`, `sim_labor.h` + STL. **Nenhum módulo inclui outro
módulo `.cpp`**; o único hub é `sim_core.h`, que só aponta para `sim_labor.h`→STL e
`world_state.h`→`sim_types.h`→STL. Arestas todas convergem para folhas STL ⇒ **impossível haver ciclo**.

## 2. Goldens S0 + round-trip + bateria (executor-flash + verificador-flash)

**Goldens S0 (`economy_refactor_equiv.gd`): 3/3 com 0 diferenças.**

| Golden | save_sha256 | status_sha256 | Resultado |
| --- | --- | --- | --- |
| `real_60_ai` | `991ee806…0e82ab` | `b6ecd262…4116415` | ✅ 0 diffs |
| `real_60_noai` | `e41dad60…6607` | `3a16be88…138817d` | ✅ 0 diffs |
| `demo_365_noai` | `c9449042…98725` | `3b73f514…9921d0` | ✅ 0 diffs |

Hashes **idênticos** aos baselines commitados (`docs/economy/refactor/baseline/`, tracked/limpos) — prova
byte a byte de que a partição em 11 TUs não alterou **nada** da simulação nem da serialização.

**Round-trip (`economy_save_load_roundtrip_test.gd`) — 0 diffs nos dois modos:**

| Modo | direto vs split | Resultado |
| --- | --- | --- |
| mundo real (`--mode=world --days=60 --save-at=30`) | `e41dad60…`/`3a16be88…` | ✅ 0 diffs |
| demo (`--mode=demo --days=40 --save-at=20`) | `913b6b1a…`/`4ba7da2b…` | ✅ 0 diffs |

Nota: o save do round-trip world (no-AI) é **o mesmo hash** do golden `real_60_noai` — consistência cruzada
entre os dois harnesses.

**Bateria 8/8 — todos verdes (exit 0):** `economy_integration_test`, `economy_ai_test`,
`population_demo_test`, `production_chain_test`, `investment_test`, `labor_market_test`,
`pop_needs_test`, `services_test.tscn` (via cena).

**Verificador-flash: APROVADO** (re-executou `demo_365_noai` e o round-trip demo de forma independente;
conferiu os 3 baselines como tracked/limpos; confirmou a DLL em uso == `.real_debug` por SHA256
`3979FBB2…`). Única observação (não bloqueante): existem 3 DLLs de backup (`*.old`, `*.ws.debug`,
`~*.TMP` de 08–14/11) em `gdextension/bin/` — **pré-existentes** e gitignorados, não gerados por este
trabalho; o `.real_debug` é convenção mandatada pelo protocolo (§6/C++).

## 3. Perf — o "152 ms" é a DLL debug; A/B release não regride

**Causa do "tick p95 152 ms" do @gerente-simulacao:** é a **DLL debug**, não regressão nem contenção de
máquina. O headless `--path .` carrega o slot `windows.debug.x86_64` (a `.gdextension` resolve o template
debug em run de projeto). Após o trabalho S8–S14 a DLL debug está no slot, então qualquer `perf_day_peak`
headless mede a debug: tick **−O0 + `OGS_PROFILING`** ≈ **145–152 ms**. O próprio Q-009 já registrou esse
número ("tick Debug 152 ms" = 27% do tick debug). Probe deste parecer com a debug no slot: **145,11 ms
p95** (6 d); trocando a release para o slot: **~50 ms**. Os 48,7 ms do Q-009 foram medidos com a DLL
release no slot (técnica `template_release` → `template_debug`). **A assinatura está nos números do
gerente:** `152,09 / 152,09 / 152,18 ms` (3 runs idênticos a 0,1 ms) e baseline `158,90 ms` — o
comportamento ultra-determinístico da debug (carga fixa, sem a variância de ±3 ms que a release mostra);
uma máquina 3× "throttled" manteria a variância, não congelaria em 152,09.

**A/B release ac3bcad vs e2f6b30 — 3 runs intercalados** (`perf_day_peak --days=30 --tick-mode=worker`,
headless, editor fechado, sem nenhum processo godot/clang/ninja):

| run | ac3bcad (baseline) p95 | e2f6b30 (HEAD) p95 |
| --- | ---: | ---: |
| 1 | 69,67 | 63,30 |
| 2 | 59,18 | 60,34 |
| 3 | 60,12 | 59,52 |
| **mediana** | **60,12** | **60,34** |

- **Delta mediana: +0,22 ms (ruído)** → **não regride**, como esperado da realocação byte-idêntica
  (goldens 0 diffs ⇒ mesmas operações executadas). O run 1 de cada lado é o "quente" (file cache frio).
- Os valores absolutos (~60 ms) estão acima dos 48,74 ms do Q-009 **nos dois lados** — inclusive no
  baseline `ac3bcad` (que é o mesmo código do Q-009). Ou seja, o ~+11 ms é **estado da máquina** de hoje,
  não código: se fosse regressão do S8–S14, o lado `e2f6b30` ficaria sistematicamente acima do `ac3bcad`, o
  que não ocorre.
- **Investivação de processos:** nenhum `godot*` órfão/headless antigo rodando (lista vazia no início, no
  meio e ao fim); editor fechado (0 processos godot); nenhuma compilação paralela durante a medição (build
  do baseline concluído antes). Não houve o que matar (>10 min) — nada travado.

**Curiosidade (não bloqueia):** a DLL release **encolheu** 1.522.688 → 1.499.648 B (−1,5%) com a partição
(ICF/dead-code do compilador entre TUs) — consistente com a realocação, sem impacto observável.

## 4. Veredito

**APROVADO.** O S8–S14 é uma **realocação pura e correta** do `SimCore` em 11 módulos por domínio:

- **0 linhas de código alteradas** (prova automática — só wrap de `namespace ogs`); 0 `godot_cpp` em `sim/`;
  **nenhum estado static/global mutável novo** (S16 seguro); grafo de includes **acíclico**.
- Goldens S0 **3/3 0 diffs** (hashes idênticos aos baselines); round-trip **0 diffs** real+demo; bateria
  **8/8**; verificador-flash **APROVADO**.
- Perf release A/B **não regride** (+0,22 ms mediana = ruído). O **"152 ms" é a DLL debug** (normal, ~3× a
  release por −O0+`OGS_PROFILING`), **não** regressão nem contenção de máquina — para medir release, o
  @gerente-simulacao deve colocar a DLL release no slot debug (ou usar export release).

- **Ambiente:** release (`-O3 -DNDEBUG`, clang 23.1.5/llvm-mingw, Ninja), tick worker, headless, editor
  fechado, pt-BR. DLL release `ac3bcad` 1.522.688 B (SHA `B063B968…`); DLL release `e2f6b30` 1.499.648 B
  (SHA `34D9517D…`); DLL debug real restaurada 18.956.800 B (SHA `3979FBB2…`, idêntica ao `.real_debug`).
- Nenhuma cena principal aberta; editor não morto; nenhum processo de teste morto.
