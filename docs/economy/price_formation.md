# Formação de preço dos bens (D-091)

Decisão do dono (2026-11-18): **preço livre, sem teto fixo**. Este documento
descreve como o preço de um bem é formado hoje no núcleo (`sim/prices.cpp`),
o que entrou na D-091 e **o que ainda falta** da diretriz de formação de preço
do plano (custo como piso + markup da firma ajustado a estoque/ocupação +
demanda efetiva).

## 1. O que muda (D-091)

| Antes | Depois |
| --- | --- |
| `market.price` preso à banda fixa `[0,5×, 3×]` do preço-base em `phase_prices`, `phase_clamp` e nos invariantes | Sem banda econômica. Só há **guarda numérica** (finito e > 0) e o amortecimento diário de ±5% do base |
| Pressão de escassez sobre `desired − latent` (demanda desejada financiada) | Pressão sobre **`Market::effective`**: o que os compradores realmente tentam comprar, acumulado na fonte |
| Mina estatal de mineral de nicho (ouro, diamante, terras raras, tungstênio, urânio) limitada a `kMaxStateMineCapacity` = 10 t/d (D-083/D-086) | **Sem teto de nicho**: todos os minerais seguem a mesma régua de demanda externa × 1,2, limitada pelo orçamento, com os estágios/custo da D-086 |

## 2. A regra do dia

Para cidade × produto **com produtor local** (`sim/prices.cpp`,
`phase_prices`):

```
scarcity = effective / max(stock + supply, 0.001)
target   = max(base × scarcity, cost_floor × 1.05)
next     = price + (target − price) × kPriceSmoothing        // 0,20
price    = clamp(next, open − step, open + step)             // step = 0,05 × base
price    = price_guard(price)                                // finito e > 0
```

- `base` = preço-base do bem (`goods.json`), a âncora de exibição.
- `cost_floor` = piso **econômico**: custo dos insumos da receita (ou do
  insumo único, `input_per_output × preço`) + mão de obra/opex por unidade de
  capacidade (`workers × wage + opex` ÷ capacidade da cidade). O alvo nunca
  fica abaixo de `cost_floor × 1.05` — os 5% são a **margem declarada** do piso.
- `phase_clamp` roda **uma vez** no fim do dia, para que importação e reação
  de mercado não somem dois passos de 5%.

Para cidade **sem produtor local** o preço é disciplinado pelo CIF da
importação (`city.import_price`): a paridade é um **teto** (o consumidor pode
importar), mas não puxa o preço para cima. A D-091 removeu a banda
`[0,5×, 3×]` que envolvia esse CIF — o limite passa a ser o preço de
importação real, que é dado.

O choque de preço (`price_shock`) continua sendo amortecido pelo passo diário:
um choque ×10 entra no máximo +5% do base por dia.

## 3. Demanda efetiva (`Market::effective`)

`effective` é um fluxo **diário e interno** (não vai ao save nem ao status; é
zerado em `phase_reset`). Ele acumula, em cada fonte, a tentativa de compra
que tem caixa/insumo/uso por trás:

| Fonte | O que entra em `effective` | Por quê |
| --- | --- | --- |
| `phase_income` (comida urbana) | `desired_tons` já limitado pelo orçamento | compra financiada |
| `consume_category` (life/luxury) | só `backed_tons` (parcela coberta pelo orçamento) | o desejo acima da renda fica em `latent` |
| `consume_government` | `desired_tons` da compra institucional | orçamento público por trás |
| `phase_production` (receita e insumo único) | `needed` **só quando `sold_prev > 0`** | a usina ociosa/faminta pede plena capacidade todo dia (bootstrap); sem essa condição o insumo saturava o preço (ferro ≈ 20,8 M t/d × ~17,8 mil de oferta) |
| `phase_raw` (demanda externa) | `ceiling` | o resto do mundo paga o preço de mercado |
| `phase_region` (comida do interior) | `desired_tons` limitado pelo caixa da região | compra financiada |
| `phase_construction` | `bought` (material efetivamente comprado) | obra com fundo reservado |
| `fleet_buy_core` | `amount` do pedido da frota | limitado por `budget_share` |
| `phase_services` | `needed` do insumo de serviço | demanda pelo uso real (`units`) |
| `trade_food` / `trade_goods` | `amount` exportado | venda realizada ao exterior |

`desired` e `latent` continuam publicados/expostos como **sinal da IA**
(inclusive o bootstrap de plena capacidade), mas não pressionam o preço.
`register_category_demand` alimenta **só** `desired`/`latent` (desejo sem renda).

## 4. O que falta da diretriz (não implementado)

A diretriz do plano pede três camadas: **custo como piso + markup da firma
ajustado a estoque/ocupação + demanda efetiva**.

