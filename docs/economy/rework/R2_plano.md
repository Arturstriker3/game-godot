# R2 — BANCOS: plano de implementação (rev2)

> Branch `rework/r2-bancos`, worktree `G:/projetos/opengs-wt-r2`, base `main` `dbf5e73d`
> (merge do R1). Spec: `REWORK_spec_mestre.md` §1.5 e §12 (R2), no repo
> `opengs-icons-private`. **Status: plano. Nenhuma linha de código, nenhum número de
> mundo muda nesta entrega.**
>
> **rev2 (27/11/2026):** incorpora o QA independente **t185**
> (`G:/opengs_tmp/qa_r2plano.md`, 10 achados: 3 críticos, 6 altos, 1 médio).
> O QA não executou teste Godot; esta revisão fecha os três críticos com o código
> real e acrescenta a medição de PIB que faltava. O que mudou:
>
> | # | rev1 dizia | rev2 passa a dizer |
> | --- | --- | --- |
> | 1 | `deposits` único; "M1 constante" | `deposits` (criado por empréstimo) e `deposits_funding` (claim) separados; M0/M1/broad definidos contra `sim_core.cpp:504-509` e `central_bank.cpp:390`; **broad money** constante, não M1 |
> | 2 | capitalizar 10% do PIB no dia 0 | medir o PIB (24,0–25,1 bi XGS/ano, não 6,6 bi); alvo real vira **gap publicado**; seed limitado ao caixa existente; convergência por retenção (~1,1 ano até 10% da carteira) |
> | 3 | float de 5 dias de opex | colchão = cesta de 6 meses + orçamento regional do mês; depósitos entram em `private_liquidity()`/`firm_liquidity()`; saque no mesmo dia acima do float |
> | 4 | bail-in cobra "depósito não segurado" | bail-in corta o **claim do depositante** (D_f) e o caixa do tomador (D_l), na ordem leilão→bail-in→fundo→Tesouro |
> | 5 | dividendo por meta | dividendo **debita `bank.cash`** e credita o dono; nunca cria depósito |
> | 6 | "qualquer banco" | servicing por credor real (`phase()`/safra/C6 param de assumir o público); dívida por banco |
> | 7 | OFF bit a bit contra `6287b12d…` em toda fase | baseline **por sub-fase**: R2a-ON vira o baseline do R2b, etc.; `6287b12d…` só no combo tudo-Off |
> | 8 | cascata Tesouro→dívida→imprimir | ordem alinhada a `rescue_with_debt`/`emission`; dívida só com comprador; válvula ≤1% M0/dia; `treasury_debt_rescue=false` no R2c |
> | 9 | fallback capitaliza todos | fallback só em papel `present`; `none`/`no_data` não são capitalizados |
> | 10 | load v43 com seed no 1º load | seed só em **mundo novo**; save v43 carregado não recebe capital retroativo; converge por retenção |

Base herdada do R1 (default ON desde `97e869df`): caixa único regional com
sub-saldos `firm_sub`/`service_sub`, Tesouro Nacional + orçamento regional
(limite, não carteira), empresas operando 100% do caixa próprio. Save v43.
Baselines pós-merge: default/ON `81eccf2a…`, legado OFF `6287b12d…`.

---

## 0. Escopo

**Entra no R2** (decisões do dono já fechadas — não rediscutidas aqui):

1. **Capitalização no dia 0 com dado real** (% do PIB por país), nos bancos que o
   dado declara.
2. **Competição real** entre bancos privados por taxa e capacidade; fim do monopólio
   público em giro/safra/investimento.
3. **Depósitos** vindos das **regiões** (pool privado) e das **empresas** (caixa de
   grupo); pessoas físicas **ficam no colchão** (`pop.savings`), sem depósito.
4. **Alocação de depósitos** por relacionamento + solidez + pêndulo.
5. **Sem juros de depósito** (fase política, se o dono quiser).
6. **Dividendo do banco por meta de adequação de capital** (retém até ~10% da
   carteira, distribui o excedente) — morre o 50% fixo.
7. **Resolução de falência**: leilão de carteira com deságio; banco público como
   **comprador de última instância**; **cascata** do banco público (Tesouro
   recapitaliza → dívida → imprimir controlado/resolução → deserto bancário).

**Fica fora, mas com gancho pronto:**

- Títulos públicos / Selic como ativo bancário e canal de política (fase política).
  O R2 só deixa a estrutura para o banco carregar títulos (`Bank::gov_bonds = 0`).
- **Rating A–CCC por estrato** e crédito imobiliário (bloco moradia, R7).
- Garantia de depósito como *decisão* política: o R2 mantém `insured_share` como
  default declarado (C6) e trata o rombo não coberto como perda a resolver.
- Bancos por região (cooperativas/regionais) — o R2 é por nação, como a C4.
- Emissão de M0 além da válvula existente (≤1% M0/dia, regime "controlled").

**O que a rev2 NÃO rediscute:** as sete decisões acima continuam. O que a rev2
faz é **traduzir** a decisão 1 para a escala real do modelo (seed limitado +
convergência) e declarar os números que a decisão 1 não tinha.

---

## 1. Inventário do que existe hoje

### 1.1 Bancos (C4 / D-110)

Até **três bancos por nação**, por papel:

| Papel | `role` | Dono (`owner_kind`) | Existe? |
| --- | --- | --- | --- |
| Público | 0 | Estado/Tesouro (`state`) | linha sempre criada por `ensure()`; o dado decide se é instituição real |
| Privado nacional | 1 | famílias da nação (`national`) | idem |
| Estrangeiro | 2 | nação de origem (`foreign`) | só quando `banks.json` declara presença |

`data/definitions/economy/banks.json` (cobertura 179):

| Papel | `present` | `none` | `no_data` |
| --- | ---: | ---: | ---: |
| Público | 127 | 36 | 16 |
| Privado | 140 | 12 | 27 |
| Estrangeiro | 119 | 11 | 49 |

Sem estrangeiro, o papel é fundido no privado conservando valores. `apply_definitions`
cria a linha do banco público e do privado para toda nação mesmo quando o dado diz
`none`: a linha existe zerada (`name_status = "no_data"`), **não é uma instituição
capitalizada**. A rev2 usa isso explicitamente (§4.4 e §10.4).

Campos hoje (`Bank`): `loans`, `arrears`, `deposits`, `bc_borrowing`, `cash`,
`capital`, `interest_income/expense`, `writeoffs`, `dividends`, `remittance`,
`harvest_interest`, `defaults`, `loans_count`, mais o bloco C6
(`confidence`, `run_days`, `resolved`, `resolution`, `absorbed_into`,
`capital_flight`, `insured_payout`, `bailin_cut`, `treasury_rescue`).

Matemática monetária (C4, `docs/economy/C4_banks.md`), confirmada no código:

- empréstimo: `bank.loans += A`, `bank.deposits += A`, **e o caixa do tomador
  recebe A** (`bank.cpp:428-433`) → cria M1 **e** caixa; `money_total` cresce A,
  `M0 = money_total − depósitos` fica constante;
- principal e juro: `bank.deposits -= P`, `bank.loans -= P`, o caixa do tomador
  paga (`bank.cpp:776-804, 964-968`) → **destrói** depósito; juro vira patrimônio;
- dividendo mensal: `bank.deposits += payout` e credita o dono **sem debitar
  `bank.cash`** (`bank.cpp:663-699`) → **cria dinheiro** (achado ALTO do QA);
- `equity = cash + loans + arrears − bc_borrowing − deposits` (`bank.cpp:297-300`);
- `retained = equity − capital` (`bank.cpp:302`);
- `m0_total() = money_total() − Σ depósitos` (`sim_core.cpp:504-509`);
- por moeda, `M0 = circulação − depósitos`, `M1 = circulação − reservas`
  (`central_bank.cpp:386-391`), onde `circulação` é o caixa total atribuído à
  moeda (inclui o cofre dos bancos e o caixa criado pelo empréstimo).

**Fato medido no mundo real (365 d default, `G:/opengs_tmp/r1/merge_r1_on365.json`):**

| Métrica | Valor |
| --- | ---: |
| `banks.capital` | **0,0** |
| `banks.cash` | 2.382.539,78 |
| `banks.loans` | 89.906.543,57 |
| `banks.deposits` | 91.620.558,29 |
| `banks.equity` | 857.452,08 |
| `banks.m0` / `banks.m1` | 6.524.097.043 / 6.613.335.062 |
| `interest_income` / `dividends` / `retained` | 4.491.209 / 6.539.560 / 857.452 |
| eventos de crise bancária | **0** |
| `money_end` | 6.615.717.601,43 |

Ou seja: **não há capitalização no jogo hoje** (capital pago = 0), o patrimônio é o
juro acumulado (≈1% da carteira) e o dividendo anual (6,54 M) é **maior** que o
caixa do banco (2,38 M) — o código paga o que não tem emitindo depósito. É esse o
buraco que o R2a fecha.

### 1.2 Crédito — três canais, um credor só

| Canal | Regra atual | Credor | Onde |
| --- | --- | --- | --- |
| Empresa (grupo) | C3/D-107, ligado por D-111: `loan_rate = taxa básica + spread` (2 p.p. fixo); limite por alavancagem (1× PL) e cobertura de juros (2×); calote em 30 d de insolvência; repaga 50% do excedente/dia | estatal → banco público; demais → **sorteio por hash estável do id** contra as participações | `bank::lend`, `bank::lender` (`bank.cpp:334-357`) |
| Giro de crise (D-126) | 60 dias de custo, até 50% do ativo imobilizado; juro = banco + 2 p.p.; 1 renegociação (15 d de atraso, multa 5%); 60 d → bancarrota | **só banco público** | `bank::lend_city` (`bank.cpp:441-497`), `crisis::borrow_working_capital` |
| Investimento do cérebro (D-126/D-131) | `city_credit_limit = 0,5 × ativos − firm_debt − atrasos`, multiplicado pelo aperto do banco público | **só banco público** | `crisis::city_credit_limit` (`crisis.cpp:191-209`), `ai_private::ai_investment_credit` (`ai_private.cpp:275-287`) |
| Juro da safra (D-105) | caixa do tomador já debitado; credita o banco público; sem banco, **não cobra** | banco público | `bank::receive_harvest_interest` (`bank.cpp:632-644`) |

