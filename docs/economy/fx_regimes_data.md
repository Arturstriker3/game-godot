# Regimes cambiais reais por moeda (FMI/AREAER 2026)

Tarefa **só de dados**: nenhum código de jogo lê este arquivo ainda. O que existe aqui é
o dado de origem, o validador e o que a C2b2 precisa para consumi-lo (seção 7).

Arquivo: `data/definitions/economy/fx_regimes_2026.json`
Validador: `tools/validate_fx_regimes.py`

## 1. Fonte

- **IMF Annual Report 2026 — Appendices, Appendix II.9**: "De Facto Classification of
  Exchange Rate Arrangements, as of **April 30, 2026**, and Monetary Policy Frameworks".
  <https://www.imf.org/annual-report/2026/pdfs/imf-annual-report-2026-appendices.pdf>
- **Edição conferida em 2026-11-20**: o PDF existe (publicado em 15/09/2026, 39 páginas) e o
  quadro está nas pp. 19–23. Foi essa a edição lida; não há versão posterior.
- É a edição mais recente disponível (a de 2025, com data de 30/04/2025, foi substituída).
  A própria tabela avisa que as classificações são **preliminares** até o AREAER 2026.
- A tabela foi lida por coluna de âncora (USD, EUR, composta, outra) e pelas notas 3–12 do
  quadro. A coluna de arcabouço monetário (meta de agregado, meta de inflação, outro) **não**
  altera o regime cambial e foi ignorada para o mapeamento.
- Classificações com data antiga, declaradas pelo próprio quadro: Afeganistão e Venezuela
  **30/04/2021**; Síria **30/04/2017**. O Egito aparece com a nota 9 (reclassificado duas
  vezes no período).
