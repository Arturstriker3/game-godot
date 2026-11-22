# B7 — informalidade, serviços informais, mortalidade e insatisfação (D-093)

> Fase B7 do `docs/plans/perf_pop_financas_plano.md` §B7. Especificação fechada em
> `docs/plans/b7_informality_spec.md` — este documento é o relatório de implementação
> (desenho, constantes e resultados).

## Desenho (curto)

- **Dado observado**: `data/definitions/economy/informality.json` (taxa de emprego
  informal por nação, ILOSTAT/ILO `EMP_NIFL_SEX_RT`, harmonizada ICLS-13, último ano
  disponível 2022–2024, cobertura 95/179 nações) e `data/definitions/population/health_access.json`
  (UHC Service Coverage Index, WHO GHO `UHC_INDEX_REPORTED`, ano 2023, cobertura
  175/179). Nações sem entrada caem na âncora de renda (informalidade) ou em
  `kHealthAccessDefault` (saúde) — sem número inventado.
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
  A receita realizada é limitada à **capacidade de pagamento** do comprador (caixa do
  setor no urbano, soma das poupanças dos quintis no interior) e o crédito é exatamente
  o débito efetivado — o serviço informal **não cria dinheiro**.
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
| `kInformalDemandBase` / `PriceElasticity` / `Unemployment` / `Max` | 0,20 / 1,0 / 1,0 / 0,75 | Base calibrada para ~24,5% da cesta em preço normal (0,20×1,667×1,05×0,7); cresce com o formal caro/escasso e com o desemprego, teto declarado. |
| `kFoodMortalityThreshold` | 0,90 | Satisfação abaixo disto conta como fome. |
| `kFamineMortalityWeight` / `kHealthMortalityWeight` | 1,0 / 0,30 | Peso da fome (quadrático) e da saúde no excesso de mortes. |
| `kMortalityMaxExcess` | 3,0 | Teto do multiplicador de mortalidade (não explode). |
| `kAgeMortalityVulnerability` | 1,6 / 0,8 / 0,7 / 1,0 / 1,9 | Crianças e idosos concentram o excesso. |
| `kFamineMigrationMonths` / `kFamineMigrationBoost` | 3 / 0,50 | Fome prolongada empurra a migração B4. |
| `kDissatisfaction*` (pesos) | 0,30 / 0,20 / 0,25 / 0,15 / 0,10 / 0,00 | Pesos dos componentes (Σ=1; terra = gancho B6). |
| `kDissatisfactionYouthWeight` | 0,50 | Jovens pesam mais na insatisfação com desemprego. |

## Resultados

**Bateria de verificação (2026-11-18, Godot 4.7.2 debug, um teste por vez, todos exit 0):**
`b7_informality_test`, `b4_migration_test`, `b3_region_test`, `mine_stage_test`,
`services_test.tscn` (cena), `economy_integration_test`, `economy_ai_test`,
`economy_save_load_roundtrip_test`, `economy_save_load_v11_compat_test`,
`population_age_test` e `strata_test`.

**Conservação do serviço informal (correção da verificação).** A primeira versão creditava
aos quintis a receita cheia e debitava só `min(receita, poupança)`: o serviço **criava
dinheiro**. Medido pelo parecer da verificação: drift **210,005** no dia 31 sem IA,
**37,57** no dia 24 com IA e mundo com IA **travado no dia 23** (o tick violado é revertido
pelo invariante e o dia não avança). Fechamento: a receita realizada é limitada à
capacidade de pagamento (poupança dos quintis no interior; caixa do setor no urbano) e o
crédito rateado é exatamente o débito efetivado, com o resíduo do arredondamento no último
quintil. Provas:

- `b3_region_test` passou a falhar em QUALQUER violação de invariante (`_tick_checked`);
  conservação em 8 d `drift=0,000000000`, crescimento em 35 d e determinismo 1×16 threads
  (`834e82aa…`) sem violações.
- `b7_informality_test` (3b): interior com demanda muito acima da poupança — receita
  ≤ poupança, receita > 0, `invariant_violations` vazio e `money_total` conservado.
- `b7_world_ai_365.gd` (365 d, IA, urbano): **dia final 365, 0 dias travados, 0 violações**,
  falências **4** (teto 28), `money_total` 6.523.551.982,90 → 6.523.551.982,90088
  (**drift +0,000883**, ruído de ponto flutuante).

