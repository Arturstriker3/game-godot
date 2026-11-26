# G2 — Bootstrap industrial e demanda potencial na IA

**Data:** 2026-11-25 · **Branch:** `feat/g1-chips` (worktree `opengs-wt-g1b`) · **Spec:** `G2_bootstrap_industrial.md` (revisão 2)

Implementa as partes A (semente do dia 0 por produtores reais), B (piso de
demanda potencial na visão da IA) e D (computer/phone/robots passam a consumir
`microchips`), conforme a spec aprovada.

## A — `bootstrap_industry.json` + `set_bootstrap_industry`

- Dado novo: `data/definitions/economy/bootstrap_industry.json` (v1) com
  `enabled`, `budget_share` 0,50, `seed_fraction` 0,10, `capacity_min` 0,05,
  `capacity_max` 20, `max_cities_per_nation` 1, `score_inputs` e a lista
  ordenada de 21 bens com `source`/`params_source` por bem e
  `producers {ISO3: participação}` (a soma inclui `outros`, residual declarado).
- **Sem capacidade literal**: `cap_planta = share × âncora_mundial ×
  seed_fraction`, com a âncora saindo da mesma propagação de demanda potencial
  da parte B (§3.1), somada nas cidades. O piso `capacity_min` só exclui a
  nação quando `share × âncora × f < 0,05` (residual publicado).
- Cidade: `score = cobertura_insumo + escolaridade + 0,01×log10(1+orçamento)`,
  com a 1ª cidade `pop ≥ 5 M`, senão a maior `≥ 1 M`; o teto de 20 divide em
  blocos. Orçamento limitado a `budget_share` do municipal; o que não couber é
  `bootstrap_shortfall`. `charcoal` da spec é semeado como o bem `coal` (a
  receita `charcoal` produz `coal`).
- Leitor `set_bootstrap_industry(dict)` antes de `initialize_world`; o bloco novo
  de `seed_industry` roda no fim, com `build(..., cost, recipe_id)` e
  `strategic=true`. Arquivo ausente/`enabled=false` → vetor vazio → zero build.
- `status.bootstrap` publica `enabled`, `plants`, `shortfall` e
  `unseeded_share`.

## B — demanda potencial no cérebro

- `sim/potential_demand.{h,cpp}`: demanda final de `needs.json` (substitutos
  divididos igualmente, `durable_scale` como no consumo, computador
  institucional incluído) propagada para trás pela árvore de receitas, uma rota
  por produto (maior `Σ qty/yield`, empate = ordem do JSON), profundidade ≤ 8,
  desconto 0,5 por nível e cap 1,0 × âncora final.
- Um único ponto: `snapshot_ai_view` depois de copiar os mercados e antes dos
  totais. `d = max(0, potencial − desired)`; `row.desired += d` e
  `row.shortage += d` (piso, não soma). Produção, preços e o mercado real não
  mudam. Os totais passam a somar a cópia `view.markets` (idênticos quando o
  piso está desligado).
- Parâmetros em `ai_brain.json` → `potential_demand {enabled, discount, cap,
  recompute_days}`, espelho em `AiBrainParams`. Off (`potential_demand` ou
  `ai_brain.enabled`) não executa cálculo nenhum. No save, a seção
  `potential_demand` só é gravada quando difere do default compilado
  (`false/0,5/1,0/30`), preservando o documento da rodada desligada.
- **Cache mensal (perf):** o piso por cidade × produto é recalculado no dia 0 e
  a cada `recompute_days` dias (default 30) e reusado no meio; a população do
  piso fica congelada de propósito no mês. `stale` cobre cache vazio, dimensões
  e definições mudadas (catálogo, receitas, needs, governo, durabilidade,
  discount/cap), dia para trás (save/load) e período vencido. O cache vive na
  `AiPhase` (não é serializado): num load em outro processo ele recalcula no dia
  do load — determinístico, mas não é o valor congelado antes do save.

## C/D — fase D

- `docs/economy/production_chains_spec.json`: `computer`, `phone` e `robots`
  trocam `chips` → `microchips`; `use` de `chips`/`microchips` atualizado.
- `tools/build_goods_chain.py build`: 125 bens / 124 receitas, `check` 0.
  Efeito nos preços: computer 22,3%, phone 22,2%, robots 25,8% (aceitos na G1).

## Testes

