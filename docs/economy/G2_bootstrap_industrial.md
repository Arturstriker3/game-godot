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

## Rodada 365 d (cache mensal, 25/11/2026)

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
| MA30 de `chips` × base | ≤ 1,10 | **1,1046** | **REPROVA (+0,46%)** |
| OFF estável entre 2 rodadas e igual ao C/D | bit a bit | `ded6f54b…` (2/2) | OK |

### ON — `--bootstrap=1 --ai-brain=1 --goods-diag=…`

- 365 d em **2 094,038 s (5,74 s/dia)**; 0 violações, 0 dias travados,
  **2 falências**, `trajectory_sha256 489dd83f…`.
- **689 investimentos** (602 privado / 87 grupo); anti-manada máxima
  3 plantas/30 d.
- Termômetro (união dos 30 dias finais do diagnóstico — país com oferta **ou**
  venda > 0): `chips` **9**, `microchips` **11**, `computer` 5, `phone` 6,
  `steel` 7, `plastic` 8, `precision_tools` 8.
- MA30 do dia 365 × base: `chips` 16 828,0 / 15 234,9 = **1,1046**;
  `microchips` 22 758,1 / 19 668,6 = 1,1571; `computer` 1,0925;
  `phone` 1,5082; `steel` 1,2895; `plastic` 1,0584; `precision_tools` 1,1318.
  **`chips` segue acima do teto 1,10×** (1,1046): a decisão de aceitar é do
  dono; nada foi afrouxado.
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

### Testes

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
