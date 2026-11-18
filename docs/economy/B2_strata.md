# B2 — Estratos de renda com patrimônio

Fase B2 do plano `docs/plans/perf_pop_financas_plano.md`. Sai de "a POP é uma
massa com três níveis de escolaridade usados como estrato" para **cinco
quintis de renda por cidade**, separados da educação, cada um com renda,
poupança, dívida (campo zerado até a Parte C), patrimônio e condição de
moradia. **Muda resultado de propósito** (recalibração da Parte B).

## 1. Por que separar educação de estrato

A F7 usava os três níveis de escolaridade como estratos de renda: a mesma
variável decidia quem ocupava a vaga e quem consumia life/luxury. Isso não
permite representar um país com Gini alto e escolaridade homogênea (o pobre
com curso básico e o rico com curso básico ganham muito diferente) nem a
riqueza (poupança, imóvel e participações) que a Parte C precisa.

Na B2:

- **Educação** (3 níveis) define o **acesso à vaga** e a composição
  educacional de cada quintil.
- **Estrato** (5 quintis) define **renda de capital, dispersão salarial,
  consumo, poupança, patrimônio e posse de moradia**.
- Os campos antigos `Pop::stratum_*[kEducationLevels]` continuam existindo e
  publicados (contrato de status/UI); os campos novos são `Pop::stratum_*[kIncomeStrata]`.

## 2. Dado inicial (2026, fonte pública)

`data/definitions/economy/inequality.json` (já existia no projeto, não era
lido pelo núcleo):

| campo | uso |
| --- | --- |
| `nations.<tag>.gini` | Gini observado (World Bank SI.POV.GINI, ano mais recente) — **154/179** nações observadas; 25 regionais estimadas (`estimated: true`) |
| `nations.<tag>.deciles.lowest_20 … highest_20` | quintis de **renda** observados (World Bank SI.DST) — 154/179 nações |

O `GeographyService` passou a ler esse arquivo: o Gini declarado ao núcleo é o
observado quando existe (antes era sempre a estimativa regional de
`income_distribution.json`, que continua como fallback) e os cinco quintis vão
por `set_city_strata(city_id, shares)`.

A **população** de cada quintil é 20% (quintil = 20% das pessoas, por
definição). O dado entra na repartição da **renda**, não no tamanho do grupo.
Sem os quintis observados, o núcleo usa a proxy lognormal do Gini declarado
(mesma família da F7, sigma do Gini) e, sem Gini, quintis iguais — ausência
explícita.

## 3. Estado e save v17

`Pop` ganhou, indexado por quintil: `stratum_people`, `stratum_wage`,
`stratum_capital`, `stratum_rent`, `stratum_transfer`, `stratum_net_income`,
`stratum_savings`, `stratum_debt` (0), `stratum_equity` (0 até C5),
`stratum_wealth`, `stratum_saving_rate`, `stratum_rent_paid`,
`stratum_rent_bill`, `stratum_food_spend`, `stratum_need`,
`stratum_consumed`, `stratum_wage_factor`, `stratum_capital_share`,
`stratum_education_mix[5][3]`, `stratum_tenure[5][3]`, `stratum_level[5]` e
`lifecycle_saving[5]`.

**Save v17** grava o bloco `pop.strata` (pessoas, quintis-alvo, dispersão,
participação no capital, poupança, dívida, patrimônio, taxa, aluguel pago,
nível, mistura educacional, posse e ciclo de vida). O `pop.savings` gravado é
a **soma** dos quintis (mesma ordem do load) — round-trip sem tick é
bit-exato. Save v16 e anteriores **migram** no fim do load (depois do bloco
`urban`, para a posse de moradia já estar carregada): os quintis são semeados
pela proxy do Gini declarado e a poupança escalar é repartida pela renda.
`city.rent_income_next` também entra no save (é dinheiro pendente às famílias,
como `capital_income_next`).

## 4. Modelo (o que é dado e o que é declarado)

Tudo o que segue é **modelo declarado** em `sim_constants.h`; nenhum número
por país foi inventado. A estrutura de cada dia:

1. **Salário** — a folha por escolaridade (`wage_income_by_level`, já paga pelo
   mercado de trabalho) é repartida por `população do quintil ×
   composição educacional × vantagem salarial da composição ×
   stratum_wage_factor`. O `wage_factor` é a **dispersão dentro do nível**
   (experiência, jornada, informalidade) e é semeado para reproduzir os
   quintis observados: `factor_s ∝ q_s / w_s`, normalizado com média 1. É ele
   que permite um Gini de 0,50 com prêmio por escolaridade de 1,00–1,30.
2. **Capital e aluguel recebido** — distribuição pela **participação na
   riqueza** `∝ q_s^1,6` (riqueza mais concentrada que a renda; proxy
   documentada, UBS Global Wealth Report 2024). O aluguel que as famílias
   donas recebem na fase de moradia foi separado em `city.rent_income_next`
   e cai nos mesmos donos. Vai para os estratos donos, não para a cidade.