Consequências no desenho atual: (i) não há **preço por banco** — todos cobram a mesma
taxa; (ii) não há **capacidade por banco** — o limite é do tomador ou da cidade;
(iii) o banco público é o único credor de giro/safra/investimento, então ele não
compete: ele é o mercado; (iv) **o servicing assume o credor por hash**: `phase()`
recalcula `lender()` (`bank.cpp:752-757`), a cidade sempre cai no público
(`bank.cpp:934-945`) e o juro da safra só credita o público (`bank.cpp:632-644`).
Um `firm_debt_bank` singular não rastreia múltiplos empréstimos da mesma cidade.

### 1.3 Crise bancária (C6 / D-114) — o esqueleto do R2c

O que a C6 já entrega:

1. **indicadores** por banco/dia: capital/ativos, caixa/depósitos, NPL;
2. **confiança** do depositante (solvência, liquidez, perdas, contágio, bolsa);
3. **corrida** abaixo de `run_threshold` (8%/dia), com fuga para o banco público
   ou papel-moeda — **mas `run_cash` não entra no `money_total`**: a corrida
   "papel-moeda" **destrói** dinheiro (`bank_crisis.cpp:831-856`), ao contrário do
   que o plano rev1 dizia ("conserva");
4. **emprestador de última instância** (BC redesconta com haircut, cria M0 e dívida
   `bc_borrowing`); dolarizado/sem moeda própria **não tem** LoLR;
5. **aperto** abaixo do capital mínimo (0,1% do modelo) e **hierarquia de resolução**:
   acionistas → bail-in do depósito não segurado → fundo de seguro → Tesouro/dívida
   pública (com emissão registrada) → **incorporação pelo banco público**.

O que falta para a spec: **leilão de carteira com deságio** (hoje a incorporação é
automática e sem preço), **comprador de última instância como comprador do leilão**,
**cascata do banco público** (hoje ele é o socorrista, não o socorrido), **bail-in do
depositante** (hoje `charge_depositors` debita o **caixa do tomador** proporcional à
dívida, `bank_crisis.cpp:250-315`) e o **deserto bancário** como estado explícito.

### 1.4 Onde o dinheiro está e para onde flui

`SimCore::money_total()` (R1, ON) soma:

`transport_cash + foreign_cash + construction_cash + power_cash + Tesouro (por nação)`
`+ [por cidade: pop.savings + private_total() + capital_income_next + rent_income_next + region.cash]`
`+ group.cash + bank.cash + fundo de seguro + construction_projects.reserved + city_gov`.

- `private_total()` = `City.private_cash` (pool com `firm_sub` + `service_sub`).
- **Depósito não é dinheiro**: é passivo do banco e claim do depositante; o dinheiro
  real correspondente é `bank.cash` (ou papel-moeda em circulação). Por isso `M0`
  subtrai os depósitos.
- **Não existe hoje**: depósito de região, depósito de empresa, depósito do Tesouro,
  taxa por banco, capacidade por banco, adequação de capital, leilão de carteira.
- O Tesouro do R1b **não é depositante**: o saldo é dinheiro em `nation_treasury`
  (`treasury::balance`, `treasury.cpp:39-42`); o orçamento regional é **limite**, não
  carteira.

### 1.5 O que o R2 substitui ou absorve

| Hoje | R2 |
| --- | --- |
| `capital = 0` no mundo real; `set_bank_capital` só em teste | **R2a**: capitalização no dia 0 com dado real + política de adequação |
| dividendo 50% fixo do lucro retido, mensal | **R2a**: retém até a meta de adequação; distribui só o excedente em caixa |
| taxa única `base + 2 p.p.` para todos | **R2b**: spread por banco (custo de capital, liquidez, NPL, política dirigida) |
| `lender()` por hash; giro/investimento só no público | **R2b**: escolha por taxa+capacidade; público participa e pode ter desconto dirigido |
| depósitos só criados por empréstimo/dividendo | **R2b**: depósitos de funding vindos do pool regional e do caixa das empresas |
| resolução automática por incorporação | **R2c**: leilão de carteira com deságio → público comprador de última instância |
| banco público é o socorrista, nunca o socorrido | **R2c**: cascata Tesouro → dívida → imprimir/resolução → deserto |
| títulos públicos não existem como ativo de banco | gancho (`gov_bonds`, 0) para a fase política |

### 1.6 O que a rev2 mediu (e o que o rev1 supunha)

| Medida | Valor | Como |
| --- | ---: | --- |
| PIB agregado anualizado, 10 d | **25,09 bi XGS** | `city.gdp` é **diário** (`sim_core.cpp:1113`, `economy_sim.cpp:5703`); soma das 849 cidades ×365 |
| PIB agregado anualizado, 60 d | **24,01 bi XGS** | idem, no dia 60 (`G:/opengs_tmp/r2/gdp60.json`) |
| `money_end` no dia 60 | 6,556 bi | idem |
| PIB usado no rev1 ("6,6 bi") | **errado por ~3,7×** | o rev1 confundiu PIB com estoque de dinheiro |
| PIB anual por nação (dia 60, top) | CHN 3,40 bi; IND 2,24 bi; COD 1,20 bi; IDN 1,20 bi; USA 1,05 bi | `gdp_nations` do probe |
| Tesouro: arrecadado/ano e saldo final | 331,03 M / 4,69 M | `R1.md:790-792` (365 d) |
| Cesta de 6 meses (91 nações com `food_chain`) | **2,89 bi** | `food_spend_per_capita × população × 182,5` |

O probe está em `G:/opengs_tmp/r2/gdp10.json` e `gdp60.json`; o harness é o
`c1_world_ai_365.gd` copiado para `G:/opengs_tmp/r2_probe/` com uma soma de `gdp`
por cidade. **Nenhum código do repositório foi alterado.**

---

## 2. Contrato contábil (crítico 1)

### 2.1 O problema do rev1

O rev1 dizia: "sweep D → `private_cash−D`, `bank.cash+D`, `bank.deposits+D`, claim+D;
`money_total` constante, M0 cai, **M1 constante**". Com as fórmulas do código isso
**não fecha**:

- `m0_total()` subtrai **todos** os depósitos (`sim_core.cpp:504-509`); com o
  funding, M0 cai D — mas M0 não deveria mudar (o dinheiro só trocou de forma);
- `M1 = circulação − reservas` (`central_bank.cpp:390-391`); com o funding,
  `reservas +D` e o caixa público `−D` → **M1 cai D**, não fica constante;
- o claim D **não é enumerado** no `balance::ledger` (`balance.cpp:95-129`): a
  identidade até continua zero porque `monetary_base` funciona como **plug**
  contábil, mas o patrimônio das firmas fica subdeclarado em D e o M0 publicado
  fica errado. Sem o claim explícito, o gate "0 violações" passa **pelo motivo
  errado**.

### 2.2 Definições rev2 (uma para cada agregado)

| Símbolo | Definição | Onde entra |
| --- | --- | --- |
| `T` | `money_total()` — todo o caixa do mundo (inclui cofre dos bancos) | `sim_core.cpp:449-499` |
| `D_l` | `Bank::deposits` — depósito **criado por empréstimo** (dinheiro bancário) | `bank.cpp:428-433, 491-497` |
| `D_f` | `Bank::deposits_funding` — **claim do depositante** (região/empresa), lastreado por `bank.cash` | novo |
| `D_t` | `D_l + D_f` — base de depósitos para a C6 (confiança, seguro, corrida) | C6 |
| `M0` | **`T − Σ D_l`** — base monetária; funding **não** subtrai | substitui `sim_core.cpp:504-509` |
| `M1_legacy` | **`T − Σ bank.cash`** — dinheiro fora dos bancos (o `m1` publicado hoje) | `central_bank.cpp:391` |
| `broad` | **`M1_legacy + Σ D_f`** — dinheiro fora dos bancos + claims de funding; é o agregado que o sweep **conserva** | novo, publicado no status |

Por que `M1_legacy` cai e `broad` fica constante no sweep: o caixa que virou
reserva saiu do "dinheiro fora dos bancos" (−D_f) e o claim entrou (+D_f). O
`m1` atual não enxerga o claim; por isso o rev1 não podia prometer M1 constante.
O `m1` **publicado** continua sendo `M1_legacy` para não quebrar UI/save; o status
ganha `broad_money`, `deposits_funding` e `m0_loan_deposits`.

### 2.3 O ciclo completo no papel (cada agregado conferindo)

Cenário inicial: R=0, C=100, D_l=0, D_f=0, L=0, K=0, T=100, M0=100, M1=100,
broad=100. As linhas são **deltas**; a coluna "checagem" é a identidade que o gate
tem de reproduzir.

