# A5 — IA econômica em C++ como fase do tick

> Branch `perf/a5-ai-cpp`, base `balance/global-resources` (ca1f832, A3 + A2 + recal).
> Plano: `docs/plans/perf_pop_financas_plano.md`, §A5. Decisão do dono: portar os
> **três** investidores de uma vez (`private_investor`, `government_ai`,
> `group_investor`) e o `industry_templates` para uma fonte única em C++.
> Medição A0 (host ocioso): dia completo ≈ 155 ms, sendo ~114 ms de IA em
> GDScript e ~36 ms de tick C++.

## 1. Inventário (antes do porte)

Lido de `core/economy/ai/` no HEAD ca1f832. Tudo determinístico; não há RNG em
nenhum dos três investidores.

### 1.1 Agendador (`economic_ai.gd`)

- Cadência **por cidade**: `interval_days = 7`; a cidade de índice `i` planeja
  quando `day % 7 == i % 7` (a cadência por cidade continua 7 dias, com ~1/7 do
  trabalho por dia). A vez vem do índice, não do status.
- Cadência **por grupo**: `INTERVAL_DAYS = 30`; o grupo na posição `i` do
  ranking (`get_group_ranking`: receita total desc, caixa desc, id asc) decide
  quando `day % 30 == i % 30`.
- Ordem do dia: (1) `plan()` agenda o grupo, mede os totais e enfileira as
  cidades da vez; (2) o grupo decide (fatiado entre frames no jogo, mas sempre
  **antes** das cidades); (3) por cidade, `private` e depois `government`, na
  ordem do índice.
- `plan()` chama `prepare_totals` **antes** de o grupo decidir, então o privado
  copia o caixa dos grupos anterior às decisões de grupo do dia.

### 1.2 Entradas lidas do status

| Bloco | Campos | Quem lê |
|---|---|---|
| `cities[i]` | `id`, `nation`, `landmass`, `firm_cash`, `government_budget`, `suitability[]`, `external_demand[]`, `deposits{}` | privado, governo |
| `light.m_*` | `price`, `stock`, `supply`, `surplus`, `sold`, `shortage`, `desired` | os três |
| `light.p_*` | `need`, `consumed`, `satisfaction`, `labor_force`, `unemployed` | privado, governo, grupo |
| `light.b_*` | `id`, `output_product`, `owner`, `capacity`, `utilization`, `costs`, `site_suitability` | os três |
| `light.c_*` (obras) | `output_product`, `capacity`, `expand` | os três |
| `light.i_*` (incumbentes) | cidade, recurso, grupo dono | grupo |
| `market_totals` | `surplus`, `sold`, `demand` (só positivos, ordem das cidades) | privado |
| grupos | ranking (id, origem, setor, caixa, estatal, receita) | privado, grupo |
| sinais | `fdi_policies`, `confidence`, `get_tariff` | grupo |

`light` é uma cópia plana dos mesmos doubles do bloco pesado, montada **antes**
da IA no dia; portanto a IA lê o fecho do dia e **não vê** as obras que ela
mesma abriu no mesmo dia.

### 1.3 Regras de decisão

- **PrivateInvestor** (`CAPITAL_SHARE=0.35`, `MIN_CAPACITY=2`,
  `MAX_CAPACITY=40`, `MIN_MARGIN_PER_TON=0.5`, `MIN_SHORTAGE_TONS=0.5`,
  payback 150 d / 730 d bulk / 730 d indústria, risco = desvio/média de 30
  preços, `BASE_RISK=0.15`): comida primeiro (déficit local → melhor
  aptidão×preço; mundo/região curto → expande em cidade excedente); depois
  matérias-primas e receitas na ordem de `TEMPLATES` + `recipes.json`; gates de
  excesso local/global (comida isenta), escassez efetiva descontando obras,
  aptidão/jazida, insumo, margem, payback ajustado; capacidade limitada por
  demanda externa (mineral), área cultivável (comida) e caixa; **uma** ação por
  cidade/visita; expande produtor existente, abre privado ou via grupo com
  caixa.
- **GovernmentAI** (`BUDGET_SHARE=0.25`, capacidades 2/20/10, cooldown 30 d,
  teto 6/tipo, utilização ≥ 0,60): segurança alimentar quando
  `need − consumed ≥ 1` e a cidade não tem produtor nem obra de comida; depois
  mineração estratégica para cada mineral com jazida, sem produtor/obra,
  margem por profundidade > 0 e sem excedente, candidatos **ordenados por
  margem decrescente**; uma ação por cidade.