`sim_unit_tests` (novo `tests/test_g2_bootstrap.cpp`): propagação com desconto
0,5; cap como piso da âncora; desconto 0; piso não soma e não toca o mercado
real; piso desligado mantém a view e os totais; semeadura determinística
(duas cargas = mesmo plano); cache mensal (dia 0 recalcula, dia 1 reusa com a
população congelada, dia 30 recalcula e mudança de parâmetro invalida).

## Rodada 365 d — rodada 1 (cache mensal, HEAD `6e62fe9b`, 25/11/2026)

> Rodada anterior à varredura mensal do investidor (rodada 2, abaixo). Os
> números de ON desta seção não valem para o HEAD novo; o OFF
> (`--bootstrap=0 --ai-brain=0`) é o controle bit a bit e continua `ded6f54b…`.

DLL debug `17:44:17`, `threads=1`, mundo real. As rodadas passaram pelo
`tools/run_tests.ps1` (mapa temporário em `G:\opengs_tmp`), com o script
`tools/c1_world_ai_365.gd`.

| critério | meta | medido | resultado |
| --- | --- | --- | --- |
| violações de invariante | 0 | 0 | OK |
| falências | ≤ 3 | **2** | OK |
| portão de comida | ≤ 97/179 | **97/179** (mediana 0,96930) | OK |
| drift máximo diário | < 0,03 | **0,014076** | OK |
| `chips` e `microchips` produzidos + vendidos | ≥ 3 países | **9 e 11** | OK |
| MA30 de `chips` dia 365 ÷ dia 0 | ≤ 1,15 | **0,9289** | OK |
| OFF estável entre 2 rodadas e igual ao C/D | bit a bit | `ded6f54b…` (2/2) | OK |

> **Critério de preço (decisão do dono, 2026-11-25):** a razão do aceite é
> **MA30(dia 365) ÷ MA30(dia 0) ≤ 1,15**; o quociente ÷ preço-base é
> **informativo**. O MA30 do dia 0 já nasce **1,1892× a base** (preços da C/D),
> então medir ÷base confundia nível herdado com deriva. O teto antigo (1,10×
> sobre a base) foi substituído, não afrouxado: 1,15 sobre a deriva do dia 0.

### ON — `--bootstrap=1 --ai-brain=1 --goods-diag=…`

- 365 d em **2 094,038 s (5,74 s/dia)**; 0 violações, 0 dias travados,
  **2 falências**, `trajectory_sha256 489dd83f…`.
- **689 investimentos** (602 privado / 87 grupo); anti-manada máxima
  3 plantas/30 d.
- Termômetro (união dos 30 dias finais do diagnóstico — país com oferta **ou**
  venda > 0): `chips` **9**, `microchips` **11**, `computer` 5, `phone` 6,
  `steel` 7, `plastic` 8, `precision_tools` 8.
- Aceite de preço (dia 365 ÷ dia 0, informativo fora de `chips`): `chips`
  **16 828,0 / 18 117,2 = 0,9289 ≤ 1,15 → APROVA** (÷base 1,1046,
  informativo); `microchips` 1,1132; `computer` 0,8973; `phone` 1,5991;
  `steel` 1,0958; `plastic` 0,9422; `precision_tools` 1,0923.
  Só `chips` tem critério declarado na spec (G1 §9.6 / G2 §5.7); os demais são
  leitura informativa. Nada foi afrouxado: a decisão do dono trocou a razão
  (÷dia 0) e o teto (1,15).
- A trajetória mudou vs a rodada sem cache (`751933e0…`): o piso agora é
  congelado dentro do mês — esperado e declarado.

### OFF — `--bootstrap=0 --ai-brain=0`

- 365 d em **361,538 s (0,99 s/dia)** (2ª rodada: 361,469 s); 0 falências,
  0 violações, 0 travados, comida **97/179** (mediana 0,96840), drift
  **0,016673**, `trajectory_sha256 ded6f54b…`.
- **Idêntico ao mundo "dados C/D sem mecanismos"**: o sha é o mesmo
  `ded6f54b…` da rodada que abriu a pendência do G2 (a C/D troca o insumo de
  `computer`/`phone`/`robots` de `chips` para `microchips`, então o mundo não é
  o controle D-126 `49c797ef…`).
- **Determinismo:** as 2 rodadas de 365 d deram o MESMO sha (`ded6f54b…`).
- Nada novo roda com as flags off: `ai_brain.enabled=0` /
  `potential_demand.enabled=0` fazem `apply_potential_floor` sair antes de
  qualquer cálculo (o cache nem é alocado nem atualizado), e o save só ganha a
  chave `recompute_days` quando o piso está ligado.

