# R2 — BANCOS: plano de implementação

> Branch `rework/r2-bancos`, worktree `G:/projetos/opengs-wt-r2`, base `main` `dbf5e73d`
> (merge do R1). Spec: `REWORK_spec_mestre.md` §1.5 e §12 (R2), no repo
> `opengs-icons-private`. **Status: plano. Nenhuma linha de código, nenhum número de
> mundo muda nesta entrega.**
>
> Base herdada do R1 (default ON desde `97e869df`): caixa único regional com
> sub-saldos `firm_sub`/`service_sub`, Tesouro Nacional + orçamento regional
> (limite, não carteira), empresas operando 100% do caixa próprio. Save v43.
> Baselines pós-merge: default/ON `81eccf2a…`, legado OFF `6287b12d…`.

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
- Emissão de M0 além da válvula existente (≤1% M0, regime "controlado").

---

## 1. Inventário do que existe hoje

### 1.1 Bancos (C4 / D-110)

Até **três bancos por nação**, por papel:

| Papel | `role` | Dono (`owner_kind`) | Existe? |
| --- | --- | --- | --- |
| Público | 0 | Estado/Tesouro (`state`) | linha sempre criada por `ensure()`; o dado decide se é instituição real |
| Privado nacional | 1 | famílias da nação (`national`) | idem |
| Estrangeiro | 2 | nação de origem (`foreign`) | só quando `banks.json` declara presença |

`data/definitions/economy/banks.json` (cobertura 179): público **127 present /
36 none / 16 no_data**; privado **140 / 12 / 27**; estrangeiro **119 / 11 / 49**.
Sem estrangeiro, o papel é fundido no privado conservando valores. **Importante:**
`apply_definitions` cria a linha do banco público e do privado para toda nação mesmo
quando o dado diz `none` — a linha existe zerada (`name_status = "no_data"`), não é
uma instituição capitalizada.

Campos hoje (`Bank`): `loans`, `arrears`, `deposits`, `bc_borrowing`, `cash`,
`capital`, `interest_income/expense`, `writeoffs`, `dividends`, `remittance`,
`harvest_interest`, `defaults`, `loans_count`, mais o bloco C6
(`confidence`, `run_days`, `resolved`, `resolution`, `absorbed_into`,
`capital_flight`, `insured_payout`, `bailin_cut`, `treasury_rescue`).

Matemática monetária (C4, `docs/economy/C4_banks.md`):

- empréstimo: `bank.loans += A`, `bank.deposits += A`, tomador recebe caixa →
  **cria M1** (depósito), não M0;
- principal e juro: `bank.deposits -= P`, `bank.loans -= P` → **destrói** dinheiro
  bancário; juro vira patrimônio;
- dividendo mensal: credita o dono **criando depósito** (`bank.deposits += payout`);
- `equity = cash + loans + arrears − bc_borrowing − deposits`;
- `m0_total() = money_total() − Σ depósitos`; M1 = `money_total`.

**Fato medido no mundo real (365 d default, `G:/opengs_tmp/r1/merge_r1_on365.json`):**

| Métrica | Valor |
| --- | ---: |
| `banks.capital` | **0,0** |
| `banks.cash` | 2.382.539,78 |
| `banks.loans` | 89.906.543,57 |
| `banks.deposits` | 91.620.558,29 |
| `banks.equity` | 857.452,08 |
| `banks.m0` / `banks.m1` | 6.524.097.043 / 6.613.335.062 |
| eventos de crise bancária | **0** |

Ou seja: **não há capitalização no jogo hoje**. Os bancos vivem do juro da safra e
do crédito de giro; o patrimônio é o juro acumulado (≈1% da carteira). O `capital`
só é injetado em teste (`set_bank_capital`). É esse o buraco que o R2a fecha.

### 1.2 Crédito — três canais, um credor só

