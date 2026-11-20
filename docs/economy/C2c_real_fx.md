# C2c — regimes cambiais reais, âncora, cesta e moeda de união

Decisão **D-108**. Antecessoras: C2b2 (D-106, regimes/padrão/ouro), C3 (D-107,
banco agregado e crédito). Dado de origem: `data/definitions/economy/fx_regimes_2026.json`
(IMF *Annual Report 2026*, Apêndice II.9, **de facto** em **30/04/2026**),
trazido da branch `data/fx-regimes-real` (`5258d89d`) e validado por
`tools/validate_fx_regimes.py`.

Código: `gdextension/src/sim/exchange.{h,cpp}`, `sim/save_state.cpp`,
`sim/sim_types.h`, `sim/world_state.h`, `sim/sim_constants.h`,
`gdextension/src/economy_sim.{h,cpp}`, `core/economy/economy.gd`,
`tools/c1_world_ai_365.gd`.

## 1. O que muda

1. **Regime inicial real por moeda.** `set_fx_regimes` carrega o arquivo antes
   de `set_currencies`; o mundo nasce com `floating` / `band` / `peg` do
   AREAER — não mais o "peg administrativo" único da C2a. Moeda sem entrada no
   arquivo mantém o que o dado do F5 declarou.
2. **Peg contra qualquer âncora.** A âncora pode ser EUR, ZAR, INR, SGD, USD…
   A paridade é **cruzada**: `taxa = valor_da_âncora ÷ paridade`, recalculada
   todo dia a partir da taxa da moeda-âncora. Antes (C2b2) o peg ficava preso
   ao XGS e uma âncora flutuante arrastava todas as moedas presas nela
   (lacuna §7.2 de `fx_regimes_data.md`).
3. **Peg contra cesta.** Âncora = lista `(moeda, peso)`; alvo = **média
   geométrica ponderada** das taxas cruzadas. Defesa com reservas é a mesma do
   peg de moeda única.
4. **Emissor e moeda de união.** Só o emissor muda regime/padrão; comando de
   outra nação é recusado com motivo. Moeda compartilhada por várias nações
   (EUR, USD, XOF, XAF, XCD, ILS, MAD) fica **travada**: "moeda de união —
   regras de bloco futuras". Nação dolarizada/sem moeda própria não tem
   política (as `exceptions` do arquivo).
5. **Bulgária no euro (D-108e).** `BGN` saiu do cenário: `BGR` passou a `EUR`
   (Decisão (UE) 2025/1407; 1 EUR = 1,95583 BGN). `migrate_currency(from,
   to, units)` funde nação, paridade, reservas, ouro do BC, emissão e bancos na
   moeda destino **dividindo a paridade local** por 1,95583. O dinheiro do
   núcleo é em XGS (âncora): a conservação fecha por construção.
6. **As 4 moedas sem classificação AREAER** (`CUP`, `KPW`, `TWD`, `SLS`) não
   ficam sem regime: o bloco `game_fallback` do próprio arquivo declara
   `band` como **default de jogo** (TWD: flutuação administrada, sem
   classificação AREAER porque Taiwan não é membro do FMI; as outras três:
   `default de jogo, sem fonte`). Nenhuma delas recebeu número ou fonte
   inventada.
7. **Status A4** (por moeda, dentro de `currencies[moeda].fx`):
   `regime_source`, `anchor`, `anchor_type`, `anchor_source`, `anchor_basket`
   (pesos), `basket_source`, `issuer`, `locked`, `anchor_value`,
   `target_rate`; e `monetary_policy.regime_source` / `regime_currencies`.
8. **Save v31** com bloco aditivo `fx_regimes` (`source`, `no_policy`,
   `currencies`) e `basket`/`basket_source` dentro de `fx.currencies[moeda]`.
   Migração **v30 → v31**: sem o bloco, o save antigo recebe os regimes do
   arquivo (carregado antes do load) e passa pela sucessão de moeda.

## 2. Regra da cesta quando a composição não é pública (lacuna declarada)

O Apêndice II.9 põe 7 moedas na coluna "Composite" (FJD, LYD, BWP, SGD, MAD,
KWD, SYP) **sem publicar a composição nem os pesos**. O núcleo **não inventa**
a cesta:

- composição publicada (campo `basket` no arquivo) → usada como está;
- sem composição → `resolve_baskets` pesa as **3 moedas com maior comércio
  medido no próprio mundo** (exportação + importação das cidades), normalizado,
  e grava `basket_source = "default de jogo: participacao no comercio medido
  (a composicao da cesta nao e publicada pelo FMI)"`. Antes de existir comércio
  medido a cesta fica vazia e a moeda não tem alvo (`anchor_value = 0`): o
  regime fica administrativo, sem número inventado.

**O núcleo não tem matriz bilateral de comércio** (a troca internacional é um
fluxo por cidade contra o exterior, não por parceiro). Por isso "participação
no comércio daquela nação" não é resolvível como o dono pediu; a medida
disponível é a participação no comércio **mundial**. Fica registrado como
lacuna para a fase em que houver parceiros comerciais (C6/long run).

## 3. Mapa das decisões (D-108)

| item | decisão | onde está |
|---|---|---|
| a | regime real do AREAER por moeda | `set_fx_regimes`, `fx_regimes_2026.json` |
| b | âncora em qualquer moeda e em cesta (geométrica ponderada) | `exchange::anchor_value` |
| b | pesos oficiais quando públicos; senão comércio medido, marcado default de jogo | `exchange::resolve_baskets` |
| c | só o emissor muda; união travada; dolarizado sem política | `set_exchange_regime` + `apply_one_locked` |
| d | as 4 `unknown` viram `band` default de jogo | `game_fallback` do JSON + loader |
| e | BGN → EUR a 1,95583 conservando valor | `exchange::migrate_currency`, `currencies.json` |
| f | banda não publicada = ±5% | `exchange_policy.json` (já existia) |

## 4. Medição — 365 d com IA, threads=1, DLL debug, mundo real

`tools/c1_world_ai_365.gd`, 365 d, 190,0 s de parede, sem comando de câmbio.
Saída em JSON (campos `fx`, `monetary`, `food_gate`, `balance`). A tabela abaixo
foi **reproduzida número por número** em 20/11/2026 pelo A/B da §4.1
(`tools/c2c_ab_365_test.gd`, variante C): mesmas falências, portão, inflação,
juros e hash `685d7b83…`.

| métrica | base (D-105/106/107) | **C2c (este commit)** |
|---|---:|---:|
| falências | 2 | **3** |
| portão de comida | 86/179 | **90/179** |
| mediana do portão | 0,9778281 | **0,9746829** |
| violações de invariante | 0 | **0** |
| dias travados | 0 | **0** |
| desvalorizações forçadas | 0 | **0** |
| dinheiro no fim | 6 523 551 982,90 | **6 523 551 982,90112** |
| drift máximo diário | 4,18e-13 | **1,71e-13** |
| hash de trajetória | `f759f4c0…` | **`685d7b83…`** |

Regime inicial efetivo (135 moedas): **floating 44, band 64, peg 27** e
**7 moedas travadas** (EUR, USD, XOF, XAF, XCD, ILS, MAD). O arquivo de dados
é 44/60/28 sobre 136; aqui `BGN` saiu (era peg) e as 4 `unknown` entraram como
band (60 → 64).

Âncoras no mundo: USD 58, EUR 10, ZAR 3, INR 2, SGD 1 (inclui os pares
ZAR/INR/SGD que a C2b2 não conseguia representar).

Inflação 12 m por nação (179): **p10 −8,553 %, p50 −0,106 %, p90 +1,997 %**.
Taxa básica por moeda (135): **p10 0 %, p50 0 % (1,97e-29), p90 3,682 %**.

**Aceite:** falências 3 (≤ 3), portão 90/179 (≤ 90), violações 0 → **aprovado**,
com **margem zero** nos dois primeiros. O mundo mudou como esperado (regimes
reais + Bulgária no euro); nenhum parâmetro foi calibrado às cegas.

## 4.1 A/B por canal (365 d, IA threads=1)

`tools/c2c_ab_365_test.gd` roda **três mundos em sequência no mesmo processo**
(1 Godot por vez; 649,7 s no total, 196-197 s por mundo), mesmas definições e
mesmas flags da §4 (sem crédito, sem patrimônio, `fx-seed-days=0`):

- **A `regimes_only`** — `set_fx_regimes` ligado, **sem** a Bulgária no euro:
  `currencies.json` como em `53ce78dc^` (BGR com `BGN` próprio, taxa 0,493812).