- **GroupInvestor** (`INTERVAL_DAYS=30`, `MIN_CAPITAL=6000`,
  `MIN_RETURN=0.0018`, `CASH_SHARE=0.5`, `MAX_CAPACITY=12`, food-secure 0.98):
  base de candidatos por recurso (uma varredura de todas as cidades por dia);
  enquanto a satisfação média do mundo < 0,98 só a cesta alimentar; depois a
  cesta + os recursos do setor (ordem do `goods.json`); risco de tarifa,
  confiança, IDE e JV; melhor score estritamente maior; uma ação por grupo.

### 1.4 Aleatoriedade, ordenações e ordem de dicionário

- **Zero RNG** nos três investidores (e no núcleo). A semente do harness só
  aparece em `meta`.
- Ordenações explícitas: candidatos do governo por margem desc
  (`sort_custom`, não estável); ranking de grupos do núcleo (receita desc, caixa
  desc, id); `best_recipe` = menor custo e, no empate, a primeira da ordem do
  arquivo.
- Ordem de dicionário do GDScript = ordem de inserção, reproduzível:
  `TEMPLATES`, `_recipe_templates` (primeira aparição da saída), `_all_cache`
  (primário vence receita de mesmo id — `charcoal → coal` não duplica),
  `_non_food_templates`, `_resource_by_product`, `_groups_by_key`, `_sector_of`.
  O porte reconstrói essas ordens em vetores; nenhum `unordered_map` é iterado
  (só lookup).
- Estado não serializado que precisou viver no núcleo: janela de risco de 30
  preços por cidade×produto, contadores de id do privado/governo,
  `last_build_day` por cidade e as visões locais de caixa de grupo.

## 2. Arquitetura do porte

```text
gdextension/src/
  sim/actions.{h,cpp}      aplicadores invest/build/group_invest (C++ puro)
  sim/commands.{h,cpp}     fila A3 + last_seq/take_by_seq
  sim/ai/ai_templates.*    tabela dos 21 primários + receitas (fonte única)
  sim/ai/ai_view.*         retrato congelado do dia + helpers de leitura
  sim/ai/ai_private.*      investidor privado
  sim/ai/ai_government.*   governo
  sim/ai/ai_group.*        investidor de grupos
  sim/ai/ai_phase.*        ordem do dia + emissão/aplicação pela fila
```

- **Fronteira:** `sim/ai/` é STL puro (0 `godot_cpp`, 0 `Dictionary`/`String`
  do Godot). Os aplicadores saíram da fachada para `sim/actions` para que o
  mesmo código rode no Godot e no CLI; a fachada só converte o resultado em
  `Dictionary`.
- **Quando roda (D-076):** `EconomySim::tick_and_publish` roda
  `AiPhase::run(ws, core, commands, instant, bootstrap)` com o dia fechado e só
  então monta o **único** status do dia, guardado para `tick_day()` — o status
  já traz os efeitos das decisões. A IA não lê o status: lê o `AiView`, retrato
  congelado montado no início da fase. Antes da D-076 a IA rodava depois do
  status e o invalidava: o status era construído duas vezes por dia e a segunda
  construção publicava `nations[].labor` do cache do slot anterior (até 13 dias
  de atraso).
- **Leitura:** `snapshot_ai_view` congela mercados, POP, construções, jazidas,
  obras, incumbentes, totais por produto e caixa dos grupos **no início da
  fase**. Comandos aplicados no meio da fase não vazam para as decisões
  seguintes (o GDScript lia o mesmo status congelado).
- **Escrita:** toda ação passa por `ai_emit_*` → `Command` da A3 (`push` →
  `take_by_seq`, que retira só o próprio comando sem roubar os da UI → aplica
  por `sim/actions` → `log` com `apply_day = dia`). A IA não muta o mundo
  direto.
- **Ordem:** ranking de grupos (caixa copiado) → `private.prepare` (comida,
  obras, grupos) → grupo decide → cidades `i % 7 == day % 7`, privado e depois
  governo; grupos `i % 30 == day % 30`. Serial por desenho: as decisões de um
  mesmo lote são encadeadas (caixa de grupo, obras em voo, ids), então
  paralelizar por nação exigiria fases de leitura separadas — o porte prefere
  determinismo e velocidade (o trabalho diário é 1/7 das cidades).
