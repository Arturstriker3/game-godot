# C5 — Bolsa de valores por país, confiança e expectativas

> Fase C5 de `docs/plans/perf_pop_financas_plano.md` (PARTE C). Branch
> `feat/c5-stock-exchange`, worktree `opengs-wt-c5`, base `balance/global-resources`
> `f0d87fd4` (D-111: crédito e juro da safra ligados por padrão, save v32).
> Decisão **D-112**. Save **v33** (migração v32 → v33).
> Desenho escrito **antes** do código, como pedido na fase.

## 1. Escopo desta fase (o que entra e o que não entra)

Entra:

1. **Uma bolsa por nação**, com nome genérico/fictício e sem marca real.
2. **Grupos listados** por critério declarado (§3) e o papel (ticker) de cada um.
3. Ação como **ativo financeiro no balanço da C1**: detentores de um lado,
   emissor do outro, com valor de mercado (§4).
4. **Preço** = valor fundamental × índice de confiança, com **passo
   convergente** (sem salto, sem teto artificial) (§5).
5. **Índice de confiança por nação** a partir de sinais **já medidos**, com
   **expectativas adaptativas** (§6).
6. Efeitos mínimos e explícitos: **riqueza das famílias** (medida e publicada),
   **custo de capital / decisão de investimento dos grupos (q de Tobin)**,
   **dividendos pagos aos acionistas** e **remessa do estrangeiro pelo câmbio**
   (§7).
7. Status A4 (`get_stock_status()`), resumo diário em `get_status()` e save v33.

Não entra (por decisão de fase, declarado no status):

- **Livro de ordens, negociação, IPO e regulação** — a carteira só muda por
  dividendos nesta fase. Compra/venda de ações, IPO pelo jogador e regulação
  ficam para depois da C5.
- **Crash, corrida e resolução** — C6.
- **Canal de consumo pela riqueza** (a queda de preço reduzir consumo dos
  ricos, citado no plano): a riqueza é medida e publicada, mas o consumo não
  muda nesta fase. Declarado em `effects` (§8) para não fingir que existe.

## 2. Identidade e nome da bolsa

- Uma bolsa por nação, identificada pelo **ISO3 da nação** (`id = nation`).
- **Nome:** `<ISO3> Exchange`, com `name_status: "game_default"`. É um nome
  **fictício e genérico**: nenhuma bolsa real (sigla ou marca) é referida,
  porque o repositório não tem fonte para nomes e inventar 179 marcas seria
  pior do que assumir o default de jogo. Um nome por nação pode entrar depois
  por dado, sem mudar o contrato (o campo já existe no save e no status).
- A bolsa de uma nação lista os grupos **cuja origem é aquela nação**
  (`Group::origin_nation`). Grupo sem origem declarada não é listado
  (`reason: "no_origin"`).

## 3. Quem é listado (critério declarado)

Não existe fonte de listagem real por empresa no repositório. O critério é
**default de jogo declarado** e verificável no estado:

| condição | efeito | motivo publicado |
|---|---|---|
| `origin_nation` vazio | não listado | `no_origin` |
| `state_owned == true` | não listado | `state_owned` (estatal é unidade orçamentária; o Estado entra na bolsa pela carteira do **banco público**, cujo dono é o Tesouro) |
| `revenue_total <= 0` | não listado | `no_measured_earnings` |
| caso contrário | **listado** | `listed` |

Um papel por grupo. A lista é recalculada a cada fechamento semanal; grupo que
deixa de ter receita medida sai da lista (e o valor das ações vai a zero de
forma declarada, `delisted_day`), não é removido em silêncio.

## 4. Ação no balanço da C1

A ação é um **ativo financeiro do detentor** e um **claim do emissor**:

```
valor de mercado = price × shares
Σ carteiras (famílias, bancos, grupos, exterior) = Σ market cap dos emissores
```

