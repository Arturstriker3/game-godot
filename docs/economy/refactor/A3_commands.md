# A3 - Fila de comandos (docs/plans/perf_pop_financas_plano.md, Parte A)

> Branch `perf/a3-commands`, base `balance/global-resources` (85ca5b8).
> Escopo: **fronteira** — o binding (`economy_sim.cpp/.h`), uma fila nova em
> `sim/commands.{h,cpp}` e **uma** chamada no início do tick. `world_state`,
> `investment`, `logistics` e `trade` **não** foram tocados (a A2 mexe neles em
> paralelo).

## 1. O que a A3 entrega

Toda ação do jogador ou da IA que muda o mundo **fora do tick** passou a ser um
`Command`: tipo + parâmetros + dia de emissão + ordem de emissão. A fila é
aplicada, em ordem determinística (a ordem de emissão, desempate pelo `seq`):

1. **No início do dia** — `apply_commands()` é a primeira coisa que
   `EconomySim::tick_core()` faz, antes do prólogo e de qualquer fase.
2. **Na hora** — quando o método do binding precisa devolver o efeito. Ver §4.

Nenhum número mudou: o mundo que resulta de "aplicar entre os dias" e o de
"aplicar no início do dia seguinte" é o mesmo, porque nenhum tick acontece no
meio e a ordem é a mesma. O aceite é o golden A0 (§7).

## 2. Inventário — quem muda o estado fora do tick

Levantado no binding (`ClassInfo`) e nos chamadores em
`core/economy/ai/*.gd`, `core/economy/geography_service.gd`, `core/ui/` e
`tools/*.gd`.

| Tipo de comando | Método público (assinatura intacta) | Slots | Chamadores |
|---|---|---|---|
| `invest` | `invest(city, building, amount)` | `s0` city, `s1` building, `v0` amount | `private_investor.gd`, `economy_lab.gd`, `economy_integration_test` |
| `build` | `build(city, building, resource, in, out, cap, jobs, wage, opex, i/o, owner, cost, recipe)` | `s0..s4`, `i0..i1`, `v0..v5` | `private_investor.gd`, `government_ai.gd`, construtores de teste |
| `group_invest` | `group_invest(group, city, building, resource, out, cap, recipe, partner)` | `s0..s5`, `i0`, `v0` | `private_investor.gd`, `group_investor.gd`, `investment_test` |
| `fund_group` | `fund_group(group, amount)` | `s0`, `v0` | `investment_test` |
| `set_fdi_policy` | `set_fdi_policy(nation, sector, mode, min_local)` | `s0..s2`, `v0` | `investment_test`, política do jogador |
| `set_sector_incentive` | `set_sector_incentive(nation, sector, holiday, subsidy)` | `s0..s1`, `v0..v1` | idem |
| `create_state_company` | `create_state_company(nation, name, capital)` | `s0..s1`, `v0` | idem |
| `privatize` | `privatize(city, building, buyer)` | `s0..s2` | idem |
| `nationalize` | `nationalize(city, building)` | `s0..s1` | idem |
| `spend_government` | `spend_government(city, amount)` | `s0`, `v0` | UI/ações do jogador |
| `price_shock` | `price_shock(product, multiplier)` | `s0`, `v0` | `population_demo_test`, `production_chain_test`, jogador |
| `set_suitability` | `set_suitability(city, product, value)` | `s0..s1`, `v0` | `geography_service.gd`, IA/testes |
| `set_deposit` | `set_deposit(city, product, kt, quality, depth)` | `s0..s1`, `v0..v2` | `geography_service.gd`, IA de mineração |
| `set_external_demand` | `set_external_demand(city, product, ceiling)` | `s0..s1`, `v0` | `geography_service.gd` |
| `set_external_income_index` | `set_external_income_index(city, index)` | `s0`, `v0` | `geography_service.gd` |
| `set_productivity_index` | `set_productivity_index(city, index)` | `s0`, `v0` | geografia/eventos |
| `set_income_gini` | `set_income_gini(city, gini)` | `s0`, `v0` | `geography_service.gd` |
| `set_minimum_wage` | `set_minimum_wage(nation, value)` | `s0`, `v0` | `labor_market_test`, jogador |
| `set_wage_rigidity` | `set_wage_rigidity(value)` | `v0` | idem |
| `set_tariff` | `set_tariff(buyer, seller, product, rate)` | `s0..s2`, `v0` | `currency_trade_test`, `investment_test`, jogador |
| `set_currency_rate` | `set_currency_rate(code, rate, floating, base)` | `s0`, `v0..v1`, `b` | `currency_trade_test`, jogador |
| `expand_port` | `expand_port(port, t_per_day)` | `s0`, `v0` | `port_capacity_test`, jogador |
| `set_port_capacity` | `set_port_capacity(port, t_per_day)` | `s0`, `v0` | `port_capacity_test` |
| `set_market_stock` | `set_market_stock(city, product, stock)` | `s0..s1`, `v0` | testes e ferramentas |
| `set_market_price` | `set_market_price(city, product, price)` | `s0..s1`, `v0` | idem |
| `fleet_buy` | `fleet_buy(nation, product, amount, budget_share)` | `s0..s1`, `v0..v1` | `fleet_fuel_test`, jogador |
| `urban_expand` | `urban_expand(city, zone, km2)` | `s0`, `i0`, `v0` | §19 (prefeitura/jogador) |
| `urban_densify` | `urban_densify(city, zone)` | `s0`, `i0` | idem |
| `urban_social_housing` | `urban_social_housing(city, units)` | `s0`, `v0` | `city_gov_test`, `construction_queue_test` |
| `set_service_capacity` | `set_service_capacity(city, sector, units)` | `s0..s1`, `v0` | `services_test` |
| `set_service_business_state` | `set_service_business_state(id, loss_days, profit)` | `i0..i1`, `v0` | `services_business_audit_test` |

