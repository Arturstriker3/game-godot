# C4 — Bancos público, privado e estrangeiro (bancos com dono)

Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE C / C4.
Antecessoras: C1 (D-102, balanço), C2a (D-103, BC por moeda), C2b1 (D-104,
taxa/emissão), C2b1-fix (D-105, medição e envelope da safra), C2b2 (D-106,
câmbio/padrão/ouro), C3 (D-107, banco agregado e crédito de empresa), C2c
(D-108/D-109, câmbio real e repasse).
Código: `gdextension/src/sim/bank.{h,cpp}` (reescrito), `sim/sim_types.h`
(`Bank`), `sim/balance.{h,cpp}`, `sim/save_state.cpp`, `sim/sim_core.cpp`,
`sim/monetary.cpp` (M0 × M1), `sim/agriculture.cpp` (juro da safra com
credor), `src/economy_sim.cpp`; dados
`data/definitions/economy/banks.json` (nomes já existentes) e
`data/definitions/economy/bank_credit.json` (regra).

## 1. O que a C4 faz

1. O "1 banco agregado por nação" da C3 vira **bancos com dono**: por nação,
   um **público** (dono = Tesouro/Estado), um **privado nacional** (dono =
   famílias da própria nação) e, **quando o dado disser que há banco
   estrangeiro**, um **estrangeiro** (dono = grupo/nação de outro país).
2. **Nomes** vêm de `banks.json` (genéricos/fictícios, política de marcas do
   projeto). Sem dado → nome ausente declarado (`name_status: "no_data"`),
   nunca nome inventado.
3. **Participação de mercado inicial** declarada (default de jogo, §3) — o
   repositório não tem fonte de participação por banco/nação.
4. **Depósitos de firmas** passam a existir: o empréstimo cria **dinheiro
   bancário** (depósito) em vez de M0; o cofre do banco (reserva) é M0 e não
   M1, então `M1 = M0 + depósitos − reservas`. O depósito é
   do tomador (grupo), que é quem recebe o crédito nesta fase.
5. **Capital próprio por dono**: cada banco tem capital pago pelo seu dono
   (Tesouro, famílias domésticas ou exterior), com a mesma conservação da C1.
6. **Lucro e dividendo ao dono**: no fechamento mensal, metade do lucro retido
   é distribuída ao dono; o banco estrangeiro **remete ao país de origem pelo
   câmbio da C2c** e publica o valor na moeda de origem.
7. **Status A4 por banco** (`banks[]`) e os agregados por nação (`nations[]`,
   compatível com a C3) e do mundo.
8. **Save v32** com migração v31→v32: o banco agregado da C3 é **dividido
   pelos três papéis com as participações** e o valor é **conservado**
   (resíduo de arredondamento fica no papel de maior participação).
9. **Juro do envelope de crédito da safra** (D-105) deixa de ser custo sem
   recebedor: quando ligado, o valor que o envelope desconta é **cobrado do
   tomador e creditado no banco público da nação** (§7). Continua **DESLIGADO**
   por default: a D-105 só libera o juro quando existir credor, e a ligação é
   uma decisão de calibração medida, não um default novo.

## 2. Identidade, dono e papéis

| papel (`role`) | chave | dono (`owner_kind`) | fonte do nome |
|---|---|---|---|
| `public` (0) | `<NAÇÃO>/public` | `state` (Tesouro da nação) | `banks.json` → `nations[X].public.name` |
| `private` (1) | `<NAÇÃO>/private` | `national` (famílias da nação) | `banks.json` → `nations[X].private.name` |
| `foreign` (2) | `<NAÇÃO>/foreign` | `foreign` (`owner_nation` = país de origem do dado) | `banks.json` → `nations[X].foreign.branch_candidate` |

- **Público e privado sempre existem** (são instituições do modelo). Se o nome
  não está no dado, o banco existe com `name_status: "no_data"`.
- **Estrangeiro só existe quando `nations[X].foreign.status == "present"`**
  (119 de 179 nações no dado). Sem banco estrangeiro, as participações são
  renormalizadas entre público e privado (§3).
- O dono do banco estrangeiro é a nação de origem declarada no dado
  (`foreign.origin_iso3`); sem origem, o estrangeiro não é criado mesmo que o
  dado diga "present" (ausência declarada, não palpite).
- O dono do banco privado são **as famílias da nação**: o dividendo é pago na
  poupança delas (a mesma conta que a C1 usa para o setor famílias).

## 3. Participação de mercado inicial

Não existe fonte pública de participação de mercado por banco e nação no
repositório. A participação é **default de jogo declarado** (não é dado
observado), usada para (a) dividir o banco agregado da C3 na migração e (b)
atribuir o tomador de cada grupo:

