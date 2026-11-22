# Q-017 — parecer de núcleo da Parte A fechada (A0–A7 + TURBO)

- **Objeto:** `balance/global-resources` @ **`2264cf9`** — merge `--no-ff` de
  `perf/a6-cadence` (`4a21c79`) sobre `perf/a4-status-query` (`dbe4e88`).
  Worktree própria `G:/projetos/opengs-wt-q17` (detached em `2264cf9`); a pasta
  principal não foi tocada, nenhum merge foi feito e só este parecer é commitado.
- **Relatórios lidos:** `docs/economy/refactor/A1_core_lib.md`, `A2_ids.md`,
  `A3_commands.md`, `A4_status_query.md`, `A5_ai_cpp.md`, `A6_cadence.md`,
  `A7_trade_parallel.md`, `docs/economy/perf/turbo.md`, `GAME_STATE.md` §13.8–§13.12
  e decisões D-068…D-081.
- **Data:** 2026-11-17. **Revisor:** `gerente-qualidade`. Modelos: só DeepSeek Flash
  (execução própria nesta rodada; sem delegação).
- **Ambiente:** Godot 4.7.2 stable, `--headless`; DLL **debug própria** buildada na
  worktree (`gdextension/bin/economy_gdextension.windows.template_debug.x86_64.dll`,
  **35.037.696 B**, SHA256 `EEE6320FB79157D6…FD47A230C`, clang 23.1.5 / llvm-mingw,
  CMake + Ninja, `CMAKE_BUILD_TYPE=Debug`); `opengs_sim_cli.exe` do mesmo build.
  `--headless --import` exit **0** (1.692 recursos importados). 16 núcleos; nenhum
  editor Godot nem `godot-editor-mcp` aberto, encerrado ou tocado.
- **Veredito:** **APROVADO COM RESSALVA.** Save v15, conservação de dinheiro,
  determinismo, A3 sobre a cadência, A4 e A5 sobre a cadência **passam** com
  evidência reproduzível. As ressalvas são de **higiene e medição** (R1–R8), nenhuma
  delas reprova a entrega nem exige conserto antes do merge.

## Resumo das checagens

| # | Checagem | Resultado | Evidência principal |
| --- | --- | --- | --- |
| 1 | Save v15: v11..v14 → v15, padrões, round-trip 32/59, 30 d sem violação | **PASSA** | §1 |
| 2 | Conservação de dinheiro + invariantes diários com cadência; fluxo mensal em dobro/esquecido | **PASSA** (com R1–R3) | §2 |
| 3 | Determinismo: threads 1 × 16 (120 d), CLI × Godot com IA (120 d), turbo | **PASSA** | §3 |
| 4 | A3 + A6: comando no dia de cadência mensal aplicado no dia certo | **PASSA** | §4 |
| 5 | A4: `get_status()` fora do caminho diário do jogo | **PASSA** | §5 |
| 6 | A5 × A6: IA lendo campo que só atualiza mensalmente | **PASSA** (tabela) | §6 |
| 7 | Riscos de manutenção (arquivos > 3.000 linhas, duplicação GDScript×C++, APIs antigas) | **REGISTRADO** | §7 |

Baseline nomeado usado nas comparações: `docs/economy/perf/partA_world_sweep.json`
(variante `real`: 9 falências, `money_drift` 0,0002, `bankruptcy_limit` 28) e os
meta-goldens `docs/economy/refactor/baseline/*.json`. Nenhum número "de cabeça".

## 1. Save v15 (A6)

**O que o v15 acrescenta de fato** (e não mais que isso):

```text
git diff 535168c^ 535168c -- gdextension/src/sim/save_state.cpp
+ data["cadence"] = { weekly_period, monthly_period }
+ city_gov[].month_marker
```

São **4 linhas de escrita** e dois pontos de leitura com default (`cadence` 7/30 e
`month_marker` 0, `save_state.cpp:976-983` e `:2130`). `housing[].month_marker` já
existia antes do v15 — portanto o "v14" real difere do v15 exatamente nesses dois
pontos.