**Ficaram de fora de propósito** (não são ação de jogo: são construção/carga do
mundo, e enfileirá-las quebraria a ordem de carga):
`initialize_scenario`, `initialize_world`, `set_goods`, `set_recipes`,
`set_needs`, `set_services`, `set_sector_employment`, `set_urban`,
`set_balance`, `set_fleet`, `set_currencies`, `set_labor_profile`,
`set_group_names`, `seed_industry`, `seed_groups`, `set_instant_construction`,
`from_dict`, `from_snapshot`, `from_snapshot_file`, `set_sim_threads`,
`set_profiling`.

## 3. A fila (`sim/commands.h/.cpp`)

- `ogs::Command`: `type`, `day`, `seq`, `s[6]` (strings), `v[6]` (doubles),
  `i[2]` (ints), `b` (bool). **Só dados** — nada de Godot, então a fila é C++
  puro como o resto de `sim/`.
- `ogs::CommandQueue`: `push()` (trava própria, `std::mutex` simples; atribui o
  `seq` 1-based), `take_pending()` (devolve em ordem de emissão), `log()`,
  `log_entries()`, `reset()`. O log e a fila pendente são separados: o log é a
  evidência/replay, a pendente é o trabalho do próximo dia.
- A fila **não** conhece a simulação nem os aplicadores. Quem aplica é a
  fachada (`apply_one_locked` → `apply_<nome>`).
- `initialize_scenario`, `initialize_world`, `from_dict`, `from_snapshot` e
  `from_snapshot_file` chamam `commands_.reset()`: fila e log pertencem ao
  mundo anterior.

## 4. Quando a fila é aplicada (e o único caminho síncrono)

