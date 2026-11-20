# Regimes cambiais reais por moeda (FMI/AREAER 2026)

Tarefa **só de dados**: nenhum código de jogo lê este arquivo ainda. O que existe aqui é
o dado de origem, o validador e o que a C2b2 precisa para consumi-lo (seção 7).

Arquivo: `data/definitions/economy/fx_regimes_2026.json`
Validador: `tools/validate_fx_regimes.py`

## 1. Fonte

- **IMF Annual Report 2026 — Appendices, Appendix II.9**: "De Facto Classification of
  Exchange Rate Arrangements, as of **April 30, 2026**, and Monetary Policy Frameworks".
  <https://imf.org/annual-report/2026/pdfs/imf-annual-report-2026-appendices.pdf>
- É a edição mais recente disponível (a de 2025, com data de 30/04/2025, foi substituída).
  A própria tabela avisa que as classificações são **preliminares** até o AREAER 2026.
- A tabela foi lida por coluna de âncora (USD, EUR, composta, outra) e pelas notas 3–12 do
  quadro. A coluna de arcabouço monetário (meta de agregado, meta de inflação, outro) **não**
  altera o regime cambial e foi ignorada para o mapeamento.
- Classificações com data antiga, declaradas pelo próprio quadro: Afeganistão e Venezuela
  **30/04/2021**; Síria **30/04/2017**. O Egito aparece com a nota 9 (reclassificado duas
  vezes no período).
- Fonte secundária, só para a Bulgária: BCE, 08/07/2025 — o país adotou o euro em
  **01/01/2026** (Decisão (UE) 2025/1407; 1 EUR = 1,95583 BGN). O AREAER 2026 ainda lista
  "currency board" e o jogo ainda tem a moeda `BGN`; a entrada é mantida com âncora `EUR`.

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
`arear_label`. `band` entrou como nona classificação porque o AREAER a tem e ela mapeia 1:1
no regime `band` do jogo (Marrocos).

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

Por **nação** (179 nações): `floating` 64, `band` 61, `peg` 50, `unknown` 4.
A diferença vem das moedas compartilhadas (EUR com 22 nações, XOF com 8, XAF e XCD com 6,
USD com 4, ILS e MAD com 2).

`unknown`: `CUP` (Cuba), `KPW` (Coreia do Norte), `TWD` (Taiwan) e `SLS` (Somalilândia) —
nenhum é membro do FMI, então não há classificação AREAER. `PAB` é `no_separate_legal_tender`
(o Panamá usa o dólar) e por isso aparece em `peg` com âncora `USD`.

## 4. Pegs e âncoras

28 moedas em `peg`: 20 com âncora declarada no dado, 8 sem.

**Âncora USD (15):** `AED`, `BBD`, `BHD`, `BSD`, `BZD`, `DJF`, `ERN`, `IQD`, `JOD`, `OMR`,
`PAB`, `QAR`, `SAR`, `TMT`, `XCD`.

**Âncora EUR (5):** `BAM`, `BGN`, `DKK`, `XAF`, `XOF`.

**Sem âncora declarada no Apêndice II.9 (8):** `BND`, `BTN`, `FJD`, `LSL`, `LYD`, `NAD`,
`NPR`, `SZL`. O regime é conhecido; só a moeda-âncora não está no quadro (ela fica nos
capítulos por país do AREAER). Ficaram `anchor: null`, `anchor_type: "not_stated"` — não
foram preenchidas por memória.

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
diferente da própria moeda; `source` presente; `band_width` só com fonte; `unknown` sem
regime nem âncora; `nations` igual a `currencies.json`; exceções pertencentes à moeda.
Sai com código ≠ 0 em erro.

## 9. O que não mudou

- Nenhum arquivo de código, cena ou save foi alterado.
- `currencies.json` continua sem `regime` (o dado novo é um arquivo separado).
- O jogo continua nascendo com todas as 136 moedas no default `peg` administrativo até a
  C2b2 carregar este arquivo.