**Migração v11..v14 → v15** (teste próprio, não commitado, `_tmp_q17_save_and_cmd.gd`;
cada variante é o save v15 com o bloco/chaves da versão removidos e `version`
rebaixada):

| Versão simulada | O que foi removido | Resultado |
| --- | --- | --- |
| v14 | `cadence`, `city_gov[].month_marker` | carrega; 7/30; marker 0; **30 d sem violação** |
| v13 | + `construction_projects` e `housing[].projects` | idem |
| v12 | + `city_gov` e `housing` | idem (a fase deriva o estado urbano do estoque) |
| v11 | + as chaves aditivas do pré-S6 | idem |

O teste oficial `economy_save_load_v11_compat_test.gd` também fecha: *"save v11 antigo
(sem os 5 campos novos) carregou sem erro e com defaults 0/false"* (exit 0).

**Round-trip no meio do ciclo mensal** (`economy_save_load_roundtrip_test.gd`, compara
"rodar N dias direto" × "salvar no dia S e continuar"):

| Caso | Save SHA-256 | Status SHA-256 | Diferenças |
| --- | --- | --- | --- |
| salvar no **dia 32**, até 64 | `2265db83e550a095…` | `70e23aef160e0acc…` | **0** (direto == split) |
| salvar no **dia 59**, até 90 | `1a86870b47f013af…` | `7e406b5bce6447dc…` | **0** |
| salvar no dia 30, até 60 (default) | `ffd0f49ef7acca07…` | `6a150d67fc98e246…` | **0** |

O dia 59 fecha os dois ciclos possíveis do lote: cidade de índice 29 (fecha em 59) e
índice 59 (fecha em 59); o dia 32 exercita uma janela aberta. `city_gov_test` (save/load
com obra em voo) e `urban_land_test` (save/load urbano) também passam.

## 2. Conservação de dinheiro e invariantes diários

**365 d, `--ai=1 --urban=1`, mundo real (834 cidades / 179 nações):**

```text
[world] dinheiro inicial=2177839209.15 final=2177839209.15 drift=0.0002
[world] baseline docs/economy/perf/partA_world_sweep.json: 179 nacoes comparadas, 0 pioraram
[world] pior nacao (media 30d): SAH 0.214
[world] OK: modelo demo rodou 365 dias no cenario real sem violacao.   (exit 0)
```

- **0 violações de invariante** em 365 dias (o teste falha no primeiro dia que reverter).
- **Drift 0,0002 XGS** contra o portão 0,01 (50× de folga); `money_total` final idêntico
  ao da varredura da Parte A (`2.177.839.209,15021`).
- **9 falências** = as 9 da varredura `real` (portão 28; baseline + 2 = 11).
- `trajectory_hash e5ecfc5b85d3df46…` **igual** ao `partA_world_sweep.json` e ao sweep
  da A6 — a corrida independente reproduz a trajetória publicada.

**Fechamento do ciclo, cidade a cidade** (teste próprio, 95 d, lendo `month_marker` do
save todos os dias; 3 a 4 fechamentos por cidade no intervalo):

```text
[cad365] cidades=834 | fechamentos prefeitura=2642 (media 3.17) |
         moradia=2642 (media 3.17) | gaps!=30: 0 | contagem fora de 3-4: 0
```

Ou seja: **nenhum fechamento duplo e nenhum ciclo pulado** em 834 cidades, tanto na
prefeitura (IPTU/taxas/repasse/manutenção/decisão de obra) quanto na moradia (nível de
aluguel, vacância, ônus, incorporador, favela). O escalonamento é derivado de
`(dia − índice % período) % período == 0` (`world_state.h:74-80`), sem cursor no save —
por construção não há estado de "em dobro".

**Onde o dinheiro está no caminho mensal (leitura de código):**

