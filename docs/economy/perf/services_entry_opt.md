# Perf dos serviços v2 — otimização da entrada de negócios (Q-006 / ETAPA 2)

> A regressão do tick (~55 → ~65 ms release) veio do serviços v2 (`b95c7f3`),
> confirmada em Q-006. Aqui: identificação da sub-fase quente e otimização **sem
> mudar resultado** (golden S0 `real_60_ai` = 0 diffs).

## 1. Diagnóstico (build debug, `perf_day_peak --days=30 --tick-mode=worker --profile=1`)

Instrumentação temporária subdividiu a fase `services` em 5 sub-fases
(acumulada por cidade/dia; removida após o diagnóstico). Antes da otimização:

| Sub-fase | média | p95 | O quê |
| --- | ---: | ---: | --- |
| `services_businesses` | 9,31 | 11,96 | passo 6–8: receita/insumos/folha por negócio (O(negócios)) |
| `services_entry` | 7,50 | 9,94 | passo 9 abertura: **varredura O(setores × negócios)** do `existing` |
| `services_demand` | 5,23 | 6,35 | passos 1–5: matching de demanda (O(20 setores)) |
| `services_exit` | 0,86 | 1,09 | passo 9 falências |
| `services_food_reserve` | 0,77 | 0,95 | reserva alimentar (nova no v2, 1×/cidade/dia) |

**Achado:** a entrada (`services_entry`) era quase tão cara quanto o laço
principal, por causa do `existing`-count que varria **todos os negócios da
cidade para cada setor** com demanda não atendida (O(20 × negócios) por cidade).
Também havia um laço **morto** (`active_businesses_total`) que contava negócios
ativos e nunca era usado (só `(void)` no fim).

## 2. Otimização (bit-idêntica, golden 0)

Em `EconomySim::demo_services_day`:

1. **Entrada → O(1):** a contagem de negócios ativos por setor
   (`active_by_sector`) agora é feita **na mesma passada** que soma
   `sector_supply` (passo 4). O laço de entrada (passo 9) lê
   `active_by_sector[sector]` em vez de varrer os negócios por setor. Nenhum
   negócio abre/fecha entre o passo 4 e o passo 9 (abertura/fechamento são o
   próprio passo 9), então a contagem é idêntica à varredura antiga.
2. **Remoção de código morto:** o laço `active_businesses_total` (uma passada
   inteira sobre os negócios, resultado descartado) foi removido.

## 3. Resultado

| Métrica | Antes | Depois | Δ |
| --- | ---: | ---: | ---: |
| `services` p95 (debug) | 29,44 ms | 21,94 ms | **−7,5 ms (−25 %)** |
| `services_entry` média (debug) | 7,50 ms | 2,65 ms | −4,85 ms |
| `services_entry` p95 (debug) | 9,94 ms | 7,29 ms | −2,65 ms |
| golden `real_60_ai` | — | save `0f6e7870…`, status `b6ecd262…` | **0 diffs** |

Bateria relevante: `economy_integration_test`, `economy_ai_test`,
`population_demo_test`, `economy_tick_async_test`, `services_test` — **5/5 OK**.
Build debug+release verdes.

## 4. Release (tick de parede, 3 runs)

DLL release copiada sobre `template_debug` (convenção D-030/F9), worker, 30 dias:

| Run | média | p95 | máx |
| --- | ---: | ---: | ---: |
| 1 | 58,71 | 67,36 | 74,62 |
| 2 | 51,03 | 58,49 | 64,92 |
| 3 | 64,22 | 79,23 | 90,93 |

Amplitude alta (58,5–79,2 ms p95): a máquina estava sob contenção variável
(pump da IA > 1 s em dias esparsos). A evidência rigorosa é o **debug**
(29,44 → 21,94 ms na fase `services`, mesmas condições). Em release a otimização
vale ~2,5 ms na fase `services`; o run 2 (média 51,03 / p95 58,49) mostra o tick
típico próximo de 57 ms, mas a medição de p95 desta sessão não é conclusiva sob
a carga observada — re-medir com editor fechado e 3 runs intercalados A/B.

## 5. Pendência

A fase `services_businesses` (9,1 ms média debug, laço principal por negócio)
segue como maior custo de serviços; é o alvo natural de S13 (`services.cpp`) e
S15 (SoA/partição), não desta correção pontual. `demo_service_by_sector_` está
escrito mas nunca lido no caminho quente (candidato a limpeza em S13).