| # | Evento | R | C | D_l | D_f | L | K | T | M0 | M1 | broad | Checagem |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | Funding: região deposita 40 | +40 | −40 | 0 | +40 | 0 | 0 | 0 | 0 | −40 | **0** | claim +40 = passivo +40; broad conserva |
| 2 | Empréstimo 50 ao grupo | 0 | +50 | +50 | 0 | +50 | 0 | +50 | 0 | +50 | +50 | M0 constante; M1/broad +50 (dinheiro criado) |
| 2a | Pagamento 15 (folha/insumos) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | transferência dentro de C; nenhum agregado muda |
| 2b | Pagamento 15 via claim (mesmo banco) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | claim do pagador −15, do recebedor +15; D_f total igual |
| 3 | Juro 5 pago pelo tomador | 0 | −5 | −5 | 0 | 0 | +5 | −5 | 0 | −5 | −5 | depósito destruído; K +5 |
| 4 | Principal 20 | 0 | −20 | −20 | 0 | −20 | 0 | −20 | 0 | −20 | −20 | dinheiro bancário destruído |
| 5 | Calote/baixa 10 | 0 | 0 | 0 | 0 | −10 | −10 | 0 | 0 | 0 | 0 | perda absorvida pelo capital; depósito não some |
| 6 | LoLR +20 (antes do saque) | +20 | 0 | 0 | 0 | 0 | 0 | +20 | +20 | 0 | 0 | M0 +20; M1 não (reserva não é caixa público) |
| 7 | Saque 30 do funding | −30 | +30 | 0 | −30 | 0 | 0 | 0 | 0 | +30 | **0** | claim vira caixa; broad conserva |
| 8 | Bail-in 10 do funding | 0 | 0 | 0 | −10 | 0 | +10 | 0 | 0 | 0 | −10 | claim cortado; broad cai 10 (perda do depositante) |
| 9a | Tesouro recapitaliza 10 | +10 | −10 | 0 | 0 | 0 | +10 | 0 | 0 | −10 | −10 | caixa público vira reserva; M1/broad caem 10 |
| 9b | Banco paga 10 de depósitos | −10 | +10 | 0 | −10 | 0 | 0 | 0 | 0 | +10 | 0 | claim vira caixa; broad não muda; a perda fica no K |
| 10 | Válvula emite 5 (LoLR) | +5 | 0 | 0 | 0 | 0 | 0 | +5 | +5 | 0 | 0 | M0 +5; dívida `bc_borrowing` +5 |

Leituras que o R2 tem de publicar e testar:

- **M0** só muda com LoLR/emissão (linhas 6 e 10) e com a destruição de `D_l`
  (linhas 3–4 não mudam M0 porque T e D_l caem juntos);
- **broad** conserva no funding e no saque (linhas 1 e 7) e cai no bail-in
  (linha 8) e na recapitalização líquida (9a+9b) — a perda é do depositante ou do
  Tesouro, não dinheiro novo;
- **K** sobe com juro retido (3), cai com baixa (5), sobe com bail-in (8) e com a
  recapitalização (9a); a soma dos passivos do banco fecha com ativos em todas.

### 2.4 O que muda no `balance::ledger`

1. `Bank` ganha `deposits_funding`; `deposits` continua sendo **só** o criado por
   empréstimo (compatibilidade de save e de C6).
2. `Ledger` ganha:
   - `bank_deposits_loan` e `bank_deposits_funding` (separados);
   - `deposit_claims` (claim dos depositantes: cidade + grupo);
   - `monetary_base = money_total − monetary_claims − bank_deposits_loan`
     (não subtrai o funding).
3. `Sheet` ganha `deposit_claims` em `financial_assets()`; `firms()` soma os
   claims de cidade/grupo (o depositante do R2 é a região/empresa); `families()`
   **não** soma (poupança não é depositante no R2). `cash_total()` **não** muda
   (depósito não é caixa).
4. `banking` subtrai `bank_deposits_loan + bank_deposits_funding` (passivo total).
5. `financial_sum()` passa a fechar **estruturalmente** com o claim, não por plug.
   A conta (com o claim em firmas e o passivo em `banking`):
   `famílias+firmas+governo+exterior + banking + claims − gov_debt − M0 = 0`.
6. C6 passa a usar `D_t = deposits + deposits_funding` em confiança, liquidez,
   seguro, corrida e bail-in; **M0 usa só `deposits`**.

### 2.5 Dividendo: debita `bank.cash`, nunca cria dinheiro

Hoje `distribute_dividends` faz `bank.deposits += payout` e credita o dono sem
debitar caixa (`bank.cpp:663-699`). No R2a:

```
payout = min( max(0, profit − retain), max(0, bank.cash − liquidity_floor) )
bank.cash -= payout
dono: Tesouro -> ws.nation_treasury[nation] += payout (caixa real);
      famílias -> credit_households; exterior -> foreign_cash
bank.dividends += payout
```

- **público:** credita `nation_treasury` (caixa real), não `government_budget`
  (limite). Com `national_treasury` desligado, mantém o caminho legado para o OFF.
- **privado:** `credit_households` (como hoje), mas só o que o caixa permite.
- **estrangeiro:** `foreign_cash` (como hoje), com o mesmo limite.
- `liquidity_floor` = `min_reserve_ratio × D_t` (default declarado 10% no R2;
  0 no OFF).
- Nada de `bank.deposits += payout`. O lucro não distribuído fica em `K` (retenção).

Medido: o banco pagou 6,54 M/ano com 2,38 M de caixa. Com o floor de 10% de 91,6 M
(9,16 M), o payout de caixa seria **0** até o banco acumular reserva; a rev2 declara
isso e transforma o dividendo em "excedente de caixa", não em criação de depósito.

### 2.6 `run_cash` conserva

Hoje a corrida debita `bank.cash` e `bank.deposits`, mas o `to_cash` só entra em
`run_cash` (`bank_crisis.cpp:848-856`); `money_total` cai e o dinheiro "desaparece".
No R2b (claims) a corrida é **claim-based**:

```
para cada depositante (cidade/grupo), pro rata determinístico:
  saque = min(claim, bank.cash × share)
  claim -= saque; bank.deposits_funding -= saque; bank.cash -= saque
  private_cash/group.cash += saque
```

`run_cash` vira **métrica** (quanto saiu como papel-moeda), não uma conta de
dinheiro. Gate: `money_total` antes = depois; `ΔD_f = −saques`; `ΔC = +saques`.
Para o `D_l` (depósito de crédito, que hoje mora no caixa do tomador), o saque é
creditado ao tomador pro rata (proxy determinístico: ordem dos grupos/cidades do
banco); `ΔD_l = −saques` e `ΔC = +saques`. Sem claim (mundo antigo), o caminho
legado continua e o gate do OFF não muda.

### 2.7 Gates contábeis (bloqueantes)

- `financial_sum` = 0 **com** `deposit_claims` (não por plug);
- `cash_residual` = 0;
- `Σ deposit_claims == Σ deposits_funding` ao centavo;
- `M0 = T − Σ D_l` e `broad = M1 + Σ D_f` publicados e idênticos ao cálculo do
  ledger;
- sweep conserva `T`, `M0` e `broad`; `M1` cai exatamente `ΔD_f` (declarado);
- dividendo: `Δbank.cash = −payout`, `Δdono = +payout`, `T` constante,
  `Δdeposits = 0`;
- corrida: `T` constante; `ΔD_f = −saques`;
- bail-in: `D_f` → `ΔD_f = −corte`, `Δclaim = −corte`, `ΔK = +corte`, `T`
  constante; `D_l` → `ΔD_l = −corte`, `ΔC = −corte`, `ΔK = +corte`, `T`
  constante;
- recapitalização: `Δtreasury = −payout`, `Δbank.cash = +payout`, `T` constante;
  pagamento de depósito: `ΔD_f = −pago`, `ΔC = +pago`, `Δbank.cash = −pago`,
  `T` constante;
- LoLR/emissão: `ΔT = ΔM0 = Δbc_borrowing` (ou `emission`), `M1` constante.

---

## 3. Escala e capitalização (crítico 2)

### 3.1 O PIB do modelo é diário — anualizar ×365

`city.gdp` é o valor adicionado **do dia** (`sim_core.cpp:1113`;
`economy_sim.cpp:5703` publica `gdp_day`; `city_gov.cpp:373-374` usa `×30` para o
mês). Medido no harness:

| Horizonte | PIB/dia | PIB/ano (×365) | Cidades | `money_end` |
| --- | ---: | ---: | ---: | ---: |
| 10 d | 68.747.489 | **25,09 bi** | 849 | 6,524 bi |
| 60 d | 65.782.250 | **24,01 bi** | 849 | 6,556 bi |

O rev1 usava "PIB ≈ estoque de dinheiro (6,6 bi)" e errava por ~3,7×. O PIB anual
é ~3,7× o dinheiro do mundo — velocidade ~3,7, coerente para o modelo.

**Nominal vs PPP (correção do QA):** `capital_pct_gdp` é um **ratio unitless**
(capital nominal / PIB nominal). Aplicá-lo ao PIB do modelo (XGS, preços correntes)
é dimensionalmente válido; **não** se converte por `gdp_ppp_bn` nem por câmbio. O
PPP só entra se o dono quiser distribuir o total real por país; nesse caso, numerador
e denominador têm de ser **ambos nominais** (ou ambos PPP), e isso fica declarado.
A rev2 **não** usa `gdp_ppp_bn` para o nível do seed.

### 3.2 Dois alvos, não um

| Alvo | Fórmula | Valor medido | Papel |
| --- | --- | ---: | --- |
| **Alvo de referência** (dado real) | `capital_pct_gdp_i × PIB_anual_i` | **2,40 bi** (10% de 24,01 bi) | gap publicado (`reference_gap`); **não** é seed |
| **Alvo do modelo** (convergência) | `capital_to_loans_target × carteira_steady_i` | **9,0 M** (10% de 89,9 M) | alvo do seed e da retenção |

A referência real é 36,6% de todo o dinheiro do mundo (2,40/6,56) e 7,25× a
arrecadação anual do Tesouro (331 M); a parte pública (1,08 bi) é 3,26×. O alvo
do modelo é 0,14% do dinheiro. O R2
capitaliza para o alvo do modelo; a referência fica como **gap honesto** no status,
não como injeção.