| Canal | Regra atual | Credor | Onde |
| --- | --- | --- | --- |
| Empresa (grupo) | C3/D-107, ligado por D-111: `loan_rate = taxa básica + spread` (2 p.p. fixo); limite por alavancagem (1× PL) e cobertura de juros (2×); calote em 30 d de insolvência; repaga 50% do excedente/dia | estatal → banco público; demais → **sorteio por hash estável do id** contra as participações | `bank::lend`, `bank::lender` |
| Giro de crise (D-126) | 60 dias de custo, até 50% do ativo imobilizado; juro = banco + 2 p.p.; 1 renegociação (15 d de atraso, multa 5%); 60 d → bancarrota | **só banco público** | `bank::lend_city`, `crisis::borrow_working_capital` |
| Investimento do cérebro (D-126/D-131) | `city_credit_limit = 0,5 × ativos − firm_debt − atrasos`, multiplicado pelo aperto do banco público | **só banco público** | `crisis::city_credit_limit`, `ai_private::ai_investment_credit`, desembolso em `crisis::borrow_city` |
| Juro da safra (D-105) | caixa do tomador já debitado; credita o banco público; sem banco, **não cobra** | banco público | `bank::receive_harvest_interest` |

Consequências no desenho atual: (i) não há **preço por banco** — todos cobram a mesma
taxa; (ii) não há **capacidade por banco** — o limite é do tomador ou da cidade;
(iii) o banco público é o único credor de giro/safra/investimento, então ele não
compete: ele é o mercado.

### 1.3 Crise bancária (C6 / D-114) — já existe e é o esqueleto do R2c

O que a C6 já entrega:

1. **indicadores** por banco/dia: capital/ativos, caixa/depósitos, NPL;
2. **confiança** do depositante (solvência, liquidez, perdas, contágio, bolsa);
3. **corrida** abaixo de `run_threshold` (8%/dia), com fuga para o banco público
   ou remessa ao exterior — tudo conservando dinheiro;
4. **emprestador de última instância** (BC redesconta com haircut, cria M0 e dívida
   `bc_borrowing`); dolarizado/sem moeda própria **não tem** LoLR;
5. **aperto** abaixo do capital mínimo (0,1% do modelo) e **hierarquia de resolução**:
   acionistas → bail-in do depósito não segurado → fundo de seguro → Tesouro/dívida
   pública (com emissão registrada) → **incorporação pelo banco público**.

O que falta para a spec: o R2 pede **leilão de carteira com deságio** (hoje a
incorporação é automática e sem preço), **comprador de última instância como
comprador do leilão**, **cascata do banco público** (hoje ele é o socorrista, não o
socorrido) e o **deserto bancário** como estado explícito.

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
- Fluxo relevante para o R2: a região paga fornecedores/folha/importação do pool;
  a empresa paga a operação do `group.cash`; o Tesouro arrecada e gasta por
  reconciliação; as famílias recebem folha/dividendos e guardam em `pop.savings`.

### 1.5 O que o R2 substitui ou absorve

| Hoje | R2 |
| --- | --- |
| `capital = 0` no mundo real; `set_bank_capital` só em teste | **R2a**: capitalização no dia 0 com dado real + política de adequação |
| dividendo 50% fixo do lucro retido, mensal | **R2a**: retém até a meta de adequação; distribui só o excedente |
| taxa única `base + 2 p.p.` para todos | **R2b**: spread por banco (custo de capital, liquidez, NPL, política dirigida) |
| `lender()` por hash; giro/investimento só no público | **R2b**: escolha por taxa+capacidade; público participa e pode ter desconto dirigido |
| depósitos só criados por empréstimo/dividendo | **R2b**: depósitos de funding vindos do pool regional e do caixa das empresas |
| resolução automática por incorporação | **R2c**: leilão de carteira com deságio → público comprador de última instância |
| banco público é o socorrista, nunca o socorrido | **R2c**: cascata Tesouro → dívida → imprimir/resolução → deserto |
| títulos públicos não existem como ativo de banco | gancho (`gov_bonds`, 0) para a fase política |

---

## 2. Sub-fases, flags e gates

### 2.1 Recomendação de flag: **bloco único `rework.banks` com sub-flags**

Uma chave na raiz do `rework.json`, no mesmo padrão do R1 (`regional_cash`,
`national_treasury`, `firms_own_cash`), com o guarda-chuva e três sub-flags de
medição:

```json
"banks": {
  "enabled": true,
  "capital": true,
  "deposits": true,
  "resolution": true,
  "dividend": { "target_ratio": 0.10, "cap_share": 1.0 },
  "deposits_alloc": { "min_capital_ratio": 0.08, "pendulum_month": 0.10 },
  "auction": { "discount_private": 0.30, "discount_public": 0.45 }
}
```

Regras de dependência (config inválida = tratada como desligada, com motivo no
status, como no R1):