| Situação | O que acontece | Por quê |
|---|---|---|
| Chamada **entre dias** (a IA no `pump`, a UI, um teste) | `dispatch_command()` enfileira e aplica na hora (`apply_pending_locked`) | O método devolve um resultado (`invest` → `{invested, capacity_added, error}`, `build` → `{created, cost, error}`, ...). A IA e dezenas de testes **leem esse retorno e o estado logo em seguida**; adiar sem resposta os quebraria. É o "caminho síncrono só para ele" previsto no plano. |
| Chamada **de outra thread com o dia em andamento** (thread do `tick_scheduler`) | só enfileira; devolve `{queued: true, seq, apply_day}` (bool: `true`) | Nada do `ws_` é tocado fora do escritor único → **não há disputa com o mutex recursivo**. O efeito entra no início do dia seguinte. |
| Chamada **da própria thread do dia** (o tick chamando por dentro, ex. `seed_industry → build`) | aplica na hora | O tick é o escritor e já está sob o mutex; a ordem interna do dia faz parte do resultado. Sem isso, as plantas de referência do bootstrap ficariam na fila e o mundo divergiria (foi o único desvio pego pelo golden na primeira versão). A distinção é o hash da thread (`tick_thread_`). |
| Início do dia (`tick_core`) | `apply_commands()` — aplica tudo o que ficou pendente, antes do prólogo | É a barreira determinística do plano. |
| Replay | `replay_commands(entries, target_day)` avança o relógio até o `apply_day` de cada comando e aplica | Mesma hora da execução original, inclusive para o caminho adiado. |

Ordem: **sempre** `seq` crescente (a ordem em que o emissor chamou). Como o
`push` é serializado pela trava da fila, a ordem é a de emissão mesmo com
emissões de threads diferentes.

`pending_command_count()` mostra a fila; `apply_commands()` força a descarga
(útil em ferramenta/teste).

## 5. `status_invalidate()` e o mutex

- Antes: **40** chamadas espalhadas pelos métodos que mutam estado. Depois: **1**
  ponto, em `apply_pending_locked`, quando o lote foi aplicado **fora** do dia
  (dentro do tick o status vai ser reconstruído de qualquer forma, porque o dia
  avança). Os aplicadores (`apply_*`) não invalidam mais. As chamadas que
  restam fora da barreira são só as dos **setters de carga**, que continuam no
  caminho direto e invalidam por conta própria: `set_goods` (economy_sim.cpp
  ~701), `set_currencies` (~621), `set_fleet` (~999), `set_needs` (~3806 e
  ~3861, um por caminho), `set_labor_profile` (~4101), `set_sector_employment`
  (~4213), `seed_groups` (~4888) e `set_group_names` (~4908). `initialize_*` e
  `from_*` **não** passam por `status_invalidate()`: eles descartam o cache
  diretamente (`status_built_day_ = -1`).
- `set_fdi_policy`, `set_sector_incentive`, `set_service_capacity` e
  `set_service_business_state` **já não** invalidavam antes da A3 (o status
  delas só aparece via consulta sob demanda); isso não mudou.
- O `std::recursive_mutex` **continua** existindo e é o lock do escritor
  (tick, `get_status`, `to_dict`/`to_snapshot`, barreira da fila). O que mudou é
  que o caminho de mutação com o dia em andamento **não o toma** — ele só usa a
  trava simples da fila. Como o `tick_scheduler` não deixa a IA rodar com o dia
  em andamento (§13.4), na prática o mutex nunca é disputado.

## 6. Log e replay

`get_command_log()` devolve `Array[Dictionary]` (ordem de aplicação), pronto
para `JSON.stringify`:

```json
{"type": "set_market_stock", "day": 3, "apply_day": 3, "seq": 7,
 "s": ["AGRIA", "wheat"], "v": [1236.0, 0.0, 0.0, 0.0, 0.0, 0.0]}
```

- `day` = dia da emissão; `apply_day` = dia em que o comando entrou no mundo.
  Coincidem para as ações entre dias; diferem em 1 para as que esperaram a fila
  (dia em andamento em outra thread). O replay avança o relógio pelo
  `apply_day`, então **os dois caminhos são rejogáveis**.
- `s`/`v`/`i` só aparecem quando têm algum slot não vazio; `b` só quando `true`.