| papel | com estrangeiro | sem estrangeiro |
|---|---|---|
| público | 0,45 | 0,55 |
| privado | 0,40 | 0,45 |
| estrangeiro | 0,15 | — |

Razão declarada: em boa parte das economias o banco estatal é um bloco grande
de ativos, o privado doméstico é o maior bloco único e a presença estrangeira
é minoritária. Os três números são **arredondamentos de ordem de grandeza**
escolhidos para o jogo, não medição; o teste confere que somam exatamente 1 e
que a migração conserva valor com eles.

**Tomador (contraparte do empréstimo).** Cada grupo tem **um** banco credor,
determinado por regra declarada:
- grupo com `state_owned == true` → **público** (crédito dirigido do Estado,
  o que a C4 do plano pede);
- demais grupos → papel sorteado por **hash FNV-1a estável** do `group.id`
  contra as participações acumuladas dos bancos existentes na nação.

Um credor por grupo mantém a matemática da C3 (juro, atraso, amortização e
calote são por par banco↔grupo) e a identidade `Σ group.debt = Σ bank.loans`.

## 4. Contabilidade (dinheiro bancário, soma zero)

`Bank` (campos novos em negrito):

| campo | significado |
|---|---|
| `id`, `nation`, `currency` | identidade e moeda (a do BC da C2a) |
| **`role`**, **`name`**, **`name_status`** | papel e nome declarado |
| **`owner_kind`**, **`owner_nation`** | dono (`state`/`national`/`foreign`) |
| **`share`** | participação inicial declarada (§3) |
| `loans`, `arrears` | empréstimos e juros vencidos a receber (ativo) |
| **`deposits`** | depósitos à vista criados pelo crédito (passivo) |
| **`capital`** | capital pago pelo dono (claim do dono) |
| `bc_borrowing` | dívida com o BC (reserva criada; 0 na C4) |
| `cash` | caixa do banco (dinheiro do mundo) |
| **`dividends`**, **`remittance`**, **`remittance_origin`** | distribuição acumulada, remessa em âncora e na moeda de origem |
| `interest_income`, `interest_expense`, `writeoffs`, `defaults`, `loans_count` | contadores da C3 |
| **`harvest_interest`** | juro do envelope da safra recebido (§7) |

Patrimônio: `equity = cash + loans + arrears − bc_borrowing − deposits`.
Lucro retido = `equity − capital` (o capital pago pelo dono não é lucro).

**Empréstimo (A)** — o banco cria dinheiro bancário:

```
bank.loans += A ; bank.deposits += A
group.cash += A ; group.debt += A
```

Famílias/firmas (+A caixa, +A dívida) e banco (+A ativo, +A depósito) somam
zero; `money_total` sobe A e `M0` **não** muda (`M1 = M0 + depósitos −
reservas`; o cofre do banco é M0).

**Pagamento de principal (P)**: `group.cash −= P` (o depósito do grupo cai),
`bank.deposits −= P`, `bank.loans −= P`. Dinheiro bancário destruído; soma 0.

**Juro (D × taxa / 365)**: `group.cash −= I` e `bank.deposits −= I`; o
patrimônio do banco sobe I (o dinheiro é destruído e vira lucro retido). Sem
caixa, o juro fica em `credit_arrears`/`arrears` (não capitalizado).

**Calote**: igual à C3 (caixa tomado, construções fechadas, `writeoffs`), mas
o depósito do grupo é baixado junto: `bank.deposits −= min(seized, deposits)`.
A perda cai no patrimônio; nada some.

**Capital pago pelo dono** (`set_bank_capital`):
- público: sai do orçamento da maior cidade (o Tesouro do modelo), como na C3;
- privado: sai da poupança das famílias da nação (proporcional às cidades),
  `bank.capital` é claim **das famílias**;
- estrangeiro: sai de `foreign_cash` (posição do exterior), `bank.capital` é
  claim **do exterior** (`external_claims`).

**Dividendo** (fechamento mensal, dia múltiplo de 30): `payout = 0,5 ×
max(0, lucro retido)` (default de jogo: metade distribui, metade capitaliza).
O pagamento **cria depósito para o dono** (`owner.cash += payout`,
`bank.deposits += payout`, patrimônio −payout): o banco converte lucro em
dinheiro do dono sem destruir a identidade.
- público → orçamento do Tesouro (maior cidade da nação);
- privado → poupança das famílias da nação;
- estrangeiro → `foreign_cash` (exterior) **e** remessa publicada na moeda do
  país de origem pela paridade da C2c:
  `origin_amount = anchor × parity(origem)` (0 e `rate: null` quando a moeda
  de origem não existe no estado — ausência declarada).