### 3.3 Seed limitado ao dinheiro existente

`seed_target_i = model_target_i` (não o alvo real), alocado só aos papéis
`present` (§10.4), com as shares renormalizadas. O seed é **realocação**: debita o
dono e credita `bank.cash`/`bank.capital`; `money_total` antes = depois.

| Papel | Share do alvo (9,0 M) | Fonte | Medida | Seed viável |
| --- | ---: | --- | ---: | ---: |
| Público | 45% = 4,05 M | `nation_treasury` (via `treasury::spend`) | saldo 4,69 M; gasto/dia 0,894 M; piso 3 d = 2,68 M | **2,0 M** (piso 3 d) / 0 (piso 7 d) |
| Privado | 40% = 3,60 M | `pop.savings` (`pay_from_households`) | caixa das famílias 8,9 bi (365 d); piso social irrelevante a 0,04% | **3,6 M** |
| Estrangeiro | 15% = 1,35 M | `foreign_cash` | posição **−7,26 bi** | **0** |
| **Total** | 9,0 M | — | — | **5,6 M** (~62% do alvo) |

- `seed_scale_i = semeado_i / alvo_modelo_i` (publicado por banco e agregado).
- `reference_scale_i = capital_i / alvo_referência_i` (publicado; será ~0,2%).
- O seed público usa `treasury::spend` quando `national_treasury` está ON
  (`treasury.cpp:44-61`); **nunca** `government_budget` como caixa (o código atual
  faz isso em `bank.cpp:596` e em `pay_from_government`, `bank_crisis.cpp:81-84` —
  os dois têm de mudar no R2).
- Piso do Tesouro = `floor_days × gasto_diário` (declarado; default 3 d); o dono
  escolhe 0/3/7 d na P1. Com 3 d, o seed público é 2,0 M e o Tesouro fica com
  ~2,7 M (3 dias); com 7 d, o seed é 0. **Os valores de saldo/gasto acima são os
  do 365 d (referência); o gate do R2a mede o saldo do dia 0 e publica o cap.**
- Sem emissão para cobrir seed. Se o caixa do dono não basta, `seed_scale < 1` e o
  déficit fica publicado.

### 3.4 Convergência por retenção (o número que fecha)

Medido: `equity` 0,857 M; alvo do modelo 9,0 M; gap **8,13 M**; lucro anual antes
do dividendo = `dividends 6,54 M + retained 0,86 M` = **7,40 M**. Retendo 100%:

- sem seed: 8,13 / 7,40 = **~1,1 ano** até `equity/loans = 10%`;
- com seed público 2,0 M + privado 3,6 M (5,6 M): gap 2,5 M → **~0,4 ano**.

O seed acelera, mas **não é o mecanismo principal**: a retenção é. Isso torna a
decisão 1 da spec ("capitalizar no dia 0") executável na escala do modelo: o dia 0
semeia o que o dinheiro existente permite; a meta é atingida em ~1 ano.

### 3.5 Capitalização pública dentro do Tesouro real

- Receita anual 331,03 M; saldo final 4,69 M; gasto/dia ~0,894 M; saldo ≈ 5,2 dias.
- Alvo público do modelo 4,05 M = **1,2% da receita anual**, mas **86% do saldo**.
  É comportável no fluxo (0,4 M/mês de sobra), não no estoque sem um piso.
- Recomendação: seed público 2,0 M no dia 0 (piso 3 d) + 2,05 M por retenção em
  ~7 meses. Sem emissão, sem vender títulos sem comprador.
- O alvo de referência público (1,08 bi) é **3,26× a receita anual**; está fora do
  R2 por decisão, não por falta de dado — é o `reference_gap`.

### 3.6 Gates do R2a (escala)

- conservação do seed (`money_total` antes = depois; `Σ débitos = Σ créditos`);
- `seed_scale ∈ [0,1]`; `Σ seed = Σ bank.capital` (só o pago);
- `equity/loans ≥ 10%` em ≤24 meses no mundo ON, com 0 violações;
- `reference_gap` publicado por nação; **não** é gate;
- nenhum papel `none`/`no_data` capitalizado;
- seed público ≤ `max(0, treasury_balance − floor_days × daily_spend)`;
- dividendo: caixa debitado, `Δdeposits = 0`, `T` constante.

---

## 4. Sweep e liquidez (crítico 3)

### 4.1 Por que o rev1 reabria o R1a

`private_liquidity()` e `firm_liquidity()` leem **só** `private_cash`/`firm_sub`
(`sim_types.h:1902-1916`). A IA (`ai_private.cpp:275-287`), o giro de crise
(`crisis.cpp:191-209`), a comida e o `city_credit_limit` usam essa leitura. O rev1
sweepava o excesso sobre **5 dias de opex** e deixava o depósito ilíquido: a região
não podia pagar com ele, e o colchão `service_sub` que salvou o R1a (SOM/JAM/QAT,
132/179) era justamente o excesso sweepado. O QA está certo: **5 dias de opex não é
colchão e depósito ilíquido repete o R1a**.

### 4.2 Colchão protegido = cesta de 6 meses + orçamento do mês

```
protected_cash_i = 182,5 × food_cost_per_capita_i × população_i
                 + regional_budget_limit_month_i
sweep_i = max(0, private_cash_i − protected_cash_i)
```

- `food_cost_per_capita` = o gasto alimentar medido (`food_spend_per_capita`);
  `182,5` = 6 meses. Medido: **2,89 bi** para as 91 nações com `food_chain`
  (mundo completo a medir no R2b1); é ~44% do dinheiro do mundo (6,56 bi) — o
  floor é grande de propósito, e o sweep só pega o excesso, que pode ser pequeno.
- O `regional_budget_limit_month` é o limite do R1b (não caixa); somar o limite
  protege o mês de gasto regional.
- O sweep é do **excesso**; a divisão entre `firm` e `service` é proporcional aos
  sub-saldos, e o sub que estiver abaixo da sua fatia protegida **não** é sweepado.
  Isso preserva explicitamente o colchão de serviços do R1a.
- Se o floor dominar (sweep ~0), o R2b1 publica o ratio e o dono pode reduzir a
  cesta para 3 meses na P1; o default da spec é 6.

### 4.3 Depósitos contam na liquidez

- `firm_liquidity() = firm_sub + firm_deposit_claim` (claim no banco de
  relacionamento);
- `service_liquidity() = service_sub + service_deposit_claim`;
- `private_liquidity() = private_cash + Σ claims` (nunca conta o mesmo dinheiro
  duas vezes: `private_cash` é caixa, claim é depósito);
- `private_total()` continua **caixa** (não soma claim); o status ganha
  `private_wealth = private_cash + Σ claims`;
- `money_total` **não** soma claim (só `bank.cash`), como hoje.

O claim é "liquidez para decisão", não dinheiro: se o banco não tem caixa, o
pagamento espera/LoLR; o gate de comida mede o efeito.

### 4.4 Saque no mesmo dia quando o pagamento excede o float

Ordem determinística de pagamento (por sub-saldo):

1. debita o **float** (`firm_sub`/`service_sub`);
2. se faltar, **saque no mesmo dia** do claim no banco de relacionamento
   (`claim -= x`, `D_f -= x`, `bank.cash -= x`, `private_cash += x`) e paga;
3. se o banco não tiver caixa, o saque é limitado por `bank.cash × claim/total`
   (pro rata) e o resto **espera** (despesa adiada determinística) ou vai ao LoLR
   na crise;
4. o saque não pode deixar `bank.cash < liquidity_floor` sem LoLR; se deixar, o
   banco entra no sinal de liquidez da C6.

Isso é o "depósito líquido" da P3: não se roteia **todo** pagamento por banco
(fica para a fase política), mas o depósito **paga** quando o float não basta.

### 4.5 Gates do R2b1 (liquidez)

- A/B de 90 d: float 5 d **vs** floor de 6 meses + orçamento; comida ≤97/179;
  falências ≤3; drift <0,03; 0 violações;
- identidade `Σ claims = Σ D_f` ao centavo;
- sweep conserva `T`, `M0`, `broad`; `M1` cai `ΔD_f` (declarado);
- nenhum claim > `bank.cash` sem LoLR no dia;
- `private_liquidity` inclui claims e o gate de comida não piora vs R1a;
- round-trip v44.

---

## 5. Sub-fases, flags e gates

### 5.1 Bloco único `rework.banks` com sub-flags

```json
"banks": {
  "enabled": true,
  "capital": true,
  "deposits": true,
  "resolution": true,
  "dividend": { "target_ratio": 0.10, "cash_floor_ratio": 0.10 },
  "seed": { "enabled": true, "target": "model_loans", "floor_days_treasury": 3.0 },
  "deposits_alloc": {
    "min_capital_ratio": 0.08, "pendulum_month": 0.10,
    "weights": { "rel": 0.50, "sol": 0.35, "pend": 0.15 }
  },
  "liquidity": { "min_reserve_ratio": 0.10, "protected_food_months": 6.0 },
  "auction": { "discount_private": 0.30, "discount_public": 0.45 },
  "charter": { "max_pct_gdp": 0.005, "max_treasury_months": 3.0 },
  "gov_bonds": { "enabled": false }
}
```

Regras de dependência (config inválida = tratada como desligada, com motivo no
status, como no R1):

- `capital` e `deposits` exigem `enabled` e `rework.regional_cash.enabled`;
- `deposits` exige `capital` (sem capital não há solidez para alocar);
- `resolution` exige `enabled` (funciona sem depósitos, mas o efeito pleno é com eles);
- `seed` exige `capital`; `deposits_alloc` exige `deposits`;
- `banks.enabled = false` → **OFF bit-a-bit** em tudo: nenhum campo novo entra no save,
  nenhuma soma muda, `money_total`, `m0`, `allocate_money` e o digest idênticos ao
  baseline da fase anterior (§6).

