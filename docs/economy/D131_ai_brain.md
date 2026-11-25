# D-131 (fases A, B e C) — Cérebro econômico da IA

Base: `balance/global-resources` (`4605ce90`, com a D-130); fase A no merge
`e6270a23`, fase B sobre ele. Branch `feat/d131-ai-brain`, worktree
`opengs-wt-d131`. Spec: `D-131_cerebro_economico_ia.md` (aprovada pelo dono).
**Nenhum número aqui é dado observado**: todos são defaults de balanceamento
declarados em `data/definitions/economy/ai_brain.json`.

## O que entrou (fase A)

1. **Parâmetros** — `AiBrainParams` em `sim/world_state.h` (espelho do JSON),
   `set_ai_brain_params`/`get_ai_brain_params`/`get_ai_brain_status` na fachada e
   o bloco aditivo `definitions.ai_brain` no **save v42**. Save v41 e anteriores
   não têm o bloco; o load de definitions é **aditivo**, então os parâmetros já
   aplicados pelo JSON (`enabled=true` no jogo) permanecem. O default compilado
   `false` do struct só vale para um mundo criado sem definitions e sem o bloco.
2. **`eff::mean_between`** — média da curva D-125 num intervalo de dias de
   operação, com o MESMO laço diário de `mean_efficiency`
   (`mean_between(0, d-1) == mean_efficiency(d)`).
3. **R1 — forecast mensal** (`sim/ai_brain.{h,cpp}`) — mês de 30 d,
   `H = ceil(payback_limit/30)` limitado a `months_max = 12`; a obra dura
   `construction_days` (15 comida / 30 primário / 45 mina / 60 fábrica escalados
   pela capacidade) sem folha nem receita; depois rampa D-125 e folha CHEIA.
   Fluxos (fase C): receita `prod_m × preço_esperado` (o excesso de oferta já
   está no preço, R3); `fixo = op_m × jobs × wage × capacity`,
   `var = op_m × opex × capacity + insumo` (opex por capacidade-dia, não por
   tonelada), juro do capital emprestado (`loan_rate` anual/365). A rampa
   desconta os `construction_days` do índice (dia de OPERAÇÃO, não de projeto).
   Gates:
   (a) `capex ≤ caixa × capital_share + crédito`;
   (b) `min_m saldo_m ≥ −crédito`;
   (c) `saldo_H ≥ −crédito` **e** operação ≥ 0 no último mês;
   (d) lucro operacional acumulado ≥ −crédito até `trough_months_max = 6` e
   sequência de perdas < `crisis.mothball_days` (com a crise ligada);
   (e) payback D-125 com preço esperado ≤ limite.
   Dimensionamento fechado:
   `cap_max = (caixa + crédito) / max_m(unit_capex + burn_m)` e
   `capacidade = min(candidata, cap_max)`; abaixo de 2 t/dia não investe
   (grupo: 0,5 t/dia). O crédito aprovado é o teto D-126 × aperto do banco.
   Na fase A o privado usa `credit_approved = 0`; a fase C liga o crédito de
   investimento com `capital_share = 0,35`.
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

## O que entrou (fase B)

6. **R2 — oferta efetiva e concorrência** (`sim/ai/ai_risk.{h,cpp}`, consumido em
   `ai_private.cpp`) — `supply_effective` soma a capacidade existente, a das
   obras em construção (com a rampa D-125 já herdada) e a `pending_committed`
   do mesmo turno; `region_slot` (landmass) para comida e `world` para os demais
   bens. `demand_future` usa a média móvel de `tot_desired` por
   `demand_growth_days` (30 d). Oferta e demanda do escopo alimentam o forecast
   (share de mercado e excesso do preço esperado); a fase A usava a oferta de
   hoje. Duas cidades no mesmo dia não duplicam a obra.