`clear_command_log()` limpa; `replay_commands(log, target_day)` aplica o log
inteiro sobre o mundo carregado (avança até o `apply_day` de cada entrada,
aplica, e no fim avança até `target_day`). `tools/command_replay_test.gd` prova
o contrato: mesmo snapshot + mesmo log (passando por `JSON.stringify` +
`JSON.parse_string`) = **0 diferenças** no `to_snapshot()`, e o replay é
determinístico entre duas execuções.

Limite conhecido: o caminho **adiado** (dispatch de *outra* thread com o dia em
andamento) não é exercitado pelo teste — por contrato o `tick_scheduler` não
deixa a IA rodar durante o dia (§13.4), então na prática ele só aparece com UI/
ferramenta clicando no meio de um tick. Mesmo assim ele é rejogável: o log grava
`apply_day`, e é por ele que o replay avança o relógio.

O log **não** entra no save v11: é evidência de sessão, não estado do mundo.
Incluí-lo mudaria o schema e quebraria `economy_save_load_*`.

Memória: o log para de crescer em **200 mil** entradas (`kMaxCommandLog`,
~40 MB); a sessão continua e `command_log_truncated()` passa a devolver `true`.
200 mil comandos ≈ 700 dias de IA no mundo real (~270 decisões/dia).

## 7. Aceite (o que foi medido)

Comandos no rodapé. Tudo rodado no worktree `opengs-wt-a3`, DLL debug própria.

- `world_equivalence.ps1 -Baseline baseline/A0_60_urban.json` (60 d, IA +
  urbano): run1 × run2 = 0 diferenças e **os dois shas iguais ao golden**
  (`save 3e3ebbbe…1a3d0b7`, `status d79bfe32…c95ac9b`, money
  2.200.539.587,40, 0 falências) → **PASS**.
- `economy_integration_test`, `economy_ai_test`, `population_demo_test`,
  `economy_save_load_roundtrip_test` (0 diffs), `investment_test`,
  `construction_project_test`, `housing_market_test`, `city_gov_test`,
  `economy_tick_async_test`, `command_replay_test` (12 checagens, 14 comandos,
  6 dias, 0 diferenças): **verdes**.
- `tools/convoy_lifecycle_test.gd` **falha na base também** (`Identifier not
  found: Economy` com `--script`) — pré-existente, não regressão da A3.
- CLI × Godot sem IA: `export_world_snapshot.gd` → `snapshot_replay.gd` (60 d,
  834 cidades) × `opengs_sim_cli --days=60` → arquivos **byte-idênticos**
  (`sha256 79c7897a…`) e `world_equivalence_compare.py` = 0 diferenças. A A3
  não muda esse caminho: o CLI não emite comandos e a fila nasce vazia.
- Threads 1 × 8: `world_equivalence.gd --threads=1` e `--threads=8` (60 d, IA)
  → mesmos `save`/`status`; só `meta.threads` e `meta.generated_utc` diferem.
- `tools/stress_shutdown.ps1 -Script tools/economy_ai_test.gd -Runs 10`: 0
  falhas (o núcleo continua com o join no encerramento; a A3 só acrescentou uma
  trava simples na fila, nunca tomada pelo caminho do encerramento).

### 7.1 Nota sobre o golden A0

O `A0_60_urban.json` é um meta-golden (sha256 de save e status). A execução da
A3 reproduz run1 × run2 bit a bit **e** bate os dois shas do arquivo versionado.
Registro em `GAME_STATE.md` §13.8.

## 8. O que a A3 **não** faz

- Não move `invest`/`build` para o núcleo: os aplicadores continuam na fachada,
  então **o CLI não aplica comandos** (o replay é no binding).
- Não persiste a fila no save (ver §6).
- Não mexe em `world_state`, `investment`, `logistics` nem `trade` (fronteira
  combinada com a A2).
- Não muda ordem de fases, nem preços, nem dinheiro: `gdextension/src/sim/*`
  ganhou um arquivo novo, nada mais.