### Perf — `potential_demand` (G2, cache mensal; mesma DLL debug)

O harness mostrava **+191%** por dia (30 d) com o piso ligado. A hipótese — e a
razão do cache — era a propagação pela árvore de receitas rodando em TODO
`snapshot_ai_view` diário. O cache mensal
(`potential_demand.recompute_days`, default 30; 1 = diário) recalcula o piso no
dia 0 e a cada N dias e reusa o valor entre recálculos — determinístico e
coberto por `sim_unit_tests` (dia 0 recalcula, dia 1 reusa, dia 30 recalcula,
mudança de parâmetro invalida).

A/B de 30 d pelo `tools/run_tests.ps1` (mapa temporário em `G:\opengs_tmp`),
IA e bootstrap ligados nos dois braços (`--brain-override=PATH`, laboratório):

| braço | wall 30 d | s/dia |
| --- | --- | --- |
| `potential_demand.enabled=false` (antes do cache) | 55,773 s | **1,859** |
| `potential_demand.enabled=true` (antes do cache) | 162,961 s | **5,432** |
| `potential_demand.enabled=false` (depois) | 56,394 s | **1,880** |
| `potential_demand.enabled=true` (depois, cache 30 d) | 164,628 s | **5,488** |

- **O cache funciona:** a fase `ai_view` (onde a propagação roda) fica em
  ~5 ms/dia **igual nos dois braços**; a propagação amortizada é <0,3 ms/dia
  (uma execução custa poucos ms). Ou seja: `recompute_days=1` — o cálculo
  diário antigo — também seria barato; o 30 é o default pedido e congela o
  piso no mês de propósito.
- **Mas o overhead total não caiu** (+192%, alvo ≤ 20%): o custo dominante não
  era a propagação. Causa medida nos contadores do cérebro: `group_attempts`
  **211 067 → 835 581** em 30 d (4×) — o piso deixa `shortage > 0` em muito mais
  pares (cidade, produto) e o investidor de grupo avalia (forecast/gates) 4×
  mais candidatos; `group_rejected` 63 803 → 397 124. O tick medido no harness
  vai de **1,487 → 5,146 s/dia** (5 d, parede).
- O braço OFF é bit a bit estável: mesmo `trajectory_sha256` antes e depois do
  cache (30 d: `7fd0a848…`), porque com `enabled=false` o bloco do piso não
  executa nenhum cálculo (e `reset()` limpa o cache).
- **Conclusão honesta:** o cache mensal é necessário (a propagação nunca mais
  escala), mas o alvo de 20% **não é alcançável sem mexer na semântica do
  piso** (ex.: restringir a quais bens/cidades ele sinaliza). Decisão do
  dono/designer; o critério de perf segue reprovado.

Números brutos: `docs/economy/perf/g2_potential_cache.json`.

### Perf rodada 2 — varredura mensal escalonada do investidor (25/11/2026)

Com o cérebro ligado, cada agente só avalia candidatos a cada
`scan_interval_days` (default 30, declarado em `ai_brain.json`), com offset
determinístico `(dia − índice) % intervalo == 0`: a cidade `i` no investidor
privado e o grupo `i` no investidor de grupo. Entre varreduras não há candidato
novo (obras em andamento seguem); governo e armazém continuam semanais. O
privado saiu da visita semanal (a cada ~7 d) para a varredura mensal; o grupo
já era mensal pela A6 e agora lê o mesmo parâmetro. `enabled=false` ignora o
campo e mantém as cadências antigas — o braço OFF continua bit a bit.

Também foram movidos para antes do forecast do grupo os gates baratos: caixa
mínimo (`0,5 × capital`) e payback D-125 (capital ÷ margem × eficiência), que
reprovam sem montar o fluxo mensal.

A/B de 30 d na mesma DLL (`19:46:36`, HEAD `f91e77a3`), IA e bootstrap ligados
nos dois braços (`--brain-override` só alterna o piso):

| braço | wall 30 d | tick do harness | IA (perfil) | `group_attempts` |
| --- | --- | --- | --- | --- |
| piso OFF | 56,815 s | **1,574 s/dia** | 36,4 ms/dia | 208 258 |
| piso ON | 142,404 s | **4,425 s/dia** | 134,0 ms/dia | 687 981 |