| Fluxo | Onde | Cadência | Cobrança/pagamento |
| --- | --- | --- | --- |
| IPTU | `city_gov.cpp:389-411` | dia da cidade | débito em `pop.savings` + `firm_cash`, crédito em `gov.cash`, **mesmo valor** |
| Taxas locais | `city_gov.cpp:416-425` | dia da cidade | de `service_cash` para `gov.cash` |
| Repasse do tesouro | `city_gov.cpp:186-230` | lote do dia | `pay_transfers` debita `government_budget` e credita `gov.cash` **com o mesmo número** |
| Manutenção urbana | `city_gov.cpp:439-452` | dia da cidade | `gov.cash` → `service_cash` |
| Aluguel | `housing.cpp` (fase diária) | **diário** | cobrança diária; só o fechamento do nível é mensal |
| Preço do solo | `urban_land.cpp:337-…` | dia da cidade | não move dinheiro |
| Serviços (abre/fecha) | `services.cpp:55-70` | semanal por cidade | demanda/faturamento seguem diários |
| Obras (§15/§19) | `advance_projects` | **diário** (prazo) | capital reservado no início; conclusão não cobra de novo |
| Frota | `logistics.cpp:495` | **mensal global `dia % 30`** | compra do governo (move dinheiro interno) — ver R3 |

Proporcionalidade da primeira janela: `elapsed = clamp(dia − month_marker, 1, 30)` e
`month_fraction = elapsed/30` aplicado a IPTU, taxas, repasse e manutenção
(`city_gov.cpp:380-383`, `:442-445`). Numa cidade de índice 5 o primeiro fechamento
cobre 5 dias, não um mês — **não há mês cheio cobrado sobre poucos dias** no mundo novo.

## 3. Determinismo

| Prova | Resultado |
| --- | --- |
| threads **1 × 16**, 120 d, IA + urbano | `save 807d49a9ba406062…`, `status 146f09f9c0e6b29d…`, `money 2.177.839.209,1499906`, 44.409 negócios — **idênticos**; só `meta.threads` difere |
| **CLI × Godot** com IA, 120 d (snapshot do dia 0 `--ai=1`) | `world_equivalence_compare.py` tolerância **0**: **0 diferenças**; replay com `inv_violations=0` |
| **TURBO × normal**, 60 d | `turbo_equivalence_test`: 4 mundos iguais (A/B e C/D) — exit 0 |
| **60 d × golden A0** regenerado pela A6 | meta byte-idêntico: `save c8dd374a0ddfbad2…`, `status 1783b7e55a1b455b…`, `money 2.177.839.209,1499767`, 42.107 negócios, 0 falências |

O `snap_ai.json` exportado carrega `version 15` e `cadence {7,30}` — a paridade CLI × Godot
também cobre o bloco novo do save.

## 4. A3 + A6 — comando no dia da cadência mensal

Teste próprio (mundo real, sem IA, dia 30): `set_market_stock` é emitido **entre dias**
para `AUS-ALI`, que é a cidade de índice 30 e fecha o mês exatamente no dia 30.

```text
[q17] comando no dia de cadencia de AUS-ALI: apply_day=30, estoque 0.79 -> 4242.00
[q17] dia 31: AUS-ALI nao refechou; AFG-HER fechou no 31 (cadencia por cidade).
[q17] dia 31: estoque de AUS-ALI ainda 7.77 (comando nao foi perdido).
```

- Log da fila: `day=30`, `apply_day=30` — o comando entra no dia em que foi emitido,
  **sem esperar o próximo fechamento mensal** e sem ser descartado pelo lote do dia.
- O fechamento do dia 30 não se repete no dia 31 (`month_marker` continua 30) e o
  fechamento do dia 31 pertence à cidade de índice 1 — o escalonamento por cidade vale
  para o fluxo de dinheiro e para a fila.
- `command_replay_test` (A3): 12 checagens, 14 comandos, 6 dias, **0 diferenças**.
- Limite conhecido, herdado da A3 §6: o caminho **adiado** (emissão de outra thread com
  o dia em andamento) não é exercitado por teste; ele é rejogável pelo `apply_day`.

