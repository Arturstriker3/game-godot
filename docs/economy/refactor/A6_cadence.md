# A6 — Cadência por sistema, escalonada

> Fase **A6** de `docs/plans/perf_pop_financas_plano.md` (Parte A). Diferente de
> A1–A5 e A7, **esta fase muda o resultado de propósito**: a recalibração dos
> goldens foi aprovada pelo dono (decisão 2 de 2026-11-17).
>
> Branch `perf/a6-cadence`, worktree `opengs-wt-a6`, base
> `balance/global-resources` @ `941120c` (a A7 mesclada + D-076).

## 1. Inventário — o que roda hoje, em que cadência e a que custo

Medido com `tools/bench_canonical.ps1` (release `-DOGS_PROFILE_RELEASE=ON`,
headless, tick no worker, 834 cidades/179 nações, N=3, host ocioso). A coluna
"antes" é o bench da A7 (`docs/economy/perf/a7_main_bench.json`, mediana de 30 d
no portão): **dia 59,80 ms**, tick 59,80 ms, status 20,89 ms.

| Fase / subsistema | Cadência antes | Custo medido (ms, antes) |
| --- | --- | --- |
| `prologue` (retrato p/ invariantes) | diária | 6,56 (`prologue_snapshot` 6,56) |
| índices + preços de abertura | diária | 0,01 + 1,17 |
| `urban_land::phase_month` (preço do solo, decadência do sinal de zona) | **mensal global** (`day % 30 == 0`, todas as cidades) | ver §1.1 |
| `phase_arrival` (chegada + trânsito) | diária | 7,87 (7,83 + 0,04) |
| `phase_reset` (zera fluxos) | diária | 1,10 |
| `phase_fleet` (frota/porto) | diária | ver §1.1 |
| `phase_labor` (salários/emprego) | diária | 0,12 |
| `phase_production` | diária | 0,85 |
| `phase_raw` (minerais/soja/algodão/madeira) | diária | 0,87 |
| `phase_income` (renda, impostos, consumo alimentar) | diária | 0,76 |
| `phase_housing` (§19 W3: aluguel, demanda, ajuste mensal, incorporadora) | diária (fechamento mensal por cidade via `month_marker`) | ver §1.1 |
| `phase_services` (F8: demanda/faturamento diário + abertura/fechamento) | diária | 1,78 |
| `phase_trade` (comércio de comida + bens) | diária | 6,44 |
| `phase_accounting` (receita, lucro, PIB) | diária | 0,51 |
| `phase_prices` + `phase_clamp` | diária | 0,18 + 0,13 |
| `phase_fiscal` (prefeitura + equalização + grupos) | diária; prefeitura **mensal global** | 0,75 (city_gov 0,63) |
| `phase_fx` (câmbio float) | diária | 0,23 |
| `phase_invariants` | diária | 0,40 |
| IA (`ai_phase_.run`): retrato do dia | diária | ver §1.1 |
| IA privada/governo | **semanal escalonada** (`dia % 7 == índice % 7`) | — |
| IA de grupos (F9) | **mensal escalonada** (`dia % 30 == índice % 30`) | — |
| `status` (publicação) | diária | 20,89 |

O que o inventário já mostra antes de mexer:

- **O pico mensal existe**: solo e prefeitura rodam para as 834 cidades no mesmo
  dia (`day % 30 == 0`). É o único lugar em que a cadência atual concentra
  trabalho.
- **A IA já é escalonada** desde a A5/F9 (semana por índice para privado/governo,
  mês por índice para grupos). A A6 alinha os períodos a um estado único.
- **Os pesos diários que sobram não são de sistemas mensalizáveis**: `status`
  (publicação), `arrival`, `trade`, o retrato do prólogo e o retrato da IA são
  diários por contrato (A4/A7/A2 tratam deles, não a A6).
- **Trabalho (0,12 ms) não compensa sair do diário** e mexe em salário/renda, que
  sustenta a cesta de comida — fica diário (o plano diz "avaliar").

### 1.1 Custo por subsistema mensal/semanal (medido com zonas novas)