7. **R4 — exposição por agente e anti-manada** — cache por agente (privado =
   cidade; grupo = id) de custo por (bem, setor), com `max_good_share=0,35`,
   `max_sector_share=0,60` e `max_goods_per_agent=8`; a checagem vale para
   planta nova e para expansão/aprofundamento. `herd_allows` mantém uma janela
   rolante de 30 d de plantas e capacidade por bem: no máximo
   `max_new_plants_per_good_30d=3` plantas (×`food_multiplier=2` na comida) e
   `max_new_capacity_share_30d=0,25` da capacidade existente (a primeira planta
   da janela sempre passa).
8. **Ressalvas da fase A fechadas** — `unit_month` na mesma base do `cap_max`
   (custo unitário por tonelada); `price_trend` usa a janela disponível quando
   há menos de `trend_days` de série; escopos persistem em `params_map`/save; o
   status expõe `unique_candidates_month` (primeira tentativa de cada
   (cidade, produto) por mês) e `herd_max_plants_30d` (maior número de plantas
   do mesmo bem na janela rolante no fecho).
9. **Integração** — tudo atrás de `enabled`; desligado permanece bit-idêntico à
   main (nenhum contador, cache ou janela é alimentado).

## O que entrou (fase C) — diagnóstico e correção

10. **Diagnóstico por gate** — `ForecastResult::failed_gate` (1…6 = a…e,
    `cap_max` abaixo do mínimo) e `revenue_total`/`cost_total`/`spendable`; o
    status publica `gate_rejected`/`group_gate_rejected`, amostras
    (caixa, crédito, capex, receita, custo, payback, trough, saldo) e o funil
    `group_executed`/`group_created`. Nada disto vai ao save nem ao mundo.
11. **Duas causas raiz de unidade/escala no forecast:**
    - **opex contado com `op_days` duas vezes** — `tons` já traz os dias de
      operação; o núcleo grava `opex = capacity × opex_per_ton` e paga por
      capacidade-dia, então o custo passou a `op_days × opex × capacity`
      (sem aptidão/eficiência). O código antigo inflava o custo ~30× e
      reprovava quase tudo.
    - **dupla contagem da oferta na receita** — a receita multiplicava a
      produção por ela mesma como "parcela de mercado" (`P/(oferta+P)`) e o
      excesso de oferta já estava no preço esperado (R3). A receita passou a
      ser `produção × preço_esperado`; oferta/demanda do escopo continuam só no
      R3 (o `ForecastInput` não carrega mais `supply_future`/`demand_future`,
      campos que o forecast nunca lia).
12. **Crédito de investimento nos gates (c)/(d)** — o saldo final e o trough
    podem ficar dentro da linha de crédito aprovada, mas o projeto tem de
    estar com operação ≥ 0 no último mês do horizonte. Antes o crédito só
    servia de teto do capex; o grupo ficava com saldo final < 0 e zero obras.
13. **R4 coerente com o crédito** — a base de `max_good_share`/
    `max_sector_share` é `caixa + crédito aprovado` (grupo e privado), não o
    caixa sozinho.
14. **Grupo com a mesma régua do privado** — aptidão mínima 0,30 e depósito
    para minério (antes tentava toda cidade, 24% com receita 0); com o cérebro
    ligado o setor do grupo também é avaliado (a cesta alimentar continua
    primeiro) e o score antigo (`kMinReturn`) vira só ranking — o R1/R5 é o
    portão duro. `enabled=false` mantém a regra antiga bit a bit.
15. **Contagem do grupo** — o log usa `GRP-*` (não `AI-GRP-*`); o harness
    passou a publicar `group_created` do núcleo.
16. **Default ligado** — `ai_brain.json` passa a `enabled=true` (decisão do
    dono: o cérebro guia cada país). O default compilado do struct continua
    `false` como fallback de save/mundo sem o bloco; o caminho off é o
    override `enabled=false`/`OGS_C1_AI_BRAIN=0` e continua bit a bit.

## Defaults declarados (`ai_brain.json`)