> **Correção do contador (2026-11-19).** A evidência original desta linha foi gerada com
> `++violation_count`/`++stalled_days` no harness. Em GDScript 4.7.2 `++x` é uma expressão
> sem efeito (não incrementa), então os dois zeros eram por construção. O harness passou a
> usar `+= 1` (e a sair com código 1 em violação, como o B5/B6) e foi reexecutado no HEAD
> `1069b879`: **365 d, 0 travados, 0 violações**, agora medidos de fato. O JSON
> `docs/economy/perf/b7_world_ai_365.json` foi regerado nessa execução; os números de fome,
> informalidade e insatisfação variam na terceira/quarta casa por mudanças de balance
> posteriores à evidência original, não por causa do contador.

**Mundo 365 d com IA** (`docs/economy/perf/b7_world_ai_365.json`):

- comida: satisfação mínima **AFG 0,0000** (fome estrutural pré-existente, não é regressão);
- mortes extras por fome (top 5, acumulado cidade+interior): IND 9.057.844, CHN 7.752.308,
  USA 2.254.156, NGA 1.979.590, IDN 1.862.847;
- informalidade do mundo **0,530 → 0,498** (BRA 0,399→0,361; USA 0,379→0,341;
  IND 0,829→0,791; NGA 0,830→0,791; JPN 0,367→0,328; DEU 0,044→0,040);
- insatisfação média por quintil (Q1→Q5): **0,104 / 0,102 / 0,098 / 0,093 / 0,087**.

**Goldens A0/S0 regerados (2 rodadas idênticas cada):**

| golden | save_sha256 antes → depois | status_sha256 antes → depois |
|---|---|---|
| `A0_60_urban.json` | `bdd355b3.` → `c8710110.` | `ae6c500b.` → `e9331483.` |
| `real_60_ai.json` | `9e5dab44.` → `dae5b601.` | `dd4a4ab2.` → `234ff72c.` |
| `real_60_noai.json` | `0ab8f317.` → `9c608715.` | `24bd0b78.` → `299b69b3.` |
| `demo_365_noai.json` | `98bbfde8.` → `be2b1d4f.` | `14d2c387.` → `cca00f48.` |

**CLI × Godot 10 d** (IA, `--threads=1`, sem urbano): snapshot e resultado
**byte-idênticos**, 73.743.531 bytes, SHA-256
`2C7FB702C73E4117D92A9B3B04E17CBA7A0497DEAC8FEFDCC86076BC64162555`, 0 diferenças pelo
comparador, `inv_violations=0`, `money_total` 6.523.551.982,90.

> **Depois do merge (D-093).** `tools/export_world_snapshot.gd` passou a chamar
> `set_informality`/`set_health_access` como o autoload, então o snapshot do CLI
> leva o dado ILOSTAT/OMS em vez da âncora. A equivalência continua exata e o
> número muda por desenho: 10 d `--ai=1 --threads=1` **byte-idênticos**
> (73.743.080 bytes, SHA-256
> `8BA2C4D7736E6C9120C34948C726B81F08B5A24724A23DB6F149C0FC6DD8695F`, 0 diferenças
> no comparador), e o A0 60 d continua `c8710110…`/`e9331483…` — o export não
> participa do A0.

**Round-trip do save:** 60 d direto × (30 d + load + 30 d) → mesmo save
`9dc2b322…` e mesmo status `890e8430…` (0 diffs).

Outros números medidos nos testes:

- informal share por quintil (ex. BRA): `base 0,365` → `{0,53 / 0,47 / 0,36 / 0,27 / 0,16}`.
- imposto formal × informal (share=1 → salário isento; share=0 → `kTaxRate`).
- mortes com/sem fome (`deaths_total > deaths_expected` com `satisfaction_prev=0`); o
  excesso por faixa aparece em **0–14 e 65+** (52,6% do excesso com 36,1% da população;
  0–14 e 65+ perdem mais por mil que 15–24/25–44).
- insatisfação do interior normalizada por bloco: componentes 0,20 (informalidade) e 0,30
  (inflação) somando o índice com comida.
- save v22 round-trip byte-idêntico e migração v21→v22 (informalidade semeada, contadores
  zerados).
- demografia B1 em 365 d (fixture alimento-seguro): população 1.368.443.017 →
  1.377.521.371, dinheiro com drift −0,0006.