Bench de inventário (`docs/economy/perf/a6_inventory.json`, N=1, 60 d, antes de
qualquer mudança de comportamento): dia **58,09 ms**. As zonas novas separam os
sistemas candidatos a sair do diário:

| subsistema | zona no perfil | custo médio/dia (ms) | cadência antes |
| --- | --- | --- | --- |
| IA (fase inteira) | `ai` | **9,62** | diária |
| ↳ retrato do dia (`snapshot_ai_view`) | `ai_view` | **6,83** | diária |
| moradia (§19 W3) | `housing` | **0,40** | diária (fechamento mensal por `month_marker`) |
| decisões de serviço (abre/fecha) | `services_decisions` | **0,58** | diária |
| prefeitura (§19 W2) | `fiscal_city_gov` | **0,49** | diária; fluxo mensal global no dia 30 |
| solo/zonas (§19 W1) | `land_month` | 0,04 médio (**~1,2 por execução**) | mensal global (`dia % 30 == 0`) |
| frota (F4) | `fleet_day` | 0,008 | diária |
| `status` (publicação) | `status` | **19,94** | diária |
| `arrival` | `arrival` | 7,18 | diária |
| `trade` | `trade` | 5,72 | diária |
| retrato do prólogo (invariantes) | `prologue_snapshot` | 6,03 | diária |

Leitura do inventário (é o que definiu a proposta):

1. **O pico mensal existe e é o único**: solo e prefeitura rodavam para as 834
   cidades no mesmo dia (dia 30). O resto já estava espalhado (IA por índice).
2. **Os sistemas mensalizáveis são baratos** (0,4–0,6 ms/dia somados): tirá-los
   do diário rende pouco em tempo e rende em **pico** e em coerência de contrato.
3. **O custo diário restante não é de sistema mensalizável**: `status`
   (publicação, A4), `arrival`/`trade` (diários por contrato, A7), o retrato do
   prólogo (rede de segurança dos invariantes) e o retrato da IA (6,83 ms —
   pré-requisito de qualquer fatiamento da IA, não de cadência).
4. **Trabalho (0,11 ms) e frota (0,008 ms) não compensam sair do diário** e os
   dois mexem em renda/combustível, que sustentam a cesta de comida: ficam
   diários (o plano já dizia "avaliar").
5. **Cuidado descoberto aqui**: rodar a passada de uso do solo (42 mil negócios)
   todo dia para servir só as ~28 cidades do lote custaria ~1,2 ms/dia — mais que
   o ganho. A implementação usa o índice `service_by_city` e só toca as cidades
   do dia (ver §2.2).


## 2. Proposta e desenho

**Diário (inalterado):** produção, matérias-primas, consumo/renda, preços,
limites de preço, comércio, chegada/logística, contabilidade, equalização
fiscal, câmbio, invariantes.

**Semanal, escalonado por cidade** (`(dia − i) % 7 == 0`): decisões de
abertura/fechamento de negócios de serviço. Demanda e faturamento continuam
diários.

**Mensal, escalonado por cidade** (`(dia − i) % 30 == 0`): preço do solo e
decadência do sinal de zona; fechamento do mês de moradia (nível de aluguel,
vacância, ônus, decisão do incorporador, favela) com o **aluguel do dia
inalterado** (a coleta diária é o que sustenta a conservação de dinheiro e a
prioridade da comida); fluxo mensal da prefeitura (IPTU, taxas, manutenção,
obra) e repasse do tesouro no lote do dia.

**Mensal, escalonado por índice (já existia):** IA de grupos.

Mantido diário por decisão de projeto: trabalho/salários (barato e ligado à
cesta de comida), coleta de aluguel, demanda de serviços.

### 2.1 Estado no save (v15)

`ogs::CadenceState` em `WorldState`:

- `weekly_period` (padrão 7) e `monthly_period` (padrão 30). `0` desliga o
  escalonamento (todas as cidades todo dia) — é o interruptor de teste.
