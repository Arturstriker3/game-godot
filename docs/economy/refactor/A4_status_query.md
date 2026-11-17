# A4 — Status sob demanda (`docs/plans/perf_pop_financas_plano.md` §A4)

> Branch `perf/a4-status-query`, worktree `opengs-wt-a4`, base
> `balance/global-resources` (1f14e24). Mudança de publicação/consulta: **não
> altera nenhum número da simulação** (save e status byte-idênticos ao golden
> A0). Decisão no `GAME_STATE.md` D-079.

## 1. Problema

Até a A3/A5 o núcleo montava um `Dictionary` do mundo inteiro **uma vez por
dia**, dentro do tick, e o guardava em buffer duplo: cidades (834 × ~45 chaves),
portos, pedidos, nações, moedas e tarifas. A IA já tinha saído do `Dictionary`
(A5/D-076), mas o custo continuava na fase `status`: **15,86 ms de mediana** no
bench A0 (`status_cities` 11,17 ms). O jogo pagava isso todo dia; os sweeps e o
CLI pagavam 365×.

## 2. Desenho

O tick publica apenas um **resumo leve** e o resto é **consulta sob demanda**,
lida direto do `WorldState` no fecho do dia:

```
tick_and_publish()  →  tick + IA + (semanal) retrato de trabalho   [status ≈ 0]
main thread, após join  →  get_day_summary()  (≈20 chaves, totais do dia)
HUD/mapa/histórico      →  get_city_columns() (Packed arrays por campo)
ficha de cidade         →  get_city_info(id)  / get_city_status(id)  (< 1 ms)
ficha de nação          →  get_nation_status(tag)
comboios/telas          →  get_trade_orders()
telas de dados abertas  →  Economy.ui_snapshot()  (montado só com a tela visível)
tools/testes/goldens    →  get_status()  (mundo inteiro, sob pedido)
```

### 2.1 API do binding (`EconomySim`)

| Método | Conteúdo | Custo |
| --- | --- | --- |
| `get_day_summary()` | `day`, caixas, `money_total`, contadores, violações, catálogo de produtos e os **totais do mundo** (população/produção/estoque/necessidade/consumo/déficit/trânsito), somados na ordem das cidades | ~0,05 ms |
| `get_city_columns()` | `count`, `id`/`nome`/`nação`/`moeda` (`PackedStringArray`), `x`/`y`, os **33 escalares** do bloco leve e `deposit_remaining_kt` (`PackedFloat64Array`) | ~0,3 ms, cacheado por dia |
| `get_nation_columns()` | `tag`/`nome`, `city_count`, população/produção/estoque/necessidade/déficit/trânsito | ~1 ms, sob demanda |
| `get_city_rows()` | Array de Dictionary **no mesmo formato** de `status["cities"]` (bloco leve) | ~13 ms quente, cacheado por dia |
| `get_city_info(id)` | uma linha leve (mesmo formato) | ~0,2 ms |
| `get_city_status(id)` | detalhe completo (mercados/pop/serviços/construções/jazidas) — já existia | **0,46 ms** medido |
| `get_nation_rows()` / `get_nation_status(tag)` | `status["nations"]` (frota + bloco `labor` semanal) | ~1–2 ms |
| `get_trade_orders()` | `status["trade_orders"]` do último dia | ~2,9 ms, cacheado por dia |
| `get_ports()`, `get_currency_rows()`, `get_tariffs()`, `get_products()` | os blocos equivalentes do status | ~0,1–1 ms |
| `get_light_columns()` | o bloco `light` do status (tabelas planas) | ~2–4 ms, cacheado por dia |
| `get_city_ranking(field, count)` / `get_nation_ranking(field, count)` | top-N por campo, ordenado desc, desempate por id/tag | lê as colunas |
| `get_data_version()` | contador que sobe a cada dia/mutação — a UI invalida o `ui_snapshot` por ele | O(1) |

Os blocos do status que viraram consulta foram extraídos do `get_status()` para
`build_ports`/`build_orders`/`build_nations`/`build_currencies`/`build_tariffs`
(movimentação de código, não reescrita) e `fill_city_status` passou a calcular
os escalares num POD (`city_scalars`) compartilhado com as colunas. O
`status_query_test` prova que cada consulta é **bit a bit** o campo
correspondente do status no mesmo dia.