- **A varredura do privado funcionou:** o ON caiu de 164,628 s (rodada 1) para
  142,404 s em 30 d (−13,5%). O `ai_view` segue ~1,9 ms/dia nos dois braços (o
  cache mensal do piso continua).
- **O alvo de 20% NÃO fecha:** +181% no tick do harness. O delta de IA é
  **97,6 ms/dia** (3,4% do delta de 2,85 s/dia); o resto do `tick_day()` é a
  montagem completa do `get_status()`, que fica **fora** do profiler e que o
  jogo (A4) não monta por dia. Mesmo zerando a IA, 20% não fecharia nesse
  harness.
- **A causa é o piso, não a frequência:** o grupo já varria uma vez por mês; a
  varredura mensal do privado corta a frequência dele, mas o piso faz o grupo
  avaliar **3,3×** mais candidatos por varredura (688 mil × 208 mil). Os gates
  baratos derrubaram as tentativas de 835 mil para 688 mil (−17,6%) sem alterar
  a decisão do gate (e).
- **Conclusão:** a varredura mensal é determinística e necessária (privado
  4,3× menos visitas), mas o critério de perf segue reprovado. Para fechar 20%
  é preciso mexer na semântica do piso no escopo do grupo (decisão do dono) ou
  medir sem o `get_status()` do harness. Números brutos:
  `docs/economy/perf/g2_scan_perf.json`.

## Rodada 365 d — rodada 2 (varredura mensal, HEAD `f91e77a3`)

DLL debug `19:46:36`, `threads=1`, mundo real, `--days=365`.

### ON — `--bootstrap=1 --ai-brain=1 --goods-diag=…`

- 365 d em **1 759,868 s (4,82 s/dia)**, contra 2 094,038 s (5,74 s/dia) da
  rodada 1: **−16,0%**.
- 0 violações, 0 dias travados, **2 falências** (≤3),
  `trajectory_sha256 f324c424…` (novo, esperado).
- Comida **97/179** abaixo de 0,975 (≤97; mediana 0,96741), drift **0,014477**
  (<0,03).
- **367 investimentos** (284 privado / 83 grupo), herd máx. **3**. A varredura
  mensal cortou as oportunidades do privado (602 → 284); o total caiu de 689
  para 367.
- Termômetro (união dos 30 dias finais; produção **ou** venda > 0): `chips`
  **8**, `microchips` **8**, `computer` 5, `phone` 3, `steel` 8, `plastic` 7,
  `precision_tools` 9.
- Aceite de preço (dia 365 ÷ dia 0): `chips` **0,9365 ≤ 1,15 → APROVA**
  (÷base 1,1137, informativo); `microchips` 0,9732; `computer` 0,9651;
  `phone` 1,4195; `steel` 1,0937; `plastic` 0,8919; `precision_tools` 1,0160.
  A cadência mensal **não** mudou o nível de `chips` (÷base 1,1046 → 1,1137);
  o que mudou foi a leitura: sobre o dia 0, o preço derivou 0,9365.

### OFF — `--bootstrap=0 --ai-brain=0`

- 365 d em **357,843 s (0,98 s/dia)**;
  **`trajectory_sha256 ded6f54b…` idêntico** ao controle (o mesmo da rodada 1
  e da pendência original).
- 0 falências, 0 violações, 0 travados, comida 97/179 (mediana 0,96840), drift
  0,016673; **3 106 investimentos** (o mundo sem mecanismos).
- Nada novo roda com as flags off: `enabled=false` ignora `scan_interval_days`
  e mantém as cadências antigas.

**Veredito da rodada 2:** a varredura mensal é determinística, reduz a parede
ON em 16% e mantém o OFF bit a bit, mas **o critério de perf (≤20%) e o MA30 de
`chips` (≤1,10) continuam reprovados**.

### Testes

`-Tier pr -Base main` (run `20261125_200945`, HEAD `f91e77a3`): **46/46 OK** —
`sim_unit_tests` **70/70** (novo caso da agenda `(dia − índice) % intervalo`),
`c1_world_ai_90`, `determinism_cross_process` e os testes de template verdes.
O A/B de 30 d (2 runs) e a rodada 365 (2 runs) também fecharam com exit 0.