| seção | chaves |
|---|---|
| root | `enabled=true` (default do jogo desde a fase C) |
| forecast | `month_days=30`, `months_max=12`, `trough_months_max=6`, `capital_share=0,35` |
| competition | `scope_food=region`, `scope_other=world`, `demand_growth_days=30` |
| price | `trend_days=30`, `history_days=60`, `trend_min=0,5`, `trend_max=2,0`, `elasticity=0,35`, `price_floor=0,60` |
| risk | `max_good_share=0,35`, `max_sector_share=0,60`, `max_goods_per_agent=8` |
| herd | `max_new_plants_per_good_30d=3`, `max_new_capacity_share_30d=0,25`, `food_multiplier=2,0` |
| crisis_link | `block_loss_days=30` |

As seções de R5 já entram no JSON e no save; a fase C lê R2, R4, herd e R5.

## Verificação (fase C + correções do QA, binário final)

- `sim_unit_tests`: **59/59 casos** (432 asserções), incluindo o caso novo
  `D-131: duas rodadas com o cerebro ligado fecham o mesmo digest` (determinismo
  do caminho ligado) e os casos de unidade da fase C.
- `tools/run_tests.ps1 -Tier pr -Base balance/global-resources`: **41/41 OK**
  (run `20261125_022712`) com o **default ligado** (`ai_brain.json`
  `enabled=true`): 90 d com **138 investimentos** (94 privado / 44 grupo),
  **0 falências**, comida **84/179** abaixo do portão (mediana 0,977164),
  **0 violações**, drift **0,010799**, anti-manada máx. 3 plantas/30 d.
- **365 d `enabled=true`** (default do jogo; JSON
  `G:\opengs_tmp\d131d_365_on.json`): **850 investimentos** (726 privado /
  **124 grupo**), **0 falências**, comida **93/179** abaixo de 0,975
  (mediana **0,973819**, média 0,908273; critério ≤97), **0 violações**, 0
  travados, **drift 0,013175** (máx. diário 0,013833; critério <0,03),
  anti-manada **máx. 3 plantas/30 d** (meta ≤3; comida permite 6). Forecast
  privado: 37.976 tentativas, 3.812 rejeições (gate c 1.262 / d 2.550). Grupo:
  3.610.671 tentativas, gates b 55.390 / c 698.792 / cap_max 6.216 / d 232.002 /
  e 107.994; **1.679 oportunidades executadas e 124 obras criadas**.
- **Equivalência `enabled=false`** (`OGS_C1_AI_BRAIN=0`; JSON
  `G:\opengs_tmp\d131d_365_off.json`): `trajectory_sha256 49c797ef…`,
  **bit-idêntico à baseline D-126**: 3.172 investimentos, 0 falências, comida
  96/179, **0 violações**, 0 travados, drift 0,016524.
- **Correções do QA (D-131b)** aplicadas neste binário: (1) `month_production`
  passa dias de OPERAÇÃO ao `mean_between` (desconta `construction_days`);
  (2) `pending_committed` é separado por (região, produto) — a oferta regional
  da comida não desconta obra de outra massa de terra; (3) o pedido de
  `city_borrow` divide o `gap` pelo aperto do banco (o desembolso não aplica o
  fator duas vezes); (4) `borrow_city` capa o pedido no teto D-126, então o
  comando via `replay_commands` respeita o mesmo limite da IA.
- **Comparação com a fase B** (`enabled=true`): investimentos **154 → 850**
  (grupo **0 → 124**), comida **94 → 93/179** (mediana 0,9703 → 0,9738), drift
  **0,012479 → 0,013175** (ainda abaixo de 0,03). A rodada de 90 d usada no
  diagnóstico mostrou o grupo com `group_created=44` já na fase C; a contagem
  do harness lê esse contador do núcleo (o log é `GRP-*`, não `AI-GRP-*`).

## Pendências / próximas fases

- R5 entrou na fase C: bloqueio D-126 por `loss_days`, crédito de
  investimento nos gates (c)/(d) e paridade do investidor de grupo.
- O forecast continua conservador por desenho, mas o caminho de grupo agora
  investe (o log é `GRP-*`; a contagem do harness é `group_created`).
- Calibrar `capital_share`, `elasticity`, `price_floor` e os tetos de
  R4/herd com as rodadas de 365 d antes de qualquer novo afrouxamento.
