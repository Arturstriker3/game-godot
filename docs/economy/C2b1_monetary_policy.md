# C2b1 — Emissão de moeda e juros do banco central

Decisão **D-104**. Base: `balance/global-resources` @ `71c980cc` (C2a/D-103).
Código: `gdextension/src/sim/monetary.{h,cpp}`, `sim/central_bank.{h,cpp}`,
`sim/balance.cpp`, `sim/labor.cpp`, `sim/ai/ai_storage.cpp`, `sim/save_state.cpp`,
`EconomySim::get_central_bank_status()` / `set_central_bank_policy()`.
Dados: `data/definitions/economy/monetary_policy.json` (fonte "default de jogo").
Teste: `tools/c2b1_monetary_test.gd`.

## 1. O que esta fase faz

1. **Emissão de M0 contabilizada.** O BC só cria base monetária por uma
   operação registrada no balanço da C1: nesta fase, **compra de títulos do
   governo** (financiamento do Tesouro) pelo BC **controlado**. Redesconto a
   bancos e venda de títulos ficam declarados como ausência: não há banco (C4)
   nem estoque de dívida pública anterior (C3).
2. **Taxa básica por moeda** (um BC por moeda, como na C2a):
   - **independente** — regra tipo Taylor sobre a inflação de 12 meses do CPI da
     C2a (ou a anualizada quando os 12 meses ainda não fecham) e o **hiato**
     medido pelo desvio do desemprego em relação ao desemprego natural do
     modelo (`natural_unemployment`), com **passo limitado** e **suavização**;
   - **controlado** — segue o objetivo do governo declarado: juro abaixo do
     neutro e **emissão para cobrir o déficit medido**; sem déficit, neutro.
3. **Canal de transmissão mínimo e explícito** (nada além disto):
   - **custo do crédito existente (D-099):** a linha de crédito da safra
     (folha/opex da fazenda com plano cobertos pelo ativo da lavoura) tem o
     **envelope** descontado pela taxa básica da moeda do país. Não há banco
     para receber o juro nesta fase (C4), então **nenhum dinheiro sai da
     firma**: o juro é o custo do crédito embutido na capacidade da linha.
   - **decisão de estoque privado (G2):** o payback do silo privado passa a
     exigir cobrir o **custo de capital** (`custo × taxa` por ano) além da
     manutenção e da perda; a taxa chega congelada na `AiView` do dia.
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

- `get_central_bank_status()`: por moeda, dentro de `currencies[code].policy`:
  `rate`, `target_rate` (o que a regra queria), `target_inflation`,
  `neutral_rate`, `max_step_per_day`, `smoothing`, `inflation_weight`,
  `gap_weight`, `rate_floor`/`rate_cap`, `has_inflation`/`inflation_used`,
  `gap`, `deficit_today`, `unfunded_today`, `emission_today`, `emission_total`,
  `securities` e `independence_saved` × `independence_effective`; no topo,
  `totals` (emissão do dia/total, títulos, dívida do governo, M0/M1/M2) e
  `government_bonds` (dívida por nação).
- `get_status()`: `balance.monetary_claims` e `balance.government_debt`
  (resumo diário); nada de novo por cidade.
- **Save v28** (aditivo): `monetary = {policies: {moeda: {rate, emission,
  securities, last_day}}, gov_bonds: {ISO3: valor}, gov_cash_open: {ISO3:
  valor}}`. Migração v27 → v28 carrega com taxa neutra (sem decisão), emissão 0,
  dívida 0 e janela do déficit vazia (o primeiro dia a captura do caixa do
  governo) — nada inventado para trás; round-trip bit-exato. `emission_today`,
  `deficit_today` e `unfunded_today` são medição do dia e não vão ao save.
- `gov_cash_open` **vai** ao save: é a base da janela do déficit usada na
  decisão seguinte; sem ela, um `load` recomeçaria a janela no caixa do load e a
  monetização divergiria do caminho direto (o teste de round-trip cobre isso).

## 5. Prova