`-Tier pr -Base main` (run `20261125_184841`, HEAD `6e62fe9b`): **46/46 OK** —
`sim_unit_tests` 69/69, `population_template_test` **35,57 s** e
`services_template_test` **28,05 s** verdes (correção abaixo), `c1_world_ai_90`
(**521,09 s**) e `determinism_cross_process` (**76,04 s**) verdes. A rodada
anterior (44/46) tinha as duas falhas de startup descritas em seguida.

`-Tier pr -Base main` (run `20261125_164155`, antes desta rodada): **44/46 OK**,
com `sim_unit_tests` e `c1_world_ai_90` verdes. **2 falhas**:
`population_template_test` e `services_template_test`, ambas com
`economia não publicou resumo antes do teste`. As duas esperavam no máximo 600
`process_frame` pelo `last_summary` do autoload; a semeadura da G2 no dia 0
(271 plantas) alonga o startup e a janela de frames vence antes. **Hipótese:** espera por frames
curta demais para o custo novo; a correção é esperar sinal ou tempo de parede
(pendência de QA, não alterada nesta rodada).

**Correção (G2 perf, `6e62fe9b`):** confirmado que a semeadura não roda nada
por frame nem recarrega nada caro — o primeiro dia roda no worker do tick
(~1,5-2 s no debug) e em headless os `process_frame` correm muito mais rápido
que o tick, então 600 frames podiam vencer antes do resumo. O teto passou a ser
de **tempo de parede (120 s)**, só nesses dois testes. Rodada direcionada:
`population_template_test` **36,17 s OK** e `services_template_test`
**28,72 s OK**. O `sim_unit_tests` ganhou o caso do cache (69/69).

## Perf rodada 3 — perfil antes de mexer (2026-11-25)

O harness `c1_world_ai_365.gd --profile` somava as fases lendo
`get_profile()` UMA vez no fim; os slots do profiler são sobrescritos a cada
tick, então o valor lido era o do ÚLTIMO dia e a divisão por N subreportava
~N×. A rodada 3 passou a acumular as fases dia a dia (`--profile`), separou o
dia (`tick_and_publish`) do resumo (`get_day_summary`) com
`--perf-path=publish` e instrumentou o interior da fase `ai` (piso, caches do
dia, grupos, privado, governo, armazém, refinaria, forecast, varredura de
`loss_blocked`). Os números de parede por relógio (`counter_tick_us_*`) fecham
com a soma das zonas.

**Decomposição (30 d, debug, threads=1, caminho do jogo; ms/dia):**

| | OFF (piso off) | ON rodada 2 (piso full) | ON rodada 3 |
|---|---:|---:|---:|
| `tick_and_publish` | 1 519,4 | 2 259 | 2 227,6 |
| core (fases do dia) | 26,2 | 26,4 | 26,1 |
| IA total | 38,0 | 63,0 | 61,2 |
| grupos | 35,1 | 59,3 | 57,9 |
| forecast | 22,7 | 37,8 | 36,8 |
| `loss_blocked` | 5,5 | 9,4 | 9,2 |
| piso aplicado (`ai_floor`) | — | 0,02 | 0,02 |
| `group_attempts` (30 d) | 208 079 | 368 851 | 368 187 |
| investimentos | 24 | 37 | 37 |

O piso **aplicado** custa 0,02 ms/dia (1 479 células cidade×bem/dia): não é
ele. O custo é o piso inflando `desired`/`shortage` na view: `append_candidate`
enumerava 849 cidades × bem (a barreira `shortage<=0 && sold<=0 && desired<=0`
nunca dispara) e o forecast mensal do grupo — 2/3 da IA — rodava 3,3× mais.
O `get_status()` do harness (52 ms/dia) e o `tick_day()` dos goldens não são o
caminho do jogo; o resumo leve da A4 custa 0,5 ms.

**Correção dirigida (uma):** a view guarda o mercado REAL antes do piso
(`real_shortage`/`real_desired`) e o candidato do grupo exige lacuna ou venda
reais. O sinal do piso em bem sem mercado fica restrito à **cidade-semente da
nação** (maior necessidade de comida) e só quando a nação ainda não tem
capacidade daquele bem — o bootstrap de cadeias novas continua, sem abrir
candidato em toda cidade. A varredura mensal do privado foi **revertida**
(privado/governo/armazém voltam à cadência semanal da A6; grupo segue mensal):
o perfil mostrou que ela não era o custo dominante e custava investimentos
(689→367 em 365 d).