- As "mudanças desde a edição anterior" foram conferidas no Apêndice II.9 de 2025
  (<https://www.imf.org/external/pubs/ft/ar/2025/pdfs/imf-annual-report-2025-appendices.pdf>):
  China estava em *crawl-like* com âncora **composta** (nota 4) e passou a âncora USD;
  Argentina era *crawling peg* (nota (12/23)) e passou a *other managed*; Índia era
  *stabilized* e passou a *crawl-like* em 04/2025.
- Fonte secundária, só para a Bulgária: BCE, 08/07/2025 — o país adotou o euro em
  **01/01/2026** (Decisão (UE) 2025/1407; 1 EUR = 1,95583 BGN). O AREAER 2026 ainda lista
  "currency board" e o jogo ainda tem a moeda `BGN`; a entrada é mantida com âncora `EUR`.
  **Pendência registrada na seção 10 e em `pending` no JSON.**

Nada foi preenchido de memória: moeda sem classificação no AREAER ficou `unknown`.

## 2. Vocabulário e mapeamento

O FMI tem dez grupos; o jogo tem três (`floating` / `band` / `peg`, da C2b2/D-106).

| classificação (dado) | rótulo AREAER | regime do jogo | por quê |
|---|---|---|---|
| `free_floating` | Free floating | `floating` | mercado determina |
| `floating` | Floating | `floating` | idem |
| `stabilized` | Stabilized arrangement | `band` | move-se, mas dentro de faixa estreita |
| `crawl` | Crawling peg / Crawl-like arrangement | `band` | paridade móvel; o jogo não modela a deriva, a banda é o que sobra |
| `band` | Pegged within horizontal bands | `band` | banda explícita |
| `other_managed` | Other managed arrangement | `band` | administrado, sem paridade fixa |
| `conventional_peg` | Conventional peg | `peg` | taxa presa na paridade |
| `currency_board` | Currency board | `peg` | idem, com regra de emissão |
| `no_separate_legal_tender` | No separate legal tender | `peg` | compartilha a moeda da âncora |
| `unknown` | — | `null` | sem fonte; o carregador mantém o default atual |

`crawl` agrupa "crawling peg" (Botswana) e "crawl-like" (25 moedas); o rótulo exato fica em
`areaer_label` (campo renomeado de `arear_label` nesta revisão; nenhum código o lia). `band`
entrou como nona classificação porque o AREAER a tem e ela mapeia 1:1 no regime `band` do
jogo (Marrocos).

**Tipo de âncora.** `anchor_type` aceita `currency` (moeda nomeada), `composite` (cesta — a
coluna "Composite" do quadro), `not_stated` (o quadro não nomeia a âncora) e `none` (não se
aplica). O jogo chama a cesta de `composite`; não se criou sinônimo novo.

**Largura de banda**: o Apêndice II.9 **não declara** largura de banda de nenhum país.
Por isso `band_width` é `null` em todas as entradas e o jogo continua usando o default de
`data/definitions/economy/exchange_policy.json` (banda de ±5%, declarada como design, não
observada). O validador recusa `band_width` sem `band_width_source`.

## 3. Contagens

Por **moeda** (136 moedas do jogo):

| regime do jogo | moedas |
|---|---|
| `floating` | **44** (12 `free_floating` + 32 `floating`) |
| `band` | **60** (24 `stabilized` + 26 `crawl` + 1 `band` + 9 `other_managed`) |
| `peg` | **28** (22 `conventional_peg` + 5 `currency_board` + 1 `no_separate_legal_tender`) |
| `unknown` | **4** |

Por **nação** (179 nações): `floating` 69, `band` 61, `peg` 45, `unknown` 4.
A diferença em relação à contagem por moeda vem das moedas compartilhadas (EUR com 22 nações,
XOF com 8, XAF e XCD com 6, USD com 4, ILS e MAD com 2).

`unknown`: `CUP` (Cuba), `KPW` (Coreia do Norte), `TWD` (Taiwan) e `SLS` (Somalilândia) —
nenhum é membro do FMI (Cuba saiu do Fundo em 1964), então não há classificação AREAER. `PAB`
é `no_separate_legal_tender` (o Panamá usa o dólar) e por isso aparece em `peg` com âncora
`USD`.

## 4. Pegs e âncoras

28 moedas em `peg`: 26 com moeda-âncora nomeada e 2 com âncora em cesta.

**Âncora USD (15):** `AED`, `BBD`, `BHD`, `BSD`, `BZD`, `DJF`, `ERN`, `IQD`, `JOD`, `OMR`,
`PAB`, `QAR`, `SAR`, `TMT`, `XCD`.

**Âncora EUR (5):** `BAM`, `BGN`, `DKK`, `XAF`, `XOF`.

**Âncora em outra moeda, fora das colunas do quadro (6).** O Apêndice II.9 coloca estes seis
na coluna **"Other"**: a classificação é conhecida, mas o quadro não nomeia a moeda-âncora.
A âncora só foi preenchida porque o banco central emissor (ou o *country report* do FMI para
o país) declara o peg; a citação fica em `anchor_source`, por entrada.

| moeda | país | âncora | onde está declarado |
|---|---|---|---|
| `BND` | Brunei | `SGD` (1:1) | BDCB, *Monetary Policy Framework* — "the local currency is pegged to the Singapore dollar at par"; MAS, *Currency Interchangeability Agreement* (CIA, 1967) |
| `BTN` | Butão | `INR` (paridade) | RMA, *FAQ* — "pegged to the Indian rupee at parity"; IMF CR 26/019 (Butão, Art. IV 2025) |
| `LSL` | Lesoto | `ZAR` (1:1) | CBL, *Monetary Policy* — "pegging the Loti to the South African Rand"; IMF CR 25/267 (Lesoto, Art. IV 2025) |
| `NAD` | Namíbia | `ZAR` (1:1) | Bank of Namibia, *FAQ* — "pegged to the South African Rand on a one-to-one basis" |
| `NPR` | Nepal | `INR` (1,6) | NRB, *Macroeconomic Report* (fev/2026) — 1,6 NPR por INR desde 12/02/1993; IMF CR 26/127 (Nepal, Art. IV 2026) |
| `SZL` | Essuatíni | `ZAR` (1:1) | Central Bank of Eswatini, *Currency* — "at par with the South African Rand through the CMA" |

**Âncora em cesta (7 entradas; 2 delas `peg`):** `FJD` e `LYD` (`peg`), `BWP` (*crawling peg*),
`SGD` (*crawl-like*), `MAD` (banda horizontal), `KWD` e `SYP` (*other managed*). O quadro os
põe na coluna "Composite (8)" da seção *Exchange rate anchor*, ou seja, cesta de moedas — mas
não publica a composição. Ficam `anchor: null`, `anchor_type: "composite"`, com nota. O oitavo
da coluna (`WSM`, Samoa) não é moeda do jogo: aparece na coluna de meta de agregado com a
nota 4.

**Âncora dos regimes que não são `peg`.** O quadro também declara âncora para estabilizados,
*crawl-like* e outros administrados, pela coluna (US Dollar / Euro) ou pela nota 5/6, e o
arquivo passou a refletir isso: `GYD`, `KHR`, `LBP`, `NIO`, `TTD` e `IRR` → `USD`; `MKD` →
`EUR`; `BWP`, `SGD`, `KWD` e `SYP` → cesta. Onde a nota 5/6 contradiz a coluna, vale a nota,
que é o texto específico do quadro — é o caso de `VND` (coluna "Composite", nota 5 → `USD`).

Nenhuma âncora foi preenchida por memória: ou está na coluna do quadro, na nota 5/6, ou tem
citação em `anchor_source`; o que não tem fonte continua `null` com `anchor_type:
"not_stated"`. Depois desta revisão **nenhuma entrada ficou nesse estado**.

Moedas âncora com vários países aparecem como um país só no quadro: EUR = UEM, XOF = WAEMU,
XAF = CEMAC, XCD = ECCU, USD = EUA. `MNE` e `XKX` usam o euro e `ECU`, `SLV` e `TLS` usam o
dólar **sem moeda própria**; isso está em `exceptions` nas entradas `EUR` e `USD`.

`HKD` e `MOP` não são moedas do jogo (Hong Kong e Macau não são nações do cenário), então a
regra HKD→USD do pedido não tem linha própria aqui.

## 5. Exemplos

| moeda | classificação | regime do jogo | âncora |
|---|---|---|---|
| `BRL` | Floating | `floating` | — |
| `USD` | Free floating (EUA) | `floating` | — (compartilhada com ECU/SLV/TLS) |
| `EUR` | Free floating (UEM) | `floating` | — (compartilhada com MNE/XKX) |
| `CNY` | Crawl-like | `band` | `USD` (mudou de composta para USD) |
| `ARS` | Other managed | `band` | — (era crawling peg na edição anterior) |
| `SAR` | Conventional peg | `peg` | `USD` |
| `XOF` | Conventional peg | `peg` | `EUR` |
| `RUB` | Free floating | `floating` | — |
| `INR` | Crawl-like | `band` | `USD` (era stabilized) |
| `BND` | Currency board | `peg` | `SGD` (CIA; fora do quadro, com `anchor_source`) |
| `FJD` | Conventional peg | `peg` | cesta (`composite`; coluna "Composite" do quadro) |

## 6. Independência do banco central — adiada

Não foi embutida nesta passada, por dois motivos declarados:

- os índices mais atuais (**Romelli, CBIE 2025**: 155 países, 1923–2023) liberam os dados
  para uso acadêmico/pessoal, mas **exigem consentimento para redistribuição** — embutir os
  valores no repositório não é permitido sem isso;
- a alternativa (**Garriga, CBI v3**: 192 países, 1970–2023) precisa da licença conferida
  antes de qualquer valor entrar no jogo.

Ficou registrado em `central_bank_independence` no JSON. Hoje `monetary_policy.json` declara
`bank_independence = "independent"` como default de jogo (não observado), e a C2a marcou
`bank_independence = no_data`.

## 7. O que a C2b2 precisa para ler isto (sem implementar)

Levantado no código atual:

1. **Default de regime fixo no C++.** `economy_sim.cpp` cria as moedas com
   `std::string default_regime = "peg"` e aceita por moeda só `float` e `band`; qualquer
   outro valor cai em `peg`. `currencies.json` hoje tem `regime: null`. Para o default vir
   do dado, o carregador precisa ler um campo de default (por exemplo
   `exchange_policy.json.default_regime`) e aceitar o vocabulário `floating`/`band`/`peg`
   deste arquivo — hoje `floating` e `float` são nomes diferentes.
2. **Âncora só existe como taxa contra a âncora interna (XGS).** `exchange::parity_of`
   devolve `ws.fx[code].parity` ou `currency.base_rate`, uma taxa única contra o XGS; não
   há campo de moeda-âncora. `peg` e `band` prendem a taxa nessa paridade absoluta e o
   `tick` compara o câmbio sombreado com ela (`exchange.cpp`). Para XOF→EUR, SAR→USD ou
   XCD→USD a paridade tem de ser **cross-rate**: `taxa(âncora) / unidades_por_âncora`
   recalculada todo dia, e a defesa tem de olhar o desvio contra a moeda-âncora, não contra
   o XGS. Sem isso, uma moeda que flutua contra o XGS (EUR, USD) arrasta todas as que estão
   ancoradas nela.
3. **Persistência.** O save v29 grava `exchange_regime` por moeda e o bloco `fx`
   (`parity`, `band_width`, `reserves`, `devaluations`, `last_event_day`,
   `conversion_price`). Duas decisões precisam de migração:
   - mundo novo: regime e âncora vêm deste arquivo;
   - save existente: o regime do save continua valendo (é o estado do mundo rodando). Se a
     decisão for adotar os regimes reais em saves antigos, a migração v29→v30 deve trocar o
     regime **só** de moedas com estado `fx` intocado (`parity` 0, reservas 0, sem
     desvalorização), gravar um `fx_event` da troca e deixar as demais como estão.
   Comandos (`set_exchange_regime`) continuam sendo a autoridade por cima do dado.
4. **Status.** Vale publicar por moeda a origem do regime (`AREAER 2026`, data de
   referência) e o motivo de `unknown`, para o jogador não ler um regime sem fonte.

## 8. Validador

```powershell
python tools/validate_fx_regimes.py
```

Confere: cobertura exata das 136 moedas; classificação dentro de `classification_values`;
`game_regime` igual a `game_regime_map[classification]`; âncora existente no jogo e
diferente da própria moeda; `anchor_type` coerente com `anchor`; `anchor_source` só com
âncora em moeda; `areaer_label` em toda entrada classificada; `source` presente;
`band_width` só com fonte; `unknown` sem regime nem âncora; `nations` igual a
`currencies.json`; exceções pertencentes à moeda; e a lista `pending` citando moeda do jogo.
Sai com código ≠ 0 em erro e imprime as pendências declaradas.

## 9. O que não mudou

- Nenhum arquivo de código, cena ou save foi alterado.
- `currencies.json` continua sem `regime` (o dado novo é um arquivo separado).
- O jogo continua nascendo com todas as 136 moedas no default `peg` administrativo até a
  C2b2 carregar este arquivo.

## 10. Pendência declarada — Bulgária no euro

O lev deixou de existir em **01/01/2026**: a Bulgária entrou na área do euro com a taxa
irrevocável **1 EUR = 1,95583 BGN** (Decisão (UE) 2025/1407, de 08/07/2025; comunicados do
BCE de 08/07/2025 e de 01/01/2026). O Apêndice II.9, com data de referência de 30/04/2026,
**ainda lista a Bulgária como "Currency board" com âncora euro** — classificação preliminar
que o próprio quadro admite revisar no AREAER 2026.

O cenário do jogo **não** foi alterado nesta passada (a moeda `BGN` continua em
`currencies.json`, e a entrada `BGN` segue `currency_board`/`peg`/âncora `EUR`, fiel ao
quadro). Fica registrado, em `pending` no JSON e na entrada `BGN`, que:

- o jogo deveria tratar `BGR` como membro da área do euro (como `MNE`/`XKX`, em `exceptions`
  da entrada `EUR`), e `BGN` como moeda legada com paridade fixa 1,95583;
- enquanto isso não acontecer, qualquer leitura do regime da Bulgária herda a classificação
  preliminar do FMI, que já está desatualizada em relação ao fato.
