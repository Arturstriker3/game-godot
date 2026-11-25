# D-131 (fase A) — Cérebro econômico da IA: previsão de caixa e preço esperado

Base: `balance/global-resources` (`4605ce90`, com a D-130). Branch `feat/d131-ai-brain`,
worktree `opengs-wt-d131`. Spec: `D-131_cerebro_economico_ia.md` (aprovada pelo dono).
**Nenhum número aqui é dado observado**: todos são defaults de balanceamento
declarados em `data/definitions/economy/ai_brain.json`.

## O que entrou (fase A)

1. **Parâmetros** — `AiBrainParams` em `sim/world_state.h` (espelho do JSON),
   `set_ai_brain_params`/`get_ai_brain_params`/`get_ai_brain_status` na fachada e
   o bloco aditivo `definitions.ai_brain` no **save v42**. Save v41 e anteriores
   carregam sem o bloco e ficam com `enabled=false` (default do struct).
2. **`eff::mean_between`** — média da curva D-125 num intervalo de dias de
   operação, com o MESMO laço diário de `mean_efficiency`
   (`mean_between(0, d-1) == mean_efficiency(d)`).
3. **R1 — forecast mensal** (`sim/ai_brain.{h,cpp}`) — mês de 30 d,
   `H = ceil(payback_limit/30)` limitado a `months_max = 12`; a obra dura
   `construction_days` (15 comida / 30 primário / 45 mina / 60 fábrica escalados
   pela capacidade) sem folha nem receita; depois rampa D-125 e folha CHEIA.
   Fluxos: receita `prod_m × share × preço_esperado`,
   `fixo = op_m × jobs × wage`, `var = op_m × opex × prod_m + insumo`, juro do
   capital emprestado (`loan_rate` anual/365). Gates:
   (a) `capex ≤ caixa × capital_share + crédito`;
   (b) `min_m saldo_m ≥ −crédito`;
   (c) `saldo_H ≥ 0`;
   (d) lucro operacional acumulado ≥ 0 até `trough_months_max = 6`;
   (e) payback D-125 com preço esperado ≤ limite.
   Dimensionamento fechado:
   `cap_max = (caixa + crédito) / max_m(unit_capex + burn_m)` e
   `capacidade = min(candidata, cap_max)`; abaixo de 2 t/dia não investe.
   Na fase A o privado usa `credit_approved = 0` (o crédito de investimento é
   fase C); a alça (a) usa `capital_share = 0,35`.
4. **R3 — preço esperado** — `trend = ma30(preço)/ma30_anterior` limitado a
   `[0,5; 2,0]` (série de 60 d, mínimo 5 observações, senão 1);
   `excesso = max(0, (oferta − demanda)/max(demanda, 0,001))`;
   `preço_esperado = preço_hoje × clamp(trend) × clamp(1 − elasticidade × excesso, 0,60, 1)`.
   A série é amostrada uma vez por dia por (cidade, produto) e o valor fica em
   cache até o próximo dia. Fase A: oferta futura = oferta de hoje (R2 vem na
   fase B).
5. **Integração** — em `ai_private.cpp`, `enabled=false` mantém o preço de hoje
   e nenhum forecast é chamado; `enabled=true` troca o preço de referência pelo
   esperado e aplica o gate/dimensionamento somente em **planta NOVA**
   (expansão e aprofundamento seguem a regra atual nesta fase).

## Defaults declarados (`ai_brain.json`)

| seção | chaves |
|---|---|
| root | `enabled=false` |
| forecast | `month_days=30`, `months_max=12`, `trough_months_max=6`, `capital_share=0,35` |
| competition | `scope_food=region`, `scope_other=world`, `demand_growth_days=30` |
| price | `trend_days=30`, `history_days=60`, `trend_min=0,5`, `trend_max=2,0`, `elasticity=0,35`, `price_floor=0,60` |
| risk | `max_good_share=0,35`, `max_sector_share=0,60`, `max_goods_per_agent=8` |
| herd | `max_new_plants_per_good_30d=3`, `max_new_capacity_share_30d=0,25`, `food_multiplier=2,0` |
| crisis_link | `block_loss_days=30` |

As seções de R4/R5 já entram no JSON e no save, mas só a fase A as lê.

## Verificação

- `sim_unit_tests`: **47/47 casos, 371 asserções** (7 casos D-131:
  `mean_between`, vale de caixa rejeita, dimensionamento reduz a capacidade,
  preço cai com excesso, tendência limitada, save v42/v41 e `enabled=false`
  inócuo).
- `tools/run_tests.ps1 -Tier pr -Base balance/global-resources`: **41/41 OK**
  (run `20261124_205517`).
- **Equivalência `enabled=false`** — a rodada de 90 d do worktree é idêntica à
  da main (`trajectory_sha256 17c6b0ee…`, 0 falências, 0 violações,
  drift 0,011852, comida 83/179). A rodada de 365 d `enabled=false` reproduz a
  baseline D-126: **0 falências, comida 96/179, 0 violações, drift 0,016524**.
- **365 d `enabled=true`**: **0 falências**, comida **90/179** (critério da
  correção: ≤97/179 abaixo de 0,975; mediana 0,974846), **0 violações**,
  0 travados, **drift 0,016868** (≤0,017), **12.950 tentativas de forecast**,
  **1.065 investimentos** de planta nova (91,7% rejeitados pelo gate de caixa).
  JSONs em `G:\opengs_tmp\d131_365_{off,on}.json`.

## Pendências / próximas fases

- R2 (concorrência: `pending_committed`, oferta em rampa/em construção, escopo
  regional da comida), R4 (exposição por bem/setor/agente e anti-manada) e R5
  (bloqueio por `loss_days`, crédito de investimento da cidade, paridade do
  investidor de grupo) ficam para as fases B/C.
- O forecast da fase A é conservador: 91,7% das decisões de planta nova do
  privado morreram no gate. A medição existe para calibrar `capital_share`,
  `elasticity` e `price_floor` antes de ligar no jogo.