Por que um bloco e não três flags soltas: o default do jogo é **um** (bancos do
rework ligados), como o R1; as sub-flags existem para A/B de medição e para isolar
o risco de cada fase, não para virar três features. O `set_rework_params` recebe o
bloco inteiro e o `get_status` publica `banks_config_error` (só quando há erro).

### 5.2 R2a — Capitalização, adequação e dividendo

**Objetivo:** os bancos nascem capitalizados com dado real **até o limite do
dinheiro existente** e passam a se comportar como capitalizados (retenção por meta,
dividendo só do caixa excedente).

Escopo técnico:

1. `data/definitions/economy/bank_seed.json` (novo): por nação, `capital_pct_gdp`,
   `capital_to_assets`, `assets_pct_gdp`, `public_share`/`private_share`/
   `foreign_share`, `model_gdp_annual`, `steady_loans`, `reference_target`,
   `year`, `source`, `confidence`. O **nível** usa o PIB do modelo (harness);
   o ratio real é unitless. Builder/validator em `tools/`.
2. `bank::seed_capital(WorldState&)` no fim de `initialize_world`, **só em mundo
   novo**; `load` de save v43 **não** semeia (§7.7). Aloca só papéis `present`;
   debita Tesouro/famílias/exterior; limita pelo caixa e pelos pisos; publica
   `seed_scale`, `reference_scale`, `reference_gap`.
3. Adequação: `capital_ratio = equity / max(loans, eps)`; `target_ratio = 0.10`
   (carteira); `paid_in_ratio = capital / loans` publicado; o aperto da C6 lê
   `equity/loans` contra a meta e mantém o piso `capital.min_ratio` (0,1% dos
   ativos). **Não** usar `capital/loans` (capital pago é 0 no mundo real).
4. Dividendo mensal: `retain = max(0, target × loans − equity)`; `payout =
   min(max(0, profit − retain), max(0, bank.cash − liquidity_floor))`; debita
   `bank.cash` e credita o dono (Tesouro real/famílias/exterior); nunca
   `bank.deposits +=`.
5. **Transição:** o aperto da C6 continua lendo o piso atual (0,1% dos ativos) no
   R2a; a meta de 10% governa o dividendo/retenção e o `capacity` do R2b2, com
   rampa de 24 meses (`target_eff = min(target, current + (target − current) ×
   day/730)`). Não se corta crédito de 90% no dia 1: o banco nasce em ~0,95% de
   `equity/loans`, e o corte imediato pela meta derrubaria a comida.

Invariantes/save: capital pago é transferência (soma zero); `equity` continua a
definição C4; save v44 aditivo (`bank_seed` só no ON; `capital_ratio_target` por
banco; `deposits_funding`); OFF não ganha chave.

Gates R2a: 10/90/365 d ON e OFF; OFF = baseline da fase anterior (§6); ON dentro
de comida ≤97/179, falências ≤3, drift <0,03, 0 violações; conservação do capital
semeado (`money_total` antes = depois); `seed_scale` ∈ [0,1]; `equity/loans ≥ 10%`
em ≤24 meses; 0 papéis `none/no_data` capitalizados; dividendo conserva caixa.

Risco principal: **escala** (resolvida por dois alvos: referência 2,4 bi fica como
gap; modelo 9 M é o alvo). O seed público é limitado pelo piso do Tesouro (3 d
recomendado; 7 d zera o seed), e o resto vem de retenção (~1,1 ano).

### 5.3 R2b — Depósitos, alocação e competição

**R2b1 — funding (estoque).** As regiões e as empresas passam a manter depósito:

- sweep diário do **excesso** sobre `protected_cash` (§4.2), por sub-saldo
  (`firm_sub`/`service_sub` separados, para não quebrar "quem zerou, para");
- o depósito sai de `private_cash` e entra em `bank.cash`, com
  `bank.deposits_funding += D`; `money_total` constante, `M0` constante,
  `M1` cai, `broad` constante (§2.3);
- empresas: o excesso do `group.cash` acima do float vai a depósito no banco de
  relacionamento (o mesmo `lender()` do grupo), com `Group::bank_deposit`;
- alocação entre bancos: `score = w_rel × relacionamento + w_sol × solidez
  (equity/loans) + w_pend × pêndulo` (0,50/0,35/0,15, ±10%/mês), normalizado;
  sem juros de depósito;
- pagamentos **não** são roteados por banco no b1: o float paga; se faltar, o
  saque do claim no mesmo dia (§4.4) — o depósito é **líquido**, não reserva
  intocável. Roteamento total fica para a fase política (P3);
- C6 passa a usar `D_t` (não só `deposits`).

Gates R2b1: OFF = R2a-ON (§6); ON com A/B obrigatório do floor (6 meses vs 5 d);
comida ≤97/179; falências ≤3; drift <0,03; 0 violações; identidade
`Σ claims = Σ D_f`; `T`/`M0`/`broad` conservados; M1 cai `ΔD_f`; round-trip v44.

Riscos: (a) o sweep enxuga liquidez e reabre a falha de comida do R1a — mitigação:
floor de 6 meses + orçamento, sweep só do excesso, saque no mesmo dia, A/B de 90 d;
(b) alocação não determinística — vetores ordenados, nunca `unordered_map`;
(c) dupla contagem em `money_total`/`allocate_money` (lição do t169) — claims fora
do caixa.

**R2b2 — competição no ativo (preço e capacidade).**

- `bank::offer_rate(bank) = base_rate + spread_banco` (**aditivo**, como o
  `base + 2 p.p.` de hoje; não multiplicativo), com
  `spread_banco = spread_declarado × f(meta de capital, liquidez, NPL, política)`;
  público pode ter desconto dirigido declarado (política; número no JSON);
- `bank::capacity(bank) = min(capital_headroom, liquidity_headroom)`:
  - `capital_headroom = max(0, equity/target − loans)`;
  - `liquidity_headroom = max(0, bank.cash/min_reserve − D_t)` (com
    `min_reserve = 0`, sem restrição — o default do OFF);
  - **nunca `+`**: o menor dos dois limita;
- tomador (grupo/cidade/safra) escolhe a **menor taxa com capacidade**, com
  desempate determinístico (ordem do save); se nenhum banco tem capacidade, o
  pedido é recusado com motivo — não se cria banco;
- `lend_city` deixa de exigir o público: qualquer banco da nação pode conceder;
  `City::firm_debt` vira **claims por credor** (§8), para o par banco↔tomador
  continuar batendo e a C6 cobrar o credor certo.

Gates R2b2: OFF = R2b1-ON; ON com A/B obrigatório do **float**; comida ≤97/179;
falências ≤3; drift <0,03; 0 violações; nenhum empréstimo sem credor/capacidade;
taxa por banco medida; servicing (juro/amortização/calote) bate com o claim do
credor; round-trip v44.

Riscos: (a) competição sem servicing gera dívida órfã — mitigação: claims por
credor antes de ligar o b2; (b) taxa aditiva muda o `base+2 p.p.` se o spread
declarado for 0 — declarar `spread_banco=0` reproduz o hoje; (c) public directed
discount vira monopólio — desconto declarado e limitado; capacidade/taxa medidas
por banco no status.

### 5.4 R2c — Resolução, leilão e cascata

1. **Banco privado insolvente**: acionistas zerados; carteira (loans + arrears)
   **leiloada** com deságio declarado, em ordem determinística de compradores
   (maior `equity` primeiro; depois os demais; o público por último, com deságio
   maior). O comprador **paga em caixa** pelo book; o banco falido usa o caixa
   para pagar os depositantes; **não** há transferência de depósitos para o
   comprador (o comprador herda o ativo e o par tomador↔banco, não o passivo).
2. **Ordem da resolução** (cada degrau medido):
   (i) **leilão** dos ativos; (ii) **bail-in** do depósito não segurado — D_f
   corta o claim; D_l corta o caixa do tomador (proxy do depósito de crédito,
   `bank_crisis.cpp:250-315`), pro rata e limitado ao caixa; (iii) **fundo** de
   seguro até o saldo; (iv) **Tesouro** com caixa real (`treasury::spend` quando
   `national_treasury` ON; `pay_from_government` só no OFF), com austeridade nos
   orçamentos regionais; (v) **válvula** ≤1% M0/dia (`monetary.cpp:249-285`) como
   **LoLR** (dívida `bc_borrowing`), **não** como capital; (vi) **charter** público
   na crise com teto **ou** **deserto** (`bank_desert`), §11 P2.
3. **Banco público insolvente (cascata)**, nesta ordem e cada degrau medido:
   (i) **Tesouro recapitaliza** até o saldo real, com a austeridade regional
   declarada; (ii) **dívida pública** só se houver **comprador** (BC controlado
   ou exterior); `rescue_with_debt` **não** é dívida "de mercado" — ele emite via
   BC (`bank_crisis.cpp:96-130`), então (ii) e (iii) são a mesma operação se o
   comprador for o BC; (iii) **imprimir controlado** pela válvula ≤1% M0/dia
   **ou** resolução sem socorro; (iv) **deserto bancário**: nação sem banco vivo →
   sem giro/safra/investimento; empresas hibernam, famílias voltam ao colchão;
   possível default soberano; estado explícito (`bank_desert`), não silêncio.
4. **Ganchos**: `Bank::gov_bonds` (0 no R2) e o comprador de títulos na fase
   política; `insured_share` continua default C6 (60%); a decisão "garantia vs
   calote" fica registrada como pendência política. `treasury_debt_rescue` passa a
   **false** no R2c por default (no OFF/C6 legado continua true); `gov_bonds` só
   é emitido com comprador e dentro da válvula.

