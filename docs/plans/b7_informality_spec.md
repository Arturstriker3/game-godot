# B7 — informalidade, serviços informais, mortalidade e insatisfação (especificação de implementação)

> Fase B7 do `docs/plans/perf_pop_financas_plano.md` §B7 (aprovada pelo dono em 2026-11-18).
> Este documento fixa o MODELO e os nomes de estado/status antes do código. O relatório final
> (resultados, hashes, desvios) fica em `docs/economy/B7_informality.md`.
>
> Regra do projeto: **dado observado** entra com fonte e ano; **parâmetro de design** é constante
> nomeada em `gdextension/src/sim/sim_constants.h` e declarado aqui como modelo, nunca como dado.

## 1. Dados de entrada (observados)

| Arquivo | Conteúdo | Fonte | Uso |
|---|---|---|---|
| `data/definitions/economy/informality.json` | taxa de emprego informal (% do emprego total) por tag de nação | ILOSTAT/ILO, ano declarado no arquivo | fração informal INICIAL por nação |
| `data/definitions/population/health_access.json` | índice de cobertura de serviços de saúde (UHC SCI, 0..1) por tag | WHO (Global Health Observatory), ano declarado | acesso à saúde de REFERÊNCIA (linha de base da mortalidade) |

- Sem entrada para a nação: a fração informal inicial cai na âncora de renda que já existe
  (`sector_anchor_values().informal`, ILOSTAT por PIB PPP — `sector_employment.json`).
- Sem entrada de saúde: `kHealthAccessDefault` (0,5, declarado) como referência neutra.
- Os arquivos são carregados no GDScript (`core/economy/economy.gd`) e entregues ao núcleo por
  `set_informality(Dictionary)` e `set_health_access(Dictionary)`, como `set_sector_employment`.

## 2. Estado novo (save v22)

`Pop` (urbano) e `Region` (interior) ganham os mesmos blocos, com nomes equivalentes:

```
// trabalho informal
double informal_share;                    // fração informal do emprego (agregado)
double informal_labor_share[kIncomeStrata]; // fração informal do emprego de cada quintil
double informal_income[kIncomeStrata];     // renda informal do dia por quintil (informativa)
double informal_wage_bill;                 // folha informal do dia (informativa)
// serviços informais (agregado, SEM firma individual)
double informal_service_workers;
double informal_service_output;
double informal_service_demand;
double informal_service_revenue;
double informal_service_price;
double informal_service_share;             // fatia do gasto doméstico em serviços
// mortalidade
double deaths_expected;                    // mortes da tabela (base normal) do último fechamento
double deaths_total;                       // mortes efetivas do último fechamento
double excess_deaths_total;                // acumulado desde o dia 0 (fome + saúde)
double excess_deaths_famine_total;         // acumulado (só a parcela de comida)
double famine_months;                      // meses consecutivos com fome (migração B4)
double food_price_ref;                     // custo unitário da cesta no último fechamento mensal
double food_inflation_month;               // variação do custo desde food_price_ref (0 = sem dado)
double health_access_ref;                  // cobertura de saúde observada na semeadura
// insatisfação (medição; 0..1)
double dissatisfaction;
double dissatisfaction_by_stratum[kIncomeStrata];
double dissatisfaction_unemployment;       // componentes publicados
double dissatisfaction_rent;
double dissatisfaction_food;
double dissatisfaction_inflation;
double dissatisfaction_informality;
```

`WorldState` ganha `std::map<std::string,double> informal_rates;` e
`std::map<std::string,double> health_access;` (definição estática, NÃO entra no save, como
`age_structures`).

`City` ganha apenas derivados do dia que não precisam persistir (`informal_share`,
`informal_service_*` podem viver em `Pop`; o status lê `Pop` e `Region`).

Migração v21 → v22 (em `save_state.cpp`, sem mudar o resto do layout):
- semeia `informal_labor_share[s]`/`informal_share` do dado da nação (ou âncora de renda);
- `health_access_ref` = dado da nação (ou `kHealthAccessDefault`);
- contadores (`excess_deaths_*`, `famine_months`) = 0;
- `food_price_ref = 0` (o primeiro fechamento mensal apenas grava a referência, sem inflação);
- `dissatisfaction*` = 0 (recalculado no dia seguinte, como os fluxos derivados).

