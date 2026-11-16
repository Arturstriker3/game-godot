# Q-009 — Revisão do pré-S6 (round-trip save/load) + S6 (serialização → `edge/save_load`)

- **Commits revisados:** `93c5440` (pré-S6: correção do round-trip do mundo real — estado + ordem + UTF-8)
  e `5ae696c` (S6a) · `7e8f673` (S6b-1) · `f9c4133` (S6b-2) — a serialização sai de `economy_sim.cpp`
  para `edge/save_load.{h,cpp}`; `a4ddaf3` (docs: D-042 + perfil S7 no QUADRO/GAME_STATE).
- **Pedido:** `docs/team/QUADRO.md` (@gerente-simulacao → @gerente-qualidade, revisão S-001/S6 + perfil S7,
  junto com o pré-S6 `93c5440`, conforme decisão do head).
- **Veredito:** **APROVADO** (com uma observação de processo sobre a medição do status em release — §3).

---

## 1. Diff — campos aditivos, `edge/` isolado, 0 `godot_cpp` em `sim/`

| Gate | Resultado |
| --- | --- |
| Campos novos no save são **aditivos** e compatíveis com save v11 antigo | ✅ 5 campos, todos com default no load (`data.get(key, default)`): `bankruptcies` (int, 0), `depleted_mines` (int, 0), `industry_seeded` (bool, false) na raiz; `bankrupt`/`depleted` (bool, false) por construção. **Schema continua v11** (sem migração). |
| Save v11 antigo carrega **sem erro** e com defaults sensatos | ✅ testado (guarda nova `economy_save_load_v11_compat_test.gd`): removendo os 5 campos de um save atual, `demo_from_dict` carrega e re-serializa com `bankruptcies=0`, `depleted_mines=0`, `industry_seeded=false`, `bankrupt=false`, `depleted=false`. |
| 0 `godot_cpp` em `sim/` (gate §2.2) | ✅ nenhum `#include <godot_cpp…>` em `gdextension/src/sim/` (as 6 ocorrências da string são comentários "nada de godot_cpp"); só STL e headers internos. |
| `edge/` isolado | ✅ `edge/save_load.{h,cpp}` é a única TU nova com `godot_cpp` (`dictionary/string/array/variant`); vive em `edge/` (não `sim/`), como manda o plano §3.2. `CMakeLists.txt` só adiciona `src/edge/save_load.cpp`. |

O `93c5440` corrige três causas do round-trip divergente do mundo real (achado Q-008, pré-existente):
(1) estado verdadeiro não serializado (`industry_seeded`/`bankruptcies`/`depleted_mines`/marcas por
construção `bankrupt`/`depleted`); (2) ordem não determinística de 3 `unordered_map`
(`group_flows`/`confidence`/`minimum_wages`) → `std::sort` por nação; (3) encoding assimétrico
`String(const char*)`=Latin-1 × `.utf8()`=UTF-8 → helper `sim_str()` = `String::utf8()`.

O S6 é **realocação byte-idêntica**: `demo_to_dict` (~508 l.) → `ogs::edge::to_dict`; `demo_from_dict`
(~884 l., migrações v1..v11) → `ogs::edge::from_dict`; 4 helpers de load/setup
(`demo_rebuild_tariff_lookup`/`demo_apply_labor_profile`/`demo_seed_groups`/`demo_recipe_index`) →
`SimCore`; `demo_sane_token` → `ogs::demo_sane_token` (inline). A fachada mantém lock + delegar +
invalidar cache de status (`demo_from_dict` zera os dois slots e incrementa as versões antes de
delegar). `economy_sim.cpp` −1.610 linhas; `edge/save_load.cpp` +1.539; `sim/sim_core.cpp` +150.

## 2. Goldens S0 + round-trip + bateria + não-ASCII (executor-flash + verificador-flash)

**Goldens S0 (`economy_refactor_equiv.gd`): 3/3 com 0 diferenças.**

| Golden | save_sha256 | status_sha256 | Resultado |
| --- | --- | --- | --- |
| `real_60_ai` | `991ee806…0e82ab` | `b6ecd262…4116415` | ✅ 0 diffs |
| `real_60_noai` | `e41dad60…6607` | `3a16be88…817d` | ✅ 0 diffs |
| `demo_365_noai` | `c9449042…8725` | `3b73f514…21d0` | ✅ 0 diffs |

Os `save`/`status` sha256 são **idênticos aos regenerados no `93c5440`** (prova de que o S6 não alterou
nada da simulação nem da serialização — realocação pura). O `status_sha256` é o mesmo do pré-S6
(prova de que a simulação continua intacta).

**Round-trip `economy_save_load_roundtrip_test.gd` — 0 diffs nos dois modos:**

| Modo | comando | direto vs split | Resultado |
| --- | --- | --- | --- |
| mundo real | `--mode=world --days=60 --save-at=30` | `e41dad60…`/`3a16be88…` | ✅ 0 diffs |
| demo (AGRIA/PORTUS) | `--mode=demo --days=40 --save-at=20` | `913b6b1a…`/`4ba7da2b…` | ✅ 0 diffs |

Estendi a guarda para aceitar `--mode=demo` (o `_make_world` usa `initialize_demo()` + geografia,
mesmo setup do `economy_integration_test`).