- **Parâmetros:** `industry_templates.gd` virou `sim/ai/ai_templates.*`; a
  capacidade de terra por cidade/cultura (dado de geografia) entrou em
  `City::land_capacity`, declarada pelo `GeographyService` e transportada no
  bloco `definitions` do snapshot (não entra no save).
- **CLI:** `opengs_sim_cli` roda COM IA por padrão (`--ai=0` desliga), com a
  mesma `AiPhase` e a mesma ordem do caminho Godot.

## 3. Equivalência (aceite principal)

Medições na branch (base ca1f832) e re-medidas no **merge** sobre
`balance/global-resources` (A3 + A2 + recal3, HEAD `fcfc107`), DLL debug
própria, `--urban=1`.

### 3.1 Mundo em 60 d contra o golden A0

- `world_equivalence.ps1 -Baseline A0_60_urban.json` (IA ligada):
  `save dcad384c67b53a1b0b5152fda8bd315b9fc331a563e66270dbdf4894dc43c06a`,
  `status 46047575196a7f60b7c708f15fa006dfa21b2789c9949c6b1093e021b93b53d4`
  — **os mesmos shas do golden**, 0 diferenças. `money_total 2.177.839.209,15`,
  0 falências, 54.611 negócios de serviço — os valores do golden da recal3
  (`fab8e0a`), não os da base ca1f832.
- Controle negativo: `-Ai 0` contra o mesmo golden → FAIL (save
  `913834f9…`, status `99305c18…`), provando que o golden é sensível à IA.
- Threads 1 × 8: mesmos dois shas; só `meta.threads` difere (echo do input).
  Medido na branch; a recal3 não toca o núcleo, e o merge re-mede o par
  golden/`-Ai 0` acima.

### 3.2 Lista de decisões em 60 d (GDScript × C++)

Harness temporário `_tmp_a5_decisions.gd` (não commitado) monta o MESMO mundo
inicial nos dois modos e grava as decisões (GDScript) e o log de comandos da A3
(C++). Comparadas por `(dia, cidade, tipo, alvo, custo/capacidade)`:

- **1.894 ações bem-sucedidas** (447 builds privados, 1.013 estatais, 428
  expansões, 6 builds de grupo do investidor de grupos): iguais, `gd_only = 0`.
- **2.278 tentativas rejeitadas** casadas por `(dia, cidade, custo)`:
  0 sem par.
- **143 `skip` do investidor de grupos** (group_invest recusado pelo núcleo)
  casados por `(dia, cidade, capacidade)`: 0 sem par.
- **0 sobras** no log C++ depois dos três casamentos → a lista de decisões é
  idêntica, inclusive as falhas.

### 3.3 CLI × Godot com IA (60 d)

Snapshot exportado com `--ai=1` (geografia + fazendas iniciais + nomes de grupo
+ `land_capacity`), replay do Godot com `--ai=1` × `opengs_sim_cli` (IA
padrão): **0 diferenças** campo a campo (`world_equivalence_compare.py`), day
60, `money_total 2.177.839.209,15` (re-medido no merge).

### 3.4 Sweep 365 d × 5 (+ sonda de câmbio)

`tools/s19_world_sweep_parallel.ps1 -OutPrefix a5_` (6 processos, 3,3 min).
Medido na branch, contra `recal2_world_sweep.json`. A recal3 (`fab8e0a`)
re-mede o sweep na linha sem a A5 (8 falências); o sweep de 365 d da linha
**mesclada** não foi re-rodado nesta entrega — pendência declarada:

| variante | falências | drift | comida vs `recal2_world_sweep` |
|---|---|---|---|
| real / real_seed1 / real_seed2 / half / xgs | 7 | −0,001 | **delta máximo 0,000 em 179 nações** |
| hash da trajetória | `e13f92a5…` idêntico nas 5 | | |
| float_probe (sonda) | — | −0,0005 | hash diferente (câmbio alcança a simulação) |

Falências 7 (limite 28), drift −0,001 (limite 0,01) e **comida idêntica** ao
baseline em todos os 179 países — a consequência esperada da equivalência de
decisões.

## 4. Testes

