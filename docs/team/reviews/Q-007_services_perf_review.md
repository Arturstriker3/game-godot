# Q-007 — Revisão dos commits 03daeec (macro OGS_CAT) e 80f9d7b (perf serviços O(1))

- **Commits revisados:** `03daeec` (fix macro `OGS_SIM_ZONE` com indireção `OGS_CAT`),
  `80f9d7b` (entrada de negócios de serviços O(1) + remoção de varredura morta).
- **Pedido:** `docs/team/QUADRO.md` (@gerente-simulacao → @gerente-qualidade, revisão
  S-001: ressalva Q-006 + perf serviços v2).
- **Veredito:** **APROVADO**.

---

## 1. Diff de `03daeec` (macro `OGS_CAT`)

| Gate | Resultado |
| --- | --- |
| Indireção `OGS_CAT_INNER`/`OGS_CAT` antes da colagem | ✅ `profiling.h:98-100` — expande `__LINE__` e gera identificador único por linha |
| Guarda de regressão `zone_same_scope_compile_guard` | ✅ `profiling.cpp` — duas `OGS_SIM_ZONE` no mesmo escopo, só sob `OGS_PROFILING` |
| Custo zero em release | ✅ sem `OGS_PROFILING`, `OGS_SIM_ZONE` vira `((void)0)` — a guarda compila como função vazia (`[[maybe_unused]]`) |

A correção é a indireção padrão de dois níveis (`OGS_CAT(a,b) → OGS_CAT_INNER(a,b) a##b`),
que força a expansão de `__LINE__` **antes** da colagem. Sem ela (`##__LINE__` cola o
literal), duas zonas no mesmo escopo colidiam no identificador `_ogs_zone___LINE__`. A
guarda de compilação reproduz exatamente esse cenário (duas zonas no mesmo escopo) e, se a
macro regredir, o build debug falha aqui. **Correto e aprovado** — fecha a ressalva
obrigatória do Q-006.

## 2. Diff de `80f9d7b` (entrada de negócios O(1))

A mudança substitui a varredura `O(setores × negócios)` da entrada de negócios por uma
contagem `active_by_sector[]` acumulada **na mesma passada** que soma `sector_supply`
(passo 4), lida em O(1) no passo 9; e remove o laço morto `active_businesses_total`.

**Conferência de equivalência (bit-idêntica) — nenhum caminho esquece o contador:**

- `active_by_sector` é um **array local na pilha**, recomputado a cada `demo_services_day`
  (passo 4), a partir da fonte autoritativa `demo_service_by_city_` + flag `bankrupt`.
  **Não há estado persistido novo** → sem impacto em save/load nem em migrações (o contador
  não é serializado; ele é derivado do estado real a cada dia).
- O passo 4 conta `!bankrupt && sector ∈ [0, kDemoMaxServiceSectors)` — exatamente a
  condição que a varredura antiga contava (`!bankrupt && sector == s`).
- **Entre o passo 4 e a leitura no passo 9, nenhum negócio abre nem fecha:**
  - passos 5 (mercados), 6 (produção/lucro — só atualiza `loss_days`, **não** seta
    `bankrupt`), 7 (empregos) e 8 (preço) são somente leitura sobre o estado dos negócios;
  - a **abertura** acontece no próprio passo 9 (`demo_service_open`), **depois** da leitura
    de `active_by_sector[sector]`, e cada setor é processado no máximo 1×/dia (laço único
    `for sector ... && opens < kDemoServiceOpensPerDay`);
  - a **falência** (`demo_service_close`, que seta `bankrupt = true`) é o laço de
    fechamento, **depois** do laço de entrada.
  Portanto `active_by_sector[sector]` lido no passo 9 == contagem antiga `existing`, para
  qualquer setor, em qualquer dia.
- `demo_service_open` incrementa e `demo_service_close` decrementa
  `city.service_business_count`, e o fim da função **reconta** esse campo — comportamento
  intacto. `demo_service_by_sector_` é mantido na abertura e não é limpo na falência
  (negócio só vira `bankrupt = true`); é candidato a limpeza em S13, **pré-existente**, fora
  do escopo desta correção.

**Conclusão: a otimização é bit-idêntica e correta.**

## 3. Goldens S0 + bateria + sweep

**Goldens S0 (`economy_refactor_equiv.gd`): 3/3 com 0 diferenças** (verificador-flash APROVADO).

| Golden | save_sha256 | status_sha256 | Resultado |
| --- | --- | --- | --- |
| `real_60_ai` | 0f6e7870…b16a1 | b6ecd262…6415 | ✅ 0 diffs |
| `real_60_noai` | 8dc96775…fdb0 | 3a16be88…817d | ✅ 0 diffs |
| `demo_365_noai` | 4239fc9b…b703 | 3b73f514…21d0 | ✅ 0 diffs |

