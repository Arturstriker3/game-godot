# A7 — Comércio paralelo determinístico

> Fase **A7** de `docs/plans/perf_pop_financas_plano.md` (Parte A). Branch
> `perf/a7-trade-parallel`, worktree `opengs-wt-a7`, saída de
> `balance/global-resources` (`fcfc107`). **Não muda nenhum número**: o golden
> A0 continua idêntico (save `dcad384c…c06a`, status `46047575…b53d4`) e o
> resultado é o mesmo com 1, 8 e 16 threads.
>
> Escopo de código: `gdextension/src/sim/trade.cpp` (com o mínimo de
> `profiling.{h,cpp}`, `sim_types.h`, `sim_core.h`, `economy_sim.cpp` e
> `tools/perf_day_peak.gd` para medir). `core/economy/ai/`, o binding e o
> save/load não foram tocados.

## 1. Perfil primeiro: onde o tempo do comércio está

Medição release com perfil (`gdextension/build-profile-release`,
`-DOGS_PROFILE_RELEASE=ON`), 30 d, 834 cidades, IA + urbano, `tick-mode=worker`,
1 rodada (30 amostras por fase), **antes** da mudança:

| fase | ms (média) | p95 | o que está dentro |
| --- | ---: | ---: | --- |
| `trade` | 6,45 | 8,38 | comida + bens |
| `trade_food` | 5,56 | 7,23 | comércio de alimento |
| ↳ *preparo* (por subtração) | 0,99 | 1,13 | `food_ge`, trânsito, `city_port`, grade, ofertas por slot |
| `trade_food_buyers` | 4,58 | 6,10 | o laço de compradores |
| ↳ candidatos do anel | **3,02** | 3,81 | anel K vizinhos + mercado externo + `sort/unique` |
| ↳ ofertas | **1,09** | 1,76 | frete + tarifa + `stable_sort` |
| ↳ liquidação | 0,30 | 0,34 | mutação de estado (orçamento, frota, porto, pedidos) |
| `trade_goods` | 0,89 | 1,09 | bens não alimentares (laço 0,36; o resto na varredura por produto) |
| `arrival` | 5,59 | 7,50 | referência (código não tocado nesta fase) |

Leitura: **96% do laço de compradores é cálculo puro e só-leitura** (candidatos
3,02 + ofertas 1,09) e só 0,30 ms muta estado. É o inverso de `arrival`, e é o
que torna a A7 viável.

Duas notas de método, porque custaram uma rodada de medição:

1. `ogs::profile::Zone` **atribui** o slot na saída (`profiling.cpp`), então uma
   zona dentro de um laço de 800 compradores media só o **último** deles
   (0,0036 ms em vez de 3,0 ms). A fase passou a ter `ZoneAdd`
   (`OGS_SIM_ZONE_ADD`, soma) e `reset_phase` (`OGS_SIM_PHASE_RESET`, zera no
   topo do dia); só depois disso os números acima fecharam com
   `trade_food_buyers`.
2. `SinkScope` só é instalado na thread chamadora. Uma zona em volta de
   `pool().parallel_for` mede a **parede** da chamadora (inclui a espera pelos
   workers), mas os workers não têm sink. Em host carregado, a diferença entre
   essa zona e a soma das filhas (que são medidas na chamadora) cresce — foi o
   que fez `trade_food_buyers` aparecer com 18 ms enquanto as três filhas somavam
   4,5 ms. Em host ocioso a soma fecha (4,91 contra 4,88 na medição A/B).

## 2. Desenho: cálculo paralelo (só leitura) × liquidação serial (ordem fixa)

### 2.1 Por que a liquidação não pode ser paralela

O laço de compradores **não** é independente por comprador. Cada embarque muda o
estado que o próximo comprador lê:

- `sellable_ge[seller]` cai a cada embarque; ao chegar a zero o vendedor sai da
  lista de candidatos dos compradores seguintes (`scan_cell` filtra
  `sellable_ge <= 1e-9`) e `sellers_left` diminui — e `sellers_left` entra em
  `wanted`, ou seja, **até onde o anel de vizinhos cresce**;
