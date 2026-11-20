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
- Cenário de estresse: `--peg-top=10` põe as 10 maiores moedas em `peg` ao
  dólar e publica quantas desvalorizaram e o efeito no portão.

## 8. Lacunas declaradas

- Sem banco comercial (C4) e sem reservas privadas: a contraparte da intervenção
  é o `foreign_cash` global.
- Reservas cambiais iniciais = 0 (falta fonte por país); o acúmulo é endógeno.
- Sem confiança/custo de defesa além da reserva gasta (C6).
- A banda administrada não tem UI/painel (decisão por API, como a C2b1).
- O ouro do cofre não é consumido por joalheria/indústria como demanda final
  (só compra/venda contra o mercado); fica para a C2c.