**A/B 30 d depois (mesma DLL debug, caminho do jogo):** parede **56,2 →
77,3 s (+37,6%)**; `tick_and_publish` **1 519 → 2 228 ms/dia (+46,6%)**; IA
**38,0 → 61,2 ms/dia**; forecast **22,7 → 36,8**; tentativas de grupo
**208 079 → 368 187 (1,77×, era 3,3×)**; investimentos **24 → 37**. Alvo ≤20%
**não atingido**: o que resta é o piso ainda elevando `shortage` (dimensionar)
em bens com demanda real, que mantém 1,77× os forecasts. Nenhum número de
balanceamento foi inventado; o ganho de perf é 3,3× → 1,77× de candidatos sem
mudar a semântica do piso para bens com mercado.

**Pendente:** re-aceite 365 d ON/OFF e `-Tier pr -Base main` desta rodada; a
varredura mensal do privado foi revertida, então o aceite de 365 d da rodada 2
(367 investimentos) não vale mais.

## Perf rodada 4 — custo residual do forecast (2026-11-25)

O forecast mensal era o maior bloco da IA (36,8 ms/dia com o piso ligado). O
custo não estava na aritmética dos gates: `month_production` rodava
`eff::mean_between` **reiniciando a sonda D-125 no dia 0 para cada mês**, e o
cálculo era repetido no dimensionamento fechado (`unit_month`) e de novo no
fluxo final — O(H²) passos por candidato (H até 12 meses).

**Otimização (a) — memoização determinística dentro do forecast:** os meses do
horizonte saem de **uma única varredura** (`ai_brain::month_table`): a mesma
sonda, o mesmo passo diário e cada dia entra no mês a que pertence. Os totais
mensais têm a mesma sequência de adições e a mesma divisão de `mean_between`,
então o resultado é **bit-idêntico** — sha ON `3453589a…` e OFF `7fd0a848…`
da rodada 3 preservados nos 30 d, com os mesmos 24/37 investimentos.

**Correção barata adjacente:** o `loss_blocked` (varredura de todos os prédios
por candidato) passou para depois dos gates baratos do grupo. O predicado só
depende de grupo + bem, então o conjunto de rejeições é o mesmo.

**A/B 30 d, mesmo cenário, DLL debug (novo) × debug (rodada 3):**

| | OFF rodada 3 | OFF rodada 4 | ON rodada 3 | ON rodada 4 |
|---|---:|---:|---:|---:|
| parede 30 d | 56,19 s | **37,79 s** | 77,30 s | **48,74 s** |
| `tick_day` (`counter_tick_us_total`) | 64,3 ms/d | **44,2** | 87,3 | **54,4** |
| IA (perfil) | 38,0 | **18,1** | 61,2 | **29,0** |
| forecast | 22,7 | **4,1** | 36,8 | **6,7** |
| `loss_blocked` | 5,5 | **4,1** | 9,2 | **7,1** |
| `group_attempts` | 208 079 | 208 079 | 368 187 | 368 187 |
| investimentos | 24 | 24 | 37 | 37 |

O forecast caiu ~5,5× nos dois braços; o delta ON−OFF de parede caiu de
**21,1 s para 11,0 s em 30 d (+37,6% → +29,0%)**. A contagem de candidatos não
mudou (o resíduo continua sendo o piso elevando `shortage` em bens com mercado
real), mas cada forecast custa 5,5× menos.

### Critério de perf final — RELEASE (caminho do jogo)

DLL `template_release` compilada com `-j2` (`23:27:37`, `-O3`), A/B 30 d pelo
`c1_world_ai_365.gd --perf-path=publish`:

| braço | parede 30 d | ms/dia | sha |
|---|---:|---:|---|
| piso OFF | **8,086 s** | 269,5 | `7fd0a848…` |
| piso ON | **9,250 s** | 308,3 | `3453589a…` |

**Overhead = +1,164 s em 30 d = +38,8 ms/dia = +14,4% ≤ 20% → APROVA.**
Os shas são **os mesmos do debug** (30 d), então o release é bit-idêntico ao
debug neste mundo; a rodada de 365 d abaixo foi feita com a DLL release pelo
mesmo motivo (o caminho que o jogador roda).

## Re-aceite 365 d — rodada 4 (DLL release, HEAD do commit de perf)

### ON — `--bootstrap=1 --ai-brain=1 --goods-diag=…`

- 365 d em **121,125 s (0,33 s/dia)**; 0 violações, 0 dias travados,
  **2 falências** (≤3), `trajectory_sha256 b4a4ab38…`, drift **0,014201**
  (<0,03).