- a frota (`trucks_busy`/`ships_busy`/`fuel`) é da nação e é consumida por
  `fleet_reserve`: o `fleet_capacity_scale` do embarque seguinte depende dos
  anteriores;
- a capacidade do porto (`used_today_t`, `queue_t`, `demurrage`) é global;
- `shipments >= kMaxShipmentsPerDay` é contador do dia;
- `ws_.last_orders` e `ws_.in_transit` são escritos em ordem — e a ordem do
  vetor entra no hash do save.

Logo: liquidação **serial, na ordem de hoje** (compradores por `gap/need`
decrescente, ofertas por custo por tonelada-equivalente). Nada de
`atomic<double>`, nada de soma paralela, nada de `-ffast-math`.

### 2.2 O que é cálculo e o que é decisão

Por comprador, o caro e independente do estado mutável é:

1. **Candidatos** — varredura do anel (grade de 32 unidades de mapa ≈ 250 km de
   célula; orçamento de 1.024 células; raio 48 com porto, 8 sem) mais o mercado
   externo (as M ofertas mais baratas por slot), com `sort`/`unique` no fim.
   Inclui as distâncias: hoje o comparador do `partial_sort` chama
   `distance_km` **duas vezes por comparação** (haversine com `sin`/`cos`/`asin`).
2. **Ofertas** — por candidato × slot da cesta: frete (haversine/porto), tarifa,
   competitividade, câmbio e custo entregue por tonelada-equivalente, mais o
   `stable_sort` por custo.

O que decide é pequeno: orçamento (firma/governo/poupança), teto de carga,
frota/combustível, porto, `TradeOrder`/`Shipment`.

### 2.3 Plano por comprador (`FoodTradePlan`)

A fase paralela grava, **por cidade compradora**:

| campo | conteúdo |
| --- | --- |
| `ring[]` | entradas do anel na **ordem exata de varredura** (`seller`, `radius`, `distance_km`), só vendedores elegíveis *no topo do dia* (`sellable_ge > 1e-9`), com a máscara de células e as regras de massa de terra/porto já aplicadas; `distance_km` calculada **uma vez** por entrada |
| `ring_complete` | `false` quando o anel foi cortado pelo alvo de entradas do plano |
| `ext[]` / `ext_sorted` | mercado externo na ordem de append (com repetidos) e a versão ordenada/única |
| `universe[]` | `únicos(ring ∪ ext_sorted)` em ordem **crescente de índice de vendedor** — o universo de ofertas |
| `offers[]` | ofertas do universo (vendedor crescente, slots da cesta), já **ordenadas por custo** pelo `stable_sort` do dia, feito uma vez aqui |
| `FoodScanTail` (scratch do dia) | `radius`/`budget`: fronteira de retomada da varredura, para a liquidação **continuar** a passada em vez de refazê-la |

`kFoodPlanSlack = 8` entradas além de `min(k_neighbors, reachable)`; medido:
folga 64 dava 72 entradas por comprador (3× o necessário) e dobrava o custo da
fase paralela.

### 2.4 Liquidação serial consumindo os buffers

Por comprador, na ordem de hoje:

1. **Replay do anel.** Percorre `ring[]` em ordem; pula vendedor já esgotado
   *agora*; conta os sobreviventes; a cada **fronteira de raio** repete o teste
   `count >= wanted`, com o `wanted` do momento (`min(k_neighbors, sellers_left,
   reachable)`). A varredura é função do dia; a parada é decisão do dia.
2. Se o plano foi cortado e os sobreviventes ainda não chegam a `wanted`,
   **continua** a varredura a partir de `(tail.radius, tail.budget)` — mesma
   passada, mesmo orçamento restante, entradas acumuladas contando no teste.
3. `partial_sort` das entradas acumuladas pelas **distâncias já calculadas**
   (mesma comparação, mesma permutação) e corte em `k_neighbors`.