- `capital` e `deposits` exigem `enabled` e `rework.regional_cash.enabled`;
- `deposits` exige `capital` (sem capital não há solidez para alocar);
- `resolution` exige `enabled` (funciona sem depósitos, mas o efeito pleno é com eles);
- `banks.enabled = false` → **OFF bit-a-bit** em tudo: nenhum campo novo entra no save,
  nenhuma soma muda, `money_total`, `m0`, `allocate_money` e o digest idênticos ao
  baseline `6287b12d…` (OFF) / sem chave nova.

Por que um bloco e não três flags soltas: o default do jogo é **um** (bancos do
rework ligados), como o R1; as sub-flags existem para A/B de medição e para isolar
o risco de cada fase, não para virar três features. O `set_rework_params` recebe o
bloco inteiro e o `get_status` publica `banks_config_error` (só quando há erro).

### 2.2 R2a — Capitalização, adequação e dividendo

**Objetivo:** os bancos nascem capitalizados com dado real e passam a se comportar
como capitalizados (retenção por meta, não 50% fixo).

Escopo técnico:

1. `data/definitions/economy/bank_seed.json` (novo): por nação, `assets_pct_gdp`,
   `capital_pct_gdp` (ou `capital_to_assets`), `public_share`/`private_share`/
   `foreign_share`, `year`, `source`, `confidence`. Builder/validator em `tools/`.
   `assets_pct_gdp` entra como **alvo publicado/medido**, não força a carteira no
   dia 0: a carteira cresce por crédito e retenção, e o status publica a distância
   até o alvo (`assets_target_gap`).
2. `bank::seed_capital(WorldState&)` chamado no fim de `initialize_world` (e no
   primeiro load de save antigo com a flag ON): injeta `capital` por papel,
   debitando o dono real — **Tesouro** (público), **estratos de renda** (privado,
   `credit_households`) e **`foreign_cash`** (estrangeiro). Nunca cria dinheiro:
   `money_total` é constante; se o dono não tem caixa, a injeção é limitada e o
   déficit fica registrado (`seeded_share < 1`), como no `inject_capital` atual.
3. Política de adequação: `Bank::capital_ratio_target` (do `bank_seed`; default
   declarado 10% da carteira) e `Bank::capital_ratio` = `capital / loans` (risco
   1,0; o rating da moradia entra no R7).
4. Dividendo mensal: `retain = max(0, target × loans − capital)`; paga
   `min(lucro_retido, excedente)`; nunca deixa o banco abaixo da meta no momento do
   pagamento. Público → Tesouro; privado → estrato mais alto; estrangeiro → remessa
   (como hoje).
5. Aperto de crédito da C6 passa a ler a meta do R2a (não o piso 0,1% do modelo):
   abaixo da meta, corta crédito novo proporcionalmente até `capital_cut_floor`.

Invariantes/save: capital pago é transferência (soma zero); `equity` continua a
definição C4; save v44 aditivo (`bank_seed` só no ON; `Bank::capital_ratio_target`
por banco). OFF não ganha chave.

Gates R2a: 10/90/365 d ON e OFF; OFF = `6287b12d…` bit a bit; ON dentro de comida
≤97/179, falências ≤3, drift <0,03, 0 violações; conservação do capital injetado
(`money_total` antes = depois); 0 bancos com `seeded_share` fora de [0,1] sem motivo
declarado.

Risco principal: **escala**. O PIB do modelo é anualizado por nação e **o agregado
não é publicado no artefato** — a amostra do 365 d traz USA ≈ 898 M e CHN ≈ 2,03 bi
XGS (`G:/opengs_tmp/r1/merge_r1_on365.json`); publicar o agregado é a primeira
tarefa do R2a. Se ele ficar na ordem do estoque de dinheiro (`money_end` 6,62 bi),
capital de 10% do PIB significa ~0,6–0,8 bi XGS saindo de Tesouro/famílias/exterior
— ~10% do dinheiro do mundo. Números reais de `capital_pct_gdp` variam muito (5–20%);
a injeção tem de ser limitada pelo caixa do dono e o déficit declarado, **nunca
coberto por emissão**.

### 2.3 R2b — Depósitos, alocação e competição

Dividida em dois lotes para isolar o risco de liquidez:

**R2b1 — funding (estoque).** As regiões e as empresas passam a manter depósito:

- sweep diário do **excesso** sobre um float operacional declarado (ex.: N dias de
  opex), por sub-saldo (`firm_sub` e `service_sub` separados, para não quebrar a
  semântica "quem zerou, para");
