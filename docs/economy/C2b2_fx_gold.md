# C2b2 — Regimes de câmbio, padrão monetário e ouro físico

Decisão **D-106**. Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE C / C2
("Regimes e padrão monetário", "Ouro é mercadoria física").
Antecessoras: C2a (D-103, banco central por moeda), C2b1 (D-104, taxa e emissão),
C2b1-fix (D-105, medição).
Código: `gdextension/src/sim/exchange.{h,cpp}` (novo), `sim/fx.cpp`,
`sim/central_bank.{h,cpp}`, `sim/monetary.cpp`, `sim/save_state.cpp`,
`core/economy/storage_service.gd`.

## 1. O que esta fase faz

1. **Regime cambial por moeda** deixa de ser estado inerte: `floating` (como o
   F5 já faz), `peg` (paridade contra a moeda de reserva) e `band` (paridade ±
   largura). Fixo e banda são defendidos pelo banco central **com as reservas
   da C2a** (moeda de reserva e ouro), tudo contabilizado no balanço da C1.
   Reserva esgotada → **desvalorização forçada**, registrada como evento.
2. **Padrão monetário** do mundo age: `fiat` (default, como hoje),
   `bretton_woods` (moedas presas à moeda de reserva; a âncora conversível em
   ouro a preço fixo para os bancos centrais) e `gold` (idem + M0 limitado pela
   cobertura de ouro). Troca **só por comando na fila A3**, nunca automática.
3. **Ouro físico**: o ouro do cofre (G2) vira estoque físico real no armazém
   `vault` do governo. Compra/venda do BC passa pelo **mercado do produto ouro**
   — sem teto econômico de preço, como já vale desde a D-091 — com o débito/
   crédito no caixa do governo e no balanço do BC. A mineração é a que já
   existe; nada de ouro é criado ou semeado duas vezes.
4. **Status A4** com regime efetivo, paridade, banda, reservas, cobertura e
   eventos; **save v29** com migração aditiva v28 → v29.

O default do jogo **não muda**: todas as moedas continuam `floating` (fiduciário,
reserva USD), exatamente como a C2b1 deixa o mundo. Regime, padrão e ouro físico
só mudam o resultado quando um comando ou o dado da G2 os liga.

## 2. Regimes de câmbio

`Currency.rate` é **unidades locais por 1 unidade-âncora** (o preço interno é
exibido como `preço ÷ taxa`); maior taxa = moeda mais fraca.

| regime | regra do dia | defesa |
|---|---|---|
| `floating` | `tick_fx` do F5: pressão da balança comercial sobre a EMA de 30 d, passo 0,05 %/dia, preso à banda ±10 % do `base_rate` (`kFxBandLow`/`kFxBandHigh`) | nenhuma |
| `peg` | a taxa fica na **paridade** (default = `base_rate`) | o BC absorve o fluxo excedente com reservas |
| `band` | a taxa flutua **dentro** de `paridade × (1 ± largura)` | idem, só quando o câmbio sombreado sai da banda |

- **Paridade** (`parity`): unidades locais por 1 unidade da moeda de reserva.
  Default 0 = `base_rate` da moeda (o dado do F5; não é número novo).
- **Largura da banda** (`band_width`): default **±5 %**, declarado em
  `data/definitions/economy/exchange_policy.json` com fonte `default de jogo`
  (a banda de flutuação do F5 é ±10 %; a banda administrada nasce mais estreita
  por decisão de design, não é dado observado).
- **Câmbio sombreado**: o passo que o `floating` faria hoje, a partir da taxa
  corrente. É ele que diz para que lado o mercado empurra a moeda.
- **Defesa**: se o sombreado sai da banda, o BC vende (depreciação) ou compra
  (apreciação) moeda de reserva. O volume é `fluxo bruto EMA ÷ paridade ×
  desvio × kFxDefenceShare`, com `kFxDefenceShare = 0,5` declarado no JSON: o
  BC absorve metade do excesso do dia, não o mercado inteiro. A conversão para
  moeda local usa a paridade — a mesma taxa com que o comércio é medido.