Gates R2c: testes unitários de resolução (carteira conservada, depositantes pagos,
rombo = soma das partes, nenhum dinheiro criado fora do LoLR/emissão registrada);
cenários forçados de falha do público (austeridade → dívida → resolução → deserto)
com conservação; 365 d ON sem evento de crise (o baseline tem 0) e com choque
forçado; `run_cash` conserva; OFF = R2b2-ON.

Risco principal: o deserto bancário derruba a comida por ausência de giro — é
efeito **de design** (spec §1.5), mas precisa de baseline próprio documentado e de
um piso de emergência declarado (charter com teto) para não virar mundo morto sem
saída.

---

## 6. Baseline OFF por sub-fase

O rev1 mandava comparar **toda** fase com `6287b12d…` (OFF pré-R2). Isso é
impossível: R2a-ON muda o mundo por design. A regra rev2: **o OFF de cada fase é o
ON da fase anterior, bit a bit**; o `6287b12d…` só vale no combo tudo-Off.

| Combo | `regional_cash` | `banks.enabled` | `capital` | `deposits` | `resolution` | Referência |
| --- | --- | --- | --- | --- | --- | --- |
| R1-OFF legado | false | false | false | false | false | `6287b12d…` |
| R1-ON (pré-R2) | true | false | false | false | false | `81eccf2a…` |
| R2a-ON | true | true | true | false | false | `R2a-<sha 10/90/365>` |
| R2b1-ON | true | true | true | true | false | `R2b1-<sha>` |
| R2b2-ON | true | true | true | true | false | `R2b2-<sha>` |
| R2c-ON | true | true | true | true | true | `R2c-<sha>` |
| Tudo OFF | false | false | false | false | false | `6287b12d…` |

- `banks.enabled=false` com R1 ON **tem** de reproduzir `81eccf2a…` (não o legado).
- `capital=false` com `enabled=true` **tem** de reproduzir o baseline anterior
  (R1-ON), porque o seed não roda.
- `deposits=false` com `capital=true` **tem** de reproduzir R2a-ON.
- `resolution=false` mantém o resolvedor C6 **legado** (o de hoje); `true` liga o
  leilão/cascata. Assim R2a/R2b comparam com o C6 de sempre.
- Cada `<sha>` novo é medido no fecho da fase e gravado no `GAME_STATE.md`; o plano
  não inventa SHA.

---

## 7. Modelo de dados

### 7.1 Granularidade

**Banco por nação** (3 papéis), não por região. Motivos: a moeda, o BC e a taxa
básica já são por nação; o save e o determinismo ficam no tamanho atual (~3 × 179
linhas); a spec fala "3 bancos/nação". A **região é depositante**, não banco: o
vínculo é uma conta (claim) por cidade/grupo → banco. Bancos regionais/cooperativas
ficam para a fase política.

### 7.2 `Bank` — campos novos (aditivos)

| Campo | Tipo | Regra |
| --- | --- | --- |
| `deposits` | double | **só** depósito criado por empréstimo (compatível com o save/C6) |
| `deposits_funding` | double | claim do depositante (região/empresa); `D_t = deposits + deposits_funding` |
| `capital_ratio_target` | double | meta de adequação (dado real; default declarado 0,10 da carteira) |
| `spread_eff` | double | spread efetivo do banco (derivado, publicado; não é knob do jogador) |
| `capacity` | double | espaço de crédito = `min(capital_headroom, liquidity_headroom)` (derivado/dia) |
| `seed_scale` | double | fração do alvo **do modelo** efetivamente semeada (1,0 = integral) |
| `reference_scale` | double | fração do alvo **de referência** (dado real) já capitalizada |
| `gov_bonds` | double | gancho da fase política; 0 no R2 |
| `charter` | enum | `declared` / `chartered_in_crisis` / `none` (deserto) / `no_data` |

`equity` continua derivado (`cash + loans + arrears − bc_borrowing − D_t`), não
campo salvo. `capital` continua o capital **pago** (claim do dono). A adequação usa
`equity/loans`; o `capital/loans` é publicado como `paid_in_ratio`.

### 7.3 `City` — contas bancárias (claims)

```cpp
struct BankAccount {           // ordenado por bank_id (determinismo)
    std::string bank_id;
    double firm = 0.0;         // depósito do sub-saldo firmas
    double service = 0.0;      // depósito do sub-saldo serviços
};
std::vector<BankAccount> bank_accounts;  // City
double protected_cash = 0.0;             // floor calculado (§4.2), só leitura
```

Invariantes:

- `firm_sub + service_sub == private_cash` (R1 preservado; `private_cash` é
  **caixa**, não riqueza total);
- `Σ bank_accounts.firm == firm_deposit_total`, idem serviços;
- riqueza privada = `private_cash + Σ depósitos` (`private_wealth`, novo agregado);
- `money_total` **não** soma depósitos (claim), só `bank.cash`;
- `protected_cash` não é saldo: é o floor do sweep, recalculado todo dia.

### 7.4 `Group` e dívida por credor

- `double bank_deposit = 0.0; std::string deposit_bank_id;`
- dívida por credor: `std::vector<BankLoan> debt_claims` com
  `{ bank_id, principal, arrears, rate_add, default_days }`; `Group::debt` e
  `Group::credit_arrears` continuam como agregados (compatibilidade), mas o
  servicing itera `debt_claims`. `City` ganha o mesmo `firm_debt_claims`.
- Migração de save v43: a dívida existente é atribuída ao credor que o `lender()`
  **de hoje** escolheria (hash para grupos, público para cidade) — determinístico,
  documentado; não se inventa credor.

### 7.5 `WorldState` — política

```cpp
struct BanksRework {
    bool enabled = false, capital = false, deposits = false, resolution = false;
    double dividend_target_ratio = 0.10;
    double dividend_cash_floor_ratio = 0.10;
    bool seed_enabled = true;
    double seed_floor_days_treasury = 3.0;
    double deposits_float_days = 5.0;       // legado do rev1; não é o floor
    double protected_food_months = 6.0;
    double alloc_min_capital_ratio = 0.08;
    double alloc_pendulum_month = 0.10;
    double alloc_w_rel = 0.50, alloc_w_sol = 0.35, alloc_w_pend = 0.15;
    double liquidity_min_reserve_ratio = 0.10;
    double auction_discount_private = 0.30;
    double auction_discount_public = 0.45;
    double public_directed_discount = 0.0;
    double charter_max_pct_gdp = 0.005;
    double charter_max_treasury_months = 3.0;
    bool gov_bonds_enabled = false;
};
```

### 7.6 Regras dinâmicas

1. **Depósito (b1)**: `protected = 182,5 × food_cost_pc × pop + budget_month`;
   `excess = max(0, private_cash − protected)`; sweep diário proporcional aos
   sub-saldos; retorno automático se o caixa do dia ficar abaixo do float/saque.
2. **Alocação**: mensal, com pêndulo ±10% e normalização; score declarado
   (0,50/0,35/0,15).
3. **Taxa**: `offer_rate = base + spread_banco + prêmio_de_crise` (D-126 mantém
   `+2 p.p.` sobre a oferta do banco escolhido); sem taxa negativa.
4. **Capacidade**: `capacity = min(capital_headroom, liquidity_headroom)`, com
   `liquidity_headroom = max(0, cash/min_reserve − D_t)`; com `min_reserve = 0`,
   `liquidity_headroom = ∞` (sem restrição, como o OFF); `reserve_share` não
   existe como knob separado (o `min_reserve` é o knob).
5. **Dividendo**: mensal, pela meta; `payout` só do caixa acima do floor; nada
   distribui abaixo da meta.
6. **Resolução**: FSM `solvent → undercapitalized → insolvent → auction →
   resolved | chartered | desert`; cada transição datada e registrada em
   `BankCrisisEvent` (o anel já existe).
7. **Servicing**: `phase()` itera `debt_claims` (não `lender()`); juro/amortização/
   calote creditam/debitam o **credor do claim**; o juro da safra vai ao banco que
   financiou a safra (`receive_harvest_interest(nation, bank_id)`).

### 7.7 Save e migração

- `kSaveVersion` 43 → **44** (aditivo). OFF: só `version` muda (como v42→v43).
- ON: `banks` (política, `seed_scale`, `reference_scale`), `capital_ratio_target`
  por banco, `deposits_funding`, `bank_accounts` por cidade, `bank_deposit`/
  `deposit_bank_id` por grupo, `debt_claims` por grupo/cidade, `charter`.
- **Load de save v43 com a flag ON: sem capitalização retroativa.** `seed_scale = 0`
  declarado; o banco converge por retenção (a `equity` já existente conta para a
  meta). Não há "seed no 1º load": a decisão é **mundo novo apenas**. Se o dono
  quiser o contrário, é uma decisão explícita (seed retroativo documentado, com
  débito do dono no dia do load) — a rev2 **não** faz.
- Round-trip bit-exato por fase, como no R1.

---

## 8. Servicing por banco (ALTO do QA)

O rev1 prometia "qualquer banco concede" sem desenhar quem **recebe** o juro, a
amortização e o calote. O código atual assume o público:

- `lender()` por hash para grupos (`bank.cpp:334-357`);
- `phase()` recalcula `lender()` a cada dia (`bank.cpp:752-757`);
- cidade sempre no público (`bank.cpp:934-945`);
- safra só no público (`bank.cpp:632-644`);
- `charge_depositors`/`apply_loss` só olham grupos.

Desenho rev2:

1. **Originação**: `lender()` continua escolhendo o banco **na concessão** (hash +
   share, ou o escolhido por taxa/capacidade no b2); o empréstimo grava
   `bank_id` no claim do tomador.