- o depósito sai de `private_cash` e entra em `bank.cash`, com
  `bank.deposits_funding += D`; `money_total` constante, M0 cai, M1 constante;
- empresas: o excesso do `group.cash` acima do float vai a depósito no banco de
  relacionamento (o mesmo `lender()` do grupo), com `Group::bank_deposit`;
- alocação entre bancos: `score = w_rel × relacionamento + w_sol × solidez
  (capital/ativos) + w_pend × pêndulo (inércia mensal, ±10%)`, normalizado; sem
  juros de depósito, portanto sem competição por taxa no passivo;
- pagamentos **não** são roteados por banco no b1: o float paga; o depósito é
  reserva. Roteamento de pagamento por conta bancária fica para um lote opcional
  depois do gate do b1 (ou para a fase política), não para o b2.

**R2b2 — competição no ativo (preço e capacidade).**

- `bank::offer_rate(bank) = base_rate × (1 + spread_banco)`, com
  `spread_banco = spread_declarado × f(meta de capital, liquidez, NPL, política)`;
  público pode ter desconto dirigido declarado (política; número no JSON);
- `bank::capacity(bank)` = espaço até a meta de capital e até a reserva mínima;
- tomador (grupo/cidade/safra) escolhe a **menor taxa com capacidade**, com
  desempate determinístico (ordem do save); se nenhum banco tem capacidade, o
  pedido é recusado com motivo — não se cria banco;
- `lend_city` deixa de exigir o público: qualquer banco da nação pode conceder;
  `City::firm_debt` ganha o credor (`firm_debt_bank`) para o par banco↔tomador
  continuar batendo (a C6 usa esse par na resolução).

Gates R2b: OFF bit a bit; ON com A/B obrigatório do **float** (o R1a mostrou que
pool sem liquidez derruba a comida: 132/179); comida ≤97/179; falências ≤3; drift
<0,03; 0 violações; identidade `Σ depósitos de funding = Σ City/Group claims` ao
centavo; `M1` e `M0` com as definições antigas; round-trip v44.

Riscos principais: (a) o sweep enxuga a liquidez e reabre a falha de comida do R1a
— mitigação: float generoso, sweep só do excesso, A/B de 90 d antes de 365;
(b) alocação não determinística — mitigação: vetores ordenados, nunca `unordered_map`;
(c) dupla contagem em `money_total`/`allocate_money` (lição do t169).

### 2.4 R2c — Resolução, leilão e cascata

1. **Banco privado insolvente**: acionistas zerados; carteira (loans + arrears)
   leiloada com deságio declarado, em ordem determinística de compradores (maior
   índice de capital primeiro); o público compra o que sobrar, a deságio maior;
   depositantes recebem até a parcela segurada (fundo/Tesouro); o rombo residual
   segue a hierarquia C6, mas com preço de leilão no lugar da incorporação cega.
   O comprador herda o par tomador↔banco (dívida e `firm_debt_bank` migram juntos).
2. **Banco público insolvente (cascata)**, nesta ordem e cada degrau medido:
   (i) **Tesouro recapitaliza** até o saldo; a austeridade é real: reduz a dotação
   mensal das regiões (`regional_budget_limit_month`) por uma regra declarada;
   (ii) **dívida pública**: emite títulos (juro sobe via a regra existente);
   (iii) **imprimir controlado** (válvula ≤1% M0, regime "controlado") **ou**
   **resolução sem socorro**;
   (iv) **deserto bancário**: nação sem banco vivo → sem giro/safra/investimento;
   empresas hibernam, famílias voltam ao colchão; possível default soberano.
   O estado é explícito no status (`bank_desert`), não um silêncio.
3. **Ganchos**: `Bank::gov_bonds` (0 no R2) e o comprador de títulos na fase
   política; `insured_share` continua default C6; a decisão "garantia vs calote"
   fica registrada como pendência política.

Gates R2c: testes unitários de resolução (carteira conservada, depositantes pagos,
rombo = soma das partes, nenhum dinheiro criado fora do LoLR/emissão registrada);
cenários forçados de falha do público (austeridade → dívida → resolução → deserto)
com conservação; 365 d ON sem evento de crise (o baseline tem 0) e com choque
forçado; OFF bit a bit.

Risco principal: o deserto bancário derruba a comida por ausência de giro — é
efeito **de design** (spec §1.5), mas precisa de baseline próprio documentado e de
um piso de emergência declarado (ex.: Estado pode charter um banco na crise, ato
político) para não virar mundo morto sem saída.

