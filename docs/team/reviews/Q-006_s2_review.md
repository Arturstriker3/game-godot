# Q-006 — Revisão do S2 (profiler por fase) + sweep baseline pós-serviços

- **Commits revisados:** `3bc10be` (S2: `sim/profiling.{h,cpp}`), `a97c787` (QUADRO),
  `8c34663` (sweep 365 d baseline de comida pós-serviços).
- **Pedido:** `docs/team/QUADRO.md` (@gerente-simulacao → @gerente-qualidade, revisão S-001/S2).
- **Veredito:** **APROVADO COM RESSALVA**.

---

## 1. Diff do profiler (`gdextension/src/sim/profiling.{h,cpp}`)

| Gate | Resultado |
| --- | --- |
| `sim/` é STL puro (0 `godot_cpp`) | ✅ grep `godot_cpp` em `sim/` = 1 ocorrência, e é um **comentário** (`profiling.h:7`) |
| Custo zero em release (`OGS_PROFILING` só debug) | ✅ `CMakeLists.txt:38-40` define `OGS_PROFILING` apenas sob `CMAKE_BUILD_TYPE STREQUAL "Debug"`; sem ela as macros expandem para `((void)0)` |
| Macros `OGS_SIM_ZONE`/`OGS_SIM_PHASE_LAP`/`OGS_SIM_COUNTER` | ✅ presentes e corretas (RAII `Zone`, `lap()`, `counter()`) |
| Release sem símbolo de profiling | ✅ DLL release (6FC2AA85, 1.506.304 B) ~13,3 MB menor que a debug; `profiling.cpp` compila como TU vazio em release |

### ⚠️ Macro `##__LINE__` (pendência confirmada)

`profiling.h:98`:

```cpp
#define OGS_SIM_ZONE(phase) ::ogs::profile::Zone _ogs_zone_##__LINE__(phase)
```

`##__LINE__` **cola o literal** `__LINE__` em vez de expandir o número da linha — todas as zonas
geram o mesmo identificador `_ogs_zone___LINE__`, e **duas `OGS_SIM_ZONE` no mesmo escopo
quebram o build debug** (redefinição de variável). Hoje há **uma única** zona
(`economy_sim.cpp:3758`, fase `status`), então compila; é bug **latente**.

**Correção exigida (antes de S3/S7)**, com a indireção `OGS_CAT`:

```cpp
#define OGS_CAT_(a, b) a##b
#define OGS_CAT(a, b) OGS_CAT_(a, b)
#define OGS_SIM_ZONE(phase) ::ogs::profile::Zone OGS_CAT(_ogs_zone_, __LINE__)(phase)
```

Não corrigi aqui: é arquivo do @gerente-simulacao (núcleo C++), e a correção exige rebuild
debug para validar.

---

## 2. Regressão de perf — medição comparativa (o cerne da pergunta)

**Pergunta:** tick p95 release agora 64,8–66,6 ms; antes ~55 ms (D-030/F9). O S2 piorou, ou a
diferença vem do conserto de serviços v2 (`b95c7f3`) / carga da máquina?

**Método:** isolei o S2 medindo `8c34663` (serviços v2 **sem** profiler) vs `a97c787` (serviços v2
**com** profiler), release, `perf_day_peak --days=30 --tick-mode=worker`, **3 runs cada**,
intercalados A/B/A/B/A/B para neutralizar drift de máquina.

**Ambiente:** build Release (`-O3 -DNDEBUG`), DLL release copiada sobre `template_debug`
(padrão D-030/F9); editor **fechado** (0 processos Godot); mundo **858 → 1.925 construções
(máx 1.943)** — idêntico nos dois commits (comparação justa).

| Commit (DLL release) | run | tick média | **tick p95** | tick máx |
| --- | --- | ---: | ---: | ---: |
| `8c34663` (sem S2) | 1 | 59,79 | 67,94 | 70,86 |
| `8c34663` (sem S2) | 2 | 55,21 | 63,79 | 64,92 |
| `8c34663` (sem S2) | 3 | 56,77 | 65,90 | 67,30 |
| `a97c787` (com S2) | 1 | 59,67 | 67,20 | 79,01 |
| `a97c787` (com S2) | 2 | 57,16 | 65,21 | 69,74 |
| `a97c787` (com S2) | 3 | 60,89 | 69,44 | 74,31 |