### 2.2 O retrato semanal de `nations[].labor` (por que os goldens não mudam)

O bloco `nations[].labor` é publicado com cadência semanal desde a S17
(D-058), com o valor guardado **por slot** do buffer duplo: no dia `d`, cada
slot publica o agregado do seu último build em múltiplo de 7. Sem status diário,
um `get_status()` no dia 60 leria o estado do dia 60 e o hash mudaria.

A solução tem três peças:

1. o fecho do dia `day % 7 == 0` **captura o agregado em doubles puros**
   (`NationLaborSnapshot`, sem Variant), dentro da antiga fase `status`;
2. no caminho do **jogo/agendador** (que não constrói status todo dia), o
   primeiro build de um slot parado é **semeado** por esse retrato — o valor do
   último múltiplo de 7, nunca o de semanas atrás;
3. o caminho `tick_day()` (tools/testes/goldens) **mantém a cadência por slot
   exatamente como era** e desliga a semeadura. É o caminho de todos os goldens
   S0 e do `world_equivalence`: dia 60 publica o retrato do 56; o demo de 365
   publica o do 357, como a publicação diária antiga (o retrato, no jogo,
   publicaria o do 364 — uma semana mais fresco, não um número inventado).

Custo do retrato: uma passada sobre 834 cidades a cada 7 dias; a fase `status`
mede **0,009 ms** de média (o retrato é raro). Os goldens de 60 d
(`4f18522d…`) e os três S0 (`real_60_ai`, `real_60_noai`, `demo_365_noai`)
ficam byte-idênticos.

### 2.3 Thread e memória

- O tick continua sem criar/transportar Variant: `tick_and_publish()` só roda
  C++ e deixa o retrato semanal; `get_day_summary()` é chamado **na main
  thread, depois do join** (`tick_scheduler._collect`), e devolve um Dictionary
  novo (imutável para o consumidor).
- As consultas usam cache por dia, invalidado por `status_invalidate()` (as
  mutações do jogador entre dias). Nada disso participa do caminho diário.
- A memória do jogo **cai**: o autoload não chama mais `warm_status()` (dois
  slots de ~20 MB cada) e o cache de linhas (`get_city_rows`) só nasce quando
  uma tela de dados é aberta. `warm_status()` continua existindo para
  tools/testes que comparam o status completo.
- O buffer duplo permanece apenas para as chamadas de `get_status()` de
  tools/testes (o `economy_tick_async_test` exige segurar o snapshot de ontem);
  ele saiu do caminho diário do jogo, que não monta mais o mundo.

## 3. Inventário dos leitores migrados

| Consumidor | Antes | Agora |
| --- | --- | --- |
| `core/economy/tick_scheduler.gd` | `get_status()` após o join | `get_day_summary()` |
| `core/economy/economy.gd` | publicava o status; `trade_orders` do status | publica o resumo leve; `get_trade_orders()` no sinal |
| HUD barra superior | soma das cidades (população/estoque/déficit/trânsito) | totais do `get_day_summary()` |
| HUD lista de cidades | linhas de cidade | colunas `id/nome/nação/população/déficit` |
| HUD alertas (`core/ui/alerts.gd`) | `summary["cities"]` | colunas `id/need/stockpile/deficit/production` |
| HUD histórico (`core/stats/history_service.gd`) | `summary["cities"]` + `light` | colunas (população/PIB/produção/necessidade/reserva/estoque/renda) |
| HUD log de pedidos / tela Comércio | `summary["trade_orders"]` | `get_trade_orders()` + `Economy.ui_snapshot()` |
| HUD "ver no mapa"/breadcrumb | varria `summary["cities"]` | `get_city_info(id)` / coluna `nation` |
| Mapa — modos de calor (`map/map_mode_layer.gd`) | Array de linhas | colunas `x/y/population/production/industrialization` |
| Mapa — ícones/território (`map/city_world.gd`) | `refresh_modes(cities)` | colunas; território recalcula só quando `nation` muda |
| `game/main_game.gd` — build | `get_status()` | `get_city_rows()` + `get_ports()` (uma vez) |
| Comboios (`core/economy/trade_convoys.gd`) | `status["trade_orders"]` (via sinal `trade_orders_ready`) | o sinal leva `get_trade_orders()` (o comboio em si nunca leu o status) |
| Ficha de cidade (`core/ui/city_panel.gd`) | `summary["cities"]` | `get_city_info(id)`, `get_trade_orders()`, colunas para produtores |
| Telas de dados (Economia, Mercado, Comércio, Produção, População, Política, Estatísticas, Logística, Serviços, Alertas) | `summary` completo | `Economy.ui_snapshot()`, montado **só com tela aberta** (usa `get_city_rows/nation_rows/ports/currency_rows/tariffs/service_sectors/light/trade_orders`) |
| Tools de captura/template | `Economy.last_summary` como mundo | `ui_snapshot()`/colunas/consultas; `last_summary` agora é o resumo leve |
| `get_status()` | publicado todo dia | só tools/testes/goldens |

