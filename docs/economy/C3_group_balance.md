# C3 — Empresa com balanço (grupo, crédito e perda do credor)

Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE C / C3.
Antecessoras: C1 (D-102, balanço e identidade financeira), C2a (D-103, BC por
moeda), C2b1 (D-104, taxa e emissão), C2b1-fix (D-105, medição), C2b2 (D-106,
câmbio/padrão/ouro).
Código: `gdextension/src/sim/bank.{h,cpp}` (novo), `sim/balance.{h,cpp}`,
`sim/sim_day.cpp`, `sim/save_state.cpp`, `sim/sim_core.cpp`,
`sim/ai/ai_group.cpp`, `src/economy_sim.cpp`; dado
`data/definitions/economy/bank_credit.json`.

## 1. O que a C3 faz

1. **Balanço do grupo** (leitura, sempre ativa): cada `Group` publica caixa,
   capital fixo das construções que possui, estoque atribuído, dívida e
   patrimônio líquido. Não move dinheiro.
2. **Crédito de empresa** (mecânica): um **banco agregado por nação** — o
   "1 banco agregado por nação" da C2, que nasce aqui como credor — recebe
   depósito de reservas do banco central e **empresta ao grupo**. Emprestar
   cria M0; pagar o principal destrói M0; juros transferem caixa; **calote
   extingue a posição dos dois lados e a perda fica no patrimônio do banco**
   (dinheiro não some).
3. **Alavancagem responde ao juro**: o limite de crédito e a decisão da IA usam
   `taxa do empréstimo = taxa básica da moeda + spread`; juro maior encolhe a
   dívida do grupo e o número de projetos financiados.
4. **Status A4**: `get_bank_status()` (por nação, com invariante) e o bloco
   `groups` do `get_balance_status()`; o bloco diário `balance` ganha
   `group_debt` e `bank_loan_assets` (zero no default).
5. **Save v30**: bloco `banks`, campos de crédito por grupo e o estado do
   crédito; migração aditiva v29 → v30.

## 2. O que fica para C4 e para C6

**C4 (bancos público/privado/estrangeiro + crédito às famílias):**

- divisão do banco agregado em **3 bancos por nação** (dono pelo sistema da F9);
- **depósitos** e M1/M2 com dinheiro bancário; hoje M1 = M2 = M0;
- **capital bancário semeado** (hoje 0 — ver §4) e compulsório;
- hipoteca/moradia como ativo dos estratos e crédito dirigido do banco público.

**C6 (crise e resolução):**

- liquidez de última instância, seguro de depósito, bail-out/bail-in, fusão
  forçada, estatização, liquidação, controle de capitais e contágio entre
  bancos/nações. Na C3 o banco só **publica** patrimônio negativo; nada o
  socorre.

**Fora das duas:** bolsa (C5) e painel de UI (depois da Fase H).

## 3. O banco agregado e a identidade da C1

Um `Bank` por nação (criado no primeiro empréstimo, não semeado):

| campo | significado |
|---|---|
| `nation`, `currency` | nação e moeda (a moeda do BC que o financia) |
| `loans` | principal empréstimo a grupos (ativo) |
| `arrears` | juros vencidos e não pagos (ativo, contas a receber) |
| `bc_borrowing` | dívida com o banco central (passivo; reservas criadas) |
| `cash` | caixa do banco (lucro retido, dinheiro do mundo) |
| `capital` | capital próprio (0 — ausência declarada, ver §4) |
| `interest_income`, `interest_expense`, `writeoffs`, `defaults` | contadores |

`equity = cash + loans + arrears − bc_borrowing + capital`.

**Empréstimo** (A): o BC cria A de M0 em favor do banco
(`bc_borrowing += A`), o banco entrega A ao grupo (`loans += A`,
`group.debt += A`, `group.cash += A`). Identidade: ativo do BC (+A) contra
passivo monetário (+A); grupo (caixa +A, dívida +A); banco 0. Soma = 0.

**Pagamento de principal** (P): `group.cash -= P`, o banco recebe e abate a
dívida com o BC (`bc_borrowing -= P`), o M0 correspondente é destruído.
Idem, soma = 0.

**Juros**: o grupo paga `D × loan_rate / 365` em caixa ao banco; o banco paga
ao BC `bc_borrowing × policy_rate / 365` (abatendo a dívida de reservas — o
que destrói M0 do juro básico) e retém o spread como caixa. Sem caixa, o juro
fica em `Group::credit_arrears` (ativo do banco, passivo do grupo) — não é
capitalizado em silêncio.

**Calote**: quando o grupo é insolvente (ativos reais + caixa < dívida +
atrasados, por `kDefaultDays` dias), o banco executa: leva o caixa do grupo,
fecha as construções do grupo (`bankrupt`, a mesma regra das firmas),
`writeoffs +=` o que sobrou, `loans -=`, `group.debt = 0`. A perda cai no
`equity` do banco; `money_total` só muda pelo caixa transferido (conservado).

**Invariantes diários** (em `SimCore::check_invariants`, junto dos da C1):

1. `Σ group.debt = Σ bank.loans` e `Σ group.credit_arrears = Σ bank.arrears`
   (tolerância `max(1e-6, valor × 1e-12)`);
2. `loans`, `arrears`, `bc_borrowing`, `cash` e `group.debt` nunca negativos;
3. a identidade da C1 fecha com o setor bancário e com o ativo do BC
   (`monetary_claims` passa a incluir `bc_borrowing`, como já inclui os
   títulos do governo);
4. `equity` do banco pode ser negativa — é a perda medida, não um erro.

## 4. Por que o crédito nasce DESLIGADO

