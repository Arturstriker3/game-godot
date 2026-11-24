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
   multa de `credit_penalty` (5% sobre principal + atraso): o caixa paga o que
   puder e o resto é **capitalizado no principal** (o tomador deve os 5% a
   mais; o ativo do banco cresce no mesmo valor — nenhum dinheiro é criado).
   A renegociação é regra da D-126 e respeita `crisis.enabled`.
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

## Correção do gatilho da usina (24/11/2026)

A primeira medição de 365 d mostrou **2.960 usinas hibernadas**: `react_power`
comparava a receita do despacho com a **folha inteira** da usina mesmo quando ela
não foi despachada (geração/receita 0), então quase todo o parque acumulava
`loss_days` e hibernava, derrubando a eletricidade e a comida. O gatilho passou a
ser o de sobrevivência, não o de folha ociosa:

* **Necessidade por rede/nação.** A crise ordena as usinas pelo **custo marginal
  declarado** (O&M por MWh; o combustível do dia continua no mérito do despacho)
  e mantém ativas as que cobrem a demanda do dia × (1 + `power_reserve_margin`).
  O despacho E2/E3 não declara margem de reserva, então ela é um **default de
  balanceamento declarado em `crisis.json`** (15%). A disponibilidade usada é a
  do próprio dia (`day_avail_mw`, com CF/calibração/eficiência); usina hibernada
  entra pelo potencial de reabertura.
* **Só a excedente é candidata.** Fora do prefixo necessário, a usina pode
  demitir, hibernar e ir a leilão (custo cheio, inclusive a folha parada — a
  capacidade não é necessária). **A necessária não hiberna nem vai a leilão**:
  se a receita do despacho não cobre o custo de caixa do dia (combustível + O&M +
  salários proporcionais à geração, o mesmo que `phase_power_settle` paga), ela
  apenas **registra `loss_days`** — sem mecanismo de pagamento de capacidade, não
  se destrói a reserva do sistema.
* **Histerese.** Reabre pelo sinal de preço que já existia **ou** quando a
  necessidade da rede volta, com `reopen_signal_days` (10 d) nos dois casos; a
  reabertura **zera `loss_days`/estágio**, então a usina não volta a hibernar no
  dia seguinte por herdar o contador.
* **Derivados, não save.** `PowerPlant.day_avail_mw` e `PowerPlant.day_cost` são
  calculados no despacho/liquidação do dia e zerados no início dele; não entram
  no save nem mudam a versão (continua v41).

## Dinheiro

Todo fluxo é transferência entre contas já contadas em `money_total`:

* rescisão: caixa do dono → famílias;
* crédito: banco cria depósito (dinheiro bancário) e credita o tomador; juro e
  amortização destroem depósito (mesma regra C3/C4);
* liquidação: `foreign_cash` → caixa do dono; estoque sai do mercado;
* hibernação: caixa do dono → caixa de serviços da cidade;
* leilão: caixa do comprador → credor/dono; nada é criado;
* multa da renegociação: caixa do tomador → banco (destrói depósito); a parte
  sem caixa vira principal (dívida e ativo do banco crescem juntos).

O dia informa ao invariante o dinheiro bancário criado/destruído pelo crédito de
crise, como a C3 já faz.

## Parâmetros

`data/definitions/economy/crisis.json` — todos **defaults de balanceamento**
declarados (a spec não traz números observados; não há fonte pública de
rescisão, deságio ou custo de hibernação por país). O espelho compilado fica em
`sim/crisis.h` e o bloco `definitions.crisis` do save preserva o que foi
aplicado. `set_crisis_params` sobrescreve. Na correção da usina entrou
**`power_reserve_margin: 0,15`** (margem de reserva de pico da rede; o despacho
E2/E3 não tinha uma, então é default declarado aqui — não é dado observado).

## Save