- **Reservas**: por moeda, em unidades de âncora (a mesma unidade do preço-base).
  Elas **não são inventadas**: começam em 0 (ausência declarada — o repositório
  não tem fonte de reservas cambiais por país) e só existem por (a) declaração
  explícita do teste/lab (`set_central_bank_reserves`) ou (b) intervenção que
  compre moeda de reserva com superávit.
- **Contraparte**: o exterior é o `foreign_cash` global (não decomposto, como a
  C2a já declara). Vender reserva **destrói** moeda local que o exterior
  devolve (`foreign_cash` cai); comprar reserva **cria** moeda local creditada
  ao exterior (`foreign_cash` sobe). A identidade financeira da C1 continua
  fechando: o ativo do BC e o passivo monetário se movem pelo mesmo valor.
- **Esgotamento** → desvalorização forçada: a paridade passa a ser o câmbio
  sombreado, a taxa acompanha, as reservas vão a 0 e o evento é gravado em
  `fx_events` (dia, moeda, paridade antiga/nova, reserva usada). Nunca
  automático fora do regime fixo/banda.

## 3. Padrão monetário

| padrão | regra efetiva | conversibilidade |
|---|---|---|
| `fiat` | regimes como estão; emissão da C2b1 | — |
| `bretton_woods` | toda moeda que não é a de reserva fica **efetivamente em `peg`** contra a moeda de reserva (o regime salvo é preservado e publicado) | o BC da moeda de reserva converte reservas em **ouro do cofre** ao preço fixo do padrão |
| `gold` | idem Bretton Woods **e** `M0 ≤ cobertura × valor do ouro do cofre` | idem |

- **Preço fixo do ouro**: preço de catálogo do bem `gold` (`goods.json`,
  `base_price = 3.000`, fonte `calibrated:legacy`), congelado quando o padrão é
  ligado e salvo no estado. O par histórico de Bretton Woods (USD 35/onça) **não
  é usado**: o nível de preço do jogo não é o dólar de 1944, e misturar os dois
  inventaria uma taxa de conversão. O status publica os dois números (fixo e de
  mercado) com o método.
- **Cobertura**: `cobertura = valor do ouro do cofre ÷ M0 da moeda`. No padrão
  ouro o BC controlado só emite até `cobertura × M0` ficar ≥ 100 %
  (`reserve_ratio = 1,0`, default de jogo). Sem emissão (todas independentes por
  default) a regra só é medida; ela nunca apreende moeda existente.
- **Troca de padrão**: enfileirada (`CommandType::SetMonetaryStandard`) e
  aplicada no início do dia, como todo comando A3. Não há troca automática.

## 4. Ouro físico

- O ouro do BC é **estoque físico**: as toneladas declaradas por
  `set_central_bank_gold` (dado real da G2/D-101, World Gold Council) são
  colocadas pelo `StorageService` no armazém `vault` do governo da maior cidade
  da nação; o cofre deixa de ser `structure_only` para o bem `gold`. O relatório
  passa a `gold_applied = true` e declara as toneladas não acomodadas.
- **Duas visões, uma tonelada**: `reserve_gold_tonnes[moeda]` é o razão do BC (o
  que entra no balanço) e o estoque do cofre é o espelho físico. Toda operação
  do BC mexe nos dois pelo mesmo valor; o teste confere a igualdade e a
  conservação `mercado + cofre` (a mina produz tonelada nova, como já produz).
- **Compra/venda do BC**: `central_bank_gold_trade(currency, tonnes, buy)` usa o
  mercado da cidade do cofre, ao preço do mercado (sem teto econômico desde a
  D-091; o limite diário de ±6 % é o passo de preço, não um teto). Quem compra
  paga do orçamento do governo e as firmas locais recebem; quem vende recebe no
  orçamento do governo e as firmas pagam. Ouro não é criado nem destruído; o
  dinheiro só muda de caixa.

### 4.1 Decisão: cofre do BC isento de manutenção de armazém

