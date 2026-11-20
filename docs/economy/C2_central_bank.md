# C2a - Banco central por moeda (estado + medição)

Decisão **D-103**. Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE C / C2.
Código: `gdextension/src/sim/central_bank.{h,cpp}`, `sim/save_state.cpp`,
`EconomySim::get_central_bank_status()` / `set_central_bank_gold()`.

## 1. O que a C2a faz e o que ela não faz

A C2a cria o **banco central como entidade por moeda** e mede o que já existe.
Nada é criado, cobrado ou intervindo:

- **entidade:** um BC por **moeda** (`Currency`), não por país - zona do euro,
  franco CFA e dolarizados já são modelados por `Currency.nations`;
- **balanço medido:** passivo = M0 = moeda em circulação + reservas dos bancos;
  ativo = reservas internacionais (ouro do cofre + moeda de reserva) + títulos;
- **agregados:** M0/M1/M2 lidos do balanço da C1, sem emissão nova;
- **CPI por país** e **série mensal** da inflação (mesma cesta do IPC do G2);
- **política salva** (regime cambial, independência, padrão monetário, moeda de
  reserva) com o default atual e **sem mecânica** nesta fase;
- **ouro** do BC declarado a partir do dado real da G2/D-101 e valorado a preço
  de mercado.

Fica para a **C2b**: emissão (M0 criada ao comprar títulos, emprestar aos bancos
ou financiar o Tesouro), juros, emprestador de última instância, intervenção
cambial, conversibilidade em ouro, padrão ouro/Bretton Woods agindo, e o **ouro
como bem físico negociado** (hoje o ouro é ativo real declarado, sem fluxo).

## 2. Banco central por moeda

`central_bank::banks(ws, money, base_prices)` devolve uma linha por moeda:

| campo | significado |
|---|---|
| `code`, `nations` | moeda e países que a usam |
| `regime`, `independence` | política salva (seção 5) |
| `currency_in_circulation` | M0 em poder do público |
| `bank_reserves` | reservas dos bancos (0 - não há banco; é a C4) |
| `m0` | circulação + reservas |
| `m1`, `m2` | M0 + depósitos (0 sem banco; é a C4) |
| `gold_tonnes`, `gold_value` | reserva de ouro declarada e valorada |
| `reserve_currency_holdings` | 0 - o exterior do núcleo é um `foreign_cash` global |
| `securities` | 0 - não há dívida pública no estado (C3/C4) |
| `net_financial_worth` | ativo financeiro - passivo (= -M0, a contrapartida da moeda) |
| `total_assets` | ativo financeiro + ouro |

O patrimônio **financeiro** fica negativo em M0 por desenho: a contrapartida da
moeda em circulação é a dívida do emissor - a mesma identidade da C1. O ouro é
ativo **real** e não entra nessa conta.

## 3. Atribuição da moeda do dia (o invariante novo)

`central_bank::allocate_money()` atribui a moeda em circulação pela **nação de
quem a detém** e declara o que não é atribuível, em buckets próprios:

`sector_cash` (caixas de setor: transporte e construção), `group_cash` (grupos
sem nação de origem), `project_cash` (obra cuja cidade não resolve), `external`
(`foreign_cash` global) e `no_currency` (nação sem moeda na tabela).

Nada é rateado por palpite: `allocated + unallocated` reproduz o `money_total`
canônico (`residual` ~ 0) e a soma dos M0 por moeda não excede a moeda atribuída.
O invariante roda em `SimCore::check_invariants`, junto com os da C1.

## 4. CPI e a série mensal

`central_bank::cpi_by_nation()` é a **única dona** do CPI: a mesma cesta do IPC
do G2 (alimentos do `needs.json` ponderados pelo fator calórico, razão
preço/base ponderada pela necessidade de cada quintil da cidade; sem quintis, a
necessidade da cidade; sem ela, população de simulação × necessidade per capita).
`get_storage_status()` e o BC chamam a mesma função - os números são idênticos
por construção e o teste `c2_central_bank_test` prova a igualdade.

A série é amostrada a cada **30 dias** (`kCpiSampleDays`), com **13 slots**
(12 meses de 360 dias + a amostra atual). `inflation_of()` lê a inflação de 12
meses da série; **sem 12 meses a ausência é declarada** (`has_12m = false`), e o
teste confirma que nada é completado com número inventado. A amostra só ocorre
depois dos invariantes do dia: um tick revertido não amostra.

## 5. Política (estado salvo, sem mecânica)

> **Atualização C2b1 (D-104):** `bank_independence` passou a agir. O valor salvo
> continua `no_data` (ausência declarada) e o default do jogo é `independent`
> (Taylor com passo limitado); `controlled` fixa o juro abaixo do neutro e
> monetiza o déficit medido. A mecânica, o canal de transmissão e as medidas
> estão em `docs/economy/C2b1_monetary_policy.md`. `exchange_regime` e
> `monetary_policy.standard` seguem sem mecânica (C2b2/C2c).

