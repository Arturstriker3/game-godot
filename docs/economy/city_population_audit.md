# População das 834 cidades — auditoria e correção (B4 / Q-015)

Data: 2026-11-17 · Dono: `@gerente-dados` · Tarefa: D-004
Entrada: achado **B4** da revisão Q-015 (`docs/team/reviews/Q-015_s19_review.md`) — `IND-MUM` com
231.280.307 habitantes de cenário contra 5.791.139 em `IND-DEL`, o que estourava a capacidade
urbana (ocupação RES 1,96 > 100%).

Conclusão curta: **não era soma de províncias/estados atribuída a uma cidade.** Era o rateio da
população nacional entre as cidades da nação usando pesos em **unidades misturadas** (cidade
própria do GeoNames em umas, aglomeração metropolitana em outras), e o resultado — a **população
regional** — sendo consumido pela camada urbana como se fosse população de cidade. As duas coisas
foram corrigidas na fonte (geradores + JSON), mantendo a soma nacional exata.

---

## 1. De onde vem a população de cenário

```
data/definitions/world/cities_source.csv        (GeoNames, city proper; 40 legadas com "aglomeração")
        │  tools/build_world_cities.py
        │    · sim_population          = cidade própria da fonte (unidade de simulação do núcleo)
        │    · agglomeration_population = citypopulation.de 2026 (tools/agglomerations_2026.csv) ou null
        │    · urban_population        = aglomeração (limitada à população da nação), senão sim_population
        │    · population              = população REAL da nação rateada pelo peso urbano  ← regional
        ▼
data/definitions/scenario/cities.csv  →  cities.json      (tools/build_scenario_data.py)
        │
        ├─ tools/build_urban_land.py   → urban_land.json   (área construída, gabarito, capacidade)
        └─ tools/build_housing_city.py → housing_city.json (estoque habitacional, posse)
```

Antes desta tarefa o rateio (`distribute_national_population`) usava `sim_population` como peso e
`population` como "população da cidade" em toda a camada urbana. Como o GeoNames mistura
município e metrópole, o peso da Índia somava ~80 M para uma nação de 1,45 bi: **Mumbai (12,69 M
de peso, o maior) absorvia 15,9% da nação = 231,3 M**, enquanto **Nova Delhi (317.797 — o município
NDMC, não a metrópole de 36,9 M) ficava com 0,4% = 5,8 M**. Não havia participação de províncias.

## 2. Inventário ANTES (HEAD anterior a este commit)

Fonte de comparação: `agglomeration_population` de `urban_land.json` no HEAD (379 cidades casadas
com a tabela citypopulation.de; as outras 455 não têm linha na fonte).

| Métrica (379 casadas) | Antes |
| --- | --- |
| `population` / aglomeração > 3× | **224** |
| `population` / aglomeração < 0,33× | **2** (KWT-KUW 0,09; USA-POR 0,19) |
| mediana / máximo | 3,34× / 17,6× (PHL-DAV) |
| `IND-MUM` | 231.280.307 para 28.100.000 reais = **8,2×** |
| `IND-DEL` | 5.791.139 para 36.900.000 reais = **0,16×** |
| soma por nação vs `nations.csv` | **exata** nas 179 (o total nacional nunca esteve errado) |
| cidades com ocupação RES > 1 | **40** (máx. IND-KAN 4,40) |

O defeito não estava na soma nacional: estava na **distribuição** (hierarquia urbana invertida) e
no **uso** da população regional como população urbana.

## 3. A correção (contrato de três populações)

| Campo | Significado | Quem usa |
| --- | --- | --- |
| `sim_population` | cidade própria da fonte (GeoNames) | **núcleo econômico** (comida, caixa, empregos, per capita) — não muda o balanço |
| `agglomeration_population` | aglomeração real, citypopulation.de 2026 (nullable; referência) | área/gabarito via `urban_population` |
| `urban_population` | `agglomeration_population`, **limitado à população da nação**; senão `sim_population` | **camada urbana**: estoque habitacional, área derivada, gabarito |
| `population` | **população REGIONAL** = população real da nação rateada pelo peso urbano (soma por nação exata) | exibição (HUD/painéis) |

