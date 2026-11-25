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
(duas cargas = mesmo plano). Rodada 365 d e medição das cadeias: preencher no
fechamento.