Verdes nesta branch: `economy_ai_test` (reescrito para o log de comandos),
`investment_test`, `economy_integration_test`, `population_demo_test`,
`economy_save_load_roundtrip_test` (0 diffs), `command_replay_test`
(12 checagens, 14 comandos, 6 dias, 0 diferenças). `economy_long_run_test` não
foi rodado (lento; o sweep de 365 d cobre o prazo longo). `economy_panel_test`
segue falhando pelo BOM pré-existente.

O `economy_ai_test` manteve as asserções de comportamento (capacidade de comida
cresce, nenhum investimento em trigo na cidade com excesso, estatal de ferro +
produção + orçamento consumido + fluxo externo, anti-loop ≤10 ociosos) usando o
log de comandos e os prédios — não foi enfraquecido.

## 5. Performance

A IA deixou de ser um trabalho de ~114 ms por dia fora do tick. O custo dela
agora está dentro do `tick_wall_ms`; as métricas `ai_plan_ms`/`ai_pump_ms` do
`perf_day_peak` ficam em 0 e `day_total_ms` passa a ser o tick.

- **Bench canônico A0, ordem com build duplo** (release de medição, 60 d):
  `tick_wall` = `day_total` com mediana **79,6 ms** (A0 sem IA: 155,5 ms) —
  **−49%**. A variação entre as 3 rodadas foi 7,95% (> 3%): o host não ficou
  limpo o bastante para o critério de repetibilidade, e o número absoluto é o
  que vale aqui. Decomposição: ~48 ms são as fases do tick/status e ~30 ms a
  fase de IA (snapshot + decisões + aplicação). `status` mediano 20,0 ms
  (status_cities 14,3 — o build duplo descrito abaixo).
- **Custo residual resolvido (D-076, decisão do head em 2026-11-17):** o
  `poll()` do `tick_scheduler` chama `get_status()` e, como a IA rodava depois
  do status e o invalidava, o status era construído **duas vezes** por dia
  (~20 ms). A ordem nova (IA antes do status único) foi adotada: `day_total` =
  `tick_wall` mediano **60,5 ms** (rodadas 64,24 / 60,19 / 59,55; uma medição
  anterior, também em janela sem Godot de outra worktree, deu 64,1 ms com
  60,76 / 63,99 / 64,34) — **−61% vs A0** e **−24% vs a ordem com build duplo**.
  `status` mediano 20,5 ms (uma construção por dia), `poll` 0,015 ms. O
  critério de 3% entre rodadas **não fechou** nas duas tentativas (6,97% e
  9,76%): o host é compartilhado com outros agentes e o número absoluto é o
  que vale. Golden: `save` idêntico (`dcad384c…`) e o `status_sha256` muda em
  **um** bloco — `nations[].labor` (agregado **semanal**, que com o build duplo
  vinha do cache do slot anterior, até 13 dias atrasado). Verificado campo a
  campo: 3.807 folhas divergentes, **0 fora de `nations[].labor`**. Por isso o
  retrato novo é o correto; `A0_60_urban.json` foi regravado (`status
  4f18522d…`) e os goldens S0 não mudam (não ligam a IA C++). JSON do bench:
  `docs/economy/perf/a5_d076_bench.json`.

## 6. O que fica e o que sai

- **Saiu do jogo:** `EconomicAI` não é mais criado nem fatiado pelo autoload
  `Economy`; `economy.gd` só liga a fase C++ (`set_ai_enabled(true)`) e nomeia
  os grupos uma vez (novo `core/economy/group_namer.gd`).
- **Fica (legado declarado):** `core/economy/ai/*.gd` continua no repositório
  apenas para 11 ferramentas de diagnóstico/estudo que ainda instanciam a IA
  GDScript (`balance_review`, `economy_bistability_test`,
  `economy_city_catchment_test`, `economy_deposit_ownership_test`,
  `economy_refactor_equiv`, `economy_rebalance_scan`, `f3_food_diag`,
  `economy_bankruptcy_diag`, `services_business_audit_test`, `ai_scale_bench`,
  `world_scale_bench`) e para `investment_test` (que usa só o `name_groups`).
  **Nada do caminho do jogo, do CLI ou da bateria de aceite as usa.** A remoção
  desses arquivos exige migrar essas ferramentas na mesma fatia — registrado em
  GAME_STATE §15.
- **Sem mudança de save:** `save_state.cpp` intocado; o schema continua v14 e
  `land_capacity` viaja só no `definitions` do snapshot.