`phase_storage` cobra manutenção por tonelada-dia em todo armazém com
capacidade — inclusive o `vault` que passou a receber as toneladas de ouro do
banco central (32 840 t no mundo da G2). O cofre é **estrutura**, não operação
de mercado: o ouro fica parado, não gira estoque, e a manutenção movia caixa do
governo (para o setor de serviços local) por um estoque que ninguém movimenta.
A conta é a mesma que já isenta `vault` e `magazine` de receber produto e de a
IA construir (`storage_type_is_structure_only`); a isenção usa esse predicado,
sem caso especial novo.

**Efeito medido (365 d, IA, DLL debug, mesmo mundo):**

| cenário | falências | portão de comida | mediana | hash |
|---|---:|---|---:|---|
| **com a isenção** (este commit) | **2** | **86/179** | 0,9778281 | `f759f4c0…` (= base D-105) |
| sem a isenção (variante rejeitada) | 3 | 85/179 | 0,9779577 | `e5389c77…` |

Ou seja: cobrar manutenção do cofre criava a **3ª falência** (a base tem 2). O
portão da variante é 85/179 contra 86/179 com a isenção — número **melhor**, e
não é o motivo da rejeição: o motivo é a falência a mais e o hash sair da base.
JSON da variante rejeitada:
`docs/economy/perf/c2b2_vault_maintenance_365.json`.

A isenção vale para a categoria **estrutura** (`vault` + `magazine`), não só
para o ouro: o cofre recebe toda a categoria de produtos do cofre
(`kVaultProducts` — ouro, diamante, urânio, joias), e cobrar manutenção de
estoque parado não discrimina bem. O efeito medido é o mesmo com o predicado de
categoria (hash idêntico à base: nenhum paiol tinha capacidade no mundo).



## 5. Status / A4

`get_central_bank_status()` ganha:

- `monetary_policy`: `standard`, `reserve_currency`, `gold_conversion_price`,
  `gold_conversion_basis`, `coverage_ratio`, `effective_peg_standard`
  (`true` em Bretton Woods/padrão ouro) e o preço de mercado do ouro;
- `currencies[code].fx`: regime salvo e **efetivo**, paridade, largura da banda,
  taxa, desvio da paridade, reservas (âncora e local), intervenção do dia,
  cobertura de ouro, desvalorizações e o último evento;
- `fx_events`: os últimos 64 eventos (dia, moeda, tipo, paridade antiga/nova,
  reservas usadas), do mais recente para o mais antigo;
- `totals`: reservas (âncora), ouro do cofre (t) e o contador de desvalorizações.

`get_status()` não muda de forma (o bloco `balance` da C1 continua igual).

## 6. Save v29 e migração v28 → v29

Bloco aditivo:

- `fx` = `{currencies: {moeda: {parity, band_width, reserves, devaluations,
  last_event_day, conversion_price}}, events: [...], counters: {...}}`;
- `monetary_policy` ganha `gold_conversion_price` (0 = preço de catálogo).

Migração: v28 e anteriores carregam com paridade 0 (→ `base_rate`), banda no
default, reservas 0, nenhuma desvalorização e nenhum evento — nada inventado
para trás. Round-trip bit-exato e continuação idêntica (teste).

## 7. Testes

- `tools/c2b2_fx_gold_test.gd`: defesa de paridade com reserva declarada;
  esgotamento → desvalorização forçada (evento, paridade nova); padrão ouro
  limita a emissão (BC controlado) e Bretton Woods força o peg; ouro físico
  conservado (mercado + cofre) e igual ao razão do BC; migração 28 → 29 e
  round-trip v29; comando A3 aplica regime/padrão no dia seguinte.
- `tools/c1_world_ai_365.gd` (default, IA, threads=1): aceite do portão.
- `tools/c2b2_fx_stress_test.gd` (roda pelo runner, `-List c2b2_fx_stress_test`):
  estresse do câmbio — moedas em `peg` sobrevalorizado, banda de 0,5 %, defesa
  0,9 e reservas de 3 dias de importação; publica quantas moedas
  desvalorizaram, em que dia, e o efeito no portão e nas falências (§9).