**Livro do dia (`balance::Ledger`)** ganha `bank_deposits`, `household_claims`
(capital do banco privado) e `external_claims` (capital do estrangeiro):

```
banking = bank_cash + b_loans + b_arrears − b_borrowing − b_capital − b_deposits
financial_sum = famílias + firmas + governo + gov_claims + household_claims
              + exterior + external_claims + banking − dívida_pública − M0
M0 = money_total − depósitos   (público + reservas dos bancos)
M1 = money_total − cofre       (público + depósitos à vista)
```

**Invariantes diários** (em `SimCore::check_invariants`): os da C3 mais
1. `Σ group.debt = Σ bank.loans` e `Σ group.credit_arrears = Σ bank.arrears`;
2. `deposits ≥ 0`, `capital ≥ 0` e `deposits` do grupo coberto pelo caixa
   (o depósito do grupo é o caixa dele — mesma conta);
3. conservação estendida: `money_after = money_before + emission_today +
   bank_money_today`, onde `bank_money_today` é a variação dos depósitos na
   fase de crédito (só o banco cria dinheiro fora do BC);
4. identidade da C1 com os três claims de capital e os depósitos.

`SimCore::m0_total() = money_total() − Σ depósitos` passa a ser o M0 entregue
à política monetária (`monetary::tick`) e à alocação por moeda da C2a. Com o
crédito desligado (default) os depósitos são 0 e **tudo é idêntico à C3**.

## 5. Save v32 (aditivo) e migração v31 → v32

```json
{
  "version": 32,
  "banks": {
    "enabled": false,
    "policy": { "spread": 0.02, "...": "..." },
    "banks": {
      "BRA/public":  { "nation": "BRA", "role": "public", "name": "...",
                       "name_status": "present", "owner_kind": "state",
                       "owner_nation": "", "share": 0.45,
                       "loans": 0.0, "arrears": 0.0, "deposits": 0.0,
                       "bc_borrowing": 0.0, "cash": 0.0, "capital": 0.0,
                       "dividends": 0.0, "remittance": 0.0,
                       "remittance_origin": 0.0, "harvest_interest": 0.0,
                       "interest_income": 0.0, "interest_expense": 0.0,
                       "writeoffs": 0.0, "defaults": 0, "loans_count": 0,
                       "last_default_day": -1, "last_dividend_day": -1 }
    }
  }
}
```

**Migração v31 → v32:** cada linha `banks.nations[nation]` da C3 é dividida
pelos papéis com as participações declaradas; `loans`, `arrears`,
`bc_borrowing`, `cash`, `capital`, contadores e `loans_count` são rateados e o
**resíduo de arredondamento fica no papel de maior participação**, de modo que
a soma por nação é **bit-exata** antes e depois. `deposits = 0` (a C3 não tinha
dinheiro bancário), `enabled` e os parâmetros de política são preservados.
Se depois da carga o dado disser que **não há banco estrangeiro** na nação, o
papel estrangeiro é **fundido no privado** (conservando valores) por
`set_bank_definitions`; nunca se inventa banco estrangeiro.

Round-trip v32 é bit-exato e a continuação de 5 dias é idêntica.

## 6. Status A4 — `get_bank_status()`

Acrescenta, sem remover o contrato da C3:

```
policy:      {... C3 ..., deposits: "firm deposits (bank money, C4)",
              capital_source: {public: treasury, private: households,
                               foreign: external}, dividend_payout: 0.5,
              dividend_period_days: 30, resolution: "C6"}
totals:      {... C3 ..., deposits, dividends, remittance,
              remittance_origin, harvest_interest, bank_money, m1, m0}
banks[]:     { id, nation, role, name, name_status, owner_kind,
               owner_nation, share, share_loans, currency, policy_rate,
               loan_rate, loans, arrears, deposits, bc_borrowing, cash,
               capital, equity, equity_ratio, retained, dividends,
               remittance, remittance_origin, harvest_interest, writeoffs,
               defaults, loans_count, last_default_day, last_dividend_day }
nations[]:   agregado por nação (soma dos papéis) — compatível com a C3
```

`share_loans` = participação efetiva nos empréstimos da nação (0 sem crédito).

## 7. Juro do envelope de crédito da safra (D-105 → C4)

A D-105 desligou o juro porque "não há credor com contraparte no balanço".
A C4 cria o credor: quando `harvest_credit_rate` está ligado, o valor que o
envelope desconta da folha da lavoura (`daily_cost − cap`) é **cobrado do
tomador e creditado no banco público da nação** como juro de crédito
(`bank.cash += valor`, `bank.harvest_interest += valor`, caixa da firma
`−= valor`). Assim o custo não é mais sem recebedor e a soma financeira fecha.