4. União ascendente/única com `ext_sorted` (`std::set_union`).
5. **Ofertas**: `plan.offers` já está na ordem de custo; basta percorrer e
   filtrar pela marca O(1) de candidato (`food_candidate_stamp_[vendedor] ==
   bi + 1`) — sem copiar bloco e sem ordenar de novo. Se a retomada trouxe
   vendedor **fora** do universo (caminho lento, ~29% dos compradores), monta as
   ofertas na ordem de candidato com a **mesma** `append_seller_offers` e faz o
   `stable_sort` do dia — como antes.
6. **Liquidação**: o laço de hoje, byte a byte.

### 2.5 Por que é bit-idêntico

- A fase paralela **não escreve** em `ws_`; lê `cities`, `ports`, `markets`
  (preço/estoque), tarifas, câmbio e competitividade — nada disso muda dentro de
  `phase_trade` (preço de mercado muda na fase `prices`, depois).
- Uma oferta só depende de preço, frete, tarifa, competitividade e câmbio; a
  quantidade vendável (`sellable_ge`) entra **como filtro**, e o filtro de hoje é
  reaplicado na liquidação (`sellable_tons <= 1e-9` → `continue`, sem efeito
  colateral antes). Logo uma oferta pré-calculada vale para qualquer instante.
- O conjunto do plano é superconjunto do real para qualquer instante:
  `sellable_ge` só **diminui** no dia, então quem está em zero no topo nunca
  volta a ser elegível; a retomada cobre o resto.
- **Empates das ofertas**: hoje o vetor é montado na ordem de candidato
  ascendente (vendedor, depois slots) e recebe `stable_sort` por custo. O plano
  monta o universo naquela mesma ordem ascendente e recebe o **mesmo**
  `stable_sort`; como candidatos ⊆ universo e ambos são ascendentes,
  `candidatos` é **subsequência** de `universo`, e filtrar uma lista ordenada por
  custo preserva a ordem relativa de todos os pares — que é exatamente o que o
  `stable_sort` de hoje produz.
- Distância, frete, tarifa, porto e frota usam as **mesmas** funções, na mesma
  ordem de operações; nenhum acumulador compartilhado, nenhuma soma reordenada.

## 3. Implementação

| arquivo | mudança |
| --- | --- |
| `gdextension/src/sim/trade.cpp` | `trade_food` partido em preparo → cálculo (`pool().parallel_for`) → liquidação; `append_seller_offers` e `scan_ring` (retomável) extraídos; `kFoodPlanSlack`, `kFoodScanBudget`, `FoodScanTail` no anônimo; `trade_goods` **intocado** |
| `gdextension/src/sim/sim_core.h` | declarações privadas + `FoodRingEntry`/`FoodOffer`/`FoodBuyer`/`FoodTradePlan` e o scratch reaproveitado entre dias (`food_plans_`, `food_ring_live_`, `food_candidates_`, `food_candidates_union_`, `food_offers_live_`, `food_offers_settle_`, `food_candidate_stamp_`) — **nada serializado** |
| `gdextension/src/sim/profiling.{h,cpp}` | `ZoneAdd` (`+=` em vez de `=`), `reset_phase`, macros `OGS_SIM_ZONE_ADD`/`OGS_SIM_PHASE_RESET`; stubs `((void)0)` em release |
| `gdextension/src/sim/sim_types.h`, `economy_sim.cpp`, `tools/perf_day_peak.gd` | fases do perfil: `trade_food_prep`, `trade_food_calc`, `trade_food_settle`, `trade_goods_cand/offers/settle` |

Detalhes que não são óbvios no código:

- **Escopo do pool**: `parallel_for` roda serial em mundos com menos de 256
  cidades (`kThreadPoolMinCities`), então o cenário AGRIA/PORTUS e os testes de
  fixture continuam no caminho serial — sem sincronização em vazio.
- **Scratch**: os planos são reaproveitados entre dias (só crescem) e cada
  worker escreve apenas o seu índice; `scan_tails` é local à chamada.
