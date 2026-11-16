# S2 — Profiler por fase (relatório de perfil e recomendação)

> Passo S2 do `docs/economy/core_refactor_plan.md` §7. Profiler extraído para
> `gdextension/src/sim/profiling.{h,cpp}`, com macro `OGS_SIM_ZONE`/`OGS_SIM_PHASE_LAP`/
> `OGS_SIM_COUNTER` que é **VAZIA em release** (custo zero — `OGS_PROFILING` só é
> definido no build debug, `CMakeLists.txt`). Nenhuma regra econômica mudou.

## 1. Aceite do passo (gates estruturais)

| Gate | Resultado |
| --- | --- |
| Build debug + release verdes | ✅ exit 0 (ninja + clang++, DLLs em dia) |
| Golden S0 inalterado (equiv. byte a byte) | ✅ 0 diferenças — `save_sha256 0f6e7870…b16a1`, `status_sha256 b6ecd262…6415` (idênticos a `docs/economy/refactor/baseline/real_60_ai.json`) |
| `demo_get_profile()` contrato | ✅ 15 fases + 4 sub-fases, mesmas chaves; `{}` com profiling off |
| Release: macro vazia (sem símbolo) | ✅ `llvm-nm` do `profiling.cpp.obj` release = TU vazio (628 B); DLL release sem `ogs::profile` |
| Bateria | ✅ integration / ai / pop_demo / tick_async / world_test 60 d — todos exit 0 |
| Verificação independente | ✅ verificador-flash **APROVADO** (5/5 itens) |

## 2. Condição de medição

- `tools/perf_day_peak.tscn -- --days=30 --tick-mode=worker --profile=1`.
- Mundo: 834 cidades, **113 produtos**, 179 nações; construções **858 (dia 1) → 1.925 (dia 30)** (máx 1.943).
- Memória estática: **542,5 MB** (idem F9 542,8–543,2).

Duas DLLs medidas:

- **release** (DLL release copiada sobre `template_debug`, como em D-030/F9): mede o **tick de parede** e a virada da main — mas as **fases internas ficam 0,0** por desenho do S2 (profiler desligado em release).
- **debug** (DLL debug com `OGS_PROFILING`): mede **cada fase** (é onde o profiler do S2 roda). O debug é ~3,0× o release (tick p95 199,8 vs 66,6 ms) — a ordem relativa das fases é a mesma; os valores absolutos não servem para o gate.

## 3. Tick de parede (release) — gate oficial

| Run | tick média | tick p95 | tick máx | VIRADA p95 |
| --- | ---: | ---: | ---: | ---: |
| 1 | 57,13 | **66,61** | 70,61 | 19,86 |
| 2 | 58,88 | 66,38 | 68,27 | 20,84 |
| 3 | 56,48 | 64,80 | 66,32 | 20,04 |

**tick p95 = 64,8–66,6 ms → NÃO atende ≤ 50 ms** (mesma conclusão do D-030/F9: o tick não fecha
50 ms sem S15/S16; aqui o mundo satura em ~1.925 construções e o p95 fica ~65 ms).

## 4. Top fases (debug, 30 dias, p95) — onde o tick passa o tempo

| fase | média | p95 | % do tick debug |
| --- | ---: | ---: | ---: |
| **status** (total) | 46,46 | **49,88** | ~25 % |
| └ status_cities | 35,65 | 39,04 | ~19 % |
| └ status_orders | 6,04 | 7,78 | ~3 % |
| └ status_nations | 4,31 | 5,61 | ~2 % |
| └ status_misc | 0,45 | 0,58 | ~0 % |
| **trade** | 36,22 | **44,87** | ~20 % |
| **arrival** | 33,90 | **40,18** | ~18 % |
| **services** | 22,75 | **27,38** | ~12 % |
| income | 7,61 | 10,44 | ~4 % |
| raw | 6,21 | 7,54 | ~3 % |
| prices + clamp | 7,66 | 10,75 | ~4 % |
| accounting | 3,39 | 5,00 | ~2 % |
| reset + labor + production | 4,34 | 5,92 | ~2 % |
| fx + fiscal + invariants | 2,39 | 3,34 | ~1 % |