2. **Servicing**: `phase()` deixa de recalcular `lender()`; itera `debt_claims`,
   cobra juro/amortização do caixa do tomador e credita **o credor do claim**;
   atraso/calote reduzem o claim e o passivo do tomador; a baixa (`apply_loss`)
   reduz o ativo do credor.
3. **Cidade**: `lend_city(city_id, bank_id)` e `City::firm_debt_claims`; o
   `city_credit_limit` continua sendo o limite do tomador (não do banco), mas o
   desembolso e o servicing vão ao banco escolhido.
4. **Safra**: o envelope de crédito da safra grava `bank_id` (o banco que
   financiou); `receive_harvest_interest(nation, bank_id)` credita esse banco; sem
   banco, não cobra (como hoje).
5. **C6**: `charge_depositors`, `apply_loss` e a resolução usam `debt_claims` e
   `D_t`, não o hash. O leilão move o claim (ativo) e o vínculo tomador↔banco
   juntos; o passivo (depósito) fica com o banco falido e é pago pela ordem do §5.4.
6. **Migração**: save v43 sem claims → cada dívida existente é atribuída ao credor
   que o `lender()` de hoje escolheria; determinístico e documentado. O juro da
   safra existente fica no público (não há histórico de credor).

Gate: 0 dívidas sem claim; `Σ claims por banco = loans do banco`; juro/amortização
creditam exatamente o credor; nenhum `lender()` chamado no servicing (teste de
regressão: um grupo com dois empréstimos de bancos diferentes paga cada um ao seu).

---

## 9. Resolução, leilão e cascata (detalhe)

### 9.1 Leilão

- **Ativo vendido**: `loans + arrears` do banco falido, por tranche ou book inteiro.
- **Preço**: `book × (1 − deságio)`; deságio privado 30%, público 45% (defaults
  declarados; o dono pode vetar).
- **Pagamento**: caixa do comprador (`bank.cash`), não depósito; o banco falido
  recebe o caixa e paga depositantes até o saldo.
- **Compradores**: ordem determinística por `equity` decrescente; o público por
  último; sem comprador, o book é baixado (write-off) e o banco liquida.
- **O comprador não herda o passivo**: recebe o ativo e o vínculo tomador↔banco;
  os depositantes são pagos pelo banco falido (leilão + bail-in + fundo + Tesouro).
  Isso evita "incorporação com patrimônio negativo" e dá preço ao deságio.

### 9.2 Bail-in

- Alvo = `uninsured_share × D_t`, com `uninsured_share = 1 − insured_share` (40%).
- Split pro rata: `D_f` corta **claims** (cidade/grupo; sem tocar em caixa);
  `D_l` corta o **caixa do tomador** (proxy do depósito de crédito; limitado ao
  caixa e ao claim).
- O corte reduz `D_t` e a contraparte: `D_f` corta o **claim** (sem caixa);
  `D_l` corta o **caixa do tomador** (proxy do depósito de crédito). Em ambos,
  `K` sobe e `T` não muda. Se o caixa do tomador não cobre, o resto vai ao
  fundo/Tesouro (não se inventa caixa).
  - `D_f`: `ΔD_f = −cut`, `Δclaim = −cut`, `ΔK = +cut`;
  - `D_l`: `ΔD_l = −cut`, `ΔC = −cut`, `ΔK = +cut`.

### 9.3 Fundo e Tesouro

- Fundo: 459.951 medidos contra 91,6 M de depósitos; cobre <1%. O `insured_share`
  é **teto declarado**, não lastro; o rombo vai ao Tesouro/válvula.
- Tesouro: `treasury::spend` quando `national_treasury` ON; `pay_from_government`
  só no OFF. A austeridade reduz `regional_budget_limit_month` por regra declarada.
- Válvula: `kControlledEmissionShareOfM0 = 0.01`/dia (`monetary.h:61`), usada em
  `monetary.cpp:249-285`; a resolução tem de chamar a **mesma** função/teto, não
  `rescue_with_debt` sem limite. O que passar do teto fica como perda/resolução, não
  emissão.

### 9.4 `gov_bonds` / dívida

- `rescue_with_debt` (`bank_crisis.cpp:96-130`) faz `state->emission += amount`,
  `state->securities += amount` e `ws.gov_bonds[nation] += amount`: **é monetização
  com o título na carteira do BC**, não dívida de mercado. Com
  `treasury_debt_rescue=true` (default C6), (ii) e (iii) são a mesma operação.
- No R2c, `treasury_debt_rescue=false` por default; a dívida só existe se houver
  comprador **não-BC** (exterior) ou se o dono ligar a política; `gov_bonds` do
  banco (`Bank::gov_bonds`) continua 0 (gancho da fase política).
- O título do BC continua entrando em `Ledger.monetary_claims` e `government_debt`
  (identidade fecha); a diferença é a **decisão** e o teto.

### 9.5 Charter e deserto

- Nação sem banco público real (`none`/`no_data`): leilão primeiro aos privados
  (doméstico/estrangeiro presente); sem comprador, o Estado pode **charter** um
  banco na crise, com teto `min(max_pct_gdp × PIB, max_treasury_months × receita
  mensal)`; se o custo estourar o teto, **deserto** (`bank_desert`).
- Deserto: sem giro/safra/investimento; empresas hibernam; famílias no colchão;
  possível default soberano; status explícito e baseline próprio.

---

## 10. Dados reais

### 10.1 Necessidades

| # | Dado | Para quê | Cobertura esperada | Fallback |
| --- | --- | --- | --- | --- |
| 1 | **Ativos bancários / PIB** por país | alvo de referência (gap) | ~180 (GFDD) | faixa declarada por grupo de renda |
| 2 | **Capital bancário / PIB** ou **capital / ativos** | referência e meta de adequação | menor (FSI/`FB.BNK.CAPA.ZS`); medir no build | `capital_pct_gdp = assets_pct_gdp × 0,10` (declarado) |
| 3 | **Mix público / privado / estrangeiro** | dividir o alvo entre papéis `present` | ~100–120 | `banks.json` (presença) + 0,45/0,40/0,15 declarado |
| 4 | *(opcional)* **spread de empréstimo** | calibrar a competição | ~150 (`FR.INR.LNDP`) | spread 2 p.p. (atual, declarado) |
| 5 | *(gancho)* **cobertura do seguro** | `insured_share` | IADI/WB | 0,60 (C6, declarado) |
| 6 | **PIB do modelo** | denominador do nível | harness (10/60/365 d) | sem harness = sem seed (`no_model_gdp`) |

### 10.2 Fontes sugeridas (citáveis)

- **World Bank — Global Financial Development Database (GFDD)**, Beck,
  Demirgüç-Kunt & Levine: *Private credit by deposit money banks to GDP*
  (`GFDD.DI.01`) e *Deposit money banks' assets to GDP* (`GFDD.DI.02`).
- **World Bank WDI**: *Domestic credit to private sector by banks (% of GDP)*
  (`FS.AST.PRVT.GD.ZS`) e *Bank capital to assets ratio (%)* (`FB.BNK.CAPA.ZS`).
- **BIS** — *Credit-to-GDP gaps / total credit statistics*.
- **IMF FSI** — *Regulatory capital to risk-weighted assets* e *Capital to assets*.
- **Mix público/privado**: GFDD e World Bank *Bank Regulation and Supervision
  Survey*; Cull, Martínez Pería & Verrier (2015, WB Policy Research WP 7435).
- **Basileia (BCBS)** — 8% mínimo + buffers; o alvo de 10% da carteira é
  **default declarado**, não dado observado.

### 10.3 Conversão para XGS (corrigida)

1. rodar o harness (10 d é o mínimo; 60 d para convergência) e ler `gdp` por
   cidade; somar por nação; **anualizar ×365** (o `gdp` é diário);
2. `PIB_anual_i` é o **nível** em XGS; `capital_pct_gdp_i` é unitless → aplica-se
   direto: `reference_target_i = capital_pct_gdp_i × PIB_anual_i`;
3. **não** usar `gdp_ppp_bn` no nível; se o dono quiser share real, usar
   numerador e denominador **nominais** (ou ambos PPP) e declarar;
4. `model_target_i = capital_to_loans_target × steady_loans_i` (baseline 365 d por
   nação);
5. `seed_i = min(model_target_i, caixa_disponível_do_dono_i)` com os pisos;
   `seed_scale_i = seed_i / model_target_i`;
6. sem harness, o seed **não roda** e o status declara `bank_seed: "no_model_gdp"`.
7. O `reference_gap` usa o ratio real sobre o PIB do modelo; a distribuição entre
   países segue o **PIB do modelo**, não o PPP. É uma referência de ordem de
   grandeza, não uma promessa de convergência — o alvo que o R2 persegue é o do
   modelo (§3.2).

### 10.4 Fallback: só papel `present`

- `public/private/foreign.status == "present"` → papel existe e pode ser
  capitalizado.
- `none` → **não existe**: `capital = 0`, `charter = "none"`, `seed_scale = 0`,
  sem fallback de renda.
- `no_data` → **não se sabe**: `capital = 0`, `charter = "no_data"`, `seed_scale = 0`;
  o status mostra a ausência; **não** se capitaliza por palpite.
- O fallback por grupo de renda só preenche campo **nulo** de um papel `present`;
  nunca sobrescreve valor `complete`/`partial`/`estimated` e nunca cria papel
  `none`/`no_data`. Contagem: público 52 sem instituição (36+16), privado 39
  (12+27), estrangeiro 60 (11+49) — esses **não** recebem capital no dia 0.

### 10.5 Arquivo proposto (`bank_seed.json`)