## 8. Lacunas declaradas

- Sem banco comercial (C4) e sem reservas privadas: a contraparte da intervenção
  é o `foreign_cash` global.
- Reservas cambiais iniciais = 0 (falta fonte por país); o acúmulo é endógeno.
- Sem confiança/custo de defesa além da reserva gasta (C6).
- A banda administrada não tem UI/painel (decisão por API, como a C2b1).
- O ouro do cofre não é consumido por joalheria/indústria como demanda final
  (só compra/venda contra o mercado); fica para a C2c.
- **O `peg` não defende fluxo, defende desalinhamento**: fora do `floating` o
  câmbio sombreado é a própria `base_rate` (`floating_step` devolve `base_rate`
  quando `!currency.floating`), então o "gap" do dia é `base_rate ÷ paridade − 1`
  e não o passo de 0,05 % do F5. Consequência medida (§9): com a paridade igual
  à base a defesa **nunca** dispara e a reserva fica intacta; a reserva só sai
  quando a paridade está fora do mercado. Fica registrado como lacuna de
  mecânica (o fluxo comercial do dia não pressiona o peg) para a C6.
- **Banda declarada por comando não deriva**: `set_exchange_regime(..., "band")`
  grava `floating = false`, e `floating_step` devolve `base_rate` quando
  `!floating` — a banda pedida por comando fica presa na paridade declarada;
  só a banda vinda de dado (`regime: "band"` em `currencies.json`) deriva
  dentro da faixa. Não afeta o default nem as medições do §9; registrado
  para a C6.

## 9. Medição (2026-11-20, DLL debug, mundo real, IA, threads=1)

### 9.1 Default (2 rodadas idênticas)

`tools/c1_world_ai_365.gd`, 365 d, sem comando de câmbio:

| rodada | falências | violações | travados | portão | mediana | hash | parede |
|---|---:|---:|---:|---|---:|---|---:|
| 1 | 2 | 0 | 0 | 86/179 | 0,9778281 | `f759f4c0…abeafb` | 192,2 s |
| 2 | 2 | 0 | 0 | 86/179 | 0,9778281 | `f759f4c0…abeafb` | 192,6 s |

Dinheiro final 6 523 551 982,90166, com diferença relativa máxima diária de
4,18e-13 (conservação). O hash é **idêntico à base da D-105**: a C2b2 não muda
o mundo default — as 136 moedas carregam o regime default `peg` administrativo
(sem `regime` no `currencies.json`), com paridade = `base_rate`, reserva 0 e
padrão fiduciário; sem gap a defender, o câmbio anda como antes (§8).
JSONs: `docs/economy/perf/c2b2_world_ai_365_run{1,2}.json`.

### 9.2 Estresse fraco: só fundar reservas (`--fx-seed-days=30`)

Reservas = 30 dias de importação medida, fundadas no dia 30: **47 moedas
fundadas**, 5 399 876 805,30 unidades-âncora, **0 desvalorizações**, reserva
intacta ao fim, portão 86/179 e hash `f759f4c0…` **iguais ao default**. Prova
que reserva sem regime que a defenda é inerte (o default é `peg` com paridade =
`base_rate`, que não tem gap a cobrir).
JSON: `docs/economy/perf/c2b2_fx_scenario_365.json`.

### 9.3 Estresse forte: esgotamento de verdade

`tools/c2b2_fx_stress_test.gd` (runner), 365 d: no dia 30 **todas as 136
moedas** vão para `peg` com paridade 0,8 × paridade corrente (moeda 25 % mais
forte que o mercado), banda 0,5 %, defesa 0,9. Só as **47 com importação
medida** recebem reserva (= **3 dias**, 674 984 600,66 âncora); as 89 sem
importação ficam `funded = false` (peg administrativo, sem defesa). A âncora
semeada é 1,25 × o que o seed daria com a paridade de mercado (539 987 680,53),
porque o `peg` sobrevalorizado já está aplicado e `seed_reserves_from_imports`
divide a importação pela paridade.