- A flag continua **DESLIGADA** por default (`monetary_policy.json`,
  `harvest_credit.apply_policy_rate = false`): ligar muda o mundo e é decisão
  de calibração, não default.
- A ligação só é permitida com banco público existente na nação; sem banco, o
  valor **não** é cobrado (ausência declarada, comportamento da D-105).
- A/B por canal (§10) mede o canal isolado, sem calibrar às cegas.

## 8. O que fica para C6 e para a diplomacia

**C6 (crise e resolução):** corrida bancária, seguro de depósito, exigência
de capital, emprestador de última instância, bail-out/bail-in, fusão forçada,
estatização, liquidação, controle de capitais e contágio. Na C4 o banco pode
ficar com patrimônio negativo e **não** é socorrido; `resolution: "C6"`.

**Diplomacia (fora da C4):** banco estrangeiro como instrumento geopolítico
(abandono da matriz, sanções, congelamento, reciprocidade), soberania sobre
filial e o efeito de um resgate estrangeiro na relação entre países.

**Também fora:** hipoteca e imóvel como ativo dos estratos, crédito dirigido
além do `state_owned` e painel de UI (depois da Fase H).

## 9. Critérios de aceite

| # | critério | medida |
|---|---|---|
| B1 | mundo default não piora | 365 d IA threads=1: falências ≤ 3, portão ≥ 94/179, violações 0 |
| B2 | participações | por nação somam 1 (0,45/0,40/0,15 ou 0,55/0,45); `Δ ≤ 1e-12` |
| B3 | migração conserva valor | save v31 sintético: soma por nação antes = depois (bit-exato por nação), 3 papéis |
| B4 | dinheiro bancário | empréstimo: `Δmoney_total = Δdepósitos = A`, `ΔM0 = 0`; pagamento destrói `P` |
| B5 | remessa do estrangeiro | dividendo do banco estrangeiro credita `foreign_cash` e publica `origin_amount = anchor × parity(origem)` |
| B6 | nomes | todo `name` de banco é o de `banks.json` ou vazio (`no_data`); nenhuma marca real |
| B7 | save | round-trip v32 bit-exato; continuação 5 d idêntica; v31 carrega e conserva |
| B8 | identidade | `|financial_sum| ≤ max(1e-3, money×1e-12)` em todo dia com crédito ligado |

## 10. Medição

365 dias com IA, mundo real completo, `threads=1`, DLL debug recompilada,
**um Godot por vez** (`tools/c1_world_ai_365.gd`; harness de medição, não
catalogado no runner — que não passa argumentos; as três rodadas saíram em
sequência no mesmo terminal, 238-247 s cada). Saídas:
`docs/economy/perf/c4_gate_default.json`, `c4_gate_credit.json` e
`c4_gate_credit_harvest.json` (bloco `banks` novo: totais dos bancos na data
final).

O `drift` do harness é `(money_end − money_start) / money_start`. Com crédito
ligado ele **não** é vazamento: é o estoque de dinheiro bancário criado
(depósitos à vista − principal já amortizado). A conferência de conservação é
a identidade do balanço (`financial_sum ≈ 0`, `cash_residual ≈ 0`), que fecha
em todos os dias das três rodadas.

### 10.1 Default (juro da safra D-105 e crédito C3 desligados)

O default do jogo continua sem crédito (`bank_credit.json: enabled = false`) e
com `harvest_credit_rate = false`; só o harness liga por flag.

| medida | default |
|---|---|
| falências | 3 |
| portão de comida | 94/179 (mediana 0,9700) |
| violações do invariante | 0 |
| dias travados | 0 |
| drift de dinheiro | 1,6e-13 |
| inflação 12 m (mediana / p10 / p90) | 1,58 % / −0,24 % / 4,73 % (179 países) |
| taxa básica (mediana / p10 / p90) | 0,67 % / 0,00 % / 5,35 % (135 moedas) |
| dívida total dos grupos | 0 |
| calotes / baixas | 0 / 0 |
| depósitos, lucro, remessa dos bancos | 0 / 0 / 0 |
| M0 / M1 | 6.523.551.982,90 / 6.523.551.982,90 |
| hash da trajetória | `08770914` |

Igual ao portão da base (3 falências, 94/179, 0 violações): **B1 cumprido**, o
mundo default não piorou.

### 10.2 Crédito ligado (e a variante com o juro da safra)