(O harness reporta média/p95/máx, não p50; as frações são sobre o tick debug p95 199,81 ms.)

## 5. Gate `status` — como ler

O gate **`status` p95 ≤ 30 ms** é definido em **release** (F9 25,6–27,1 ms; D-030 28,2–29,2 ms).
A partir do S2 o profiler **não existe em release** (custo zero, decisão do plano §7.1), então o
`status` p95 **não é medível em release** — só em debug (49,88 ms, inflado ~3×). Como o S2 é
**golden 0** (nenhuma regra/estrutura do status mudou, só a instrumentação), o `status` em release
**não mudou**: segue ~25–29 ms (atendendo ≤ 30), conforme D-030/F9. Para re-medir `status` em
release é preciso um build intermediário (ex.: `RelWithDebInfo` com `OGS_PROFILING`), não o
release de custo zero — pendência de processo registrada abaixo.

## 6. Recomendação — onde S5–S14/S16 dão mais ganho

Ordem de alavanca, a partir do §4 (e do plano §3.3/§4.1):

1. **S7 (status_publish) + otimização do status (§8.2)** — `status` é a maior fase (~25 % do tick;
   `status_cities` sozinha ~19 %). Mitigação §8.2-1 (publicar só mercado de produto **ativo**) é a
   de maior ganho/custo: a cidade típica move poucos dos 113 produtos. É onde S7 deve focar.
2. **S16 (paralelismo por cidade)** — as fases paralelizáveis (`status_cities`? não — mantém no
   writer; mas `arrival`, `services`, `income`, `raw`, `production`, `labor`, `prices`/`clamp`)
   somam ~60 % do tick. Plano §3.3 estima 40–60 % do tick com 4–8 threads. É o único caminho
   para o gate `tick ≤ 50 ms` voltar a fechar.
3. **S9 (trade) + S8 (logistics/arrival)** — `trade` (~20 %) + `arrival` (~18 %) são as fases
   seguintes; a unificação do matching (S9) e a chegada/frota/portos (S8) abrem espaço para
   S15 (SoA) e S16 nessas fases.
4. **S13 (services)** — `services` ~12 %; o vetor global de negócios é o gargalo (partição por
   cidade com seção crítica curta, §4.1).
5. **S15 (SoA)** — só **se** o profiler (S7+) mostrar ≥15 % e ≥1,5 ms/dia numa fase alvo
   (`trade`, `clamp`, `prices`, agregação nacional). Sem medição fina, não antecipar.

Conclusão: **S7 (status) é a maior alavanca única de curto prazo; S16 (paralelismo) é o que fecha
o gate de 50 ms.** S9/S8/S13 são a base modular que precede S16.

## 7. Pendências e observações

- **Processo:** re-medir `status`/`status_cities` em release exige um build `RelWithDebInfo`
  (ou equivalente) com `OGS_PROFILING`, já que o release de custo zero não mede fases. Registrar
  o procedimento antes de S7 (que mede `status` p95).
- **Macro `OGS_SIM_ZONE` (profiling.h:98):** `_ogs_zone_##__LINE__` cola o literal `__LINE__`
  (não expande a linha) — hoje há **uma única** `OGS_SIM_ZONE` (a fase `status`), então compila;
  **duas zonas no mesmo escopo quebrariam o build debug**. Corrigir com a indireção `OGS_CAT`
  antes de S3/S7 introduzirem mais zonas. (Cópia literal do §7.1 do plano.)
- **`SinkScope::~SinkScope`** restaura o sink mas não rearma `t_mark` — o primeiro `lap` após sair
  de um sink aninhado é descartado sem gravar. Sem efeito hoje (o sink de status é o mais interno
  e a fase `status` é fechada pelo `Zone`); registrar ao introduzir sinks aninhados.
- Sub-fases finas (§7.2: `production.capacity`, `trade.offers`, …) e export JSON/Tracy (§7.3–7.4)
  ficam para passos posteriores; `OGS_SIM_COUNTER` já existe mas ainda não tem chamador.
