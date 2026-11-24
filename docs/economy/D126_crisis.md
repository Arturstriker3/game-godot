# D-126 — Sobrevivência em crise

Status: **em implementação** (núcleo, IA de reação, save e teste; UI pendente).
Spec aprovada pelo dono em 23/11/2026
(`specs/D-126_sobrevivencia_crise.md`).

## Regra

Quando a demanda cai e a planta acumula prejuízo, a firma tenta sobreviver antes
de fechar. A ordem é **fixa e determinística** (o mesmo dia e o mesmo save
produzem a mesma decisão):

1. **Cortar produção** — a regra existente de `loss_days` reduz
   `output_scale` (piso 20%).
2. **Demitir** — corta `layoff_share` (25%) das vagas, paga rescisão
   (`severance_days` × salário diário por vaga). O desemprego aparece no
   mercado de trabalho do dia seguinte; sem caixa, só a parte paga é demitida
   (não se cria passivo de rescisão). A recontratação é lenta e a eficiência
   D-125 cai pela regra de retenção.
3. **Crédito de capital de giro** — empréstimo curto no banco existente
   (público da nação), juro = taxa do banco + `credit_rate_add` (2 p.p.),
   limitado por `credit_working_days` (60 d) de custo e `credit_asset_share`
   (50%) do ativo imobilizado. Vale para firmas privadas, grupos e serviços.
   **Uma** renegociação após `credit_renegotiation_days` (15 d) em atraso, com
   multa de `credit_penalty` (5%).
4. **Liquidar estoque** — vende a fatia do estoque atribuída à planta (mesma
   regra de rateio da receita) ao exterior com `liquidation_discount` (30%),
   até `liquidation_share` (50%) por evento. O comprador é o exterior
   (`foreign_cash`); o dinheiro entra no caixa do dono e o estoque sai do
   mercado — soma zero.
5. **Hibernar (mothball)** — mantém o prédio, sem produção e sem folha, pagando
   `mothball_opex_share` (10%) do opex como manutenção. A eficiência cai para
   `mothball_efficiency_retention` (60%) da atual (mesma retenção da troca de
   produto D-125). Reabre pagando `reopen_cost_per_capacity` (20 XGS/t) quando o
   preço fica acima do custo por `reopen_signal_days` (10 d).
6. **Falência vira leilão** — o prédio **não some**: é vendido a outro grupo ou
   ao Estado com `auction_discount` (30%). O comprador paga o valor contábil
   com deságio; o dinheiro paga primeiro a rescisão em atraso e o crédito
   bancário, e o resto volta ao dono. O comprador reabre com a eficiência
   **inicial** da classe D-125. Só sem comprador o prédio é removido (regra
   antiga).

Fazenda com prejuízo persistente reduz a área em `farm_area_reduction` (25%)
para a **próxima** safra; a safra já plantada não muda (área plantada fica
congelada por estação no save).

Usinas e refinarias seguem as mesmas regras. Refinaria é `Building`; usina é
`PowerPlant` e usa os mesmos gatilhos, com manutenção de hibernação e leilão.

## Dinheiro

Todo fluxo é transferência entre contas já contadas em `money_total`:

* rescisão: caixa do dono → famílias;
* crédito: banco cria depósito (dinheiro bancário) e credita o tomador; juro e
  amortização destroem depósito (mesma regra C3/C4);
* liquidação: `foreign_cash` → caixa do dono; estoque sai do mercado;
* hibernação: caixa do dono → caixa de serviços da cidade;
* leilão: caixa do comprador → credor/dono; nada é criado.

O dia informa ao invariante o dinheiro bancário criado/destruído pelo crédito de
crise, como a C3 já faz.

## Parâmetros

`data/definitions/economy/crisis.json` — todos **defaults de balanceamento**
declarados (a spec não traz números observados; não há fonte pública de
rescisão, deságio ou custo de hibernação por país). O espelho compilado fica em
`sim/crisis.h` e o bloco `definitions.crisis` do save preserva o que foi
aplicado. `set_crisis_params` sobrescreve.

## Save

Save **v41 aditivo**: campos por construção (`crisis_stage`, `mothballed`,
`mothball_day`, dias sem manutenção, último layoff/crédito/liquidação), crédito
de giro por cidade, área/eficiência por estação de safra, campos das usinas e
contadores. Save v40 e anteriores carregam sem crise em curso (estado neutro);
nada é inventado para trás.

## Verificação

Ver `GAME_STATE.md` (D-126) e o teste
`godot --headless --path . --script res://tools/d126_crisis_test.gd`.
