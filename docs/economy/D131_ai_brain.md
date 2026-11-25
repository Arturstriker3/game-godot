# D-131 (fases A e B) — Cérebro econômico da IA

Base: `balance/global-resources` (`4605ce90`, com a D-130); fase A no merge
`e6270a23`, fase B sobre ele. Branch `feat/d131-ai-brain`, worktree
`opengs-wt-d131`. Spec: `D-131_cerebro_economico_ia.md` (aprovada pelo dono).
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

As seções de R5 já entram no JSON e no save; a fase B lê R2, R4 e herd.

## Verificação

- `sim_unit_tests`: **53/53 casos** (7 da fase A + 6 da fase B: base do
  `unit_month`/`cap_max`, janela curta do `price_trend`, oferta/demanda por
  escopo, tetos de exposição, anti-manada de 3 plantas/capacidade e duas
  cidades no mesmo dia).
- `tools/run_tests.ps1 -Tier pr -Base balance/global-resources`: **41/41 OK**
  (run `20261124_215744`). Bateria completa (`-Tier quick`/104 testes + runs):
  OFF `enabled=false` **105/106** e ON `enabled=true` **104/105**; a única falha
  era `d126_crisis_test` esperando save v41 (o D-131 subiu para v42). A
  asserção foi atualizada para v42 e o teste re-rodou verde (1/1).
- **Equivalência `enabled=false`** — a rodada de 365 d do worktree é a baseline
  D-126 da main: `trajectory_sha256 49c797ef…` (igual a
  `c1_d126_365_final.json`; re-rodada com o binário final em
  `d131b_365_off2.json`), **0 falências**, comida 96/179, **0 violações**,
  0 travados, drift 0,016524.
- **365 d `enabled=true`** (fase B): **0 falências**, comida **94/179**
  (mediana 0,9703; critério ≤97), **0 violações**, 0 travados, **drift
  0,012479**, **43.665 tentativas**, **39.045 rejeições de caixa (89,4%)**,
  **11.262 candidatos únicos (cidade, produto) rejeitados pelo caixa** de
  12.495 candidatos únicos/mês, **154 investimentos** de planta nova (154
  privado / 0 grupo), **1.914.100 rejeições de risco** (exposição/anti-manada,
  contadas fora de `attempts`) e **anti-manada no máximo 3 plantas do mesmo bem
  em 30 d** (meta ≤3). JSON em `G:\opengs_tmp\d131b_365_on.json`.
- **Comparação com a fase A** (`enabled=true`): investimentos **1.065 → 154**,
  comida **90/179 → 94/179**, drift **0,016868 → 0,012479**; tentativas
  **12.950 → 43.665** porque a planta reprovada no forecast volta a tentar
  todo dia (não há backoff nesta fase).
- **Contadores separados:** `reject_share` passou a considerar só o gate de
  caixa (39.045/43.665 = 89,4%); `risk_rejected` (exposição/anti-manada) tem
  contador próprio e não infla a razão. A rodada anterior, com o contador
  único, dava 101,6% — foi só a métrica; a trajetória (`49c797ef…` off e
  `ba2d77b0…` on) não mudou com a correção.

## Pendências / próximas fases

- R5 (bloqueio por `loss_days`, crédito de investimento da cidade, paridade do
  investidor de grupo) fica para a fase C.
- O forecast segue conservador: a fase B derrubou os investimentos de 1.065
  para 154 e o drift de 0,016524 (off) para 0,012479. A medição existe para
  calibrar `capital_share`, `elasticity`, `price_floor` e os tetos de R4/herd
  antes de ligar no jogo.