## 5. A4 — `get_status()` no caminho diário

`grep -r 'get_status\s*(' core/ game/ map/ ui/` → só:

- `core/economy/ai/economic_ai.gd:91` e `group_investor.gd:319` — **IA GDScript legada**,
  instanciada apenas por 11 ferramentas de diagnóstico (A5 §6); nenhum caminho do jogo a
  toca (`economy.gd` não referencia `EconomicAI`);
- comentários em `economic_ai.gd:19,64,72`, `private_investor.gd:110` e
  `map/city_world.gd:599`.

O caminho diário usa `get_day_summary()` (`tick_scheduler.gd:93`) e consulta sob demanda
(`economy.gd:391-422`). `warm_status`/`last_summary` não aparecem em `core/game/map/ui`
fora do resumo leve. `status_query_test` (8 d, IA + §19): **989 comparações, 0
diferenças**, ficha de cidade **0,545 ms**.

## 6. A5 × A6 — a IA lê algum campo que só atualiza mensalmente?

Não. `AiView` (`gdextension/src/sim/ai/ai_view.h`) é montado no início da fase a partir
de campos que continuam **diários**, e as três regras (`ai_private`, `ai_government`,
`ai_group`) não leem `gdp`, `housing`, `rent_*`, `service_revenue` nem nenhum acumulador
`month_*` (grep em `sim/ai/*.cpp`: 0 ocorrências).

| Campo da `AiView` | Origem | Quem atualiza | Cadência |
| --- | --- | --- | --- |
| `markets.price` | `phase_prices` | núcleo | diária |
| `markets.stock/supply/surplus/sold/shortage` | produção/comércio/chegada | núcleo | diária |
| `markets.desired` | produção + compra de frota | núcleo | diária (o pulso da frota é mensal global — R3) |
| `pop.need/consumed/satisfaction` | `phase_income`/consumo | núcleo | diária |
| `pop.labor_force/unemployed` | `phase_labor` | núcleo | diária |
| `buildings.capacity/utilization/costs` | produção/contabilidade | núcleo | diária |
| `deposits.remaining_kt` | `phase_raw` | núcleo | diária |
| obras em andamento (`c_*`) | `advance_projects` + construção | núcleo | diária |
| incumbentes (`i_*`) | construções | núcleo | diária |
| `tot_surplus/sold/desired` | derivado dos mercados | núcleo | diária |
| `groups.cash/distributable/revenue_total` | contabilidade/fiscal | núcleo | diária |
| `city.firm_cash` | contabilidade | núcleo | diária (o IPTU mensal é um débito pontual — já era antes da A6) |
| `city.government_budget` | consumo/fiscal/comércio | núcleo | diária |
| `suitability`/`external_demand`/`land_capacity` | geografia | carga do mundo | estática |
| `recipes` | catálogo | carga do mundo | estática |

A agenda da IA segue o mesmo `CadenceState`: semanal por cidade
(`ai_phase.cpp:200-208`, `interval = max(1, weekly_period)`) e mensal por grupo
(`ai_group.cpp:184-190`), equivalentes a `cadence_due`. Nenhuma decisão muda por causa
do escalonamento — a equivalência de decisões da A5 continua valendo (§3).

## 7. Riscos de manutenção

| Item | Número | Observação |
| --- | --- | --- |
| `gdextension/src/economy_sim.cpp` | **6.767 linhas** | maior arquivo do projeto; fachada do binding, tocada por A3/A4/A5/A6 |
| `map/city_world.gd` | **4.478 linhas** | maior GDScript; fora do escopo da Parte A |
| `sim/save_state.cpp` | 2.223 linhas | único ponto de schema; migrações convivem com o formato atual |
| IA GDScript legada | **2.736 linhas** (`core/economy/ai/*.gd`) × **3.338 linhas** em `sim/ai/` | duplicação declarada; 11 ferramentas ainda instanciam a versão GDScript |
| Comentário desatualizado | `tools/economy_save_load_roundtrip_test.gd:10-13` | diz "a IA vive em GDScript (EconomicAI)"; desde a A5 ela é fase do tick |
| APIs antigas | 0 | grep por `demo_*`, `initialize_benchmark`, `run_benchmark` em `tools/`: nenhum |
| Testes | 8 suítes oficiais + 2 próprios verdes | ver §8 |