| medida | default | `--credit=1` | `--credit=1 --harvest-rate=1` |
|---|---|---|---|
| falências | 3 | **2** | **2** |
| portão de comida | 94/179 (0,9700) | **98/179** (0,9661) | **98/179** (0,9693) |
| violações / dias travados | 0 / 0 | 0 / 0 | 0 / 0 |
| inflação 12 m (med / p10 / p90) | 1,58 / −0,24 / 4,73 % | 1,60 / −0,27 / 5,02 % | 1,59 / −0,39 / 4,59 % |
| taxa básica (med / p10 / p90) | 0,67 / 0,00 / 5,35 % | 0,80 / 0,00 / 5,03 % | 0,78 / 0,00 / 5,45 % |
| dívida total dos grupos | 0 | 24.287.115,20 | 24.551.743,26 |
| empréstimos (contratos) | 0 | 2.488 | 2.515 |
| calotes / baixas | 0 / 0 | 0 / 0,00 | 0 / 0,00 |
| depósitos à vista | 0 | 24.589.855,42 | 24.865.568,64 |
| juro recebido pelos bancos | 0 | 376.373,99 | 377.533,77 |
| juro pago (dívida do banco) | 0 | 0 | 0 |
| lucro retido (patrimônio) | 0 | 98.507,65 | 98.097,73 |
| dividendos pagos | 0 | 679.114,22 | 691.359,15 |
| remessa do banco estrangeiro | 0 | 32.176,97 | 30.786,97 |
| ... dos quais de origem (`origin_amount`) | 0 | 24.251,74 | 23.697,91 |
| M0 | 6.523.551.982,90 | 6.523.551.982,90 | 6.523.551.982,90 |
| M1 | 6.523.551.982,90 | 6.548.141.838,32 | 6.548.409.274,25 |
| drift de dinheiro | 1,6e-13 | 0,003769 | 0,003812 |
| hash da trajetória | `08770914` | `bfc4ed21` | `66534505` |

Leitura:

- **M0 não muda** ao ligar o crédito (Δ = 1,3e-3 em 6,5e9, ruído de soma) e
  **M1 − M0 = depósitos à vista − cofre do banco** nas duas rodadas com
  crédito: 24.589.855,42 com o cofre zerado, e 24.857.291,35 na variante com o
  juro da safra (diferença de 8.277,29 = o caixa do banco). É a definição da §3
  funcionando: `M0 = caixa − depósitos` (público + reservas),
  `M1 = caixa − reservas` (público + depósitos).
- O `drift` das rodadas com crédito **é** o estoque de dinheiro bancário:
  24.589.855,42 / 6.523.551.982,90 = 0,003769 (idem na variante). A identidade
  do balanço fecha (`financial_sum` entre −2e-5 e +2e-5).
- **Nenhum calote** em 365 d (0 de 2.488 e 0 de 2.515 contratos) e juro pago
  pelo banco 0 (sem empréstimo do BC): o banco é lucrativo, mas fino —
  patrimônio de 98,5 mil contra 24,3 milhões de dívida de grupos (a alavancagem
  efetiva vem do capital semeado pelas famílias, não de passivo).
- Com crédito ligado o mundo **melhora**, não piora: 3 → 2 falências e portão
  94 → 98 (as quatro nações a mais acima do portão). Não há canal que piore
  (inflação, juros e comida ficam dentro da dispersão da base).

### 10.3 A/B do canal da safra (D-105)

Comparando `--credit=1` com `--credit=1 --harvest-rate=1`: falências, portão e
violações **idênticos**; a diferença é o juro do envelope, que agora **tem
recebedor**: 8.277,29 entram no caixa do banco (`harvest_interest` = caixa do
banco na data final), a soma financeira continua fechando (+2,0e-5) e o lucro
do banco sobe o suficiente para pagar dividendos ligeiramente maiores
(691.359,15 vs 679.114,22) e remessa um pouco menor (30.786,97 vs 32.176,97).

O bloqueio da D-105 ("juro sem recebedor") está resolvido pela C4 quando o
crédito está ligado. A flag continua **desligada** por default: ligá-la é
decisão do dono, não default de implementação.

### 10.4 Candidato a default (decisão do dono)

Com o crédito ligado o mundo de 365 d não piorou em nenhuma medida e melhorou
em duas (falências e portão), sem calote e com a identidade fechando. Fica como
**candidato** a virar default numa decisão do dono — pendências antes disso:
C6 (resolução de crise) inexistente, capital bancário ainda 0 no estado inicial
(semeado pelas famílias no primeiro dia) e nenhuma calibração de spread/teto
contra um credor com passivo próprio.