- **Emissor:** o grupo. O market cap é claim sobre os ativos **reais** que a C1
  já conta (estoque + capital fixo + caixa − dívida). Por isso ele **não** é
  somado ao patrimônio do grupo (seria contar duas vezes — a mesma razão pela
  qual a C1 exclui `stratum_equity`). O grupo publica `market_cap`, `book_equity`
  e `q` (§7).
- **Detentores iniciais** (default de jogo declarado, sem fonte): famílias do
  estrato alto da nação **55 %**, bancos da nação **20 %** (rateado pela
  participação da C4), grupos não listados da mesma nação **15 %**, exterior
  **10 %**. Sem banco estrangeiro na nação a fatia do exterior também sai do
  dado: nesse caso ela vai para as famílias (a soma continua 100 %).
- **Preço inicial:** o **próprio fundamental** sobre o lucro medido do dia de
  formação (EMA semeada com o lucro do dia, confiança 1,0) — o papel nasce
  avaliado pelo lucro, não por um múltiplo de balanço. O patrimônio medido do
  grupo (`book_equity`) fica como referência do q. `shares` fixo por papel
  (1 000 000) só para dar granularidade. No dia 0 o market cap não é
  arbitrário: é `payout × E[lucro] × 365 / (taxa + prêmio)`.
- **Livro do dia (`balance::Ledger`)** ganha `stock_assets` (carteiras) e
  `stock_claims` (emissores). Os dois são **iguais por construção** e entram com
  sinais opostos em `financial_sum()`, portanto a identidade da C1 **não muda**
  por valorização. As carteiras são publicadas no status da bolsa por detentor;
  elas **não** entram em `Sheet::net_worth` (seria contar duas vezes o mesmo
  ativo do grupo, a mesma razão pela qual a C1 exclui `stratum_equity`).
- **Conservação de dinheiro:** valorização **não** cria nem destrói dinheiro —
  é preço de ativo, não caixa. O invariante diário continua comparando
  `money_after` com `money_before + emissão + dinheiro bancário`; a bolsa não
  aparece nessa conta (só dividendos, que movem caixa).

## 5. Preço: fundamental × confiança, passo convergente

```
E[lucro]      = EMA do lucro diário medido do grupo × 365        (k_exp = 0,05/dia)
dividendo     = payout_ratio × max(0, E[lucro])                   (payout_ratio = 0,5)
taxa_desconto = max(0,01, taxa_básica_da_nação + prêmio_de_risco) (prêmio = 0,04)
fundamental   = dividendo / taxa_desconto / shares
alvo          = fundamental × confiança_da_nação
price        += (alvo - price) × 0,35           (fechamento SEMANAL, dia % 7 == 0)
```

- O **passo é convergente**: um único movimento semanal cobre no máximo 35 % da
  distância até o alvo; nunca há salto de um dia para o outro (a atualização é
  semanal) e **não há teto de preço**. O piso do denominador (1 %) evita
  divisão por zero, não limita o preço.
- A taxa básica é a da **C2b1** por moeda da nação (`monetary::rate_for_nation`);
  juro mais alto aumenta o denominador e derruba o alvo — o canal de aceite
  "choque de juro derruba o índice".
- Lucro esperado é **medido** (EMA do lucro diário das construções do grupo,
  `balance::group_sheet`); grupo sem lucro medido tem dividendo 0 e fundamental
  0, então o preço converge para 0 — ausência declarada, não valor inventado.

## 6. Índice de confiança e expectativas adaptativas

Por nação, sinais **já medidos no estado** (nada é amostrado nem inventado):

| sinal | medida no núcleo | escala (default de jogo) | peso |
|---|---|---|---|
| inflação vs meta | inflação 12 m da moeda da nação (`central_bank`) | meta 3 %; ±10 p.p. | 0,10 |
| juros | taxa básica − neutra (`monetary`) | ±5 p.p. | 0,05 |
| crescimento/produção | variação do valor de produção das construções da nação (EMA) | ±10 % ao ano | 0,08 |
| falências | construções falidas / construções da nação | 2 % | 0,05 |
| desvalorizações | eventos `devaluation` da moeda nos últimos 365 d | 3 eventos | 0,04 |
| desemprego | desempregados / população ativa da nação vs 4 % natural | 4 p.p. | 0,03 |

