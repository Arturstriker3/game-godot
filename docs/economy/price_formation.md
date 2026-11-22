# Formação de preço dos bens (D-091 + D-092)

Decisão do dono (2026-11-18): **preço livre, sem teto fixo** — e, na D-092,
**equilíbrio por custo + markup** (a diretriz completa: custo como piso +
markup da firma ajustado a falta/ocupação + demanda efetiva). Este documento
descreve como o preço de um bem é formado hoje no núcleo (`sim/prices.cpp`),
o que entrou em cada decisão e o que foi medido.

## 1. O que muda (D-091)

| Antes | Depois |
| --- | --- |
| `market.price` preso à banda fixa `[0,5×, 3×]` do preço-base em `phase_prices`, `phase_clamp` e nos invariantes | Sem banda econômica. Só há **guarda numérica** (finito e > 0) e o amortecimento diário de ±5% do base |
| Pressão de escassez sobre `desired − latent` (demanda desejada financiada) | Pressão sobre **`Market::effective`**: o que os compradores realmente tentam comprar, acumulado na fonte |
| Mina estatal de mineral de nicho (ouro, diamante, terras raras, tungstênio, urânio) limitada a `kMaxStateMineCapacity` = 10 t/d (D-083/D-086) | **Sem teto de nicho**: todos os minerais seguem a mesma régua de demanda externa × 1,2, limitada pelo orçamento, com os estágios/custo da D-086 |

## 2. A regra do dia (D-092: custo + markup)

Para cidade × produto **com produtor local** (`sim/prices.cpp`,
`phase_prices`):

```
avail    = max(stock + supply, 0.001)
ratio    = effective / avail                                  // falta relativa
markup   = clamp(kPriceMarkupBase + kPriceMarkupGain × ln(max(ratio, 1))
                 + kPriceMarkupOccupancyGain × ocupação
                   × min(1, max(ratio − 1, 0)),               // prêmio só com falta
                 kPriceMarkupBase, kPriceMarkupMax)            // 0,08 .. 2,2
cost_floor = max(insumos + trabalho/opex por capacidade, base) // base é PISO
target   = cost_floor × (1 + markup)                           // sem teto
next     = price + (target − price) × kPriceSmoothing          // 0,20
step     = max(0,05 × base, kPriceMaxDailyMove × open)         // 6% do preço de abertura
price    = clamp(next, open − step, open + step)
price    = price_guard(price)                                  // finito e > 0
```

- `cost_floor` = custo **unitário** da firma (insumos da receita — ou do
  insumo único, `input_per_output × preço` — + mão de obra/opex por unidade
  de capacidade) **ancorado no preço-base**: `max(custo, base)`. Antes o piso
  era `custo × 1,05`; agora a margem é o markup abaixo e a âncora do base
  entra dentro do piso.
- `markup` é a margem da firma: sobe com a **falta relativa** (`ratio`, em
  log) e, **só quando há falta** (`ratio > 1`), com a **ocupação** — o prêmio
  de ocupação é modulado por `min(1, ratio − 1)`, então em fartura a margem é
  a normal e não 1,33×. Satura em `kPriceMarkupMax` = 2,2 (custo × 3,2). Com
  falta nula a margem é 8% (`kPriceMarkupBase`, custo × 1,08).
- `base` (preço real inicial do `goods.json`) vira **âncora leve de piso**
  dentro do `cost_floor`: nenhum bem é vendido abaixo do preço-base nem abaixo
  do custo + margem, e o preço sobe acima dele quanto o desequilíbrio mandar —
  não há teto.
- **Passo diário:** `max(5% do base, 6% do preço de abertura)`. O primeiro
  termo é o amortecimento antigo; o segundo é o que deixa um preço em choque
  (×10) **convergir em semanas** em vez de descer 5% do base por dia.
- `phase_clamp` roda **uma vez** no fim do dia, para que importação e reação
  de mercado não somem dois passos.

Para cidade **sem produtor local** o preço é disciplinado pelo CIF da
importação (`city.import_price`): a paridade é um **teto** (o consumidor pode
importar), mas não puxa o preço para cima. A D-091 removeu a banda
`[0,5×, 3×]` que envolvia esse CIF — o limite passa a ser o preço de
importação real, que é dado.