### 2.5 Quadro de gates

| Fase | OFF (10/90/365 d) | ON — metas | Save |
| --- | --- | --- | --- |
| R2a | `6287b12d…` bit a bit; sem chave nova | comida ≤97, falências ≤3, drift <0,03, 0 violações, capital conservado | v44 aditivo, só ON |
| R2b1 | idem | idem + identidade dos depósitos; A/B do float em 90 d | v44 (claims por cidade/grupo) |
| R2b2 | idem | idem + nenhum empréstimo sem credor/capacidade; taxa por banco medida | v44 (`firm_debt_bank`) |
| R2c | idem | resolução conserva; cenário forçado; baseline do deserto | v44 (estado da resolução já existe; + leilão) |

Regra herdada: qualquer fase que mude o mundo por design define **novo baseline
documentado**; o OFF nunca muda.

---

## 3. Modelo de dados

### 3.1 Granularidade

**Banco por nação** (3 papéis), não por região. Motivos: a moeda, o BC e a taxa
básica já são por nação; o save e o determinismo ficam no tamanho atual (~3 × 179
linhas); a spec fala "3 bancos/nação". A **região é depositante**, não banco: o
vínculo é uma conta (claim) por cidade/grupo → banco. Bancos regionais/cooperativas
ficam para a fase política.

### 3.2 `Bank` — campos novos (aditivos)

| Campo | Tipo | Regra |
| --- | --- | --- |
| `capital_ratio_target` | double | meta de adequação (dado real; default declarado 0,10) |
| `deposits_funding` | double | depósitos de caixa (região/empresa); `deposits >= deposits_funding` |
| `spread_eff` | double | spread efetivo do banco (derivado, publicado; não é knob do jogador) |
| `capacity` | double | espaço de crédito até a meta + reserva (derivado/dia) |
| `seed_scale` | double | fração do capital-alvo efetivamente semeada (1,0 = integral) |
| `gov_bonds` | double | gancho da fase política; 0 no R2 |
| `charter` | enum | `declared` / `chartered_in_crisis` / `none` (deserto) |

### 3.3 `City` — contas bancárias (claims)

```cpp
struct BankAccount {           // ordenado por bank_id (determinismo)
    std::string bank_id;
    double firm = 0.0;         // depósito do sub-saldo firmas
    double service = 0.0;      // depósito do sub-saldo serviços
};
std::vector<BankAccount> bank_accounts;  // City
double cash_float = 0.0;                 // parte de private_cash mantida em caixa
```

Invariantes:

- `firm_sub + service_sub == private_cash` (R1 preservado; `private_cash` passa a ser
  **caixa**, não riqueza total);
- `Σ bank_accounts.firm == firm_deposit_total`, idem serviços;
- riqueza privada = `private_cash + Σ depósitos` (novo agregado de status);
- `money_total` **não** soma depósitos (claim), só `bank.cash`.

### 3.4 `Group` — depósito de empresa

`double bank_deposit = 0.0; std::string deposit_bank_id;` e, para o par da C6,
`std::string debt_bank_id` em `Group` e `City` (`firm_debt_bank`).

### 3.5 `WorldState` — política

```cpp
struct BanksRework {
    bool enabled = false, capital = false, deposits = false, resolution = false;
    double dividend_target_ratio = 0.10;
    double deposits_float_days = 5.0;      // default declarado
    double alloc_min_capital_ratio = 0.08;
    double alloc_pendulum_month = 0.10;
    double auction_discount_private = 0.30;
    double auction_discount_public = 0.45;
    double public_directed_discount = 0.0; // política; 0 = sem desconto
    // ganchos: bonds_enabled = false
};
```

### 3.6 Regras dinâmicas

1. **Depósito (b1)**: `excess = max(0, sub − float_days × opex_diário)`; sweep
   diário; retorno automático se o caixa do dia ficar abaixo do float (withdrawal
   do depósito, baixando `bank.cash` — o banco pode ficar sem reserva e então
   depende do LoLR C6, que já existe).
2. **Alocação**: mensal, com pêndulo ±10% e normalização; score declarado.
3. **Taxa**: `offer_rate = base × (1 + spread_banco) + prêmio_de_crise` (D-126
   mantém `+2 p.p.` sobre a oferta do banco escolhido); sem taxa negativa.