```
raw        = 1 + Σ peso_i × clamp(sinal_i, -1, +1)      (faixa 0,65 … 1,35)
confiança += (raw - confiança) × 0,05                    (expectativa adaptativa, diária)
```

- Sinal **sem dado** (moeda ausente, sem construções, sem série) é **excluído** e
  listado em `signals.missing[]`; os pesos restantes são renormalizados. A
  confiança começa em **1,0** (nem otimista nem pessimista).
- A expectativa é adaptativa: a confiança persegue o sinal bruto a 5 %/dia
  (~14 dias para metade da distância), então um choque de hoje não vira preço
  hoje — é o que "expectativas" significa aqui.

## 7. Efeitos mínimos e explícitos

1. **Riqueza das famílias:** a carteira do estrato alto entra no patrimônio das
   famílias (`stratum_equity_market`), publicada por nação no status A4 e em
   `get_balance_status()`. Nesta fase é **medida**, não move consumo.
2. **Custo de capital / investimento (q de Tobin):** `q = market_cap /
   book_equity`. A IA de grupo multiplica o score do candidato por
   `clamp(0,5 + 0,5 × q, 0,8 … 1,2)` — q > 1 barateia o capital, q < 1 encarece.
   O fator é limitado justamente para não transformar a bolsa em multiplicador
   de investimento sem calibração; o valor publicado é o q medido.
3. **Dividendos:** no fechamento mensal (dia múltiplo de 30),
   `payout = payout_ratio × max(0, lucro do período medido)`, limitado pelo
   caixa do grupo (nunca caixa negativo; o limite é publicado em
   `dividend_limited`). O caixa sai do grupo e é creditado **proporcional à
   carteira**: famílias → `stratum_savings` + escalar da poupança; bancos →
   caixa do banco; grupos → caixa do grupo; exterior → `foreign_cash`. Soma
   zero: dinheiro conservado.
4. **Estrangeiro remete pelo câmbio:** a fatia do exterior é atribuída à
   **nação de origem do banco estrangeiro** daquela nação (dado da C4). A
   remessa é publicada em âncora e na moeda de origem pela paridade da C2c
   (`anchor_value × parity_of`); sem banco estrangeiro, a fatia não existe e a
   remessa é 0 com `origin: ""` (ausência declarada).
5. **Valorização não cria dinheiro:** não há caixa no caminho do preço; o teste
   verde da conservação diária é a prova (§9).

## 8. Status A4 e resumo diário

`get_stock_status()` (sob demanda):

```
policy:    { enabled, risk_premium, payout_ratio, step, expectation_k,
             dividend_period_days, shares, holders: {...}, price_floor_rate,
             trading: "absent (C5)", ipo: "later", crash: "C6" }
totals:    { market_cap, book_equity, holdings, index_global, confidence_p50,
             dividends_paid, remittance, remittance_origin, listed, unlisted }
exchanges: { nation, name, name_status, currency, policy_rate, confidence,
             signals: { inflation, rate, production, bankruptcies,
                        devaluations, unemployment, missing: [] },
             index, index_base, market_cap, listed, top: [ ... ] }
tickers:   { group_id, nation, exchange, name, listed, reason, shares, price,
             fundamental, market_cap, book_equity, q, pe, profit_expected,
             dividend_last, dividend_day, dividend_limited,
             holders: { households_high, banks, groups, foreign } }
```

`get_status()` ganha um resumo barato `stock: { index_p50, confidence_p50,
market_cap, dividends_today, listed }` (índice global/mediana e confiança
mediana), sem varrer papéis.

## 9. Save v33 e migração v32 → v33

Bloco aditivo `stock`:

```json
{ "version": 33,
  "stock": {
    "enabled": true,
    "policy": { "investment_channel": true, "risk_premium": 0.0,
                "payout_ratio": 0.0, "step": 0.0, "expectation_k": 0.0 },
    "exchanges": { "BRA": { "name": "BRA Exchange", "name_status": "game_default",
                            "currency": "BRL", "confidence": 1.0,
                            "index_base": 0.0, "last_price_day": -1,
                            "production_ema": 0.0, "production_seeded": false,
                            "dividends_paid": 0.0, "remittance": 0.0,
                            "remittance_origin": 0.0 } },
    "tickers": { "<group id>": { "nation": "BRA", "shares": 1000000.0,
                                 "price": 0.0, "book_equity": 0.0,
                                 "listed": true, "reason": "listed",
                                 "profit_ema": 0.0, "dividend_last": 0.0,
                                 "dividend_limited": 0.0, "dividend_day": -1,
                                 "listed_day": 0, "delisted_day": -1,
                                 "holdings": { "households_high": 0.0,
                                               "banks": {"BRA/public": 0.0},
                                               "groups": {"<id>": 0.0},
                                               "foreign": 0.0 } } }
  } }
```

`production_ema`/`production_seeded` são medição que **afeta a trajetória**
(EMA da produção da nação na confiança) e por isso vão ao save, como a janela
do déficit da C2b1: sem elas a continuação após load divergiria do caminho
direto. `book_equity` é o patrimônio medido no último fechamento (base do q).

**Migração v32 → v33:** o estado antigo não tem bolsa. A migração **cria** as
bolsas das nações com grupos listados (§3), com preço inicial igual ao
fundamental medido (lucro do dia de formação, confiança 1,0) e a carteira pelo
default declarado do §4. Nada de preço ou carteira é inventado: preço =
`payout × E[lucro] × 365 / (taxa + prêmio) / ações` e carteira = rateio
declarado. Grupo não listado fica fora (nada é criado). Round-trip v33 é
bit-exato e a continuação após load é idêntica.

## 10. Invariantes e critérios de aceite

Invariantes diários acrescentados a `SimCore::check_invariants`:

1. `Σ carteiras = Σ market cap` (tolerância relativa 1e-9) e `stock_assets =
   stock_claims` no livro do dia;
2. `price >= 0`, `shares > 0`, carteira de cada detentor em `[0, shares]`;
3. conservação de dinheiro inalterada (a bolsa só move caixa em dividendo).

Aceite da fase:

- preço converge ao fundamental × confiança em regime estável (≤ 5 % após 8
  semanas);
- choque de juro derruba o índice (mesmo sinal, sem salto diário);
- confiança cai com choque de inflação/desvalorização e volta com expectativa
  adaptativa;
- dividendos fecham no balanço (soma zero, dinheiro conservado);
- valorização não cria dinheiro (`money_total` idêntico antes/depois do passo de
  preço);
- migração v32 → v33 cria bolsa coerente e round-trip é bit-exato;
- mundo 365 d com IA: falências ≤ 3, portão de comida ≤ 98/179, violações 0
  (gate da D-111). Se estourar, A/B por canal: só preço/riqueza × só
  investimento.

## 11. Testes

- `tools/c5_stock_exchange_test.gd` — convergência, juro alto, choque de
  confiança, dividendos, valorização sem dinheiro novo, identidade do balanço,
  round-trip/migração.
- Vizinhos pelo runner: `c4_banks`, `c3_bank_credit`, `c3_group_balance`,
  `c2_central_bank`, `c2b1_monetary`, `c2b2_fx_gold`, `currency_trade`,
  `economy_integration`, `services`, `storage_g2`, `agriculture_seasonal`,
  `c1_balance` (versão de save atualizada onde era a versão corrente).
- `tools/c1_world_ai_365.gd` — aceite de 365 d com IA (`threads=1`).

## 12. Medição (365 d com IA, `threads=1`, DLL debug, um Godot por vez)

`docs/economy/perf/c5_gate_default.json`, 365 d, **240,8 s** (base D-111: 238-247 s):