O banco nasce sem capital e sem depósitos: o repositório não tem fonte de
capital bancário por nação (dado de C4) e `M1 = M2 = M0` sem depósito (C2a).
Um banco com patrimônio 0 fica insolvente no primeiro calote — socorrê-lo é
C6. Ligar o crédito por default seria calibrar às cegas contra o portão de
365 d. Portanto:

- `data/definitions/economy/bank_credit.json` declara `enabled: false`,
  `spread: 0.02`, `leverage_cap: 1.0`, `default_days: 30`,
  `repay_share: 0.5` (todos **default de jogo**, fonte declarada);
- o **mundo default não muda**: o hash de 365 d tem de ser o mesmo da base
  (`f759f4c0…`);
- o laboratório/teste liga o crédito por comando A3 (`set_bank_credit`) ou
  por `set_bank_capital`, e é assim que "alavancagem responde ao juro" e
  "calote é perda do credor" são medidos.

## 5. Contrato de dados

### 5.1 Save v30 (aditivo)

```json
{
  "version": 30,
  "credit": { "enabled": false },
  "banks": {
    "nations": {
      "BRA": { "currency": "BRL", "loans": 0.0, "arrears": 0.0,
               "bc_borrowing": 0.0, "cash": 0.0, "capital": 0.0,
               "interest_income": 0.0, "interest_expense": 0.0,
               "writeoffs": 0.0, "defaults": 0 }
    }
  },
  "groups": [ { "...": "...", "debt": 0.0, "credit_arrears": 0.0,
                "credit_default_days": 0, "credit_defaults": 0 } ]
}
```

Migração v29 → v30: `enabled = false`, `banks.nations = {}`, `debt = 0`,
`credit_arrears = 0`, contadores 0 — **nada inventado para trás**. Round-trip
bit-exato e continuação idêntica após load.

### 5.2 Status A4 — `get_bank_status()`

```
policy:   { enabled, spread, leverage_cap, default_days, repay_share, source }
totals:   { loans, arrears, bc_borrowing, cash, capital, equity, writeoffs, defaults }
nations[]:{ nation, currency, policy_rate, loan_rate, loans, arrears,
            bc_borrowing, cash, capital, equity, leverage, defaults, writeoffs }
groups[]: { id, name, origin_nation, sector, debt, arrears, equity, leverage,
            default_days, defaults }
invariant:{ loans_vs_debt_residual, arrears_vs_debt_residual, financial_sum }
```

`get_balance_status().groups[]` (leitura, sempre): `id`, `name`,
`origin_nation`, `sector`, `cash`, `inventory`, `fixed_capital`, `debt`,
`arrears`, `equity`, `leverage`, `inventory_basis`.

Estoque do grupo = fatia de capacidade do par cidade×produto nas cidades onde
ele produz (a mesma regra de rateio da receita); o que não é de grupo
publicado sai em `unallocated.groups_inventory`, declarado.

### 5.3 Comandos A3

- `SetBankCredit(enabled)` — liga/desliga a mecânica (a fila A3 já existe).
- `GroupBorrow(group_id, amount)` — pede crédito ao banco da nação do grupo,
  limitado por `leverage_cap × equity` e pelo caixa do banco/BC.
- API direta equivalente para teste: `set_bank_credit(bool)`,
  `set_bank_capital(nation, amount)`, `group_borrow(id, amount)`.

## 6. Critérios de aceite (medíveis)

| # | critério | medida |
|---|---|---|
| A1 | mundo default não muda | 365 d IA threads=1: hash `f759f4c0…`, falências 2, portão 86/179, violações 0, travados 0 |
| A2 | identidade da C1 fecha com banco | 60 d, crédito ligado, capital 1e6: `|financial_sum| ≤ max(1e-3, money×1e-12)` em **todo** dia |
| A3 | empréstimo cria e pagamento destrói M0 | `money_total` após emprestar = antes + A; após pagar = antes + A − P (tolerância 1e-6) |
| A4 | alavancagem responde ao juro | mesmo mundo, `set_bank_policy_rate` 2 % × 20 %: dívida total no dia 60 menor a 20 % (e nº de empréstimos menor) |
| A5 | calote é perda do credor | `bank.equity` cai exatamente `writeoffs`; `money_total` só varia pelo caixa tomado; grupo com dívida 0 e construções fechadas; identidade fecha |
| A6 | save | round-trip v30 bit-exato; continuação 5 d idêntica; save v29 antigo carrega com crédito desligado e dívida 0 |
| A7 | ausência declarada | banco sem capital/depósito publica `capital = 0`, `deposits: absent`; nenhum número de país inventado |

## 7. Testes

- `tools/c3_group_balance_test.gd` — balanço por grupo (caixa, capital fixo,
  estoque atribuído, dívida, PL, alavancagem), aditividade com o balanço das
  firmas da C1, `unallocated` declarado.
- `tools/c3_bank_credit_test.gd` — A2..A6: empréstimo/pagamento/juros,
  alavancagem × juro, calote com perda do banco, identidade diária, status A4,
  save v30 e migração v29 → v30.
- `tools/c1_world_ai_365.gd` — A1 (mundo default).

## 8. Riscos declarados

- O banco é **agregado e sem depósito**: não é um sistema bancário, é o credor
  mínimo que a C3 precisa para a dívida da empresa ter contraparte. C4 o
  substitui por três bancos com dono.
- Sem capital, o primeiro calote deixa patrimônio negativo: `get_bank_status()`
  publica `equity < 0` e `resolution: "C6"`. Não há resgate nesta fase.
- O estoque por grupo é rateio de leitura; só o estoque das construções do
  grupo é atribuível. O resto é declarado, não rateado por palpite.