**Bateria 8/8 — todos verdes (exit 0):** `economy_integration_test`, `economy_ai_test`,
`population_demo_test`, `production_chain_test`, `investment_test`, `labor_market_test`,
`pop_needs_test`, `services_test.tscn` (via cena — `services_test.gd` é `extends Node`, então a forma
`--script` trava sem output; o próprio cabeçalho manda rodar a `.tscn`).

**Nome não-ASCII (guarda nova `economy_save_load_v11_compat_test.gd`):** os **834 nomes de cidade**
do save batem com o `cities.json` original (incl. Córdoba, São Paulo, Brasília, Québec, Bogotá…) **antes
e depois do load** — sem mojibake (`Córdoba` não vira `CÃ³rdoba`). Verificador-flash **APROVADO**
(re-executou `demo_365_noai`, round-trip demo e compat v11 de forma independente; conferiu os baseline
files `real_60_ai`/`real_60_noai` como tracked/limpos; DLL intacta 15.838.208 B, hash `82B3AB14…`).

## 3. Perf — tick release não regride; status "≤30 ms release" é herdado (não re-medível em release)

**Tick p95 release (3×, `perf_day_peak --days=30 --tick-mode=worker`, headless, editor fechado):**

| run | tick média | **tick p95** | tick máx |
| --- | ---: | ---: | ---: |
| 1 | 45,65 | 48,74 | 48,76 |
| 2 | 45,18 | 48,74 | 48,79 |
| 3 | 45,94 | 48,76 | 48,92 |

- **HEAD `a4ddaf3`:** p95 ∈ [48,74 … 48,76] ms (mediana 48,74). Mundo 858 → 1.925 construções (máx 1.943).
- **A/B vs `7156d4f` (baseline do S5, Q-008):** 48,74 vs **48,81** ms → **−0,07 ms (ruído), não regride**.
  Coerente com a realocação byte-idêntica: os goldens 0 diffs já provam que o S6 executa as mesmas
  operações — a performance não pode ter regredido.

**Status p95 — observação de processo (não bloqueia):** a fase `status` é medida só com `OGS_PROFILING`,
que desde o S2 é compilado **apenas em Debug** (`CMakeLists.txt` linha 40–42). Em release, as macros
expandem para `((void)0)` e o perfil de fases sai zerado — **não é possível medir a fase `status` em
release** sem um build `RelWithDebInfo`+`OGS_PROFILING` (pendência já apontada no Q-006). Medido em
Debug (profiling ligado, mesmo método): **status p95 41,49 ms** (média 39,41; `status_cities` 32,59;
orders 5,54; nations 3,57; misc 0,37) — 27% do tick Debug (152 ms), consistente com o perfil do S7
(42,8 ms / 27,1%). A afirmação **"status p95 release ≤30 ms (28–29 ms)" é herdada da F7/D-024
(`docs/economy/perf/refactor_status.txt`, período pré-S2)**, quando o profiling ainda era inline e a
DLL release media fases; após o S2 essa medição deixou de ser reproduzível em release. O S6 **não toca**
a fase `status` (só adiciona 2 campos triviais `bankruptcies`/`depleted_mines` ao dicionário), então
não há regressão possível de status.

## 4. Veredito

**APROVADO.** O pré-S6 corrige o round-trip do mundo real (estado + ordem determinística + UTF-8) e o S6
é uma realocação byte-idêntica e correta:

- 5 campos novos **aditivos** no save v11 (defaults sensatos, sem migração — decisão correta, ver abaixo);
  `sim/` 0 `godot_cpp` (gate §2.2); `edge/` isolado.
- Goldens S0 **3/3 0 diffs** (save/status sha **inalterados**); round-trip **0 diffs** no mundo real **e**
  no demo; bateria **8/8**; nomes não-ASCII preservados (834/834).
- Perf release **não regride** (tick p95 48,74 ms vs 48,81 ms em `7156d4f`; −0,07 ms = ruído).

**Sobre a pergunta do @gerente-simulacao (manter v11, não subir para v12):** a decisão é **correta**.
Saves v11 antigos carregam sem erro; `industry_seeded=false` restaura o comportamento pré-fix
(re-semeia a indústria de referência no próximo tick) e os nomes não-ASCII já gravados com mojibake
permanecem como estavam (não pioram). Uma migração v11→v12 retroativa seria **ambígua** (re-encodar
`CÃ³rdoba`→`Córdoba` não é distinguível de um nome legítimo), então o custo supera o benefício em
período de desenvolvimento. Registrado, não bloqueia.

**Observação (não bloqueia, processo):** para tornar o "status p95 release ≤30 ms" **re-medível** (e não
só herdado da F7), agendar um build `RelWithDebInfo`+`OGS_PROFILING` antes de qualquer futura fatia que
toque a fase `status` (S7 definitivo). O portão **tick ≤50 ms** segue dependente de S16/S15, como o S7
corretamente conclui.

- **Ambiente:** release (`-O3 -DNDEBUG`), tick worker, headless, editor fechado (nenhum processo Godot),
  pt-BR. DLL release 1.522.688 B (SHA `318847F4…`); DLL debug real restaurada 15.838.208 B
  (SHA `82B3AB14…`, idêntica ao `.real_debug`).
- Nenhuma cena principal aberta; editor não morto.
