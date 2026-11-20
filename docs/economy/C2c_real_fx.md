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
Saída em JSON (campos `fx`, `monetary`, `food_gate`, `balance`).

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

**Lacuna de medição:** o canal não foi separado (A = só regimes, B = só
Bulgária). Como os dois indicadores ficam exatamente no teto, essa separação é
a primeira coisa a fazer se o QA quiser folga.

## 5. Testes

- `tools/c2c_real_fx_test.gd` (novo): peg cruzado (âncora dobra → taxa dobra),
  cesta (média geométrica e pesos no status), recusa de não-emissor, união
  travada, BGN → EUR conservando `money_total` e invariante, save v31
  bit-exato, continuação idêntica após load e migração v30 → v31 (inclui a
  sucessão de moeda).
- Vizinhos verdes pelo runner: `c2b2_fx_gold_test`, `c2_central_bank_test`,
  `c2b1_monetary_test`, `c3_bank_credit_test`, `c3_group_balance_test`,
  `currency_trade_test`, `economy_integration_test`, `services_test` (constante
  da versão atual do save atualizada para 31 neles).
- `tools/validate_fx_regimes.py`: **OK** (135 moedas; BGN em `retired`).

## 6. O que não mudou / limites

- O padrão monetário (fiduciário) e o ouro físico da C2b2 continuam iguais; a
  troca de padrão segue só por comando A3.
- Reservas iniciais continuam 0 (não há fonte por país): o peg sem reserva é
  administrativo e **não** gasta reserva — a lacuna "o fluxo do dia não
  pressiona o peg" (C2b2 §8) continua.
- A taxa local exibida da Bulgária passa a ser a do euro (o dado de taxa é
  estimativa PPP `derived:gdp_ppp`, não a paridade oficial): a conversão a
  1,95583 é aplicada à paridade local e a tudo o que é denominado em moeda
  local; os saldos/preços/contratos do núcleo são em XGS e não se movem.
- `docs/economy/fx_regimes_data.md` descreve o arquivo antes desta fase
  (136 moedas, BGN presente). O estado atual do dado está aqui e em
  `retired`/`pending` do próprio JSON.