| indicador | C5 (default) | base D-111 |
|---|---|---|
| falências | **3** | 2 |
| portão de comida (30 d) | **93/179** (mediana 0,9725) | 98/179 (0,9693) |
| violações de invariante | **0** | 0 |
| travados | 0 | 0 |
| hash de trajetória | `bb9b5cca…` | `66534505…` |
| dinheiro | 6 559 257 558,69 | 6 523 551 982,90 |
| drift máximo diário | 0,547 % (estoque de dinheiro bancário) | 0,377 % |
| calotes (crédito) | 1 (baixa 7 649,01) | 0 |

Aceite da fase (D-111): falências ≤ 3, portão ≤ 98/179 e violações 0 — **cumprido**
(3 / 93 / 0). A C5 **melhora o portão** (98 → 93) e adiciona **uma falência**
(2 → 3), no teto do critério; não há A/B por canal porque o limite não estourou.

Bolsa no dia 365 (`stock` do JSON):

| indicador | valor |
|---|---|
| praças / papéis listados | 179 / **260** |
| índice global (base 100 na formação) | **113,1** |
| variação no ano por praça p10 / p50 / p90 | **−78,2 % / +3,7 % / +304,7 %** |
| confiança por nação p10 / p50 / p90 | **0,952 / 1,034 / 1,096** |
| market cap / patrimônio medido | 106,9 bi / 26,4 bi |
| q de Tobin p50 | **15,39** |
| dividendos pagos no ano | **15 154 506,91** |
| remessa do exterior (âncora / origem) | 916 914,33 / 1 238 129,26 |
| investimento medido (capex acumulado dos grupos) | **19 058 661,48** |
| identidade carteiras − market cap | **0,0** |

**Artefatos declarados (não escondidos):**

1. **Preço/lucro** é limitado pela própria fórmula: `P/E = payout / (taxa +
   prêmio)` ∈ **1,5 … 12,5** (0,5/0,34 a 0,5/0,04) — plausível por construção.
2. **q de Tobin alto (p50 15,4)**: o modelo mede uma rentabilidade sobre o
   capital registrado muito alta (o livro do grupo é conservador), então o
   preço fundamental fica muito acima do patrimônio. O canal de investimento é
   **limitado** (0,8…1,2) justamente para o q não virar multiplicador; o valor
   bruto fica publicado para a calibração.
3. **Dispersão da variação anual** (p10 −78 %, p90 +305 %): a EMA do lucro é
   semeada no dia de formação com **um** dia de lucro medido; empresas com um
   dia atípico nascem muito caras ou muito baratas e o preço persegue isso sem
   teto. É dívida de calibração (semente com mais de um dia de histórico e/ou
   prêmio de risco por volatilidade), não um número inventado. A C6 herda essa
   dívida junto com crash/corrida.
4. **Falência a mais**: a terceira falência entra pelo canal de investimento
   (score × q saturado em 1,2 em quase todos os papéis), não pelo preço — o
   preço não move dinheiro. Fica no teto (3 ≤ 3) e registrada para o QA.

**Defeito latente corrigido nesta fase:** o crédito de dividendo ao domicílio
(`fiscal.cpp`) e o débito de capital da IA (`actions.cpp`) podiam arredondar
acima do caixa do grupo e deixar `cash = −1,16e-10`; o load normaliza em 0 e o
round-trip deixava de ser bit-exato (`c2_central_bank_test`). Os dois débitos
passam a ser limitados a 0 — nada de caixa negativo.

**Testes:** `c5_stock_exchange_test` (C1-C7) e 14 vizinhos verdes pelo runner
(`c1_balance`, `c2_central_bank`, `c2b1_monetary`, `c2b2_fx_gold`,
`c2c_real_fx`, `c3_bank_credit`, `c3_group_balance`, `c4_banks`,
`currency_trade`, `economy_integration`, `services`, `storage_g2`,
`agriculture_seasonal`), com a versão de save corrente (33) nos testes que a
exigem.