- O escalonamento em si é **derivado** do dia e do índice da cidade, então não
  há cursor a serializar; o que entra no save são os períodos (política) mais os
  acumuladores que já existiam (`month_marker`, `month_revenue_*`,
  `rent_paid_month`). Um save no meio do ciclo mensal reproduz o mesmo mundo.

### 2.2 Notas de implementação

- `cadence_due(dia, índice, período)` em `sim/world_state.h`; sem cursor, sem RNG.
- `urban_land::phase_month` monta o uso do solo **só para as cidades do lote**:
  a parte de serviços vem de `ws.service_by_city` (o índice que `zone_usage` já
  usa), não de uma varredura com busca por id — 42 mil buscas por dia voltariam
  a custar ~1,2 ms (o que o inventário reprovou). Sem o índice (mundo recém-
  carregado), cai na varredura anterior.
- `city_gov`: `advance_projects` (prazo das obras) continua **diário**; o fluxo
  mensal (IPTU, taxas, repasse, manutenção, decisão de obra) roda no dia da
  cidade. `CityGov::month_marker` (novo, save v15) mede a janela real: a
  primeira é curta (a cidade começa no dia `índice`) e IPTU/taxas/repasse/
  manutenção são proporcionais aos dias de PIB que existiram — não um mês cheio
  cobrado sobre poucos dias.
- `housing`: o **aluguel do dia** (cobrança da renda do dia, repasse aos donos,
  manutenção) e o sinal de aperto continuam diários; só o fechamento do mês
  (nível de aluguel, vacância, ônus, decisão do incorporador, favela) é
  escalonado.
- `services`: demanda/faturamento são diários (`services_day` no pool); a
  abertura/fechamento de negócio roda no dia semanal da cidade.
- `period <= 1` faz TODAS as cidades rodarem todo dia. É interruptor de teste —
  **não** reproduz o calendário antigo (dia 30 global), então não serve para
  prova de equivalência; a prova é o round-trip e o determinismo.
- Fora do escopo, registrado como pendência: `logistics.cpp` (manutenção mensal
  de frota em `dia % 30 == 0` para todas as nações) e a cadência semanal de
  publicação de `nations[].labor` (`economy_sim.cpp`) ainda usam literais. Não
  movem dinheiro; alinhá-los ao `CadenceState` é higiene, não ganho.


## 3. Evidência

### 3.1 Bench canônico (N=3, 60 d, release com perfil, host AMD 5700X3D)

| métrica | antes (inventário N=1) | depois (mediana de 3) |
| --- | --- | --- |
| dia completo — média | 58,09 ms | **56,89 ms** |
| dia completo — p50 | 56,43 | 55,45 |
| dia completo — p95 | 71,69 | **65,58** |
| dia completo — **pior dia** | **94,52** | **68,45** |
| `fiscal_city_gov` p50 / pior | 0,003 / **20,68** | 0,72 / **1,28** |
| `housing` p50 / pior | 0,070 / **13,33** | 0,44 / **0,75** |
| `land_month` p50 / pior | 0,000 / **1,45** | 0,04 / **0,07** |
| `services_decisions` p50 / pior | 0,41 / 2,35 | **0,18** / 1,25 |

Rodadas do bench: 55,68 / 56,89 / 63,76 ms (a terceira sob contenção de outra
sessão). A variação entre rodadas foi **14,2%**, então o critério de
reprodutibilidade do A0 (≤3%) **não fecha neste host compartilhado** — o número
que vale é a mediana e o pior dia, não a terceira rodada. Comparação de fase a
fase com o inventário (mesmo host, mesma sessão): `status` 19,94 → 20,26,
`arrival` 7,18 → 6,03, `trade` 5,72 → 5,80, `ai` 9,62 → 10,38 (ruído de host).

**O que a A6 entregou em performance:** o **pico** do dia caiu de 94,5 para
68,4 ms (−27,6%) e o p95 de 71,7 para 65,6 ms (−8,5%); o custo diário médio é
praticamente o mesmo (o trabalho mensal passou de "tudo no dia 30" para
"~28 cidades por dia"). A meta do plano (**dia ≤15 ms p95**) **não** é atingível
por cadência: sobram ~47 ms de trabalho *diário por contrato* — `status` 20,3
(A4: status sob demanda), IA 10,4 (retrato 7,6: fatiar o `AiView`), `arrival` 6,0
e `trade` 5,8 (A7 já otimizou o comércio), retrato do prólogo 4,8 (rede de
segurança dos invariantes). Isso é registro para as fases seguintes, não
pendência da A6.