| métrica | default | estresse forte |
|---|---:|---:|
| falências | 2 | **2** |
| violações de invariante | 0 | **0** |
| dias travados | 0 | 0 |
| portão de comida | 86/179 | **90/179** |
| mediana do portão | 0,9778281 | 0,9749213 |
| moedas desvalorizadas | 0 | **47 de 136** |
| desvalorizações (eventos) | 0 | **47** (1 por moeda) |
| reservas totais no fim | — | **0** (as 47 fundadas esgotadas; 89 nunca fundadas) |
| dinheiro total no fim | 6 523 551 982,90 | 6 499 950 986,04 (−0,362 %) |
| cobertura de ouro média | — | 0,00697 |
| hash de trajetória | `f759f4c0…` | `af8f6939…` |
| parede | 192 s | 196 s |

**Quando:** todas as 47 desvalorizações caem entre os dias **31 e 61** —
6 no dia 31, 3 no 32, 2 no 33, 1 no 34, 3 no 37, 1 no 38, 2 no 39, 1 no 40,
2 no 41, 2 no 42, 3 no 43, 1 no 44, 2 no 45, **7 no 46**, 1 no 47, 4 no 49,
1 no 50, 2 no 51, 1 no 53, 1 no 56, 1 no 61. Cada moeda desvaloriza **uma vez**:
a paridade nova é o câmbio sombreado (= `base_rate`), então a defesa seguinte
não tem gap a cobrir. Exemplos: `PGK`, `QAR`, `SBD` no dia 31; `BTN`
0,1740 → 0,2175 (+25 %) no dia 61; `MMK` 0,0534 → 0,0668 no dia 56.

**Efeito:** nenhuma falência nova e nenhuma violação do invariante financeiro
(a intervenção destrói moeda local pelo mesmo valor que o exterior devolve, e a
identidade da C1 fecha). O custo aparece no lado real: 4 nações a mais abaixo
do portão (86 → 90 de 179, mediana 0,9778 → 0,9749) e 23 600 996,86 (**23,6 mi**,
−0,362 % do dinheiro total) de moeda local recolhida ao vender reserva. JSON:
`docs/economy/perf/c2b2_fx_stress_365.json`.

**Achado de mecânica (lacuna §8):** a primeira tentativa do estresse — `peg` com
paridade = `base_rate`, banda 0,5 % e 3 dias de reserva — gastou **zero** de
reserva em 335 dias (`0` desvalorizações, hash igual ao default). Com a taxa
presa na paridade, o sombreado é a própria `base_rate` e o gap diário é zero;
a banda nunca é cruzada. Só paridade fora do mercado gasta reserva.

### 9.4 Goldens e equivalência (mesma DLL debug)

| prova | resultado |
|---|---|
| **S0** (`tools/economy_refactor_equiv.gd`, 60 d, `--mode=world --ai=1`) | 2 rodadas **idênticas**: save `b6dbebc1…`, status `60adbc2d…` |
| **A0** (`tools/world_equivalence.ps1`, 60 d, IA + urbano, 2 rodadas) | **0 diferenças** entre rodadas (129 s); save `e6c4564b…`, status `8047b236…`, dinheiro 6 523 551 982,900082, 0 falências |
| **CLI × Godot** (snapshot 849 cidades, 60 d, `--ai=1 --threads=1`) | **0 diferenças** com tolerância 0; arquivos **byte-idênticos** (`f3eea09012498d119db2d544142971f6c40f6aec90a1003c0dc4ca5f91ce2dfc`, 94 597 278 B); CLI exit 0, 0 violações; dinheiro 6 523 551 982,90 |

Os dois goldens foram **regerados** porque o save v29 ganha o bloco aditivo
`fx` (o payload muda) — o **`status_sha256` é o mesmo de antes** nos dois
(S0 `60adbc2d…`, A0 `8047b236…`), ou seja, o mundo não se moveu: mudou só o
formato do save. `money_total` e falências também idênticos.