Save **v41 aditivo**: campos por construção (`crisis_stage`, `mothballed`,
`mothball_day`, dias sem manutenção, último layoff/crédito/liquidação), crédito
de giro por cidade, área/eficiência por estação de safra, campos das usinas e
contadores. Save v40 e anteriores carregam sem crise em curso (estado neutro);
nada é inventado para trás.

## Verificação

Ver `GAME_STATE.md` (D-126) e o teste
`godot --headless --path . --script res://tools/d126_crisis_test.gd`.

O teste cobre: demissão+rescisão, crédito de giro, **renegociação com multa
capitalizada**, liquidação pelo caminho do exterior (valor com deságio,
`foreign_cash` cai e dinheiro conservado), hibernação/reabertura, leilão, área
da fazenda, **refinaria e usina** pelas mesmas regras, **escada automática**
(cortar→demitir→crédito→liquidar→hibernar→leilão), **necessidade de pico**
(usina necessária não hiberna; excedente hiberna; reabre quando a demanda sobe;
despacho parcial com receita cobrindo o custo variável não soma `loss_days`),
save v41, migração v40 e determinismo.

A medição de 365 d roda pelo runner canônico com `OGS_C1_CONTROL` e `OGS_C1_OUT`
(o runner não passa argumentos de usuário):
`$env:OGS_C1_CONTROL="res://docs/economy/perf/c6_gate_base.json"`.

## Medição 365 d (24/11/2026)

`tools/c1_world_ai_365.gd --days=365 --control=res://docs/economy/perf/c6_gate_base.json`,
365 d com IA, `threads=1`, crise LIGADA (default do jogo) × braço `--crisis=0`
(mesmo JSON com `enabled=0`, só laboratório):

| braço | falências | leilões (compra/sem) | hibernações | demissões | vagas | comida | violações | drift |
|---|---|---|---|---|---|---|---|---|
| crise LIGADA (antes da correção) | 0 | 83 / 48 (todas usinas) | 3.214 (2.960 usinas + 8 prédios) | 4.670 | 477.537 | **121/179** | 0 | 0,017961 |
| crise LIGADA (corrigida) | 0 | 0 / 0 | 50 (39 usinas + 11 prédios) | 47 | 4.824 | **95/179** | 0 | 0,017058 |
| `--crisis=0` | 4 | — | 0 | 0 | 0 | 94/179 | 0 | 0,005618 |

**Aceite:** falências 0 (≤4), comida 95 (≤97), 0 violações, 0 travados.
**Drift reportado: 0,017058** (não ~0,0056). O braço sem crise não tem nenhum
canal da D-126 ativo; o braço corrigido ainda cria **1,766e9 de crédito de giro**
e faz **221 renegociações (285.606 de multa)** e 47 demissões — a diferença de
drift vem desses canais, não da eletricidade: as hibernações de usina caíram de
2.960 para 39 e o portão de comida fechou. Evidência
`G:\opengs_tmp\c1_d126_365_on_fix.json`.

**Leilões sem comprador da rodada anterior (só diagnóstico):** o contador era
todo de **usina** (`power_auctions=83`, `auction_value = power_auction_value`);
não havia bem/refinaria/fazenda no leilão. Para nomear os 48, a rodada antiga foi
reproduzida com a regra anterior num build de diagnóstico
(`c1_d126_365_legacy.json`, mesmos 83/48 e drift 0,017961; 47 linhas rastreadas
+ 1 usina sem cidade): por fonte, **gás 18, hídrica 9, carvão 6, óleo 5, solar 4,
nuclear 3, eólica 1, biomassa 1**; por nação, **JPN 14** (13 no dia 246), **CAN
8**, **KWT 4**, **KOR 4**, **MYS 3**, **ARE 3**, **USA 2** e 1 em
ISR/SGP/BHR/OMN/VEN/QAT/YEM/CRI/RUS. Em todas, `gov=0,00` e nenhum grupo com
caixa ≥ preço do livro (capex × 0,70): a usina hibernada não pagou nem 10% da
manutenção por 60 d e o Estado estava sem orçamento. Na rodada corrigida esses
leilões **desaparecem** (0/0).