**Bateria 8/8 — todos verdes (exit 0):** `economy_integration_test`,
`economy_ai_test`, `population_demo_test`, `production_chain_test`, `investment_test`,
`labor_market_test`, `pop_needs_test`, `services_test.tscn` (via cena).

**Sweep seed-padrão 365 d (`economy_world_test --ai=1 --days=365`, fx=real) — idêntico a
`docs/economy/perf/services_fix_world_sweep_v2.json` (variante `real`):**

| Métrica | esperado (v2.json) | medido | |
| --- | ---: | ---: | --- |
| falências de construções | 14 | 14 | ✅ |
| negócios de serviço | 48.077 | 48.077 | ✅ |
| falências de serviço | 13.495 | 13.495 | ✅ |
| SOL food | 0,986267218193767 | 0,986267218193767 | ✅ idêntico |
| DJI food | 0,394838716690075 | 0,394838716690075 | ✅ idêntico |

Nota: o `economy_world_test.gd` retorna exit 1 sem `--baseline` (limite de falências = 0,
e 14 > 0) — artefato do harness, não regressão; os números batem bit a bit com o baseline
commitado (reprodutibilidade confirmada, fechando a ressalva de evidência obsoleta do Q-005).

## 4. Perf A/B release — `f791f55` vs `80f9d7b`

**Método:** release (`-O3 -DNDEBUG`, clang++ 23.1.5), DLL release copiada sobre
`template_debug` (convenção D-030/F9), `perf_day_peak --days=30 --tick-mode=worker`,
headless, editor fechado (0 processos Godot), **3 runs cada, intercalados A/B/A/B/A/B**
para neutralizar drift de máquina. Mundo **858 → 1.925 construções (máx 1.943), idêntico
nos dois commits** (comparação justa) — confirma também o comportamento bit-idêntico no
nível do mundo (não só no golden).

| Commit (DLL release) | run | tick média | **tick p95** | tick máx |
| --- | --- | ---: | ---: | ---: |
| `f791f55` (A, sem opt) | 1 | 57,40 | 65,16 | 65,68 |
| `f791f55` (A) | 2 | 56,86 | 63,83 | 65,94 |
| `f791f55` (A) | 3 | 55,51 | 64,34 | 66,72 |
| `80f9d7b` (B, com opt) | 1 | 55,42 | 64,98 | 65,14 |
| `80f9d7b` (B) | 2 | 57,38 | 65,69 | 67,69 |
| `80f9d7b` (B) | 3 | 55,75 | 65,30 | 66,49 |

- **`f791f55`:** p95 ∈ [63,83 … 65,16] ms (média 64,44; mediana 64,34)
- **`80f9d7b`:** p95 ∈ [64,98 … 65,69] ms (média 65,32; mediana 65,30)
- **Delta: +0,88 ms (+1,4%)** — dentro do ruído (amplitude intra-commit ~1–2 ms;
  sobreposição total entre os dois commits).

**Conclusão:** a otimização **NÃO regride o tick release** (o delta é ruído). O ganho de
−25% na fase `services` (29,44 → 21,94 ms, medido em **debug**) não aparece no p95 do tick
release: o `services_entry` custa só ~2,5 ms em release, pequeno frente ao tick total
(~65 ms) e à amplitude de ruído. A fase `services_businesses` (~9,3 ms debug) segue como o
maior custo de serviços — alvo de S13/S15, como já registrado em `services_entry_opt.md` §5.

## 5. Veredito

**APROVADO.** Os dois commits são corretos e não regridem:

- `03daeec` fecha a ressalva obrigatória do Q-006 (macro `##__LINE__` → indireção
  `OGS_CAT`), com guarda de regressão de compilação; custo zero em release.
- `80f9d7b` é **bit-idêntico** (goldens S0 3/3 com 0 diffs; mundo idêntico 858 → 1.925
  construções nos 6 runs) e **não regride o tick release** (p95 ~64–65 ms nos dois commits,
  delta +0,9 ms = ruído). A contagem `active_by_sector` é derivada do estado real a cada
  dia (sem estado persistido novo), então não há caminho de abertura/falência/save/migração
  que esqueça o contador.

**Observação (não bloqueia):** o ganho de performance desta correção é mensurável só na
fase `services` em debug (−25%); em release o tick p95 não se move. A alavanca real de tick
continua sendo `services_businesses` (laço principal por negócio) — S13/S15.

- **Ambiente de medição:** release (`-O3 -DNDEBUG`), tick worker, headless, editor fechado,
  pt-BR. DLLs: `f791f55` → 1.506.304 B (worktree isolado); `80f9d7b` → 1.505.792 B.
- **DLL debug real restaurada** (`template_debug` = `DDE0B413…`, SHA-256 idêntico ao
  `.real_debug`).
