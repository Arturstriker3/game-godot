# C2b1 — Emissão de moeda e juros do banco central

Decisão **D-104**. Base: `balance/global-resources` @ `71c980cc` (C2a/D-103).
Código: `gdextension/src/sim/monetary.{h,cpp}`, `sim/central_bank.{h,cpp}`,
`sim/balance.cpp`, `sim/labor.cpp`, `sim/ai/ai_storage.cpp`, `sim/save_state.cpp`,
`EconomySim::get_central_bank_status()` / `set_central_bank_policy()`.
Dados: `data/definitions/economy/monetary_policy.json` (fonte "default de jogo").
Teste: `tools/c2b1_monetary_test.gd`.

## 1. O que esta fase faz

1. **Emissão de M0 contabilizada.** O BC só cria ou destrói base monetária por
   uma operação registrada no balanço da C1: nesta fase, **compra de títulos do
   governo** (financiamento do Tesouro) do BC **controlado**. Redesconto a bancos
   e venda de títulos ficam declarados como ausência: não há banco (C4) nem
   estoque de dívida pública anterior (C3).
2. **Taxa básica por moeda** (um BC por moeda, como na C2a):
   - **independente** — regra tipo Taylor sobre a inflação de 12 meses do CPI da
     C2a (ou a anualizada quando os 12 meses ainda não fecham) e o **hiato**
     medido pelo desvio do desemprego em relação ao desemprego natural do
     modelo (`natural_unemployment`), com **passo limitado** e **suavização**;
   - **controlado** — segue o objetivo do governo declarado: juro abaixo do
     neutro e **emissão para cobrir o déficit medido**; sem déficit, neutro.
3. **Canal de transmissão mínimo e explícito** (nada além disto):
   - **custo do crédito existente (D-099):** a linha de crédito da safra
     (folha/opex da fazenda com plano cobertos pelo ativo da lavoura) paga juro
     diário da taxa básica da moeda do país; o juro é **transferido** do caixa da
     firma para o orçamento do governo (dinheiro conservado) e limitado ao caixa.
     Juro não suportado pelo caixa não vira dívida (rolagem é C3) e é declarado.
   - **decisão de estoque privado (G2):** o payback do silo privado passa a exigir
     cobrir o **custo de capital** (`custo × taxa` por ano) além da manutenção.
4. **Status A4 e save v28** com migração v27 → v28.

## 2. Conservação: "dinheiro total = anterior + emissão líquida registrada"

`SimCore::money_total()` continua sendo a contagem canônica. O invariante diário
passa a comparar `money_after` com `money_before + emission_today`, onde
`emission_today` é o que o BC **registrou** no dia (zero fora de operação). A
identidade financeira da C1 acompanha:

```
ativos públicos = passivo líquido do emissor
famílias + firmas + caixa do governo + exterior
    − dívida pública − (M0 − títulos do BC) = 0
```

`balance::Ledger` ganha `government_debt` (títulos emitidos) e `monetary_claims`
(títulos na carteira do BC); a soma continua fechando e a checagem cruzada do
caixa contra `money_total` não muda.

## 3. Defaults (jogo, não dado de país)

`bank_independence` está `no_data` para todas as moedas (D-103). O default
**declarado do jogo** é `independent`; o valor salvo continua `no_data` até uma
política ser escolhida, e o status publica `saved` × `effective`.

| parâmetro | valor | por quê |
|---|---|---|
| `target_inflation` | 3 % a.a. | meta declarada do jogo |
| `neutral_rate` | 4 % a.a. | taxa real neutra 1 % + meta |
| pesos de Taylor | 1,5 (inflação) / 0,5 (hiato) | forma padrão da regra |
| passo máximo | 25 pb por dia | evita degrau e instabilidade |
| suavização | 0,5 | metade do caminho por dia |
| piso / teto da taxa | 0 % / 30 % a.a. | limites declarados |
| juro do BC controlado | neutro − 2 pp (piso 0) | "juros mais baixos" |
| emissão máxima | 1 % do M0 da moeda por dia | senhoriagem com limite |
| hiato | (desemprego natural − desemprego medido) em pp | medido no modelo |

Sem inflação na série o termo de inflação **não entra** (`has_inflation=false`) e
a taxa fica em neutro + hiato — nada é completado com número inventado.

## 4. Status e save

- `get_central_bank_status()`: por moeda, `policy_rate`, `policy_target`,
  `target_inflation`, `inflation_used`/`has_inflation`, `gap`, `emission_today`,
  `emission_total`, `securities`, `independence_saved` × `independence_effective`;
  no topo, `monetary` (emissão do dia/total, títulos, dívida por nação).
- `get_status()`: `balance.monetary_claims` e `balance.government_debt`
  (resumo diário); nada de novo por cidade.
- **Save v28** (aditivo): `monetary = {policies: {moeda: {rate, emission,
  securities, last_day}}, gov_bonds: {ISO3: valor}, emission_total}`. Migração
  v27 → v28 carrega com taxa neutra, emissão 0 e dívida 0 (nada inventado para
  trás); round-trip bit-exato.

## 5. Prova

- `tools/c2b1_monetary_test.gd`: default efetivo × salvo, paridade do JSON com
  as constantes, regra de Taylor (inflação e hiato), limite de passo, BC
  controlado emitindo só o déficit medido, `ΔM0 = emissão registrada`,
  invariantes com 0 violações, round-trip v28 e migração v27 → v28.
- 365 d com IA comparado à base C2a (`f7631ba0…`): falências, portão de comida,
  inflação anual por país, taxa básica (mediana/p10/p90), violações.