4. **Capacidade**: `capacity = max(0, capital/target − loans) × reserva_share` +
   limite de liquidez `cash/deposits ≥ min` (hoje 0; passa a ser consumido no R2b
   com valor declarado, ex. 10%).
5. **Dividendo**: mensal, na data atual (`day % 30`), pela meta; o excedente
   distribui; nada distribui abaixo da meta.
6. **Resolução**: FSM `solvent → undercapitalized → insolvent → auction →
   resolved | absorbed | desert`; cada transição datada e registrada em
   `BankCrisisEvent` (o anel já existe).

### 3.7 Save e migração

- `kSaveVersion` 43 → **44** (aditivo). OFF: só `version` muda (como o v42→v43).
- ON: `banks` (política e `seed_scale`), `capital_ratio_target` por banco,
  `deposits_funding`, `bank_accounts` por cidade, `bank_deposit`/`deposit_bank_id`
  por grupo, `debt_bank_id`.
- Load de save v43 com a flag ON: **sem capitalização retroativa** (não se inventa
  capital no meio do mundo); `seed_scale = 0` declarado e o banco cresce por
  retenção. Load de save OFF: nada muda.
- Round-trip bit-exato por fase, como no R1.

---

## 4. Dados reais

### 4.1 Necessidades

| # | Dado | Para quê | Cobertura esperada | Fallback |
| --- | --- | --- | --- | --- |
| 1 | **Ativos bancários / PIB** por país | tamanho-alvo do sistema (e o alvo de convergência da carteira) | ~180 (GFDD) | faixa declarada por grupo de renda |
| 2 | **Capital bancário / PIB** ou **capital / ativos** | capitalização do dia 0 e meta de adequação | menor que a do item 1 (FSI/`FB.BNK.CAPA.ZS`); medir no build | `capital_pct_gdp = assets_pct_gdp × 0,10` (declarado) |
| 3 | **Mix público / privado / estrangeiro** (share de ativos) | dividir o capital entre os 3 papéis | ~100–120 | `banks.json` (presença) + 0,45/0,40/0,15 declarado |
| 4 | *(opcional)* **taxa de empréstimo / spread** | calibrar a competição do R2b2 | ~150 (`FR.INR.LNDP`; IFS) | spread 2 p.p. (atual, declarado) |
| 5 | *(gancho, R2c/fase política)* **cobertura do seguro de depósito** | `insured_share` | IADI/WB Deposit Insurance Database | 0,60 (C6, declarado) |
| 6 | **PIB nominal / PIB do modelo** | converter %PIB para XGS | 179 (`nations.csv gdp_ppp_bn`) + medida do harness | sem conversão = sem seed (`no_data`) |

### 4.2 Fontes sugeridas (citáveis)

- **World Bank — Global Financial Development Database (GFDD)**, Beck, Demirgüç-Kunt
  & Levine: *Private credit by deposit money banks to GDP* (`GFDD.DI.01`) e
  *Deposit money banks' assets to GDP* (`GFDD.DI.02`). Confirmar o código no catálogo
  do GFDD antes do build; a base cobre ~180 economias.
- **World Bank WDI**: *Domestic credit to private sector by banks (% of GDP)*
  (`FS.AST.PRVT.GD.ZS`) e *Bank capital to assets ratio (%)* (`FB.BNK.CAPA.ZS`).
- **BIS** — *Credit-to-GDP gaps / total credit statistics* (crédito ao setor
  não financeiro), para checar a ordem de grandeza dos ativos.
- **IMF Financial Soundness Indicators (FSI)** — *Regulatory capital to
  risk-weighted assets* e *Capital to assets* (checagem do item 2).
- **Mix público/privado**: GFDD (participação de bancos estatais nos ativos) e
  World Bank *Bank Regulation and Supervision Survey*; literatura de referência
  Cull, Martínez Pería & Verrier (2015, World Bank Policy Research Working Paper
  7435) para o peso de bancos estatais. Onde não houver valor publicado, o campo
  fica `null` com `no_data` — **não se estima por palpite**; usa-se o default
  declarado e o status mostra a origem.
- **Basileia (BCBS)** — 8% de capital mínimo + buffers; o alvo de 10% da carteira
  da spec é **default declarado** (não um dado observado).

O padrão de pipeline é o de `docs/economy/initial_world_data.md`: builder em
`tools/build_bank_seed.py`, validador em `tools/validate_bank_seed.py`, cache em
`tools/data/`, `source`/`year`/`confidence` por nação, aliases ISO tratados
(`NED→NLD`, `PSX→PSE`, `SAH→ESH`, `XKX→KOS` no FMI).