- `tools/c2b1_monetary_test.gd` (**verde**): default efetivo × salvo, paridade do
  JSON com as constantes do núcleo, regra de Taylor com passo máximo de 25 pb e
  ausência declarada da inflação no início (sem 12 meses), BC controlado
  emitindo **só** o déficit medido (gasto público gera o déficit no teste),
  teto de 1 % do M0 por dia, `ΔM0 + Δ(não atribuído) = emissão`, dívida pública
  = títulos do BC = emissão, round-trip v28 e migração v27 → v28.
- `tools/c2_central_bank_test.gd` ajustado: a política deixou de ser inerte; a
  prova da mecânica passou para o teste acima.
- **365 d com IA** (`tools/c1_world_ai_365.gd`, mundo real, `threads=1`,
  2 rodadas idênticas, `docs/economy/perf/c2b1_world_ai_365.json`):

| medida | base C2a (`f7631ba0…`) | C2b1 |
|---|---|---|
| falências | 2 | **2** |
| violações / dias travados | 0 / 0 | **0 / 0** |
| hash de trajetória | `f7631ba0…` | `f7631ba0…` (igual) |
| drift máximo diário | 4,3e-13 | 3,0e-13 |
| portão de comida | 85/179 (mediana 0,9798) | **88/179 (0,9779)** |
| inflação 12 m (mediana / p10 / p90) | — | 0,91 % / 0,71 % / 4,37 % (179 países) |
| taxa básica (mediana / p10 / p90) | — | 0,52 % / 0,00 % / 6,11 % (136 moedas) |
| emissão total | 0 | 0 (todas independentes por default) |

  **O portão de comida NÃO foi mantido** (88 vs 85). Atribuição medida em A/B de
  365 d: **+2** do juro no crédito da safra (D-099, envelope ~0,1 % menor) e
  **+1** do custo de capital do silo (G2). É uma queda real de renda rural e de
  consumo (~0,2 pp na mediana), não ruído: cada canal sozinho move o portão,
  porque a distribuição tem dezenas de países logo abaixo de 0,975. Não houve
  calibração para mascarar o efeito.
- **CLI × Godot**: 10 d byte-idênticos pelo snapshot (`75.299.780 B`, SHA-256
  `3934D855…`, 0 diferenças) — com `--ai=0` nos dois lados.
- **Goldens A0/S0** regerados no save v28, duas rodadas idênticas cada:
  `A0_60_urban` save `766ec733…`/status `c6f69c88…`, `real_60_ai`
  `cbeffb0d…`/`60adbc2d…`, `real_60_noai` `3f01e374…`/`7508ff1e…`,
  `demo_365_noai` `f06bddf1…`/`a7b50ce3…` (o status não muda; o save ganhou a
  janela do déficit).
- **Custo:** 365 d passam de 166,7 s (C1) para 174,3 s (+4,6 %) com o refresh do
  caixa do governo por dia e a leitura do CPI/taxa por moeda no fecho (medição
  da rodada com o status; a rodada final sem status fechou em 172,0 s).
- **Reversão de tick:** o `Snapshot` carrega `monetary`, `gov_bonds`,
  `gov_cash_open` e `emission_today`, e o retrato é tirado **depois** do reset
  do dia — um tick revertido não deixa emissão nem janela de déficit para trás.
  A janela também **vai ao save** (v28), então a continuação depois de um `load`
  é idêntica ao caminho direto (o teste de round-trip gasta e cobra emissão dos
  dois lados).

## 6. Lacunas declaradas

- Sem banco: reservas bancárias, redesconto, M1/M2 emergindo do crédito e o
  **credor** do juro da safra são C4. O juro hoje só reduz a capacidade da linha
  (não é pago a ninguém).
- Sem dívida pública anterior nem venda de títulos pelo BC: o único título é o
  criado pela monetização do déficit (C3).
- Sem intervenção cambial/banda administrada, padrão ouro/Bretton Woods agindo e
  ouro físico negociado (C2b2/C2c).
- O BC controlado é decisão por API (`set_central_bank_policy`), sem UI/painel.
- `bank_independence` segue `no_data` no save: o default `independent` é do
  jogo, não dado de país.