- **Sem estado global novo**: nem `static` mutável, nem RNG, nem ordem de hash
  no caminho novo.

## 4. Aceite

Tudo na mesma DLL debug (`gdextension/bin/economy_gdextension.windows.template_debug.x86_64.dll`,
build do HEAD da branch), `--headless`, editor fechado.

| verificação | resultado |
| --- | --- |
| `tools/world_equivalence.ps1 -Rounds 2 -Baseline docs/economy/refactor/baseline/A0_60_urban.json` | **PASS**, 0 diferenças run1×run2 e contra o golden: save `dcad384c67b53a1b0b5152fda8bd315b9fc331a563e66270dbdf4894dc43c06a`, status `46047575196a7f60b7c708f15fa006dfa21b2789c9949c6b1093e021b93b53d4` |
| threads 1 / 8 / 16 (60 d, IA + urbano, seed 0) | os dois shas acima em **todos** |
| CLI × Godot (60 d, sem IA) | arquivos **byte-idênticos** (`sha256 CB3B3F3B…92B668`), comparador `--tolerance 0` → **0 diferenças** |
| `economy_integration_test` / `economy_ai_test` / `population_demo_test` / `economy_save_load_roundtrip_test` | verdes (round-trip **0 diffs**) |
| `command_replay_test` / `convoy_lifecycle_test` (cena) / `port_capacity_test` / `fleet_fuel_test` | verdes |

Bench canônico (`tools/bench_canonical.ps1`, 60 d × 3 rodadas, release de
medição no slot, editor fechado). **Ressalva de host:** outra frente rodava
Godot headless no mesmo host durante a rodada 3 (dois processos Godot
simultâneos), então o critério de repetibilidade do próprio bench falhou
(4,23% em `day_total_ms`, limite 3%) e a comparação absoluta com o A0 não é
válida. As fases que a A7 **não** toca servem de controle de máquina:

| métrica (mediana das 3 rodadas) | A0 (2026-11-17) | A7 (este run) | A7 normalizado¹ |
| --- | ---: | ---: | ---: |
| `trade` | 6,27 | 6,25 | **4,68** (**−25,3%**) |
| `trade_food` | 5,47 | 5,21 | **3,90** (**−28,6%**) |
| `trade_food_buyers` | 4,40 | 3,79 | 2,84 |
| ↳ `trade_food_calc` (paralelo) | — | 1,55 | 1,16 |
| ↳ `trade_food_settle` (serial) | — | 2,24 | 1,68 |
| `trade_goods` (não tocado) | 0,79 | 1,02 | 0,76 (−3%) |
| `arrival` (controle, não tocado) | 5,48 | 7,33 | ×1,34 |
| `status` (controle, não tocado) | 15,86 | 21,11 | ×1,33 |
| `tick_wall` | 36,58 | 49,93 | 37,40 (+2,2%) |

¹ dividido pelo fator de máquina medido nas **duas** fases intocadas
(`arrival` ×1,338 e `status` ×1,331 — concordam em 0,5%); os números do A0
vieram do baseline oficial `docs/economy/perf/baseline_A0.json`.

O laço de compradores caiu de **4,58 ms para 3,79 ms** (−17%) com o mesmo
trabalho de decisão; o ganho é menor que o custo do laço porque a liquidação
serial ainda paga o replay do anel, a filtragem das ofertas e a própria
liquidação (0,30 ms). O `tick` completo fica no ruído (+2%): o comércio é ~17%
do tick e as outras fases (prólogo, `status`, `services`) dominam.

## 5. Limites e o que ficou de fora

- **`trade_goods` continua serial** (~0,9–1,1 ms; 0,36 no laço de compradores).
  A varredura por produto e os dois `stable_sort` por produto não têm o mesmo
  recorte "cálculo × liquidação" e ficam para outra fatia; o aceite da A7 é a
  fase `trade` e a equivalência.