3. **Transferências** — o orçamento é equilibrado (`kTransferShare = 1`), então
   a transferência é a **devolução do imposto pago**, repartida na proporção da
   renda de mercado. Uma transferência igual por pessoa (primeira tentativa)
   achatava o Gini em ~18 pontos e foi descartada. Aposentadoria/assistência
   por idade exige o cruzamento idade × estrato, que a B2 não tem.
4. **Renda externa de subsistência** — `kStrataExternalEqualShare = 0,40` do
   total é igual por pessoa (piso de comida) e o resto acompanha a renda de
   mercado (os serviços/pesca/turismo que geram a receita são de quem trabalha
   neles). 100% igual achatava BRA/ZAF em ~12 pontos.
5. **Imposto** — 15% da renda de mercado do quintil (igual ao agregado);
   **serviço** — a fatia de 45% da renda doméstica do dia, rateada pela renda.
6. **Consumo por estrato** — a comida é comprada **do mais pobre para o mais
   rico**, cada quintil com o seu orçamento (`poupança + renda − serviço`) e a
   mesma prateleira da cidade; depois as demais categorias de basic, life e
   luxury, com o teto de cada quintil dado pela sua renda per capita contra o
   custo acumulado da cesta aos preços do dia. O subsídio de subsistência do
   governo vai para os quintis com maior falta para a cesta.
7. **Poupança** — taxa-alvo por quintil (`kStrataSavingRate` 0,02→0,20) × fator
   de ciclo de vida da cidade (`kStrataLifecycleSaving` 0/0,4/1,0/1,15/0,5 por
   faixa etária B1). A poupança-alvo é reservada do gasto discricionário
   (depois de comida e aluguel) e fica no estrato; não é dinheiro novo.
8. **Moradia** — posse por estrato vem de `housing_city.json`
   (`tenure_by_tier`: Q1–Q2 popular, Q3–Q4 médio, Q5 alto). O aluguel é cobrado
   **dos inquilinos de cada quintil** (limitado à poupança de cada um) e vai
   para os donos; o ônus e o pago/faturado por estrato são publicados. Favela
   (§19) continua só no padrão popular, que é o dos dois quintis de baixo.
9. **Patrimônio** — mensal: `poupança + imóvel (posse própria × preço §19) +
   participações`. As participações em grupos ficam em 0 até a Parte C5 (não
   há mercado para avaliá-las); a ausência é explícita.
10. **Mobilidade** — mensal na cadência A6, determinística: cada par de
    quintis adjacentes move até `kStrataMobilityRate = 0,3%/mês` da população
    (e a mesma fração da poupança) na direção da renda per capita relativa à
    média da cidade. É um sinal pequeno, não o fenômeno migratório (B4).

### Conservação

A renda por quintil é uma **repartição** dos totais da cidade (salário pago,
capital/aluguel distribuído, transferência, renda externa): a soma é o total
por construção. A poupança dos quintis é a fonte da verdade do dia e
`pop.savings` é a soma; toda cobrança posterior (aluguel, importação, serviços,
IPTU, autoconstrução) passa por `charge_households`/`credit_households`, que
movem o escalar e os quintis juntos. `money_total` inclui `rent_income_next`.

## 5. Status e consultas

`get_city_status(id).pop.income_strata` publica, por quintil: `income_share`,
`population`, `income` (renda disponível do dia), `savings`, `debt`, `wealth`,
`saving_rate`, `level`, `rent_paid`, `rent_bill`, `food_income_share`,
`tenure_own`, `tenure_rent`, `tenure_informal`, mais `gini` (medido dos
quintis) e `lifecycle_saving[5]`. `get_urban_city(id)` publica o necessário
para o probe urbano (população, renda, aluguel pago/faturado, posse alugada,
nível). Nenhuma chave antiga mudou.

## 6. Validação de padrão real

`tools/strata_test.gd` (`--days=365`, IA, mundo real, urbano): confere a
semeadura contra os quintis observados, a soma dos quintis contra
população/poupança da cidade, o Gini **emergente** contra o dado em 10 nações,
a queda da fração da renda em comida, a poupança crescente com o estrato e o
ciclo de vida, a mobilidade viva e o drift de dinheiro. Evidência em
`docs/economy/perf/b2_strata_validation.json`.

**Gini dado × simulado (dia 365, sem âncora):**

| nação | dado | simulado | Δ |
| --- | ---: | ---: | ---: |
| BRA | 0,503 | 0,512 | +0,009 |
| USA | 0,418 | 0,420 | +0,002 |
| DEU | 0,337 | 0,349 | +0,012 |
| IND | 0,230 | 0,286 | +0,056 |
| NGA | 0,339 | 0,397 | +0,058 |
| JPN | 0,323 | 0,339 | +0,016 |
| CHN | 0,360 | 0,367 | +0,007 |
| ZAF | 0,541 | 0,556 | +0,015 |
| MEX | 0,426 | 0,423 | −0,003 |
| FRA | 0,318 | 0,333 | +0,015 |