## 8. Achados e severidade

| ID | Achado | Severidade | Evidência | Ação |
| --- | --- | --- | --- | --- |
| R1 | `elapsed` é limitado a `kMonthDays` e não ao período: com `monthly_period > 30` (save editado/teste) a fração **subcobra**; para período ≤ 30 é exato e `period ≤ 1` dá 1/30 por dia | BAIXA | `city_gov.cpp:380-383`, `:442-445` | trocar o teto por `min(period, 30)` ou documentar o limite |
| R2 | Primeira janela da moradia: `rent_burden` divide o acumulado parcial por renda **mensal cheia** (29 de 30 cidades publicam ônus subestimado no 1º fechamento); a prefeitura escala por `elapsed`, a moradia não | BAIXA | `housing.cpp:274-303` | escalar o denominador por `elapsed` no 1º fechamento |
| R3 | Manutenção mensal da frota continua **global** (`dia % 30 == 0`, todas as nações) e **move dinheiro** (`fleet_buy_core` debita `government_budget` e credita vendedores) — a A6 §2.2 a lista como higiene e diz que "não movem dinheiro" | MÉDIA-BAIXA | `logistics.cpp:495-590`; `fleet_buy_core` `:350-…` | alinhar ao `CadenceState` na próxima fatia de perf; corrigir a nota |
| R3b | `nations[].labor` semanal ainda com literal 7 | BAIXA | `economy_sim.cpp:1228` | idem (publicação, não simulação) |
| R4 | `kServiceOpensPerDay = 5` passou a ser teto por **decisão semanal** (nome enganoso); a queda de −23% dos negócios é efeito esperado/aprovado, não regressão | BAIXA | `sim_types.h:150`; `services.cpp:738` | renomear para `kServiceOpensPerDecision` |
| R5 | `tools/s19_urban_report.py` imprime `IPTU/alvo` com numerador de janela 1..30 d e denominador de mês cheio → métrica dependente do dia e não comparável entre revisões | MÉDIA (ferramenta) | `s19_urban_report.py:37-41`; janelas medidas no teste de 95 d | normalizar por `elapsed` ou ler no dia do fechamento |
| R6 | Save **v14** carregado no meio da janela: `month_marker = 0` faz o 1º fechamento cobrar mês cheio após poucos dias (desloca IPTU/taxas/repasse/manutenção **uma vez**; não cria dinheiro) — risco já declarado na A6 §3.4 | BAIXA | `city_gov.cpp:380`; `save_state.cpp:2130` | aceito como transitório de migração |
| R7 | Dívida de manutenção da §7 (arquivo de 6.767 linhas, 2.736 linhas de IA duplicada, comentário desatualizado) | BAIXA-MÉDIA | §7 | fatiar `economy_sim.cpp` e remover a IA GDScript junto com as 11 ferramentas |
| R8 | Operacional: o **primeiro `--import`** numa worktree fria terminou com `0xC0000005` e deixou `~economy_…dll~RF*.TMP` em `gdextension/bin`; o `--import` seguinte falhou em carregar a extensão ("Error copying library"). Apagar o TMP resolveu (exit 0) | BAIXA (ambiente) | log do import; `gdextension/bin/` | documentar no onboarding; considerar limpar o TMP no `bench_canonical`/scripts |

Nenhum achado é ALTA. Nenhum cria ou destrói dinheiro, muda trajetória, quebra save ou
quebra determinismo.

## 9. O que não foi medido (e por quê)

