# C1 — Contabilidade por balanço (o novo invariante)

> Fase C1 de `docs/plans/perf_pop_financas_plano.md` (PARTE C). Branch
> `feat/c1-balance-sheet`, worktree `opengs-wt-c1`, base `balance/global-resources`
> `21cd4293`. Decisão **D-102**. Save **v26** (migração v25 → v26).
> Sem banco nesta fase: criação de moeda é a C2; empresa com balanço é a C3.

## 1. O que a C1 faz e o que ela não faz

A C1 é uma camada de **leitura contábil** sobre o estado que já existe. Nada move
dinheiro, nada é criado. Ela:

1. organiza o estado em **ativos e passivos por agente** (famílias por quintil,
   firmas/grupos, governo, setor externo);
2. fecha a **identidade financeira do mundo** — todo ativo financeiro é passivo
   de alguém — como **invariante diário**, ao lado da conservação de caixa;
3. faz a **liquidez × solvência** das firmas ler o balanço (a regra provisória da
   D-099 deixa de ter a conta própria);
4. publica o **balanço por nação/agente** no status (A4, sob demanda) e um resumo
   no status diário;
5. grava o único instrumento financeiro novo no save: o **salário atrasado**.

O que ela **não** faz (fica para C2/C3/C4): banco, depósitos, empréstimos,
títulos, emissão de moeda, balanço da empresa com dívida/estoque/capital
próprios, liquidação do atrasado com juros.

## 2. Identidade financeira (o invariante)

Para cada agente, `patrimônio líquido = ativos − passivos`. No mundo:

```
(famílias + firmas + governo + exterior) − passivo da autoridade monetária = 0
```

- **Instrumentos financeiros hoje:** caixa (moeda) e salário atrasado.
- **Caixa:** `famílias` (poupança dos quintis + rendas pendentes de
  capital/aluguel + caixa da região B3), `firmas` (caixa das plantas + do setor
  de serviços + caixa dos grupos + fundos de obra reservados + caixa de
  transporte e construção), `governo` (orçamento público por cidade +
  prefeituras e obras) e `exterior` (`foreign_cash`, negativo por desenho).
- **Passivo da autoridade monetária:** enquanto não há banco central (C2), é um
  **placeholder** igual ao `money_total()` do dia — a moeda em circulação é
  dívida de quem a emitiu. A C2 substitui o placeholder pelo passivo do BC e a
  identidade continua a mesma.
- **Salário atrasado:** passivo das firmas (`Building::wage_arrears`,
  `City::service_wage_arrears`, `City::rural_wage_arrears`) e ativo das famílias
  (`Pop::arrears_receivable`, `Region::arrears_receivable`). Aparece dos dois
  lados e cancela.

**Checagem cruzada (`cash_residual`):** o balanço enumera os caixas por conta
própria e tem de reproduzir o `money_total` canônico. Um caixa esquecido em um
dos dois lados aparece como resíduo ≠ 0 — foi assim que a primeira versão pegou
o orçamento público faltando na enumeração. Tolerância: `max(1e-3, |money| ×
1e-12)`.

Os dois testes rodam dentro de `SimCore::check_invariants` a cada dia, junto da
conservação de dinheiro. Violação reverte o tick e loga (mesmo caminho do drift).

## 3. Balanço por agente

| Agente | Ativos | Passivos |
|---|---|---|
| **Famílias** | poupança dos quintis (caixa), rendas pendentes (capital/aluguel), caixa da região, **salário atrasado a receber**, duráveis a preço de mercado, **terra + moradia + anel da B6** (valor de mercado por quintil) | dívida (C4; zero) |
| **Firmas/grupos** | caixa das plantas e do setor de serviços, caixa dos grupos, fundos de obra em voo, caixa de transporte/construção, **estoque do mercado da cidade** e do **armazém privado (G2)** a preço de mercado, **capital fixo** (`capital_per_capacity × capacidade`) | **salário atrasado** (plantas, serviços, rural) |
| **Governo** | orçamento público por cidade, caixa e obras das prefeituras, **reserva nacional da G2** a preço de mercado | — |
| **Exterior** | `foreign_cash` (negativo = crédito externo usado; o exterior é credor) | — |

`net_worth = caixa + recebíveis + estoque + capital fixo + patrimônio −
passivos − dívida`.

**O que NÃO entra, por decisão declarada:**

- `stratum_equity` (participação em grupos): é avaliação da mesma base de ativos
  dos grupos; somar seria contar duas vezes.
- **Setor externo por nação:** o núcleo guarda um `foreign_cash` GLOBAL. O status
  publica a posição agregada e marca `decomposed: false` por nação — não é
  rateado por palpite.
- Caixas sem nação atribuível (fundos de obra em voo, grupos sem origem
  declarada) saem em `unallocated`, fora da soma por país.

## 4. Salário atrasado (o instrumento da D-099)