- **Custo como piso**: existe (`cost_floor × 1.05`).
- **Demanda efetiva**: existe (`Market::effective`, seção 3).
- **Markup da firma ajustado a estoque/ocupação**: **não existe** para bens.
  O preço de bem é `base × escassez` com piso de custo; não há um markup
  próprio da firma que suba com estoque baixo/ocupação alta e caia com
  estoque alto. O efeito de estoque já entra indiretamente pelo denominador
  de `scarcity` (`stock + supply`), mas não como decisão de margem da firma.
  Os serviços (F8) têm banda e margem próprias (`sim/services.cpp`), fora
  deste escopo.

Implementar o markup é uma decisão separada: exige declarar a margem-alvo por
setor/ocupação (dado de modelo) e não deve ser inventado agora.

## 5. Proteção numérica e invariantes

- `price_guard(candidate, previous)`: devolve `candidate` se for finito e > 0;
  senão mantém o preço anterior (ou 1,0 se o anterior também for inválido).
  Vale para qualquer nível de preço — não é banda econômica.
- `check_invariants` deixou de exigir a banda: o único invariante de preço é
  **finito e > 0**. Preço inválido continua revertendo o tick (snapshot do
  prólogo), como antes.
- O save não muda de esquema: `effective` é fluxo do dia (não persiste).

## 6. Verificação

`tools/price_formation_test.gd` (headless, exit ≠ 0 em falha):

- (a) escassez real persistente leva o trigo de PORTUS **acima de 3×** o base;
- (b) excesso de oferta derruba o preço semeado em 3×;
- (c) `desired` de plena capacidade (usina faminta) **não** infla o preço do
  insumo quando não há venda real (caso do ferro);
- (d) mina estatal de ouro passa de 10 t/d quando a demanda externa justifica;
- (e) choque ×10 + estoque vazio não produz NaN/inf/≤0 ao longo dos ticks.

Medições e a decisão completa ficam em `GAME_STATE.md` (D-091).

## 7. Medido (2026-11-18, debug, 849 cidades, IA + urbano)

**Bateria (10/10 verde, um teste por vez):** `b5_state_levers`, `b4_migration`,
`b3_region`, `mine_stage`, `services` (via cena), `economy_integration`,
`economy_ai`, `economy_save_load_roundtrip` (0 diffs), `economy_save_load_v11_compat`
e `price_formation`.

**Goldens regerados (2 rodadas, 0 diffs):** A0 `save 04172bc5…` / `status c134db23…`
(antes `bdd355b3…`/`ae6c500b…`), 60 d, money 6.523.551.982,90, 0 falências,
42.130 negócios; S0 `real_60_ai 602a40bc…/ffae5078…`, `real_60_noai
3d0f23f8…/4c569b6b…`; `demo_365_noai` **inalterado** (`98bbfde8…/14d2c387…` — o
demo não pressiona a banda nem tem mineral de nicho).

**CLI × Godot (10 d, IA, threads=1, snapshot do dia 0):** snapshots
byte-idênticos, SHA-256 `C8F57C23…` (74.052.127 B).

**365 d com IA (`economy_world_test`, baseline M2):**

| métrica | resultado | limite |
| --- | --- | --- |
| falências | **4** | 28 |
| drift de dinheiro | **-0,0020** | 0,01 |
| violações de invariante | 0 | 0 |
| portão de comida (avg30 por nação) | **REPROVADO: 15 nações** | 0 |

Preços mundiais (razão ao `base_price`, ponderada por venda): trigo
**4,21×** (pico 4,19–4,21), arroz 2,43×, milho 2,13×, batata 3,50×; ferro,
carvão, bauxita, petróleo e gás sobem **linearmente +0,05× do base/dia** até
**19,25×** no dia 365 (sem equilíbrio: o passo diário é o único freio); cobre
14,8×; nicho não é monotônico (ouro 11,5× → 7,4×, diamante 11,3× → 5,3×,
terras raras 12,2× → 11,1×, tungstênio 10,3× → 8,6×): o preço alto destrói a
demanda do nicho.

**Causa do portão reprovado (A/B):** com o código-base (sem D-091) o mesmo
teste dá **0 falhas e drift -0,0022** — a regressão é da mudança. Restaurando
só a banda fixa (mantendo `effective` e sem teto de nicho) ficam **7** nações;
com a D-091 completa ficam **15** (pior GMB 0,668 → 0,152; CHE 0,417 → 0,306;
NPL 0,509 → 0,407; LBY 0,163 → 0,000). Mecanismo: energia/insumos a 19×
comprimem a renda real e a comida a 2–4× do base encarece a cesta — é a
camada de markup/custo da firma que falta (seção 4) que deveria repassar e
frear o preço do insumo.