- **8c34663:** p95 ∈ [63,79 … 67,94] ms (média 65,88; mediana 65,90)
- **a97c787:** p95 ∈ [65,21 … 69,44] ms (média 67,28; mediana 67,20)
- **Delta: +1,40 ms (+2,1%)** — dentro do ruído (amplitude intra-commit ~4 ms, sobreposição total).

**Conclusão: o S2 NÃO piorou o tick release.** O custo zero em release está confirmado
empiricamente (a diferença 8c34663 → a97c787 é ruído). O salto ~55 → ~65 ms veio **junto do
serviços v2** (`b95c7f3`, que reescreveu a economia de serviços e mudou a atividade do mundo) —
não do S2. (O "~55 ms" é o baseline D-030/F9, pré-serviços; não re-medi o commit pré-serviços
neste estado porque a tarefa pediu `b95c7f3/8c34663` vs `a97c787`, e a exclusão do S2 já responde.)

---

## 3. Goldens S0 + bateria

**Goldens S0 (equivalência byte a byte via `economy_refactor_equiv.gd`): 3/3 com 0 diferenças.**

| Golden | save_sha256 | status_sha256 | Resultado |
| --- | --- | --- | --- |
| `real_60_ai` | 0f6e7870…b16a1 | b6ecd262…6415 | ✅ 0 diffs |
| `real_60_noai` | 8dc96775…fdb0 | 3a16be88…817d | ✅ 0 diffs |
| `demo_365_noai` | 4239fc9b…b703 | 3b73f514…21d0 | ✅ 0 diffs |

**Bateria 8/8 — todos verdes:**

| Teste | Resultado |
| --- | --- |
| `economy_integration_test` | ✅ OK |
| `economy_ai_test` | ✅ OK |
| `population_demo_test` | ✅ OK |
| `production_chain_test` | ✅ OK |
| `investment_test` | ✅ OK |
| `labor_market_test` | ✅ OK |
| `pop_needs_test` | ✅ OK |
| `services_test` | ✅ OK (via cena `.tscn` — é `extends Node`, não runner) |

---

## 4. Ressalvas

1. **(Obrigatória, antes de S3/S7)** Corrigir a macro `##__LINE__` com a indireção `OGS_CAT`
   (§1). Hoje não bloqueia (1 zona), mas a primeira segunda zona no mesmo escopo quebra o build
   debug.
2. **(Processo, não bloqueia)** O gate `status` p95 ≤ 30 ms deixa de ser medível em release: o
   S2 desliga o profiler em release por desenho (custo zero). Para re-medir `status`/`status_cities`
   em release antes de S7, é preciso build `RelWithDebInfo` + `OGS_PROFILING` (registrado no
   `s2_profile.md` §7).
3. **(Observação, não bloqueia)** `SinkScope::~SinkScope` restaura o sink mas não rearma `t_mark`
   (o primeiro `lap` após sair de sink aninhado é descartado). Sem efeito hoje; registrar ao
   introduzir sinks aninhados (§7 do relatório).

---

## 5. Veredito

**APROVADO COM RESSALVA.** O S2 está correto: STL puro em `sim/`, custo zero em release
(confirmado por medição), equivalência byte a byte 0 diffs (3/3 goldens), bateria 8/8. A ressalva
única obrigatória é a correção da macro `##__LINE__` (`OGS_CAT`) antes de S3/S7 — é do
@gerente-simulacao.

- **Ambiente de medição:** release (`-O3 -DNDEBUG`), tick worker, headless, editor fechado,
  pt-BR. DLLs: 8c34663 → `33D7E979…`; a97c787 → `6FC2AA85…`.
- **DLL debug real restaurada** (`template_debug` = `CB11B552…`, build-debug em dia).