- Comida **95/179** abaixo de 0,975 (≤97; mediana 0,97068).
- **571 investimentos** (484 privado / 87 grupo), herd máx. **3**.
- `chips` e `microchips` produzidos/vendidos em **8 e 12 países** (≥3);
  `computer` 7, `phone` 6, `steel` 9, `plastic` 9, `precision_tools` 8.
- Aceite de preço (dia 365 ÷ dia 0): `chips` **1,0104 ≤ 1,15 → APROVA**
  (18 306,2 / 18 117,2; ÷base 1,2016, informativo); `microchips` 1,1733;
  `computer` 0,8971; `phone` 1,2705; `steel` 1,1164; `plastic` 0,9728;
  `precision_tools` 1,0990. **Observação declarada:** `microchips` (1,1733) e
  `phone` (1,2705) passam de 1,15 na própria deriva, mas não têm critério de
  preço na spec (só `chips` tem); ficam como leitura informativa e ponto de
  vigilância, não como gate.
- Comparação: a rodada 3 (sem a rodada 4) não tinha 365 d; a rodada 4 **não
  muda decisão** (30 d idêntico), então este é o primeiro 365 d do mundo da
  rodada 3 (correção do candidato de grupo): invests 367 → **571**.

### OFF — `--bootstrap=0 --ai-brain=0`

- 365 d em **88,983 s (0,24 s/dia)**; 0 violações, 0 travados, 0 falências,
  comida **97/179** (mediana 0,96840), drift **0,016915**,
  `trajectory_sha256 ded6f54b…` **idêntico** ao controle (o sha que abriu a
  pendência do G2), 3 106 investimentos.

**Veredito da rodada 4:** critério de perf **APROVADO em release (+14,4%)**;
365 d ON dentro de **todos** os critérios, inclusive o de preço de `chips`
(1,0104 ≤ 1,15 sobre o dia 0). O número antigo (1,2016) era a razão ÷base, que
carrega o nível herdado do dia 0 (1,1892× a base). Decisão do dono
(2026-11-25): razão do aceite = dia 365 ÷ dia 0, teto 1,15.

## Fechamento pré-merge (2026-11-26)

**Métrica.** A razão do aceite é **MA30(dia 365) ÷ MA30(dia 0)** por bem;
÷preço-base é informativo. Artigos medidos da DLL release da rodada 4
(`G:/opengs_tmp/g2r4/on365_rel.json`, sha `b4a4ab38…`): `chips` **1,0104**,
`microchips` 1,1733, `computer` 0,8971, `phone` 1,2705, `steel` 1,1164,
`plastic` 0,9728, `precision_tools` 1,0990. Só `chips` tem gate declarado.

**Ressalvas do QA (t108) fechadas:**

- **(a) Cache do piso no save (aditivo, sem bump de versão):** a chave
  `potential_cache` guarda dia, dimensões, assinatura das definições e as
  células **não nulas** (esparso, `[cidade, produto, valor]`). Save antigo — ou
  brain/piso desligado — não tem a chave e o load recalcula (comportamento
  anterior). Round-trip coberto em `test_roundtrip.cpp` (valores, assinatura e
  ausência da chave).
- **(b) `choose_recipe`:** o produto sem receita é declarado `no_recipe` antes
  do plano e o comparador de cidades nunca indexa `recipes[-1]`; âncora 0 vira
  linha `no_anchor` (aviso declarado), não omissão silenciosa.
- **(c) Testes do leitor:** `no_recipe`/`no_anchor`, residual `outros`/`no_city`,
  shortfall de orçamento, receita válida em linha semeada e gate da
  cidade-semente (5 M → maior ≥1 M → `no_city`) em `test_g2_bootstrap.cpp`.
- **(d) Drift OFF reconciliado:** `0,016673` e `0,016915` vinham da **mesma**
  rodada (`ded6f54b…`): o primeiro é `drift` (dia 365), o segundo
  `max_daily_drift`. O ON da rodada 4 sofreu a mesma troca na doc: `drift`
  0,014103 e máximo diário 0,014201. Indentação do `ai_brain.json` corrigida.

**Verificação:** `sim_unit_tests` **74/74** (754 asserções); 365 d OFF release
com **`ded6f54b…` inalterado** (0 falências/violações, comida 97/179, drift
0,016673); `-Tier pr -Base main` no fechamento.