| campo | valores | default da C2a | dono da mecânica |
|---|---|---|---|
| `exchange_regime` (por moeda) | `floating`, `peg`, `band` | `peg` ou `floating`, conforme o `floating` do F5 | C2b |
| `bank_independence` (por moeda) | `no_data`, `independent`, `controlled` | `no_data` (o repositório não tem fonte de independência para 179 nações) | C2b |
| `monetary_policy.standard` (mundo) | `fiat`, `bretton_woods`, `gold` | `fiat` | C2b |
| `monetary_policy.reserve_currency` (mundo) | ISO da moeda | `USD` | C2b |

`no_data` é ausência declarada, não um palpite. O teste de política compara dois
mundos com políticas diferentes: depois de 1 dia o save sai **idêntico** - os
campos viajam sem agir.

## 6. Ouro do cofre

A reserva vem do dado real da G2/D-101 (`strategic_reserves.json`, World Gold
Council) e é declarada ao núcleo por `set_central_bank_gold({ISO3: t})`, agregada
por moeda; nação sem moeda na tabela cai em `gold_unallocated_tonnes`. O valor é
`toneladas × preço do bem gold` (média de mercado entre cidades com cotação; sem
mercado, preço de catálogo, e o método vai em `basis`).

O ouro **não** é semeado como bem físico e o contrato da D-101 fica intacto
(`gold_applied = false` no relatório do StorageService): o fluxo físico e a
formação de preço por compra/venda são C2b. A soma por moeda fecha com o
relatório do StorageService (teste).

## 7. Status / A4

- `get_status()` não muda de forma: o bloco `balance` da C1 continua igual.
- `get_central_bank_status()` (sob demanda): `monetary_policy`, `invariant`,
  `currencies` (uma linha por moeda), `cpi` (por nação, com `has_12m` /
  `has_annualized`), `unallocated`.

Nenhuma UI/painel nesta fase.

## 8. Save v27 e migração v26 → v27

Aditivo:

- `currencies[].exchange_regime`, `currencies[].bank_independence`;
- `monetary_policy` = `{standard, reserve_currency}`;
- `central_bank` = `{gold_declared, gold_tonnes_by_currency,
  gold_unallocated_tonnes, cpi_last_day, cpi: {ISO3: {index[], days[]}}}`.

Migração: **v26 e anteriores carregam com `fiat`/`USD`** (defaults declarados
pelo plano), `bank_independence = no_data`, `gold_declared = false`, ouro 0,
série vazia e `cpi_last_day = -1` - nada inventado para trás. Round-trip é
bit-exato e a continuação após load é idêntica (`tools/c2_central_bank_test.gd`).

## 9. Testes

- `tools/c2_central_bank_test.gd` - invariante de atribuição da moeda, M1 = M2 =
  M0 sem banco, CPI igual ao IPC do G2 e à conta feita à mão pela cesta, ausência
  declarada da inflação sem 12 meses, política que viaja sem agir, ouro do dado
  real fechando com o StorageService, round-trip v27 e migração v26 → v27.
- `tools/c1_world_ai_365.gd` - aceite de 365 d (violações do invariante, drift,
  portão de comida, balanço). A C2a é medição pura: o hash de trajetória tem de
  ser **idêntico** ao da C1, e é: `f7631ba0ba095ce64aff0d5a44193db83b0180901c0ac0c101b1fd45e6509c31`
  (mesmo JSON de `docs/economy/perf/c1_world_ai_365.json`, só `wall_s` muda),
  com **2 falências**, portão de comida **85/179**, **0 violações** do
  invariante, 0 dias travados e drift máximo diário **4,3e-13** (`max_daily_drift`;
  fim-vs-início 3,4e-13), `money_total` 6,52 bi.
- `tools/world_equivalence.ps1` - equivalência CLI × Godot, 60 d com IA e
  camada urbana: 0 diferenças run1 × run2 e contra o meta-golden. Os goldens
  `A0_60_urban.json` e os S0 (`real_60_ai`, `real_60_noai`, `demo_365_noai`) de
  `docs/economy/refactor/baseline/` foram regerados no save v27, cada um com
  **duas rodadas idênticas**.

## 10. Lacunas declaradas

- Sem emissão, juros, crédito ou banco: M1 = M2 = M0 e o BC só mede (C2b/C3/C4).
- Reservas em moeda de reserva = 0: o exterior é um `foreign_cash` global, não
  decomposto - a carteira do BC é C2b.
- Títulos = 0: não há dívida pública no estado (C3/C4).
- Ouro sem fluxo físico e sem formar preço contra joalheria/indústria (C2b).
- Independência do BC `no_data` para todas as moedas: falta fonte.
- Regime `band` existe como degrau do mesmo drift do F5; a banda administrada com
  intervenção e custo de defesa é C2b.