Regra de rateio: `population_i = P_nação × urban_i / Σ urban_nação` (resto por maior fração,
desempate por id). Como o peso é a população urbana real, o fator `population / urban_population`
é **uniforme dentro de cada nação** — é a definição operacional de "a cidade representa uma região":
a aglomeração mais o seu hinterland rural/periurbano.

Corte transfronteiriço: `urban_population` nunca excede a população da nação
(`urban_population_source = "agglomeration_capped"`). Único caso hoje: **SGP-SIN** — a aglomeração
da fonte (8.150.000) inclui Johor Bahru, na Malásia; o nó da cidade-Estado usa 6.111.175.

### Fator regional por nação (medido)

| Nação | fator | Nação | fator | Nação | fator |
| --- | --- | --- | --- | --- | --- |
| IND | 7,60 | CHN | 4,40 | USA | 2,58 |
| BRA | 2,56 | NGA | 4,99 | JPN | 1,49 |
| DEU | 3,81 | FRA | 3,26 | RUS | 2,82 |
| IDN | 4,52 | PAK | 4,31 | BGD | 5,62 |

Mediana 3,57 · mínimo 0,89 (KWT) · máximo 85,3 (MLT — nação de 1 nó para 0,5 M de habitantes, com
aglomeração de 6 mil). Fatores altos são a **limitação estrutural do roster**: 834 nós não cobrem
todos os hinterlands do mundo; numa nação com 1 cidade, ela recebe a nação inteira. O fator fica
registrado e uniforme — antes era arbitrário.

### Valores de referência (antes → depois)

| Cidade | `population` antes | `population` depois | `urban_population` | ocupação RES |
| --- | --- | --- | --- | --- |
| IND-MUM Mumbai | 231.280.307 | 213.443.798 | 28.100.000 | 1,96 → **0,24** |
| IND-DEL New Delhi | 5.791.139 | 280.287.407 | 36.900.000 | (n/d) → **0,05** |
| CHN-SHA Shanghai | 130.627.152 | 186.062.444 | 42.300.000 | — |
| USA-NEW New York | 61.393.311 | 58.203.289 | 22.600.000 | — |
| SGP-SIN Singapore | 6.111.175 | 6.111.175 | 8.150.000 → 6.111.175 | — |

Índia: Mumbai deixa de ser 40× Delhi e passa a **0,76×** (a razão real das aglomerações, 28,1/36,9).
O total mundial continua **8.203.501.062** e a soma por nação continua exata nas 179.

## 4. Efeito na camada urbana

- **Estoque habitacional** (`housing_city.json`): `round(urban_population / household_size)`. Mumbai
  cai de 52.803.723 para **6.415.525** domicílios (28,1 M / 4,38).
- **Área construída derivada** e **gabarito** (`urban_land.json`) passam a usar `urban_population`.
  117 áreas observadas (Atlas of Urban Expansion) ficam **intactas**; as 717 derivadas encolhem
  (total 338.533 → 230.869 km², −32%). `height_class = high` cai de 156 para 99 cidades.
- **Ocupação RES** (`estoque / capacidade`, com FAR {1,0; 2,5; 6,0} e m²/unidade {100; 85; 75}):
  **40 cidades acima de 1 → 1** (NPL-POK 1,375). Mediana 0,306 → 0,167.

## 5. Lacunas e decisões que NÃO são de dado (para o cabeça)

1. **`sim_population` continua sendo cidade própria em 98 das 380 casadas** (96 abaixo de 0,5×):
   IND-DEL 0,009×, KWT-KUW 0,012×, PHL-MAN 0,059×, USA-WAS 0,078×. É a **unidade econômica** do
   núcleo; trocá-la por aglomeração muda comida/caixa/empregos em massa e exige decisão de
   `@gerente-simulacao` + revisão do `@gerente-qualidade`. Lista em
   `tools/audit_city_population.py`, bloco E.