```json
{
  "source": "World Bank GFDD/WDI + BIS; ver docs/economy/rework/R2_dados.md",
  "year": 2024,
  "model_gdp_source": "harness c1_world_ai_365 (10/60 d), anualizado x365",
  "nations": {
    "BRA": {
      "assets_pct_gdp": null,
      "capital_pct_gdp": null,
      "capital_to_assets": null,
      "public_share": null,
      "private_share": null,
      "foreign_share": null,
      "model_gdp_annual": null,
      "steady_loans": null,
      "reference_target": null,
      "model_target": null,
      "confidence": "complete|partial|estimated|no_data",
      "source_key": "gfdd_di02|wdi_fb_bnk_capa|declared_default",
      "roles_present": ["public", "private", "foreign"]
    }
  }
}
```

Campos `null` são preenchidos pelo build ou ficam `no_data`; nenhum valor é
digitado à mão. `roles_present` vem de `banks.json` e é a única lista que o seed
pode capitalizar.

### 10.6 Fallback paramétrico (declarado, não dado)

Só para papel `present` sem valor publicado:

| Grupo | `assets_pct_gdp` | `capital_to_assets` |
| --- | --- | --- |
| Alta renda | 1,00 | 0,10 |
| Renda média-alta | 0,70 | 0,10 |
| Renda média-baixa | 0,45 | 0,10 |
| Baixa renda | 0,30 | 0,10 |

Esses números são **ponto de partida de balanceamento**, não medição; o relatório
de dados lista quantas nações usaram cada linha e o dono pode vetar. O `no_data`
continua visível no status.

---

## 11. Decisões do dono (P1/P2/P3)

As três pendências do rev1 viram decisões com recomendação do QA e alternativas.
Nenhuma delas é pré-requisito para o R2a; P1 entra no R2a, P2 no R2c, P3 no R2b1.

### P1 — Escala do seed e piso social

**Recomendação (QA):** seed limitado ao caixa disponível, com piso que protege a
cesta de 6 meses e o orçamento regional do mês; publicar `seed_scale` e a
convergência por retenção; **nunca** emitir para cobrir seed.

**Números:** alvo do modelo 9,0 M; caixa dos donos permite 5,6 M (público 2,0 M
com piso de 3 d; privado 3,6 M; estrangeiro 0 por `foreign_cash` negativo); o resto
(3,4 M) vem de retenção em ~0,5 ano. Alvo de referência 2,40 bi = 36,6% do dinheiro
e 7,25× a receita do Tesouro (a parte pública, 1,08 bi, é 3,26×) → **não** é seed;
é `reference_gap`.

**Alternativas:**
- A) **Sem seed** (só retenção): 0,857 M de equity → 9,0 M em ~1,1 ano; zero
  risco de distorção; bancos começam sem capital pago.
- B) **Seed integral do alvo de referência** (2,40 bi): rejeitado — exigiria
  emissão ou expropriar 36,6% do dinheiro do mundo.
- C) **Seed público com piso de 7 d**: 0; Tesouro fica intacto; com o seed privado
  de 3,6 M a convergência é ~0,7 ano; sem ele, ~1,1 ano.
- D) **Seed público com piso de 3 d** (recomendado): 2,0 M; convergência ~0,4 ano.

**Decisão do dono:** _pendente_ (A/B/C/D).

### P2 — Nações sem banco público real (52 papéis)

**Recomendação (QA):** leilão primeiro aos privados; sem comprador, `charter`
público na crise com teto de custo e cascata; deserto só se estourar o teto; **não**
capitalizar papel `none` no dia 0.

**Números:** 36 `none` + 16 `no_data` no público; 11+49 no estrangeiro; 12+27 no
privado. O fundo cobre <1% dos depósitos; o comprador de última instância é o menos
capitalizado. Teto sugerido: `min(0,5% do PIB, 3 meses de receita)`; medir o custo
do charter no cenário forçado.

**Alternativas:**
- A) **Sempre deserto** (sem charter): barato, mas comida/giro caem e o mundo pode
  morrer sem saída.
- B) **Sempre charter**: fiscalmente arriscado; pode virar banco público de fato.
- C) **Leilão privado → charter com teto → deserto** (recomendado).
- D) **Comprador estrangeiro** quando `foreign.status == present`; vantagem: sem
  Tesouro; risco: remessa/controle externo.

**Decisão do dono:** _pendente_ (A/B/C/D).

### P3 — Roteamento de pagamento por banco

**Recomendação (QA):** adiar o roteamento, **mas** tornar o depósito líquido:
incluir em `private_liquidity()`/`firm_liquidity()` e permitir saque no mesmo dia
quando o pagamento exceder o float; senão o b1 repete o R1a.

**Alternativas:**
- A) **Adiar o roteamento** (recomendado): float + saque no mesmo dia; menor risco,
  menor realismo.
- B) **Rotear todo pagamento** por banco já no b1: mais realista, mas mexe em todos
  os primitivos de pagamento e no determinismo; fica para a fase política.

**Decisão do dono:** _pendente_ (A/B).

---

## 12. Riscos, esforço e artefatos

### 12.1 Riscos (rev2)

| Risco | Prob. | Impacto | Mitigação |
| --- | --- | --- | --- |
| Contrato contábil errado (M0/M1/broad, claim fora do ledger) | alta | alto | §2; gate `financial_sum=0` com claim; `M0` só `D_l`; broad publicado |
| Escala do seed (alvo real 2,4 bi) | alta | alto | dois alvos; seed 5,6 M; `reference_gap`; retenção ~1 ano |
| Seed público sem caixa no Tesouro | alta | médio | piso 3 d → 2,0 M; 7 d → 0; `treasury::spend`; nunca emissão |
| Sweep enxuga liquidez e reabre R1a | alta | alto | floor 6 meses + orçamento; sweep só do excesso; saque no mesmo dia; A/B 90 d |
| Bail-in cobra o tomador, não o depositante | alta | alto | D_f corta claim; D_l corta caixa do tomador (proxy); ordem definida |
| Dividendo cria dinheiro | alta | alto | debita `bank.cash`; floor de liquidez; `Δdeposits=0` |
| Servicing por hash (dívida órfã) | alta | alto | claims por credor; `phase()` sem `lender()`; safra com `bank_id` |
| Baseline OFF errado por fase | média | alto | tabela §6; OFF = ON anterior; SHA medido no fecho |
| Cascata duplica C6 / dívida = emissão | alta | alto | um FSM; `treasury_debt_rescue=false` no R2c; válvula ≤1% M0 |
| `run_cash` destrói dinheiro | média | alto | corrida claim-based; `T` constante; gate |
| Load v43 contraditório | média | médio | seed só em mundo novo; `seed_scale=0`; retenção |
| Fallback capitaliza `none/no_data` | média | médio | só `present`; contagem no status |
| Dupla contagem em `money_total`/`allocate_money` (t169) | média | alto | claims fora do caixa; residual zero |
| Não determinismo na alocação/resolução | média | médio | vetores ordenados; desempate por id |
| R2c duplicar a C6 (dois resolvedores) | alta | médio | estender `bank_crisis::resolve`; um FSM |
| Acoplamento com R3 (`foreign_cash`) | média | médio | seed estrangeiro 0 quando `foreign_cash<0`; contrato provisório |

### 12.2 Esforço estimado

Dias de desenvolvimento (agente + build + testes unitários), sem espera de gate de
365 d:

| Sub-fase | Dados | Núcleo | Testes | Gates/medição | Total |
| --- | ---: | ---: | ---: | ---: | ---: |
| R2a — contrato, capital, adequação, dividendo | 2–3 | 3–4 | 1,5 | 1–1,5 | **7,5–10** |
| R2b1 — depósitos (estoque/alocação/liquidez) | 0 | 3–4 | 1,5 | 1,5–2 | **6–7,5** |
| R2b2 — competição (taxa/capacidade/servicing) | 0,5 | 3–4 | 1,5 | 1 | **6–7** |
| R2c — leilão, bail-in, cascata, deserto | 0 | 4–6 | 2 | 2–2,5 | **8–10,5** |
| **Total** | 2,5–3,5 | 13–18 | 6,5 | 5,5–7 | **27,5–35** |

Caminho crítico: **contrato contábil → R2a → R2b1 (gate do floor) → R2b2
(servicing) → R2c**. O desenho do R2c pode andar em paralelo ao R2b2; a
implementação do leilão depende dos claims por credor.

### 12.3 Artefatos e definição de pronto

- `G:/opengs_tmp/qa_r2plano.md` (QA t185, gravado na t186);
- `G:/opengs_tmp/r2/gdp10.json` e `gdp60.json` (PIB medido);
- doc de fase (`R2a.md`, `R2b.md`, `R2c.md`) com números medidos e limites honestos;
- sonda `--r2-diag` só leitura: `deposits`/`deposits_funding`, `M0`/`M1`/broad,
  claims por banco, float/floor por cidade, taxa/capacidade/servicing por banco,
  leilões e rombo por parte;
- artefatos de 10/90/365 d em `G:/opengs_tmp/r2/`;
- `sim_unit_tests` com os casos de conservação por fase; `-Tier pr -Base main`;
- `GAME_STATE.md`: D-xxx do R2 com flags, save v44, baselines ON/OFF por fase e as
  decisões P1/P2/P3;
- QA independente (`qa-docs`) antes do merge; **sem merge/push nesta unidade**.

---

## 13. Pendências declaradas (para o dono, não rediscutidas)

1. **P1** — escala do seed e piso social (A/B/C/D acima).
2. **P2** — nações sem banco público real (A/B/C/D acima).
3. **P3** — roteamento de pagamento por banco (A/B acima).
4. **Referência real** — o alvo de 10% do PIB (2,40 bi) fica como `reference_gap`;
   se o dono quiser persegui-lo, é uma fase posterior (aprofundamento financeiro),
   não o R2.
5. **Títulos** — `Bank::gov_bonds` é gancho; a dívida pública de mercado exige
   comprador não-BC e fica para a fase política.