- **B `bulgaria_only`** — **sem** `set_fx_regimes` (o mundo nasce com o "peg
  administrativo" da base C3) e **com** a Bulgária no euro (dado da C2c).
- **C `default`** — os dois canais ligados (default do jogo, D-108).

| variante | falências | portão | mediana | infl p10 | p50 | p90 | nações < −3 % | juros p50 | violações | travados | desvalorizações | hash |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| base C3 (D-107) | 2 | 86/179 | 0,9778281 | 0,71 | 0,91 | 4,28 | — | 0,464 | 0 | 0 | 0 | `f759f4c0…` |
| **A** regimes (sem BGR no EUR) | **2** | **89/179** | 0,9750169 | **−8,628** | +0,038 | +2,224 | **45** | 0,000 | 0 | 0 | 0 | `716ff7fd…` |
| **B** só Bulgária (regimes C3) | **2** | **86/179** | 0,9778281 | +0,714 | +0,908 | +4,278 | **0** | 0,464 | 0 | 0 | 0 | `a758c74f…` |
| **C** default (A+B) | **3** | **90/179** | 0,9746829 | −8,553 | −0,106 | +1,997 | 47 | 0,000 | 0 | 0 | 0 | `685d7b83…` |

Leitura:

1. **A deflação é 100 % do canal dos regimes.** B sozinho devolve a inflação da
   base C3 (0,714 / 0,908 / 4,278 contra 0,71 / 0,91 / 4,28) e o portão da base
   (86/179); A, sem a Bulgária, já tem p10 −8,63 % e 45 nações < −3 %. O canal
   "Bulgária no euro" não cria deflação: na média ele desloca o CPI 0,70 p.p.,
   porque muda a moeda da Bulgária e, com ela, o balanço externo do EUR (a cauda
   é grande — ver item 4).
2. **A falência a mais e o portão a mais são interação, não soma.** A: 2 / 89;
   B: 2 / 86; C: 3 / 90. Só com os dois canais ligados aparece a 3ª falência
   (§4.3), e o portão chega ao teto.
3. **Conservação intacta nas três:** `money_total` termina em
   6 523 551 982,90 com drift máximo diário ≤ 4,7e-13, 0 violações, 0 dias
   travados, 0 desvalorizações e 0 reservas (`funded=false` em 135/136 moedas).
   Nada de M0 destruído por defesa cambial.
4. **Sensibilidade é alta (aviso para o aceite):** a média |Δ| de inflação
   por nação é 3,60 p.p. entre A e B (os dois canais em sentidos opostos) e
   **0,70 p.p. entre A e C**
   — mas o máximo entre A e C é **9,3 p.p. (JPN 9,69 % → 18,96 %)**. Ou seja:
   trocar uma moeda muda o CPI anual de um país em 9 p.p.; os dois indicadores
   do aceite estão no teto exato **de um mundo que não é estável** a esse nível.

Evidência: `docs/economy/perf/c2c_ab_365.json` (blocos `variants[]` com as três
tabelas, as 179 nações por variante, as falências e o probe de paridade).

## 4.2 A deflação: quem, regime e mecanismo

15 menores inflações de 12 m na variante C (179 nações), com o regime e a âncora
da moeda e a taxa do fim ÷ taxa do dia 0 (o fator de competitividade do F5):

| # | nação | moeda | regime | âncora | infl 12 m | taxa/base |
|---:|---|---|---|---|---:|---:|
| 1 | SGP | SGD | band | — | **−16,45 %** | 1,0116 |
| 2 | BRN | BND | peg | SGD | **−15,94 %** | 1,0115 |
| 3 | MYS | MYR | floating | — | **−15,32 %** | **0,9000** |
| 4 | CMR | XAF | peg | EUR | **−14,54 %** | 0,9632 |
| 5 | GAB | XAF | peg | EUR | −13,28 % | 0,9632 |
| 6 | GNQ | XAF | peg | EUR | −12,90 % | 0,9632 |
| 7 | IDN | IDR | floating | — | −12,73 % | **0,9000** |
| 8 | YEM | YER | floating | — | −12,32 % | **0,9000** |
| 9 | COL | COP | floating | — | −12,20 % | **0,9000** |
| 10 | TTO | TTD | band | USD | −11,81 % | 1,0430 |
| 11 | JAM | JMD | band | USD | −11,77 % | 1,0430 |
| 12 | VEN | VES | band | — | −10,13 % | 0,9500 |
| 13 | SUR | SRD | floating | — | −9,91 % | **0,9000** |
| 14 | CIV | XOF | peg | EUR | −9,43 % | 0,9632 |
| 15 | BEN | XOF | peg | EUR | −8,84 % | 0,9632 |

Contagem das 47 nações < −3 %: **peg 18, band 14, floating 15**; âncora
**nenhuma 19, USD 14, EUR 12, SGD 1** (+CUP, band sem âncora). No outro extremo
as 5 maiores: JPN +18,96 % (0,9000), PSX +17,83 %, SOM +17,43 %, COD +15,96 %,
KNA +13,31 % (peg USD, 1,0984) — ou seja, a cauda alta é doméstica/custo, não
importada.

**Mecanismo, em código.** A taxa não toca o dinheiro nem os preços direto: ela
só entra na competitividade do vendedor no comércio, e o preço interno só é
puxado **para baixo** pelo CIF da importação.

1. `exchange::floating_step` (`gdextension/src/sim/exchange.cpp:38-47`): deriva
   de 0,05 %/dia pela balança (EMA) e é presa a 0,90-1,10 × taxa inicial
   (`sim/sim_constants.h:687-689`).
2. `exchange::tick` (`exchange.cpp:604-694`): flutuante → `rate = shadow`; peg
   → `rate = alvo`; banda → `clamp(shadow, alvo ±5 %)`. Com `funded = false`
   (reservas iniciais 0, §7) o caminho é administrativo (`exchange.cpp:626-631`):
   **nenhuma reserva é gasta, nenhuma M0 é destruída**.
3. `SimCore::competitiveness` (`sim/fx.cpp:80-98`): `taxa ÷ taxa do dia 0`,
   preso a ±10 %, aplicado **só** a moeda com `floating` (float e banda).
4. `origin_anchor = preço_do_vendedor × competitiveness(vendedor)`
   (`sim/trade.cpp:1362-1363`) e `import_price = origin_anchor + tarifa`
   (`trade.cpp:1509`).
5. `phase_prices` (`sim/prices.cpp:75-104`): cidade **sem produtor** do bem tem
   o CIF como **teto** — `capped = min(market.price, target)`; importação barata
   puxa o preço para baixo, importação cara **não** puxa para cima.
6. `phase_prices` (`prices.cpp:167-171`): cidade **com produtor** tem piso de
   custo `max(unit_cost, base)` → preço nunca abaixo do base do catálogo.
7. CPI = preço ÷ base por nação (`sim/central_bank.cpp:163-227`).

A assimetria de (5) contra (6) é a causa: como nenhum mercado com produtor pode
cair abaixo do base, **um CPI < 1 só pode vir dos mercados sem produtor** — e a
troca em cadeia entre cidades importadoras (cada elo desconta até ×0,90 antes de
frete/tarifa) compõe o desconto: é por isso que os casos extremos são as
economias de entreposto/importação pura (SGP −16,4 %, BRN −15,9 %, MYS −15,3 %)
e os pegs ao EUR enfraquecido (XAF/XOF −12,9…−14,5 % com taxa 0,9632).

**Hipóteses do diagnóstico, testadas:**

- *Defesa do peg/banda contrainho M0 (venda de reservas)?* **Não.** 0 reservas
  em 135 moedas, 0 desvalorizações, `money_total` com drift ≤ 4,7e-13 e
  `intervention_today` sem efeito: o caminho tomado é o administrativo
  (`exchange.cpp:626-631`). Não há venda de reserva nem destruição de moeda.
- *Cesta?* **Não.** Os casos profundos não são de moeda em cesta: SGD é banda
  livre, MYR/IDR/COP flutuam, XAF/XOF são pegs ao EUR. As 7 moedas "Composite"
  (FJD, LYD, BWP, SGD, MAD, KWD, SYP) não estão no topo.
- *Alvo de taxa cruzada errado (paridade inicial ≠ taxa vigente)?* **Não.**
  Probe no dia 0 (`parity_day0` no JSON): para as **91** moedas peg+banda de C
  (92 em A), `|target_rate ÷ base_rate − 1|` máximo = **2,2e-16** — a paridade
  cruzada nasce exatamente da taxa do dia 0 (`exchange.cpp:413-432` usa
  `paridade = âncora ÷ base_rate`, e o alvo devolve `base_rate`). No dia 1 o
  alvo anda 0,05 % (`kFxDailyStep`) porque **a âncora andou** — é o peg seguindo
  a âncora, não um ajuste forçado.

**Conclusão econômica:** não há defeito de paridade nem intervenção; a deflação
é o canal de competitividade do F5 (`fx.cpp:80-98`) *ativado pela primeira vez*
— na base C3 todas as 135 moedas eram peg administrativo e o fator valia 1 em
todo o mundo — somado a um teto de importação que só transmite para baixo
(`prices.cpp:92-104`). O sinal (deflação de importação em economia aberta) é
plausível; a **magnitude e a unilateralidade** são artefato da regra de preço.

## 4.3 A falência nova

As **2 falências da base** aparecem iguais nas três variantes: `pri_pon_copper_mine`
e `pri_pon_iron_mine` (PRI-PON, USA, dono `national:G-USA-mining-245`), fechadas
no dia 126 (no snapshot do dia 120, o último antes do fechamento, já somavam 55
dias de prejuízo). A **3ª** só existe em C:

| campo | valor |
|---|---|
| construção | `AI-SOY-26` (soja), dono `private` |
| cidade / nação | `PRT-POR` / PRT |
| capacidade | 38,35 t/dia |
| fechamento | dia **328** (contador de falências) |
| último estado medido (snapshot do dia 300) | lucro **−14,71**/dia, **33 dias** de prejuízo |
| variantes | **sobrevive em A e em B**; fecha só em C |

Causa: caso de fio da navalha da fazenda marginal, tombado pela **interação** dos
dois canais. A e C só diferem na moeda da Bulgária; com BGR no EUR o balanço
externo do EUR muda (competitividade do euro 0,9662 em A → 0,9632 em C) e a
própria taxa local da Bulgária passa de 0,4938 (BGN) para 0,6614 (EUR), mexendo
no fluxo de comércio de soja que abastece o Porto. O prejuízo final (−14,71/dia
numa planta de 38 t/dia) mostra que não é colapso sistêmico: é a margem zero de
uma planta que já vivia no limite.

## 4.4 Menor correção proposta (nenhuma calibração aplicada)

Nenhuma das três hipóteses de defeito se confirmou (§4.2), então **nada foi
alterado no núcleo nesta rodada**. O que fica proposto, em ordem de custo:

1. **P1 — piso do CIF (calibração, `prices.cpp:92-104`).** Dar ao caminho da
   importação o mesmo piso de catálogo que o produtor já tem (`prices.cpp:171`),
   ou um pass-through parcial (`target` puxando também para cima com coeficiente
   < 1). É a correção mais direta da unilateralidade: hoje o câmbio só pode
   deflacionar. **Não é defeito**: é a mesma regra calibrada da D-091/F5; muda o
   mundo inteiro (portão, falências) e exige fase própria + 365 d + A/B.
   **Resolvido como D-109 na §5 e medido na §5.1** (o pass-through completo, não
   o piso do CIF).
2. **P2 — `floating_step` de moeda não-flutuante (defeito latente, não
   exercitado).** `exchange.cpp:38-41` devolve `base_rate` para moeda fixa; com
   a âncora móvel da C2c, um peg **com reserva** compararia a taxa do dia 0 a um
   alvo que anda e defenderia/desvalorizaria sem choque real
   (`exchange.cpp:622-674`). Hoje não dispara (reservas 0, 0 desvalorizações);
   corrigir antes de qualquer fase que funde reservas (é o cenário de estresse
   da C2b2 que ficaria sensível).
3. **P3 — paridade do EUR em save v30 → v31 (defeito de status).**
   `exchange::migrate_currency` grava a paridade da moeda extinta na moeda
   destino quando ela não tem paridade (`exchange.cpp:481-483`): o EUR carregado
   de um save v30 publica `parity = 0,2525` (= 0,493812 ÷ 1,95583) em vez da
   taxa do dia 0 (0,661414). Não muda trajetória (só status/cobertura) e o teste
   da fase fixa esse comportamento (`tools/c2c_real_fx_test.gd:274`) — fica como
   pendência de revisão, não como correção silenciosa.

**Recomendação ao dono:** não fechar a C2c como "equilibrada". Os dois critérios
estão no teto exato (3/3, 90/90), 47 nações deflacionam mais de 3 % ao ano por
uma regra de preço unilateral, e a sensibilidade medida (9,3 p.p. num país ao
trocar uma moeda) mostra que o teto não tem folga real. Se a fase fechar assim,
declarar no §7 que o mundo da C2c é um mundo de **deflação de importação**.

> **Atualização (C2c-P1, D-109).** A P1 foi implementada na §5 e medida na
> §5.1: a deflação de importação deixou de existir (p10 −8,55 % → −0,24 %,
> 47 → 4 nações < −3 %), nenhuma nação fica abaixo de −5 % e não apareceu
> falência nova — mas o **portão de comida sobe a 94/179** (teto 90). As
> opções estão na §5.1.

## 4.5 Como reproduzir

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/run_tests.ps1 `
  -List c2c_ab_365_test -TimeoutSec 1500 -MinFreeRamGB 6
```

Três mundos de 365 d em um único processo (1 Godot por vez); saída
`docs/economy/perf/c2c_ab_365.json`. `OGS_C2C_AB_DAYS`/`--days=` encurtam a
fumaça e `OGS_C2C_AB_OUT`/`--out=` trocam o arquivo; o default não muda.

## 5. Desenho da C2c-P1 — repasse simétrico do câmbio (D-109)

A §4.2 mostrou que a deflação da C2c é o canal da competitividade do vendedor
(`fx.cpp:80-98`) com **uma perna só**: o CIF da importação entrava apenas como
**teto** (`prices.cpp:92-104`) e o piso de catálogo da cidade produtora
(`prices.cpp:171`) nunca deixava o preço cair abaixo do base do dia 0. Nesta
fase a regra de preço passa a transmitir o câmbio nos **dois** sentidos.

**Regra — cidade sem produtor do bem.** O CIF do dia (origem efetiva ×
competitividade do vendedor + tarifa, `trade.cpp:1509`, na âncora) é o **alvo**
do preço, não mais um teto:

```
price += (CIF - price) × kPriceSmoothing            # 20 %/dia (D-092)
price  = clamp(price, open - max_step, open + max_step)
                                  # max_step = max(5 % do base, 6 % do open)
```

Não há banda econômica nem teto de preço (D-091): o preço converge para o custo
de importação, como manda a formação por custo. A suavização e o limite diário
são os mesmos dos produtores, então **não há salto de um dia para o outro** e o
movimento é simétrico (mesma distância ao CIF, mesmo caminho para cima e para
baixo).

**Cotação do dia.** O repasse usa a cotação **renovada pelo comércio do próprio
dia** (`imported[product] > 0`); sem importação registrada hoje o preço
permanece — não se inventa cotação (mesma regra da §4.2). Isso fecha a
assimetria de estado: antes o último CIF conhecido valia para sempre como teto
e, com o alvo simétrico, uma cotação velha travaria o preço.

**Cidade com produtor.** O preço continua formado por custo + markup + demanda
(D-092); a concorrência importada passa a ser o **teto** do alvo:

```
target = min(custo_unitário × (1 + markup), CIF)   se o CIF do dia é menor
piso   = custo unitário (variável), NÃO o base do catálogo
```

O base do dia 0 é âncora leve, não piso rígido: importação mais barata aparece
no preço interno (até o custo variável); importação mais cara **não** empurra o
preço para cima, porque nesse caso quem forma o preço é o produtor local — o
mesmo motivo pelo qual o câmbio não reescreve o preço interno de quem produz
(F5, §4.2 item 3). O repasse de alta chega a essas cidades pelo **custo do
insumo importado**, que sobe junto com o CIF do insumo.

**Coerência e limites.** Não há parâmetro novo: usa-se o CIF medido, a
suavização e o passo diário já calibrados. Não há teto de preço, não há
calibração de moeda e nenhuma fonte nova é inventada. O que se espera do mundo:
as nações < −3 % da §4.2 perdem o componente artificial (repasse só para baixo)
e as nações com fornecedores desvalorizados ganham o componente de alta que hoje
é bloqueado. O critério de aceite continua o da §4 (falências ≤ 3, portão ≤ 90,
0 violações), com a meta de realismo de **nenhuma nação < −5 % sem causa
explicada**.

## 5.1 Medição — A/B/C de 365 d com a P1 (20/11/2026)

Mesmo comando e mesmo mundo da §4.5 (3 variantes num único processo, ~217 s
cada, 650 s de parede, IA threads=1, DLL debug). O arquivo
`docs/economy/perf/c2c_ab_365.json` passou a registrar **esta** medição; a
tabela da C2c sem a P1 (§4.1) fica no commit `858dc9c0`.

| variante | falências | portão | mediana | infl p10 | p50 | p90 | < −3 % | juros p50 | hash |
|---|---:|---|---:|---:|---:|---:|---:|---:|---|
| base C3 (D-107) | 2 | 86/179 | 0,9778281 | 0,71 | 0,91 | 4,28 | 0 | 0,464 | `f759f4c0…` |
| C2c sem P1 (§4.1) | 3 | 90/179 | 0,9746829 | −8,553 | −0,106 | +1,997 | 47 | 0,000 | `685d7b83…` |
| **P1 A** regimes | **2** | **95/179** | 0,9693172 | −0,272 | +1,463 | +4,637 | **4** | 0,725 | `9f0da662…` |
| **P1 B** só Bulgária | **3** | **91/179** | 0,9749206 | +0,714 | +1,201 | +3,262 | **0** | 0,504 | `c8be192d…` |
| **P1 C** default | **3** | **94/179** | 0,9700499 | **−0,242** | **+1,577** | **+4,728** | **4** | 0,666 | `08770914…` |

Leitura:

1. **A deflação de importação foi eliminada.** p10 −8,553 % → **−0,242 %**,
   nações abaixo de −3 % **47 → 4** e **nenhuma abaixo de −5 %**. As 4 são todas
   de importação mais barata, com causa: CMR −4,10 %, GNQ −3,80 % e GAB −3,37 %
   (peg XAF ao EUR, taxa 0,9726 = importação ~2,7 % mais barata) e EGY −3,20 %
   (flutuante no piso 0,90). A moeda mais apreciada da C2c (SGP, taxa 1,0127)
   sai de −16,45 % para −2,71 %.
2. **Nenhuma falência nova.** As 3 de C são exatamente as da C2c: as minas
   `pri_pon_copper_mine` e `pri_pon_iron_mine` (PRI-PON/USA, dia 126) e a soja
   `AI-SOY-26` (PRT-POR, 40 dias de prejuízo no último snapshot). A variante A
   fica com **2** (a soja sobrevive).
3. **O portão de comida piorou: 90 → 94** (teto de aceite = 90) e a mediana do
   portão caiu 0,46 p.p. (0,97468 → 0,97005, com corte fixo em 0,975). O
   mecanismo é o próprio repasse: o preço de varejo das cidades importadoras
   deixa de ficar **abaixo** do CIF de importação — antes a comida importada era
   vendida por menos do que custava importar, e esse desconto artificial
   sustentava o portão. O nível que passa a ser transmitido é o preço do
   vendedor externo (base + markup ≈ +8 %), não o câmbio.
4. **O custo do portão é de NÍVEL, não de câmbio.** A variante B não tem canal
   cambial nenhum (todas as moedas em peg ⇒ competitividade 1) e mesmo assim sai
   de 86 para **91**. Ou seja: ~5 dos 8 pontos de portão vêm de transmitir o
   nível do CIF, e o câmbio (A − B) contribui com o restante — a assimetria que
   causava a deflação acabou sem cobrar do portão.
5. **Conservação e dinheiro intactos:** 6 523 551 982,90 no fim das três
   variantes, drift máximo diário ≤ 4,2e-13, 0 violações de invariante, 0 dias
   travados, 0 desvalorizações.
6. **Juros deixam de ficar presos em zero:** p50 0,000 → **0,666** (A 0,725)
   porque a inflação de 12 m deixa de ser negativa — outro sinal de que o
   artefato deflacionário sumiu.

**Critério de aceite da C2c depois da P1:** falências 3 (≤ 3) **passa**; nenhuma
nação < −5 % sem causa **passa** (mínima −4,10 %, causa de importação);
portão 94/179 (≤ 90) **não passa**. Nada foi calibrado para maquiar o número:
96 % da correção é a regra de preço e o portão mede o custo real de não
subsidiar a comida importada no preço. **Decisão do dono**, em ordem de custo:

- **(a)** aceitar o portão em 94 com esta explicação e fechar a C2c com a regra
  simétrica (o aceite da fase muda de 90 para 94 por decisão, não por número);
- **(b)** atacar o acesso alimentar pelo lado da renda (salário/transferência) —
  fase própria, mexe em política social e não na formação de preço;
- **(c)** manter a regra unilateral da C2c — recusado pelo dono (é a causa da
  deflação).

**P2 e P3 (não corrigidos nesta fase).** P2 continua latente (sem reserva o
`floating_step` de moeda fixa não é exercitado; dispara quando houver reserva com
âncora móvel). P3 **não é pequeno**: corrigir a paridade do EUR no save v30 → v31
exige distinguir no save/load uma paridade **declarada** (comando/arquivo) de uma
paridade **derivada** da taxa — é mudança de contrato de status, com o teste
`tools/c2c_real_fx_test.gd:274` fixando o comportamento atual de propósito.

## 6. Testes

- `tools/c2c_real_fx_test.gd` (novo): peg cruzado (âncora dobra → taxa dobra),
  cesta (média geométrica e pesos no status), recusa de não-emissor, união
  travada, BGN → EUR conservando `money_total` e invariante, save v31
  bit-exato, continuação idêntica após load e migração v30 → v31 (inclui a
  sucessão de moeda).
- Vizinhos verdes pelo runner (20/11/2026, após a P1): `c2b2_fx_gold_test`,
  `c2_central_bank_test`, `c2b1_monetary_test`, `c3_bank_credit_test`,
  `c3_group_balance_test`, `currency_trade_test`, `economy_integration_test`,
  `services_test`, `storage_g2_test` e `agriculture_seasonal_test`. Os dois
  últimos estavam **vermelhos por constantes de save defasadas** (esperavam v30
  e v25; o mundo está em v31 desde a D-108) — corrigido só a constante
  (`b5bc256f`), sem tocar em comportamento; verdes depois disso.
- `tools/validate_fx_regimes.py`: **OK** (135 moedas; BGN em `retired`).
- `tools/c2c_p1_fx_passthrough_test.gd` (novo, 20/11/2026): o repasse simétrico
  no cenário mínimo (vendedor numa nação, comprador sem produtor na outra):
  câmbio que encarece a origem 10 % **sobe** o preço (40 → 43,95 com CIF 44),
  o que barateia **desce** (40 → 36,05 com CIF 36), os dois caminhos são
  **simétricos** (mesmo Δ, diferença 0), nenhum dia salta mais que
  max(5 % do base, 6 % da abertura) (20 → 22 no limite), cotação velha não move
  preço (comércio parado por 5 dias: preço constante) e, na cidade COM produtor,
  importação 10 % mais barata derruba o preço abaixo do base do catálogo
  (40 → 36,00, sem passar do CIF).
- `tools/c2c_ab_365_test.gd` (novo, 20/11/2026): A/B/C de 365 d, três mundos em
  sequência no mesmo processo, com tabela por nação, probe de paridade do dia 0,
  registro das construções que desaparecem e falências por dia (§4.1-4.5 e
  §5.1). Verde: `c2c_ab_365_test` pelo runner, 649,7 s (C2c) e 650,0 s (P1),
  0 violações e 0 travados nos três mundos.

## 7. O que não mudou / limites

- O padrão monetário (fiduciário) e o ouro físico da C2b2 continuam iguais; a
  troca de padrão segue só por comando A3.
- Reservas iniciais continuam 0 (não há fonte por país): o peg sem reserva é
  administrativo e **não** gasta reserva — a lacuna "o fluxo do dia não
  pressiona o peg" (C2b2 §8) continua.
- Com a P1 (§5), o mundo da C2c **não é mais** um mundo de deflação de
  importação: p10 −0,24 % e 4 nações abaixo de −3 % (§5.1). O preço de varejo
  das cidades importadoras passou a incluir o CIF de importação (antes ele
  ficava abaixo dele, o que sustentava o portão em 90/179); por isso o portão
  mede **94/179** e fechar a C2c exige a decisão do dono listada na §5.1.
- O repasse cambial simétrico é o da **regra de preço**, não do dinheiro: a taxa
  continua não criando nem destruindo M0, e o CIF é cotação do dia (sem importação
  no dia, o preço não se move).
- A taxa local exibida da Bulgária passa a ser a do euro (o dado de taxa é
  estimativa PPP `derived:gdp_ppp`, não a paridade oficial): a conversão a
  1,95583 é aplicada à paridade local e a tudo o que é denominado em moeda
  local; os saldos/preços/contratos do núcleo são em XGS e não se movem.
- `docs/economy/fx_regimes_data.md` descreve o arquivo antes desta fase
  (136 moedas, BGN presente). O estado atual do dado está aqui e em
  `retired`/`pending` do próprio JSON.