- **Performance**: o bench oficial da Parte A (`partA_main_bench.json`, 39,42 ms/dia,
  3,94× vs A0) não foi re-medido — este parecer é de correção, e o host não estava
  ocioso (rodadas concorrentes minhas). Os números de p95/pior dia continuam os do
  relatório da A6/Parte A.
- **UI/`layout_report.json`**: a A6/A4 não mudam layout; não houve captura com janela.
- **`economy_long_run_test.gd`**: lento e fora do escopo; o sweep de 365 d e a corrida de
  365 d com IA cobrem o prazo longo.
- **`economy_panel_test.gd`**: falha conhecida por BOM pré-existente em
  `core/ui/city_economy_panel.gd`; não rodado.
- **Caminho adiado da fila A3** (emissão de outra thread durante o tick): sem teste, como
  já registrado na A3 §6.
- O teste de cadência rodou **95 d** (3–4 fechamentos por cidade) porque ler o save
  completo todos os dias em 365 d estourava memória/CPU; a cobertura de 365 d vem do
  `economy_world_test` (invariantes + drift + trajetória).

## 10. Reprodução

```powershell
# worktree própria, sem tocar a principal
git worktree add G:/projetos/opengs-wt-q17 2264cf9 --detach
cmake -S gdextension -B gdextension/build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug `
      -DGODOTCPP_DIR=G:/projetos/war-economy/godot-cpp
cmake --build gdextension/build-debug -j 12
godot --headless --path . --import

# save v15 / cadência / A3+A6
godot --headless --path . --script res://tools/economy_save_load_roundtrip_test.gd -- --days=64 --save-at=32
godot --headless --path . --script res://tools/economy_save_load_roundtrip_test.gd -- --days=90 --save-at=59
godot --headless --path . --script res://tools/economy_save_load_v11_compat_test.gd

# conservação em 365 d (com baseline nomeado)
godot --headless --path . --script res://tools/economy_world_test.gd -- --days=365 --ai=1 --urban=1 `
      --threads=1 --baseline=docs/economy/perf/partA_world_sweep.json

# determinismo
godot --headless --path . --script res://tools/world_equivalence.gd -- --days=120 --ai=1 --urban=1 --threads=1  --out=t1.json
godot --headless --path . --script res://tools/world_equivalence.gd -- --days=120 --ai=1 --urban=1 --threads=16 --out=t16.json
godot --headless --path . --script res://tools/export_world_snapshot.gd -- --out=snap.json --ai=1 --urban=1
gdextension/bin/opengs_sim_cli.exe --snapshot=snap.json --days=120 --ai=1 --out=cli.json
godot --headless --path . --script res://tools/snapshot_replay.gd -- --in=snap.json --days=120 --ai=1 --out=godot.json
python tools/world_equivalence_compare.py godot.json cli.json
godot --headless --path . --script res://tools/turbo_equivalence_test.gd

# A4
godot --headless --path . --script res://tools/status_query_test.gd -- --days=8 --ai=1 --urban=1
```

**Bateria oficial nesta revisão (8/8 exit 0):** `economy_save_load_v11_compat`,
`command_replay`, `housing_market`, `city_gov`, `urban_land`, `economy_integration`,
`economy_ai`, `economy_save_load_roundtrip` (30+30 d, 0 diffs) — mais
`status_query` (989 comparações, 0 diffs), `turbo_equivalence` (4 mundos), o round-trip
32/59 e o teste de cadência de 95 d. Nenhum teste falhou.

## 11. Conclusão

A Parte A está **fechada e consistente**: o save v15 é aditivo e reproduz o mundo no meio
do ciclo, o dinheiro se conserva em 365 d com a cadência escalonada (drift 0,0002, 0
violações), o resultado é determinístico em 1×16 threads, idêntico entre CLI e Godot e
bit a bit igual ao golden regenerado pela A6, e a IA não decide com dado mensal. As
ressalvas (R1–R8) são de higiene, medição e dívida de manutenção — ficam para a próxima
fatia, não bloqueiam a Parte A.