O choque de preço (`price_shock`) continua sendo amortecido pelo passo diário:
um choque ×10 entra no máximo 6% do preço de abertura por dia.

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

## 4. As três camadas da diretriz (fechadas na D-092)

A diretriz do plano pede três camadas: **custo como piso + markup da firma
ajustado a estoque/ocupação + demanda efetiva**. A D-092 fechou a terceira:

- **Custo como piso**: existe (`cost_floor`, custo unitário da firma).
- **Demanda efetiva**: existe (`Market::effective`, seção 3).
- **Markup da firma ajustado a falta/ocupação**: existe
  (`kPriceMarkup*` em `sim_constants.h`): margem normal de 8%, subindo com
  `ln(effective ÷ oferta)` e com a ocupação, saturando em 2,2. A margem não é
  um número inventado por setor: é uma função declarada do desequilíbrio e da
  ocupação, com dois parâmetros de forma e um teto.
- Os serviços (F8) têm banda e margem próprias (`sim/services.cpp`), fora
  deste escopo.

O que a D-092 **não** faz: não há margem-alvo por setor (aço ≠ pão), nem
markup por região; é uma régua única para bens. Ligar isso é calibração
(Parte E) e não muda a mecânica.

## 4a. Demanda elástica e corte de produção (D-092)

Duas reações do lado da quantidade entram junto com o markup, para o preço
não ser o único freio:

- **Elasticidade-preço por classe de necessidade** (`consumption.cpp`):
  a quantidade comprada cai com `(preço ÷ base)^−elasticidade`, com piso de
  25% da cesta para não zerar quem tem renda. Básico 0,10 (quase rígido),
  vida 0,55, luxo 1,20.
- **Corte de produção por prejuízo profundo** (`production.cpp`): com margem
  negativa **maior que 25% do custo** a produção industrial cai, com piso de
  25% da capacidade (`kProductionMarginCutStart`/`kProductionMarginFloor`). O
  corte é deliberadamente conservador: combustível e material de construção
  são insumo da frota e das obras, e cortar com qualquer margem negativa
  quebrava a logística de comida no teste de 365 d. O corte por prejuízo
  **persistente** continua no `output_scale` (`kLossDaysBeforeCut`), como
  antes.

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

- (a) escassez real persistente leva o trigo de PORTUS **acima de 3×** o base e
  **plateia** (medido: 3,200×, cruzou 3× no dia 22 e ficou no platô — sem
  +5% do base/dia indefinido);
- (b) excesso de oferta derruba o preço semeado em 3× (medido: 3,0× → 1,080×);
- (c) `desired` de plena capacidade (usina faminta) **não** infla o preço do
  insumo quando não há venda real (ferro: `desired` 150.352 t com oferta 87 t
  → 1,08×);
- (d) mina estatal de ouro passa de 10 t/d quando a demanda externa justifica;
- (e) choque ×10 + estoque vazio não produz NaN/inf/≤0 ao longo dos ticks;
- (f) convergência após choque: preço semeado em 10× volta ao piso com oferta
  normal (medido: 1,081× no dia 60, 1,080× no dia 120);
- (g) elasticidade por classe: a 2× o base o luxo cai mais que o básico
  (medido: básico 0,933 vs luxo 0,435 da quantidade desejada);
- (h) piso de custo: o preço não fica abaixo de insumo + trabalho (ferro 600 →
  trigo 648, razão 1,080).

Medições e a decisão completa ficam em `GAME_STATE.md` (D-091 e D-092).

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

## 8. Medido (D-092, 2026-11-18, debug, 849 cidades, IA + urbano)

**Bateria (13/13 verde, um teste por vez):** `b5_state_levers`, `b4_migration`,
`b3_region`, `mine_stage`, `services` (via cena), `economy_integration`,
`economy_ai`, `population_demo`, `population_age`, `strata` (365 d, violações 0,
drift 0), `economy_save_load_roundtrip` (0 diffs), `economy_save_load_v11_compat`
e `price_formation` (8 casos).

**Goldens regerados (2 rodadas cada, 0 diffs):**