A D-099 diz: "o que não se paga é folha REDUZIDA — dinheiro não é criado". A C1
distingue dois casos:

- **Folha reduzida por iliquidez da safra** (D-099): não é dívida — não é devida;
  o contrato do dia é a folha efetiva.
- **Folha devida que o caixa não cobriu** (`pay_scale < 1` nas plantas;
  `full_service_bill > service_cash`; salário rural semanal maior que o caixa):
  é **salário atrasado**, reconhecido como passivo da firma e ativo das famílias.

**Regra do período corrente:** o estoque é a folha não paga do ciclo — diária nas
plantas e serviços, semanal no rural — e **substitui** a do ciclo anterior.
Acumular a carência do dia 1 viraria uma dívida eterna e inflaria o patrimônio
das famílias sem lastro. Rolagem, juros e liquidação são a C3 (a falência já
baixa o atrasado da planta dos dois lados — perda do credor, não dinheiro que
some).

**Ordem de grandeza (dia 10, mundo real):** ~9,8 bi XGS, quase todo **rural**
(1,45 bi no dia 1), contra 6,5 bi de `money_total`. Isso não é erro de conta: a
folha rural semanal do modelo é ~1,5× o estoque de dinheiro do mundo, e o
passivo é coberto 4× pelo PL das firmas (≈ 41 bi de estoque + capital). É a
evidência de que a C2 (crédito) é necessária — o modelo paga salário com um
estoque de moeda pequeno e uma velocidade alta.

## 5. Liquidez × solvência (substitui a regra provisória onde coube)

`balance::marketable_assets()` é a única dona da conta da D-099 —
`(estoque + safra em pé) × preço × fatia da firma`. `crop_liquidity_factor()`
chama essa função; `firm_production_share()` expõe a fatia. Resultado idêntico
(verificado no 365 d: hash de trajetória `f7631ba0…`, o mesmo da base
`21cd4293` no harness comparável — ver `c1_world_ai_365.json`).

O diagnóstico publicado em `get_balance_status()["world"]["firms_solvency"]`:

- **insolvente:** patrimônio da planta (capital fixo + estoque a preço de
  mercado − atrasado) < 0;
- **ilíquida:** patrimônio ≥ 0 mas menor que 30 dias do custo efetivamente pago
  (folha + opex do último dia);
- **líquida:** o resto.

É leitura: a decisão de falência continua a da D-099 (não muda resultado).

## 6. Status / A4

- `get_status()["balance"]` — resumo diário barato: `money_total`,
  `cash_total`, `cash_residual`, `receivables`, `payables` (+ composição),
  `monetary_base`, `financial_sum`, `ok`.
- `get_balance_status()` — sob demanda: `world` (famílias, firmas, governo,
  exterior, `firms_solvency`), `by_nation` (as mesmas contas por país, com
  `external.decomposed = false`), `invariant`, `unallocated`, `notes`.

Nenhuma UI/painel nesta fase (telas só depois da Fase H).

## 7. Save v26 e migração v25 → v26

Campos novos, todos aditivos:

- `city.pop.arrears` — atrasado a receber das famílias da cidade;
- `city.service_wage_arrears` / `city.rural_wage_arrears` — passivo por origem;
- `city.region.arrears` — atrasado rural a receber (interior);
- `buildings[].wage_arrears` — atrasado da planta.

Migração: **v25 e anteriores carregam com atrasado 0** (nada é inventado para
trás). O resto do balanço é derivado e não ocupa o save. Round-trip v26 é
bit-exato e a continuação após load é idêntica (`tools/c1_balance_test.gd`).

## 8. Testes

- `tools/c1_balance_test.gd` — identidade financeira diária (fixture 40 d e
  mundo real 30 d), conservação de caixa, net worth recalculado à mão,
  patrimônio B6 igual à soma das regiões (mundo e por nação), estoque do
  governo igual aos armazéns públicos da G2, aditividade das famílias por nação,
  atrasado a receber = a pagar e coberto pelo PL das firmas, censo de solvência
  fechando as classes, round-trip v26 e migração v25 → v26.
- `tools/c1_world_ai_365.gd` — aceite de 365 d (violações, drift, portão de
  comida, balanço por nação da amostra). Saída em
  `docs/economy/perf/c1_world_ai_365.json`.

## 9. Lacunas declaradas

- Sem banco: a moeda é conservada e o emissor é um placeholder.
- Exterior não decomposto por nação (`foreign_cash` global).
- Atrasado sem rolagem/juros/liquidação (C3): o estoque é a folha não paga do
  ciclo. A folha de serviços entra quando o caixa do setor não cobre a folha
  devida (o limite de receita do F8 é o próprio pagamento, não um atraso).
- Capital fixo a valor contábil (`capital_per_capacity × capacidade`), não a
  valor de mercado — não há mercado de capital nesta fase.
- A B6 valoriza terra/moradia/anel no calendário mensal: nos primeiros 30 dias o
  patrimônio por quintil ainda é zero.