- **Caminho lento ~29% dos compradores** (1,5% a 51% por dia, conforme a oferta
  do dia): quando a retomada do anel traz vendedor fora do universo planejado, a
  liquidação monta as ofertas daquele comprador de novo, como antes. Custa
  ~0,4 ms/dia no pior caso. Caminho para eliminar: ordenar as ofertas extras por
  `(custo, vendedor, slot)` e fundir com a lista do plano — mesmo resultado,
  porque a lista do plano já está nessa ordem total.
- **Perfil de sub-fase só na chamadora**: ver a nota 2 do §1. O número confiável
  de parede é `trade_food`; as filhas servem para localizar custo, não para somar
  sob carga.
- **Ganho modesto por construção**: saiu ~1,3–1,6 ms de `trade` (≈25%, ver §4),
  não os ~4 ms do laço de compradores, porque a liquidação serial ainda paga o
  replay, a filtragem das ofertas e a própria liquidação. O próximo alvo do
  plano é `arrival` (≈5,5 ms, não tocado) e a cadência por sistema (A6).
- **Nada de save/migração**: nenhum campo novo serializado; schema inalterado.

## 6. Merge na linha principal e revalidação (2026-11-17)

`perf/a7-trade-parallel` (`1883d05`, base `fcfc107`) mesclada `--no-ff` em
`balance/global-resources` sobre a A5 + D-076. Conflito apenas em documentação
(`GAME_STATE.md`, `docs/team/QUADRO.md`), resolvido mantendo os dois lados; a
decisão foi **renumerada de D-075 para D-077** (D-075 é a A5 e D-076 a ordem do
dia da A5). O código auto-mergeou: `sim_core.h` junta o scratch do plano da A7
ao que a A5 já tinha, e os arquivos de `src/sim/ai/` e `src/sim/actions.*`
ficaram intactos.

| Verificação | Resultado |
| --- | --- |
| build debug + `--headless --import` | exit 0 |
| `economy_integration_test`, `economy_ai_test`, `investment_test`, `population_demo_test` | verde |
| `economy_save_load_roundtrip_test` / `command_replay_test` | 0 diffs / 0 diffs |
| `convoy_lifecycle_test` (cena) / `port_capacity_test` | verde |
| `world_equivalence` 2 rodadas × golden A0 atual | **0 diferenças** (`save dcad384c…c06a`, `status 4f18522d…ea612`) |
| threads 1 / 8 / 16 | os mesmos dois shas |
| CLI × Godot com IA (60 d) | **0 diferenças** campo a campo, money 2.177.839.209,15 |
| sweep 365 d, 2 variantes (`main_a7_world_sweep.json`) | peg `0817959a…` = recal3; sonda `8bf2ceda…` difere; 8 falências; drift −0,001; 179 nações com delta de comida 0,000 |

O sweep deixou de rodar 5 variações peg: `real`, `real_seed1`, `real_seed2`,
`half` e `xgs` fecham o MESMO `trajectory_hash` desde o Q-016, então
`tools/s19_world_sweep_parallel.ps1` passou a aceitar `-Variants` (padrão
`real,float`: a peg `real` e a sonda `--fx=float`) e
`tools/aggregate_s19_sweep.py` passou a aceitar `--variants`, tratando qualquer
nome começando com `float` como sonda — fora do gate de comida e do teto de
falências, dentro do portão de sensibilidade do câmbio.

Bench canônico N=3 na linha mesclada (`docs/economy/perf/a7_main_bench.json`):
dia = tick **59,80 ms**, `status` 20,89, `trade` **6,44** (`trade_food` 5,24 =
prep 1,41 + calc 1,58 + settle 2,21); as fases intocadas (`arrival` 7,83)
confirmam o host mais lento que no A0. A IA custa ≈ **12,5 ms/dia** (A/B no CLI
release: 45,2 ms/dia com IA × 32,7 sem). O critério de 3% entre rodadas não
fechou (5,38%): outra worktree (`opengs-wt-turbo`) rodava Godot headless e um
bench com janela no host durante as duas tentativas.