| golden | antes | depois | leitura |
| --- | --- | --- | --- |
| A0 (60 d, IA, urbano) | `04172bc5…`/`c134db23…` | `1f4c603e…`/`792bdd31…` | money 6.523.551.982,90, 0 falência, 41.595 negócios (antes 42.130) |
| S0 `real_60_ai` | `602a40bc…`/`ffae5078…` | `71a7b58b…`/`b0d23e12…` | 41.554 negócios |
| S0 `real_60_noai` | `3d0f23f8…`/`4c569b6b…` | `09c1edbb…`/`3509efbf…` | 42.965 negócios |
| S0 `demo_365_noai` | `98bbfde8…`/`14d2c387…` | `6f4a2fd8…`/`80e1d45f…` | 630.000 XGS, 110 negócios, 0 falência |

Justificativa: preço, custo e quantidade mudam por desenho (markup, passo
diário, elasticidade, corte). O agregado não se move (money idêntico, 0
falência nas 60 d A0) e os três S0 deram 0 diferenças contra os goldens novos.

**CLI × Godot (10 d, IA, threads=1, snapshot do dia 0):** snapshots
**byte-idênticos**, SHA-256 `91975E45…` (74.113.540 B; snapshot de 64.325.114 B,
849 cidades, 3.222 construções).

**Preços no ano (365 d, IA, `price_trajectory_365`, razão ao `base_price`
ponderada por venda):**

| bem | dia 30 | dia 120 | dia 365 | D-091 (dia 365) |
| --- | --- | --- | --- | --- |
| ferro | 3,26× | 4,15× | **4,23×** | 19,25× (linear +0,05×/dia) |
| carvão | 3,17× | 3,20× | **3,20×** | 19,25× |
| petróleo | 3,17× | 3,20× | **3,20×** | 19,25× |
| gás | 3,17× | 3,23× | **3,27×** | 19,25× |
| cobre | 2,87× | 2,88× | **2,59×** | 14,8× |
| bauxita | 3,31× | 5,17× | **4,95×** | 19,25× |
| trigo | 1,14× | 1,16× | **1,14×** | 4,21× |
| arroz | 1,11× | 1,11× | **1,10×** | 2,43× |
| ouro | 2,36× | 2,24× | **1,64×** | 11,5× → 7,4× |

Todos **plateiam** (não sobem linearmente). Carvão/petróleo/gás param em 3,20×
= custo × (1 + markup máximo 2,2) — o teto é do **markup**, não do preço.
Falências no probe: **18** em 365 d (o mundo com região, IA e urbano do teste
do portão: **4**). Drift do dinheiro no teste do portão: **+0,0024** absoluto
(6.523.551.982,90 → …,90238), 0 violação de invariante.

**Portão de comida — 31 nações pioram > 2,5 p.p. vs `b4_world_sweep.json`**
(antes da D-091 o mesmo teste dava 0; com a D-091, 15; agora 31 — **30 com a
isenção canônica do sweep** `--food-exempt=SOL,DJI,SAH,JOR,YEM`, que roda sem
a flag nesta medição). A sonda
`_tmp_gate_probe` reproduz o mesmo mundo (food_avg30 idêntico, 0,000 de
diferença) e mede, por nação, a importação de comida, a exportação de
energia/minerais e o **preço local** médio (razão ao base, ponderada por venda).