2. **Exibição**: HUD e painéis somam `population` (regional). O jogador vê "Mumbai 213 M" — que é a
   região, não a cidade. Recomendação: mostrar `urban_population` como população da cidade e
   `population` rotulada como região. É UI/painel (`ui/`, `core/ui/`), fora do meu escopo.
3. **NPL-POK com ocupação 1,375**: área observada do Atlas é de 2014 e a população é de 2026, com
   `height_class = low` (FAR 1). O modelo não tem como acomodar a diferença — decidir entre limitar
   o estoque inicial à capacidade ou derivar o gabarito da densidade observada.
4. **Ocupação mediana 0,167**: as constantes de capacidade (FAR 6 e 75 m²/unidade no gabarito alto)
   são generosas — a cidade nasce com folga. É constante de modelo (`gdextension/src/sim/`), não dado.
5. **IND-DEL**: sem cobertura no Atlas; área derivada pela faixa de renda (36,9 M / 11.000 hab/km² =
   3.354 km²), acima do território da NCT. Limitação declarada do proxy, não medição.
6. `population` mudou de valor em 736 das 834 cidades; em 15 nações a "maior cidade por população"
   mudou (IND Mumbai→New Delhi, CHN Shanghai→Guangzhou, PAK Lahore→Karachi...). Consumo afetado:
   `core/economy/economy.gd` (`_capital_names`, nomes de grupo da IA) e ordenações de UI — é
   consequência direta do rateio correto, registrada aqui para revisão.

## 6. Arquivos e regeneração

Alterados: `tools/build_world_cities.py` (rateio por população urbana, colunas novas),
`tools/build_scenario_data.py`, `tools/build_urban_land.py` (área/gabarito por `urban_population`,
exônimo `New Delhi → Delhi`), `tools/validate_urban_land.py`, `tools/build_housing_city.py`,
`tools/validate_housing_city.py`, `tools/audit_city_population.py` (novo).
Dados: `scenario/cities.csv`, `scenario/cities.json`, `world/out/cities.csv`,
`economy/urban_land.json`, `economy/housing_city.json`.

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"
& $py tools/build_world_cities.py --write-scenario   # 0 AVISO
& $py tools/build_scenario_data.py
& $py tools/build_urban_land.py
& $py tools/build_housing_city.py
& $py tools/validate_scenario_data.py
& $py tools/validate_urban_land.py
& $py tools/validate_housing_city.py
& $py tools/audit_city_population.py                 # exit 0
```

## 7. Verificação

- `validate_scenario_data` — 0 erros/0 avisos em 11 arquivos; `validate_urban_land` e
  `validate_housing_city` — exit 0, sem problemas (834/834 cidades).
- `audit_city_population.py` — exit 0: somas nacionais exatas (8.203.501.062), rateio uniforme
  (spread < 0,001 em todas as nações), `urban_population` = aglomeração em 380/380 (1 cortada),
  estoque = urbana/domicílio em 834/834, ocupação > 1 apenas NPL-POK.
- `verificador-flash` independente: **APROVADO** em C1–C9 (amostra de 12 cidades contra
  `tools/agglomerations_2026.csv`; recálculo do zero de somas, rateio, estoque e ocupação;
  áreas observadas intactas; diffs sem mudança fora de escopo).
- Godot headless **não** foi rodado nesta tarefa: mudança só de dado/gerador, e a máquina está
  compartilhada com outros agentes. O consumidor (`demo_set_urban`) lê apenas
  `housing_stock_by_tier`, `built_up_km2`, `zoning_split`, `height_class` e `reserve` — nenhum
  contrato de campo foi removido, só valores corrigidos e campos novos aditivos.