### 4.3 Arquivo proposto (`data/definitions/economy/bank_seed.json`)

```json
{
  "source": "World Bank GFDD/WDI + BIS; ver docs/economy/rework/R2_dados.md",
  "year": 2024,
  "nations": {
    "BRA": {
      "assets_pct_gdp": 0.0,
      "capital_pct_gdp": 0.0,
      "capital_to_assets": 0.0,
      "public_share": 0.0,
      "private_share": 0.0,
      "foreign_share": 0.0,
      "confidence": "complete|partial|estimated|no_data",
      "source_key": "gfdd_di02|wdi_fb_bnk_capa|declared_default"
    }
  }
}
```

Campos `0.0` acima são placeholders de schema — o build **preenche ou grava `null`
com `no_data`**; nenhum valor é digitado à mão.

### 4.4 Fallback paramétrico (declarado, não dado)

Quando o país não tem o dado, usa-se uma faixa **declarada** por grupo de renda
(marcada `source: "declared_default"`, nunca `confidence: complete`):

| Grupo | `assets_pct_gdp` | `capital_to_assets` |
| --- | --- | --- |
| Alta renda | 1,00 | 0,10 |
| Renda média-alta | 0,70 | 0,10 |
| Renda média-baixa | 0,45 | 0,10 |
| Baixa renda | 0,30 | 0,10 |

Esses números são **ponto de partida de balanceamento**, não medição; o relatório
de dados deve listar quantas nações usaram cada linha e o dono pode vetar. O
`no_data` continua visível no status (não vira "dado").

### 4.5 Conversão para XGS (o ponto delicado)

O modelo não usa USD: o PIB é anualizado por nação em XGS e o estoque de dinheiro
é da ordem de 6,6 bi. A conversão é **uma constante global medida**, não um câmbio
inventado:

1. rodar o harness de 10 d e ler `gdp` por nação no status;
2. `XGS_por_int$PPP = Σ GDP_modelo / Σ gdp_ppp_bn` (uma constante para o mundo);
3. `capital_alvo_i = capital_pct_gdp_i × gdp_ppp_bn_i × XGS_por_int$PPP`;
4. `capital_semeado_i = min(capital_alvo_i, caixa_disponível_do_dono_i)`;
   `seed_scale_i = capital_semeado_i / capital_alvo_i` (publicado).

A conversão preserva a **distribuição** do PIB real entre países (o share), não o
nível em USD: o nível é o do modelo. Por isso a constante global é medida no próprio
harness e não um câmbio.

Sem o passo 1–3 (harness indisponível), o seed **não roda** e o status declara
`bank_seed: "no_model_gdp"`. Nunca se converte por um câmbio de fantasia.

---

## 5. Integração com o R1 e riscos

### 5.1 Depósitos × sub-saldos (a decisão de arquitetura)

O R1 tem `firm_sub + service_sub == private_cash`. O R2 **não** quebra isso: o
`private_cash` vira **caixa** (float) e os depósitos são claims separados por
sub-saldo (`BankAccount.firm/service`). Assim:

- "quem zerou, para" continua valendo (cada setor só gasta o seu float + o seu
  depósito);
- o sweep é do **excesso**, não do total; a região nunca fica sem caixa para a
  folha/comida (o R1a mostrou o custo de enxugar liquidez: comida 132/179);
- `private_total()` continua sendo o caixa; o status ganha
  `private_wealth = private_cash + depósitos` (agregado novo, só leitura);
- `money_total()` não muda de lista: `bank.cash` já está lá; depósito é claim.

### 5.2 Tesouro

- **Não é depositante no R2** (decisão do dono: depósitos vêm de regiões e
  empresas). O saldo do Tesouro continua dinheiro em `nation_treasury`.
- É **dono do banco público**: recebe dividendo pela meta; recapitaliza na cascata
  (debita `nation_treasury`, nunca o limite regional — o limite é teto de gasto).
- A austeridade da cascata incide sobre `regional_budget_limit_month` (a lei do R1),
  não sobre saldo de região (que não existe).
- O gancho de títulos é o lugar natural para o Tesouro *captar* do banco na fase
  política; no R2, `gov_bonds = 0`.

### 5.3 Empresas

- O `group.cash` continua operando 100% próprio (R1c); o depósito é o **excesso**
  acima do float, no banco de relacionamento.