### 3.2 Invariantes

- **Dinheiro conservado todo dia:** `economy_world_test --ai=1 --days=60/365`
  sem violação de invariante; drift do sweep 365 d **0,0002 XGS** (real) e
  0,0001 (sonda float).
- **Determinismo 1 × 3 × 8 × 12 × 16 threads:** os cinco `--out` do mundo real
  (60 d, IA, urbanos) têm o **mesmo SHA-256** `fdda411e94cfed54…` e o mesmo
  `money_total` 2.177.839.209,14998.
- **CLI × Godot com IA:** snapshot do dia 0 (`export_world_snapshot --ai=1`),
  60 d no `opengs_sim_cli --ai=1` e no `snapshot_replay.gd --ai=1`; comparador
  em tolerância **0**: **0 diferenças**.
- **Save/load no meio do ciclo mensal:** `economy_save_load_roundtrip_test`
  (save no dia 30, segue até 60) **0 diffs**; `city_gov_test` faz save/load no
  dia 32 (meio da janela mensal) **0 diffs**; `economy_save_load_v11_compat_test`
  carrega save v11 sem o bloco `cadence` e recebe os padrões 7/30.

### 3.3 Recalibração (sweep 365 d, `real` + sonda `float`)

`docs/economy/perf/a6_world_sweep.json` (baseline `main_a7_world_sweep.json`):

- **Falências de construção: 9** (portão ≤28).
- **Drift de dinheiro: 0,0002 XGS** (portão ≤0,01).
- **Comida (média de 30 d): nenhuma nação passa do portão de 2,5 p.p.** (o pior
  caso do último dia é SAH 0,190, isento).
- `trajectory_hash` da variante de aceite `e5ecfc5b…`; sonda `float`
  `750ce5b2…` (difiere — o câmbio alcança a simulação).
- **Sonda `float` reprova 4 itens** (12 falências vs 8+2 do baseline; AFG −5,7
  p.p., EGY −13,4 p.p., PSX −5,4 p.p. na média de 30 d). É a sonda de
  sensibilidade, fora do gate por decisão do Q-016/A7 — registrada, não usada
  como muleta.

### 3.4 Goldens regenerados (mudança esperada)

| golden | antes | depois |
| --- | --- | --- |
| `baseline/A0_60_urban.json` | save `dcad384c…` / status `46047575…` | save `c8dd374a…` / status `1783b7e5…` |
| `baseline/real_60_ai.json` | save `042f6f54…` / status `4c69889f…` | save `4c27c483…` / status `5cbac9ae…` |
| `baseline/real_60_noai.json` | save `ee3c5e85…` / status `8d6f3d44…` | save `b251e282…` / status `2b7db383…` |
| `baseline/demo_365_noai.json` | save `652b5f8b…` / status `65a414f0…` | save `46717a3e…` / status `10b6f339…` |

A diferença é **esperada e aprovada** (a A6 muda o resultado de propósito): o
calendário de fechamento mudou de "dia 30 global" para "dia da cidade", e o
`services_open_close` passou a decidir semanalmente. `money_total` do mundo
segue 2.177.839.209,15 e o demo 630.000,00; 0 falências nos dois.

### 3.5 Bateria

17 suítes verdes (DLL debug do worktree): `economy_integration`,
`economy_ai`, `population_demo`, `economy_save_load_roundtrip`,
`command_replay`, `economy_save_load_v11_compat`, `housing_market`,
`urban_land`, `urban_load`, `construction_project`, `construction_queue`,
`investment`, `labor_market`, `economy_light_signals`, `economy_city_status`,
`economy_tick_async`, `services` (cena). `turbo_equivalence_test` **não existe
nesta base** (`941120c` é anterior ao modo turbo).