O maior desvio é +0,058 (Índia/Nigéria, países pobres em que o modelo gera um
pouco mais de dispersão que o dado). **Nenhuma âncora de Gini participa do
consumo ou da renda**; o número publicado é medido dos quintis.

**Fração da renda gasta em comida (mundo, dia 365), Q1→Q5:**
0,742 / 0,481 / 0,350 / 0,246 / 0,107. **Taxa de poupança (Q1→Q5):**
0,0129 / 0,0257 / 0,0451 / 0,0772 / 0,1287; fator de ciclo de vida por faixa
(0-14, 15-24, 25-44, 45-64, 65+): 0 / 0,40 / 1,00 / 1,15 / 0,50. Quintis movem
(mobilidade viva; desvio máximo de 20% medido no teste). **Drift de dinheiro
0,00000000 e 0 violações de invariante em 365 dias.**

`tools/b2_urban_strata_report.py` lê o probe urbano de 365 d
(`b2_urban_365.json`). Ônus e pago/faturado por estrato inquilino ficam
**fora** da faixa em parte das seis cidades — ver §8.

## 7. Recalibração (sweep 365 d, urbano, bench)

**Sweep 365 d** (`b2_real_365.json` / `b2_float_365.json`, baseline
`b1_world_sweep.json`, 2 processos, threads=1): falências **17** (real) e
**20** (float) contra teto 28; drift **0,0000** nas duas; hash real
`1515e7b44000…`, sonda float `489cd2ea3219…` (difere, como deve). **O portão de
comida de 30 dias NÃO fechou:** 25 nações no real (e 24 no float) caem mais de
2,5 p.p. contra a B1 fora das isenções. Pior: SGP −20,3, MNE −19,7, URY −15,5,
ISL −14,4, SAU −11,5, OMN −11,1, QAT −10,7, MNG −10,5, NZL −10,4; EGY (em
diagnóstico) −12,5. O padrão é de **importadores**: o modelo B2 compra a cesta
do orçamento agregado, mas o **capital de giro das firmas** (firm_cash) drena
mais cedo nos importadores (SGP: 53k no dia 2 contra 509k na B1) e a reposição
via poupança das famílias não repõe o caixa da firma na mesma velocidade; a
partir daí o importador compra menos e a espiral de estoque se fecha. É uma
lacuna de **financiamento do comércio** (Parte C), não da repartição por
estrato: a cesta comprada do agregado restaurou os países pobres (antes COG
−19,2 etc.) — os que restam são os ricos sem terra. Reportado, **sem isenção
nova**.

**Urbano 365 d (SP, TOK, LAG, MUM, NEW, WEI; `b2_urban_365.json`):** o probe
rodou 365 dias sem violação de invariante. Ônus por estrato inquilino:
- BRA/TOK/NEW: Q1 e Q2 com ônus/pago **0** (a poupança do quintil acabou antes
  do aluguel; o aluguel é cobrado por estrato e o pobre não tem caixa);
- AUS Q1 **0,545** (acima do teto 0,40); IND Q2 0,268; NGA Q2 pago 0,196;
  NEW Q2 pago 0,560;
- Q3–Q5 ficam em 1,000 de pago/faturado e ônus 0,01–0,16.
A cobrança por estrato (em vez do pool da cidade, como na B1) é a causa: o
aluguel está ancorado na renda média e o quintil pobre não tem margem. Isso é
**calibração em aberto** (§8), não um erro de conservação.

**Determinismo:** threads **1 × 16** no mundo real (60 d, IA, urbano) fecham o
mesmo `trajectory_hash` `4acc4940…` e o mesmo `money_total`
(`docs/economy/perf/b2_threads1.json` / `b2_threads16.json`).

**Bench canônico N=3:** não rodado nesta sessão (o host esteve ocupado com o
sweep e o probe urbano em paralelo). Pendente para o fechamento, assim como os
goldens A0/S0 e a equivalência CLI×Godot.

## 8. Limites conhecidos

- A composição educacional por quintil e a dispersão salarial dentro do nível
  são **modelo**; o dado observado entra nos quintis de renda.
- A participação na riqueza é proxy (`q^1,6`); não há estoque de riqueza por
  país no repositório. Participações em grupos só na Parte C5.
- Transferências são devolução pro-rata do imposto; aposentadoria por idade
  exige idade × estrato, que a B2 não cruza.
- A posse de moradia é herdada do padrão de renda (§19), não medida por
  quintil; a favela foi restrita ao Q1 (o Q2 é renormalizado para
  própria/alugada).
- Mobilidade move população e poupança proporcional, sem herança nem
  atributos de domicílio (B4).
- **Comida dos importadores ricos** e **aluguel por estrato** são os dois
  desvios abertos desta fase (números em §7). Ambos têm mecanismo identificado
  e não foram mascarados por isenção.