| nação | comida avg30 (base → D-092) | Δ | necessidade t/d | preço local comida | preço local energia/mineral | exporta energia/mineral t/d | leitura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ARE | 0,653 → 0,284 | −0,369 | 2.607 | 1,24× | 3,20× | 119,4 | mercado fino de 2 cidades; cai para quase zero mesmo exportando gás |
| BFA | 0,748 → 0,622 | −0,126 | 1.436 | 1,67× | 3,20× | 1,1 | importador de energia/mineral; sem receita de exportação |
| SOM | 0,763 → 0,657 | −0,106 | 855 | 1,79× | 2,92× | 37,0 | importador de energia; comida local a 1,79× |
| MWI | 0,923 → 0,828 | −0,094 | 662 | 2,35× | 1,08× | 4,3 | comida local caríssima (2,35×), sem mineral |
| NER | 0,675 → 0,594 | −0,080 | 796 | 1,52× | 3,08× | 1,1 | importa 42% da comida e paga energia a 3,08× |
| GMB | 0,115 → 0,047 | −0,068 | 371 | 2,90× | — | 0,0 | já no colapso; comida local a 2,90× sem mina |
| COD | 0,989 → 0,938 | −0,051 | 8.592 | 1,40× | 2,75× | 540,7 | exporta cobre, mas a receita não chega à cesta |
| CAF | 0,570 → 0,523 | −0,047 | 584 | 1,49× | — | 0,0 | sem energia própria; comida a 1,49× |
| NIC | 0,583 → 0,543 | −0,040 | 476 | 2,04× | — | 0,0 | comida a 2,04×, sem energia/mineral |
| DEU | 0,920 → 0,881 | −0,039 | 6.547 | 1,88× | 2,88× | 35,0 | economia grande; importa 11% da comida |
| SLE | 0,548 → 0,511 | −0,036 | 526 | 1,70× | 3,20× | 0,9 | importador de energia |
| TZA | 0,751 → 0,719 | −0,033 | 3.514 | 1,79× | 2,27× | 53,2 | importador de energia |
| ZMB | 0,695 → 0,664 | −0,031 | 1.354 | 1,84× | 1,97× | 125,2 | exporta cobre; receita não compensa |
| HTI | 0,401 → 0,372 | −0,029 | 1.051 | 1,27× | 1,95× | 10,7 | importador; comida a 1,27× |
| POL | 0,867 → 0,842 | −0,025 | 1.609 | 2,25× | 2,96× | 92,1 | importador de energia; comida a 2,25× |

**Mercados finos (necessidade < 300 t/d, 16 nações):** DJI (0,406 → 0,066),
GNB (0,751 → 0,655), MLT (1,000 → 0,904), ERI (0,697 → 0,610), LUX (1,000 →
0,928), BRB (0,989 → 0,925), GUY (1,000 → 0,942), SVK (1,000 → 0,954), TLS
(0,767 → 0,722), SLB (0,758 → 0,715), SVN (1,000 → 0,960), TTO (1,000 →
0,961), BIH (1,000 → 0,964), HRV (1,000 → 0,965), SWZ (0,825 → 0,793), LTU
(1,000 → 0,974). Nesses a razão consumido ÷ necessidade é hipersensível: um
dia de falta derruba a média 30 d. O preço local da comida é o mais alto do
conjunto (mediana **2,06×** contra 1,79× das nações robustas) — são
importadores que pagam a paridade de importação + custo/piso.

**Depois do merge com a B7 (2026-11-18, `9348c627`):** o mesmo sweep (`merge_real_365.json`) dá
**34 nações (33 com a isenção canônica)** — entram LSO, ARM, KGZ, MMR, KEN, AFG, HND, EST e ECU e voltam
MWI, NER, CAF, ERI e BIH; os valores por nação desta tabela se repetem (MLT 0,904, LUX 0,928, DEU 0,881,
POL 0,842, SOM 0,657). Falências 4, drift 0,001343, 0 violações. A dívida #10 do M2 (Parte E) segue
aberta.

**Causa (termos de troca energia/mineral × comida):** a D-092 trocou o
runaway de +0,05× do base/dia por um platô de **2,6–5,0×** no mineral e
**1,10–1,14×** no alimento (mundo). O efeito no portão é de **dois lados**:

1. **Importadores de energia/mineral** pagam insumo a 1,9–3,2× o base local
   sem receita de exportação (BFA, NER, SLE, TZA, HTI, CAF, NIC, GMB) e a
   comida local fica em 1,3–2,9×: a renda real cai e a cesta não fecha.
2. **Exportadores de mineral** (COD 540 t/d, ZMB 125, ARE 119) perderam a
   renda que o preço a 19× lhes dava na D-091 — a receita de exportação caiu
   ~6× — e ela não é repassada à cesta das famílias (falta o item de modelo
   de tesouro/renda externa, dívida #2 do M2).

As duas leituras são **calibração**, não lógica: nenhum invariante violou,
dinheiro conservado e as falências ficaram dentro do teto. Ficam registradas
em `M2_marco.md` §8 e no plano (Parte E) como **aceite de devolver as nações
importadoras de energia ao portão de comida** — corrigir na Parte E (energia)
e na calibração da oferta de minerais.