## 4. Evidência

- **Equivalência A0** (`tools/world_equivalence.ps1 -Days 60 -Rounds 1`):
  `save dcad384c67b5…`, `status 4f18522de942…` — **0 diferenças**.
- **Goldens S0** (`tools/economy_refactor_equiv.gd`): `real_60_ai`
  (`4c69889f…`), `real_60_noai` (`8d6f3d44…`) e `demo_365_noai` (`65a414f0…`) —
  **0 diferenças** nos três (save e status).
- **TURBO × normal** (`tools/turbo_equivalence_test.gd`): A/B `save dcad384c…`/
  `status 4f18522d…`; C/D `15f703dd…`/`f4e0b4d1…`/log `70e6ef33…` — 4 mundos de
  60 d iguais.
- **Consultas × status** (`tools/status_query_test.gd`, mundo real, 8 d, IA +
  §19 + geografia): **989 comparações, 0 diferenças** — linhas, ficha de uma
  cidade, 33 colunas × 834 cidades, nações, pedidos, portos, moedas, tarifas,
  produtos, `light`, resumo/totais e rankings. Ficha de cidade: **0,46–0,73 ms**
  (média de 20 nas rodadas; alvo < 1 ms).
- **Bateria:** `economy_integration_test`, `economy_ai_test`,
  `population_demo_test`, `economy_save_load_roundtrip_test` (0 diffs),
  `command_replay_test`, `economy_tick_async_test` (60 d síncrono × assíncrono,
  269 polls, pico < 0,4 ms) e `convoy_lifecycle_test` (cena; 20 d, 426 viagens)
  — verdes.
- **Cena do jogo:** `godot --path . -- --turbo --bench-days=30` → 30 dias em
  3,9 s (7,17 dias/s), sem erro funcional (só os avisos de
  `Image.load_from_file` do atlas, pré-existentes, e os RID leaks de saída).
- **Captura de UI com janela:** HUD (`--set=minimal`) e **39 telas/estados**
  (`--set=full --screens=on`) em 1920×1080, `layout_report.json` com
  **0 issues** em todos. Captura de mapa (`map_capture --set=map3d
  --route-days=2`) OK.
- **Perf** (release de medição `OGS_PROFILE_RELEASE=ON`, 60 d, N=3):
  `status` **0,009 ms** (era 15,86 ms no A0); `status_cities`,
  `status_orders`, `status_nations` = 0. Tick mediano **39,10 ms**
  (39,40 / 39,10 / 36,84), variação 6,56% — **o host não estava ocioso**: a
  frente A6 rodava `construction_queue_test`/`services_test` no worktree
  `opengs-wt-a6` durante as três rodadas. Numa rodada isolada de 30 d a tick
  média foi 34,5 ms. JSON: `docs/economy/perf/a4_status_query_bench.json`.
  Memória estática no mundo real: **130,3 MB** no `perf_day_peak` (sem mapa
  3D; a leitura inclui as consultas de tool).
- **Bench a refazer:** em host ocioso, para fechar o critério de 3%.

## 5. O que NÃO mudou

- Nenhum número da simulação: save v14, decisões da IA, preços, produção,
  invariantes. A equivalência byte a byte é a prova.
- `prefers`/demo (`initialize_scenario`): os tools que usam `tick_day()`
  continuam recebendo o status completo (montado sob demanda) — nenhum contrato
  de teste mudou.
- A interface mínima (`SHOW_DATA_SCREENS=false`) não mudou: as telas continuam
  ocultas por decisão do dono; o caminho de `ui_snapshot()` existe e foi
  exercitado com `--screens=on`.
