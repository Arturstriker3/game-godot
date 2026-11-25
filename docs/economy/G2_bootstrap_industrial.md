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
- Parâmetros em `ai_brain.json` → `potential_demand {enabled, discount, cap}`,
  espelho em `AiBrainParams`. Off (`potential_demand` ou `ai_brain.enabled`)
  não executa cálculo nenhum. No save, a seção `potential_demand` só é gravada
  quando difere do default compilado (`false/0,5/1,0`), preservando o documento
  da rodada desligada.

## C/D — fase D

- `docs/economy/production_chains_spec.json`: `computer`, `phone` e `robots`
  trocam `chips` → `microchips`; `use` de `chips`/`microchips` atualizado.
- `tools/build_goods_chain.py build`: 125 bens / 124 receitas, `check` 0.
  Efeito nos preços: computer 22,3%, phone 22,2%, robots 25,8% (aceitos na G1).

## Testes

`sim_unit_tests` (novo `tests/test_g2_bootstrap.cpp`): propagação com desconto
0,5; cap como piso da âncora; desconto 0; piso não soma e não toca o mercado
real; piso desligado mantém a view e os totais; semeadura determinística
(duas cargas = mesmo plano).

## Rodada 365 d (t98, 2026-11-25)

DLL debug `15:29:44`, `threads=1`, mundo real. Os dois braços rodaram pelo
`tools/run_tests.ps1` (mapa temporário em `G:\opengs_tmp`), com o script
`tools/c1_world_ai_365.gd`.

| critério | meta | medido | resultado |
| --- | --- | --- | --- |
| violações de invariante | 0 | 0 | OK |
| falências | ≤ 3 | 3 | OK |
| portão de comida | ≤ 97/179 | **96/179** (mediana 0,96702) | OK |
| drift máximo diário | < 0,03 | **0,013952** | OK |
| `chips` e `microchips` produzidos + vendidos | ≥ 3 países | **6 e 7** | OK |
| MA30 de `chips` ÷ base | ≤ 1,10 | **1,1097** | **REPROVA (+0,97%)** |
| OFF idêntico ao baseline `49c797ef…` | bit a bit | `ded6f54b…` | **REPROVA (estrutural)** |

### ON — `--bootstrap=1 --ai-brain=1 --goods-diag=…`

- 365 d em **2 120,786 s (5,81 s/dia)**; 0 violações, 0 dias travados,
  **3 falências**, `trajectory_sha256 751933e0…`.
- **605 investimentos** (517 privado / 88 grupo), 11 445 tentativas e 2 880
  rejeições; anti-manada máxima **3 plantas/30 d**. Bloqueios contados fora das
  tentativas: exposição 2 071 381, anti-manada 1 233 718, perda 2 764,
  concorrência 14 279; gates privados c 901 / d 1 979; gates de grupo
  b 284 065 / c 1 969 817 / cap_max 29 320 / d 1 531 459 / e 1 730 809.
- **Termômetro de produção** (união dos 30 dias finais do diagnóstico — país
  com oferta **ou** venda > 0; o diag publica a janela, não o dia isolado):
  `steel` **6**, `plastic` **8**, `precision_tools` **5**, `computer` **5**,
  `phone` **3**; `chips` **6** (oferta em 5, venda em 6) e `microchips` **7**
  (oferta em 4, venda em 7). MA30 do dia 365 ÷ base: `chips` 16 905,50 /
  15 234,86 = **1,1097**; `microchips` 26 259,13 / 19 668,58 = **1,3351**;
  `computer` 1,1128; `phone` 1,6122; `steel` 1,2427; `plastic` 1,0593;
  `precision_tools` 1,1912.

### OFF — `--bootstrap=0 --ai-brain=0`

- 365 d em 373,011 s (1,02 s/dia); 0 falências, 0 violações, 0 travados,
  comida **97/179** (mediana 0,96840), drift **0,016673**,
  `trajectory_sha256 ded6f54b…`.
- **O controle bit a bit `49c797ef…` (D-126) não se reproduz neste branch.** As
  flags foram aplicadas (`bootstrap enabled=0`, cérebro `enabled=0`); a linha
  “12 plantas de referência semeadas” é a semente **antiga R1/Q-014**
  (`SEED-FLOUR`/`SEED-BREAD` em `economy_sim.cpp`), não a G2. O delta vs
  D-126 é a **parte C/D (dado)**: `computer`, `phone` e `robots` passam a
  consumir `microchips` em vez de `chips`, o que muda a produção mesmo com a
  semente e o piso desligados (drift 0,016524 → 0,016673 e comida 96 → 97).
  A G1 parte 1 (A′) era neutra no digest; a C/D não é. **O critério é
  inalcançável sem condicionar a C/D ou sem redefinir o baseline OFF** —
  decisão do dono/designer, sem afrouxar o limite.

### Perf — `potential_demand` (30 d, mesma DLL debug)

Para isolar o custo sem tocar em dados nem no núcleo, o harness ganhou o
override de laboratório `--brain-override=PATH` (merge profundo em
`ai_brain.json`; não existe no jogo). IA e bootstrap ligados nos dois braços:

| braço | wall 30 d | s/dia |
| --- | --- | --- |
| `potential_demand.enabled=false` | 57,264 s | **1,909** |
| `potential_demand.enabled=true` | 166,588 s | **5,553** |

A diferença é **2,9× (+191%)**, acima do limiar de 20% → **pendência de
otimização** (cache mensal da propagação da demanda potencial), **não
otimizada nesta rodada**. O A/B inclui o efeito indireto nas decisões da IA
(24 → 37 investimentos em 30 d), então o custo da propagação pura é ≤ 3,64 s/dia.

### Testes

`-Tier pr -Base main` (run `20261125_164155`): **44/46 OK**, com
`sim_unit_tests` e `c1_world_ai_90` verdes. **2 falhas**:
`population_template_test` e `services_template_test`, ambas com
`economia não publicou resumo antes do teste`. As duas esperam no máximo 600
`process_frame` pelo `last_summary` do autoload; a semeadura da G2 no dia 0
(271 plantas) alonga o startup e a janela de frames vence antes. Os dois
passavam na bateria de 12:21 (antes da G2). **Hipótese:** espera por frames
curta demais para o custo novo; a correção é esperar sinal ou tempo de parede
(pendência de QA, não alterada nesta rodada).


