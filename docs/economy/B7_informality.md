# B7 — informalidade, serviços informais, mortalidade e insatisfação (D-090)

> Fase B7 do `docs/plans/perf_pop_financas_plano.md` §B7. Especificação fechada em
> `docs/plans/b7_informality_spec.md` — este documento é o relatório de implementação
> (desenho, constantes e resultados).

## Desenho (curto)

- **Dado observado**: `data/definitions/economy/informality.json` (taxa informal por
  nação, ILOSTAT ICLS-13, ano 2022–2024) e `data/definitions/population/health_access.json`
  (UHC SCI, WHO — ainda não presente; o loader cai em `kHealthAccessDefault`).
- **Estado (save v22)**: `Pop` (urbano) e `Region` (interior) ganham os mesmos três
  blocos — `informal` (share por quintil, renda, folha, serviço agregado), `mortality`
  (mortes esperadas/efetivas, excesso acumulado, meses de fome, referência de saúde) e
  `dissatisfaction` (índice 0..1 por quintil + componentes). Migração v21→v22 semeia a
  informalidade do dado nacional (ou âncora de renda ILO) e zera os contadores.
- **Trabalho informal**: o quintil só REPARTE a base nacional (gradiente
  `kInformalQuintileGradient`); a média ponderada reproduz o dado. A folha total NÃO
  muda — o peso do quintil é multiplicado pelo fator informal e renormalizado (quem é
  formal no mesmo quintil recebe o residual). O imposto incide SÓ sobre a parcela formal
  do salário. Na crise (`pay_scale < 1`), a renda informal sofre o dobro. Formalização
  mensal (renda/educação/infra B5).
- **Serviços informais** (agregado, sem firma): desvia uma fatia do gasto doméstico em
  serviços ANTES da divisão formal; o dinheiro realizado vai aos quintis informais
  (crédito nas poupanças), não entra em `service_cash`. Interior: redistribuição dentro
  do caixa da região (débito rateado pela poupança, crédito rateado pela renda informal).
- **Mortalidade**: a tabela CDR é a BASE (mult 1 em situação normal); fome
  (`satisfaction` abaixo de `kFoodMortalityThreshold`) e falta de saúde adicionam o
  excesso, concentrado em crianças (0–14) e idosos (65+). Fome prolongada (3 meses)
  empurra a migração B4.
- **Insatisfação**: medição pura (0..1) por quintil/cidade/região, com componentes
  desemprego/aluguel/comida/inflação/informalidade (terra fica como gancho B6, peso 0).

## Constantes (valor + porquê)

| Constante | Valor | Porquê |
|---|---|---|
| `kHealthAccessDefault` | 0,50 | Referência neutra quando a nação não tem dado de saúde (ausência explícita). |
| `kInformalQuintileGradient` | 1,55 / 1,30 / 1,00 / 0,75 / 0,45 | MODELO: informalidade concentrada na base; reparte a base sem mudá-la. |
| `kRegionInformalRuralFactor` | 1,15 | Informalidade rural maior que a urbana (param. declarado). |
| `kInformalFormalizationRate` | 0,02 /mês | Velocidade de convergência ao alvo (formalização lenta). |
| `kFormalizationIncome` / `Education` / `Infra` / `Enforcement` | 0,35 / 0,25 / 0,20 / 0,0 | Drivers da formalização (renda, escolaridade, infra B5; política futura = 0). |
| `kInformalWagePenalty` | 0,75 | O informal ganha menos no mesmo quintil; a folha é renormalizada (conserva). |
| `kInformalCrisisSensitivity` | 2,0 | Na crise a renda informal sofre o dobro. |
| `kInformalServiceEmploymentShare` | 0,60 | Fração da força informal empregada em serviços informais. |
| `kInformalServiceUnitsPerWorker` / `kInformalProductivityFactor` | 1,0 / 0,55 | Produtividade menor que a formal (declarado). |
| `kInformalPriceDiscount` | 0,60 | Preço menor (concorre com o formal). |
| `kInformalDemandBase` / `PriceElasticity` / `Unemployment` / `Max` | 0,55 / 1,0 / 1,0 / 0,75 | Demanda puxada pela base, cresce com desemprego, teto declarado. |
| `kFoodMortalityThreshold` | 0,90 | Satisfação abaixo disto conta como fome. |
| `kFamineMortalityWeight` / `kHealthMortalityWeight` | 1,0 / 0,30 | Peso da fome (quadrático) e da saúde no excesso de mortes. |
| `kMortalityMaxExcess` | 3,0 | Teto do multiplicador de mortalidade (não explode). |
| `kAgeMortalityVulnerability` | 1,6 / 0,8 / 0,7 / 1,0 / 1,9 | Crianças e idosos concentram o excesso. |
| `kFamineMigrationMonths` / `kFamineMigrationBoost` | 3 / 0,50 | Fome prolongada empurra a migração B4. |
| `kDissatisfaction*` (pesos) | 0,30 / 0,20 / 0,25 / 0,15 / 0,10 / 0,00 | Pesos dos componentes (Σ=1; terra = gancho B6). |
| `kDissatisfactionYouthWeight` | 0,50 | Jovens pesam mais na insatisfação com desemprego. |

## Resultados

_(a preencher com os números da bateria — ver o relatório final da tarefa.)_

- informal share por quintil (ex. BRA): `base 0,365` → `{0,53 / 0,47 / 0,36 / 0,27 / 0,16}`.
- imposto formal × informal (share=1 → salário isento; share=0 → `kTaxRate`).
- mortes com/sem fome (`deaths_total > deaths_expected` com `satisfaction_prev=0`).
- save v22 round-trip byte-idêntico (60 d, "load + continuar == rodar direto", 0 diffs).
- migração v21→v22 (informalidade semeada, contadores zerados).