## 3. Trabalho informal (por quintil e por cidade/região)

### 3.1 Nível inicial (dado)
`base = informal_rates[nation]` (ou âncora de renda). O quintil apenas REPARTE a base:

```
weight[s]   = kInformalQuintileGradient[s]        // modelo: 1,55 / 1,30 / 1,00 / 0,75 / 0,45
share[s]    = base * weight[s] / Σ(weight[t] * people[t]/people_total)
```
ou seja, a média ponderada por população reproduz exatamente o dado nacional; o gradiente por
quintil é MODELO (informalidade concentrada na base da distribuição).

Interior (Region): `base_region = clamp(base * kRegionInformalRuralFactor, 0, 1)` —
`kRegionInformalRuralFactor = 1,15` (informalidade rural maior; param. declarado).

### 3.2 Formalização mensal (cadência A6, mesmo dia do fechamento de estratos)
```
driver = kFormalizationIncome  * clamp(income_index, 0, 1)      // renda (PIB PPP da nação)
       + kFormalizationEducation * education_above_basic[s]     // escolaridade do quintil
       + kFormalizationInfra     * infra_coverage(region)       // B5 (estrada/luz/água)
       + kFormalizationEnforcement;                             // gancho de política (0 por padrão)
target[s] = clamp(share[s] * (1 - driver), 0, 1)
share[s] += (target[s] - share[s]) * kInformalFormalizationRate
```
Constantes: `kInformalFormalizationRate = 0,02` (mês), `kFormalizationIncome = 0,35`,
`kFormalizationEducation = 0,25`, `kFormalizationInfra = 0,20`,
`kFormalizationEnforcement = 0,0` (política futura; sem efeito no jogo hoje).
`informal_share` agregado = Σ(share[s] × people[s]) / Σ people[s].

### 3.3 Renda, imposto e crise
- **Ganha menos:** dentro do mesmo quintil, a renda do trabalho informal é
  `kInformalWagePenalty = 0,75` da formal. A folha total NÃO muda (o total pago pelas firmas é
  fixo): o peso salarial do quintil é multiplicado por
  `(1 - share[s]) + share[s] * kInformalWagePenalty` e renormalizado — quem é formal no mesmo
  quintil recebe o residual. Isso mantém a conservação de dinheiro.
- **Sem imposto de renda nem contribuição:** a alíquota `kTaxRate` (0,15, que já É o cunho
  combinado imposto+contribuição) incide só sobre a parcela FORMAL do salário:
  `tax[s] = (wage[s] * (1 - share[s]) + capital[s] + rent[s]) * kTaxRate`.
  `pop.taxes` passa a ser a SOMA dos impostos por estrato (não mais `(wage+capital+rent)*rate`).
  Previdência/aposentadoria continua fora (depende da parte política/leis) — documentar.
- **Primeiro a perder renda na crise:** quando a folha é paga com desconto de liquidez
  (`pay_scale < 1` em `labor_day`), a renda informal sofre o dobro:
  `informal_factor = clamp(1 - (1 - pay_scale) * kInformalCrisisSensitivity, 0, 1)`,
  `kInformalCrisisSensitivity = 2,0`. A diferença NÃO é criada nem destruída: a folha efetiva já
  descontada pelas firmas é a fonte; o que o informal deixa de receber permanece no caixa de quem
  pagou (contraparte explícita no código).

## 4. Serviços informais (agregado por cidade e por região, SEM firma)

Setores agregados: comércio ambulante, transporte informal, construção/reforma informal, serviços
pessoais/domésticos. Não há `ServiceBusiness` — é um bloco por cidade/região.

```
workers   = Σ_s labor_force_quintil[s] * informal_share[s] * kInformalServiceEmploymentShare
output    = workers * kInformalServiceUnitsPerWorker * kInformalProductivityFactor
price     = formal_price_index * kInformalPriceDiscount
```
- `formal_price_index` = média dos `service_markets[].price` ponderada pela demanda de serviços
  do dia (proxy do custo do serviço formal na cidade); sem serviço formal, usa a âncora de preço
  do setor `personal_services` escalada pelo salário local.
- Demanda: uma fatia do `pop.service_spend` (já debitado das famílias em `phase_income`) é
  desviada para o informal ANTES da divisão entre setores formais:

```
low_share   = (people[Q0]+people[Q1]) / people_total
compete     = kInformalDemandBase
            * pow(formal_price / informal_price, kInformalDemandPriceElasticity)   // mais barato → mais demanda
            * (1 + kInformalDemandUnemployment * unemployment_rate)               // formal faltando → informal cresce
            * (0,5 + 0,5 * low_share)                                              // consumido pela base
share       = clamp(compete, 0, kInformalDemandMax)        // kInformalDemandMax = 0,75
informal_revenue = min(informal_demand_units * price, output * price, pool * share,
                       capacidade_de_pagamento)
```

`capacidade_de_pagamento` é o que FECHA a conservação: no urbano, o caixa do setor de
serviços (`service_cash`); no interior, a soma das poupanças dos quintis. A receita
creditada aos quintis é exatamente o débito efetivado (o resíduo do rateio fica no último
quintil), então o serviço nunca cria dinheiro — sem o teto, a receita cheia era creditada
com o débito truncado em zero (drift de centenas de unidades no fechamento mensal e, com
IA, tick revertido pelo invariante: dia travado).
- O dinheiro realizado vai para os trabalhadores informais (crédito nas poupanças dos quintis na
  proporção da renda informal) — **não** entra em `service_cash` nem em `firm_cash`. O resto do
  pool segue o caminho formal atual. Conservação total: `pool` continua igual.
- Sem imposto (não há receita tributada no caminho informal), produtividade menor (constante) e
  preço menor (constante).
- Interior: o pool é uma fração declarada da renda diária do interior (mesma fração usada no
  consumo urbano, `kServiceSpendShare`), paga da poupança dos quintis e creditada a eles
  mesmos (redistribuição: o caixa agregado da região não muda).

Constantes: `kInformalServiceEmploymentShare = 0,60`, `kInformalServiceUnitsPerWorker = 1,0`,
`kInformalProductivityFactor = 0,55`, `kInformalPriceDiscount = 0,60`,
`kInformalDemandBase = 0,20`, `kInformalDemandPriceElasticity = 1,0`,
`kInformalDemandUnemployment = 1,0`, `kInformalDemandMax = 0,75`.

## 5. Mortalidade ligada a comida e saúde

A tabela observada (CBR/CDR da nação, `age_structure_2026.json`) continua sendo a BASE:
em situação normal o multiplicador é 1.

```
food_gap   = clamp((kFoodMortalityThreshold - satisfaction) / kFoodMortalityThreshold, 0, 1)
health_gap = clamp(1 - health_access / max(health_access_ref, eps), 0, 1)
severity   = clamp(kFamineMortalityWeight * food_gap^2
                 + kHealthMortalityWeight * health_gap, 0, kMortalityMaxExcess)
```
- `mult = 1 + severity`.
- Repartição por faixa: o total de mortes vira `deaths_base * mult`; o PESO de cada faixa é
  `band[faixa] * kAgeDeathWeight[faixa] * (1 + severity * kAgeMortalityVulnerability[faixa])`
  normalizado para o total — criança (0–14) e idoso (65+) recebem peso maior.
- `kFoodMortalityThreshold = 0,90`; `kFamineMortalityWeight = 1,0`;
  `kHealthMortalityWeight = 0,30`; `kMortalityMaxExcess = 3,0`;
  `kAgeMortalityVulnerability[5] = {1,6; 0,8; 0,7; 1,0; 1,9}`.
- A mesma regra vale para o interior (`region_demography_month`).
- Contadores: `deaths_expected` (tabela), `deaths_total`, `excess_deaths_total`,
  `excess_deaths_famine_total` (só a parcela de comida). Publicados por cidade/região e somados
  por nação no relatório de 365 d.
- **Fome prolongada empurra migração (B4):** `famine_months` incrementa no fechamento mensal
  quando `satisfaction < kFoodMortalityThreshold` e zera acima; a partir de
  `kFamineMigrationMonths = 3` meses, o êxodo rural→urbano (e a saída cidade→cidade) recebem
  `+ kFamineMigrationBoost = 0,50` de pressão (proporção declarada), limitada pela vaga de
  moradia já existente. Pessoas e poupança continuam conservadas pela B4.

## 6. Insatisfação (só medição + status)

Por quintil, cidade e região (0..1), média ponderada dos componentes:

```
u   = unemployment_rate * (1 + kDissatisfactionYouthWeight * youth_labor_share)   // 15–24
rent = clamp(stratum_rent_paid[s] / max(stratum_net_income[s], eps), 0, 1)
food = clamp(1 - stratum_consumed[s] / max(stratum_need[s], eps), 0, 1)
infl = clamp(max(food_inflation_month, 0), 0, 1)         // alta de preço, não deflação
inf  = informal_share[s]
land = 0.0                                               // gancho B6 (patrimônio/terra), ainda ausente

index[s] = ( kDissatisfactionUnemployment * u
           + kDissatisfactionRent         * rent
           + kDissatisfactionFood         * food
           + kDissatisfactionInflation    * infl
           + kDissatisfactionInformality  * inf
           + kDissatisfactionLand         * land ) / Σ pesos
```
Pesos (design): desemprego 0,30, aluguel 0,20, comida 0,25, inflação 0,15, informalidade 0,10,
terra 0,00 (B6 pendente) — Σ = 1,00. `kDissatisfactionYouthWeight = 0,50`.
Agregado = média ponderada pela população dos quintis.
Normalização POR BLOCO: o urbano divide pela soma dos pesos que aplica
(desemprego+aluguel+comida+inflação+informalidade = 1,00) e o interior pela sua
(comida+inflação+informalidade = 0,50) — normalizar o interior pelo total 1,00 o capava
em 0,50 e impedia o índice de chegar a 1,0 no cenário extremo. Os componentes publicados
dividem pelo MESMO peso do índice, então somam o índice.
`food_inflation_month`: no fechamento mensal, `unit_cost/food_price_ref - 1` e atualiza
`food_price_ref` (primeiro fechamento só grava a referência).

## 7. Status (A4) — chaves novas

`get_city_status(city_id)` e `get_region_status(city_id)`:

```
"informal": {
  "share", "by_quintile": [5], "wage_bill",
  "service": {"workers","output","demand","revenue","price","share"}
}
"mortality": {
  "deaths", "expected_deaths", "excess_total", "excess_famine_total",
  "famine_months", "health_access", "health_access_ref"
}
"dissatisfaction": {
  "index", "by_quintile": [5],
  "components": {"unemployment","rent","food","inflation","informality","land"}
}
```

`get_day_summary()` publica só agregados leves (`informal_share`, `dissatisfaction` médios
ponderados, `excess_deaths_total` mundial), sem entrar no caminho quente pesado.
Nenhum painel/UI nesta fase.

## 8. Invariantes e testes

1. **Dinheiro conservado:** `money_total` do save igual antes/depois do tick com informal ativo
   (serviço informal paga dos quintis, não cria dinheiro; imposto a menos fica com as famílias).
2. **Pessoas conservadas:** Σ coortes urbanas + Σ coortes da região = população da cidade antes e
   depois dos fechamentos; mortes só reduzem com contraparte no total.
3. **Informal sem imposto:** com `informal_share = 1` a alíquota efetiva sobre o salário é 0; com
   `0` é `kTaxRate`.
4. **Serviço informal absorve demanda:** sem oferta formal a demanda informal atende > 0 e a
   receita vai para as famílias; com oferta formal abundante e barata, a fatia informal cai.
5. **Mortalidade sobe com fome:** dois mundos idênticos, um com `satisfaction` forçada baixa →
   `deaths_total > deaths_expected` e as faixas 0–14/65+ concentram o excesso.
6. **Insatisfação reage:** aumentar desemprego/aluguel/fome/informalidade eleva o índice
   (monotônico) e o agregado fica em [0,1].
7. **Save v22:** roundtrip byte-idêntico e migração de um fixture v21 (sem os campos novos)
   semeando os valores acima.

Novo teste: `tools/b7_informality_test.gd` (SceneTree, `quit(0/1)`), no padrão do
`b5_state_levers_test.gd`. `services_test` continua CENA (`--headless res://tools/services_test.tscn`).

## 9. Ordem de implementação

1. dados + estado + save v22 + migração;
2. trabalho informal (renda/imposto/crise) + serviços informais + status;
3. mortalidade (urbano e região) + fome→migração;
4. insatisfação + status;
5. testes, bateria, goldens, CLI×Godot, 365 d, bench, relatório e decisão D-093 (renumerada de D-090 no merge).