- `Group::debt_bank_id` fecha o par banco↔tomador que a C6 já pressupõe; a migração
  de dívida no leilão move dívida, atraso e vínculo juntos.

### 5.4 Riscos

| Risco | Prob. | Impacto | Mitigação |
| --- | --- | --- | --- |
| Capitalização distorce a oferta de dinheiro | alta | alto | injeção limitada ao caixa do dono; `seed_scale` publicado; gate de conservação; nunca emissão para cobrir seed |
| Sweep de depósitos enxuga liquidez e derruba a comida (lição R1a) | alta | alto | float generoso, sweep só do excesso, A/B 90 d antes de 365, meta de comida inalterada |
| Dupla contagem em `money_total`/`allocate_money` (lição t169) | média | alto | enumerar cada novo claim; teste de residual zero com depósitos |
| Não determinismo na alocação/resolução | média | médio | vetores ordenados, sem `unordered_map`, desempate por id |
| R2c duplicar a C6 (dois resolvedores) | alta | médio | estender `bank_crisis::resolve` com leilão; um único FSM |
| Banco público com desconto dirigido vira monopólio de novo | média | médio | desconto declarado e limitado; capacidade e taxa medidas por banco no status |
| 36 nações com `public.status = none` não têm comprador de última instância | certa | médio | regra explícita: sem público real, o leilão vai só aos privados; sem comprador → deserto (ou charter na crise, ato político) |
| Dividendo por meta muda M1 (payout cria depósito) | média | médio | gate de M1/drift e de preço; payout só do excedente |
| Acoplamento com R3 (`foreign_cash`/`transport_cash` mudam) | média | médio | R2 não lê esses saldos para capitalização de estrangeiro além do `foreign_cash` atual; contrato marcado como provisório até o R3 |

---

## 6. Esforço estimado

Dias de desenvolvimento (agente + build + testes unitários), sem contar espera de
gate de 365 d:

| Sub-fase | Dados | Núcleo | Testes | Gates/medição | Total |
| --- | ---: | ---: | ---: | ---: | ---: |
| R2a — capital, adequação, dividendo | 2–3 | 2–3 | 1 | 1–1,5 | **6–8,5** |
| R2b1 — depósitos (estoque/alocação) | 0 | 3–4 | 1,5 | 1,5–2 | **6–7,5** |
| R2b2 — competição (taxa/capacidade) | 0,5 | 2–3 | 1 | 1 | **4,5–5,5** |
| R2c — leilão, cascata, deserto | 0 | 4–6 | 2 | 2–2,5 | **8–10,5** |
| **Total** | 2,5–3,5 | 11–16 | 5,5 | 5,5–7 | **24,5–32** |

Caminho crítico: R2a → R2b1 (o gate de comida do float) → R2b2 → R2c. O **desenho**
do R2c pode andar em paralelo ao R2b2; a implementação do leilão depende do par
banco↔tomador que o b2 introduz.

---

## 7. Artefatos e definição de pronto

Por sub-fase, no padrão R1:

- doc de fase (`R2a.md`, `R2b.md`, `R2c.md`) com números medidos e limites honestos;
- sonda `--r2-diag` só leitura: capital/meta, depósitos de funding por banco,
  float por cidade, taxa/capacidade por banco, leilões e rombo por parte;
- artefatos de 10/90/365 d em `G:/opengs_tmp/r2/`;
- `sim_unit_tests` com os casos de conservação por fase; `-Tier pr -Base main`;
- `GAME_STATE.md`: D-xxx do R2 com flags, save v44, baselines ON/OFF e a decisão
  do dono registrada;
- QA independente (`qa-docs`) antes do merge; **sem merge/push nesta unidade**.

## 8. Pendências declaradas (para o dono, não rediscutidas)

1. **Teto de viabilidade da capitalização**: o plano limita a injeção ao caixa do
   dono e publica `seed_scale`. Se o dono preferir garantir 100% do alvo mesmo sem
   lastro, isso exige uma decisão explícita (cria dinheiro) — hoje o plano **não**
   faz.
2. **Nações sem banco público real (36)**: sem comprador de última instância, a
   falha termina em deserto. Alternativa (charter público na crise) é ato político;
   o plano deixa o gancho, não a regra.
3. **Pagamento roteado por banco**: lote opcional depois do gate do b1; se o
   float bastar, o roteamento pode esperar a fase política.
