# Q-016 — população de cenário: Kandahar × Mazar, Cairo e varredura geral

- **Data:** 2026-11-17 · **Dono:** `@gerente-dados` · **Branch:** `data/q016-city-pop`
  (worktree `G:/projetos/opengs-wt-data-q16`, base `balance/global-resources` @ `e77857d`).
- **Entrada:** ressalva de dado do parecer Q-016 (`docs/team/reviews/Q-016_q015_int_review.md`
  §7/R4) e a auditoria Q-015 (`docs/economy/city_population_audit.md`).
- **Modelos:** só DeepSeek Flash (`executor-flash` + `verificador-flash`).
- **Escopo:** só dado e geradores. **Nenhum** comando Godot foi executado (havia medição de
  performance na máquina); só Python e leitura.

## 0. Resultado curto

| Item | Antes | Depois |
| --- | --- | --- |
| `AFG-MAZ` (Mazar-i-Sharif) `sim_population` | 523.300 (valor de Kandahar) | **484.500** (CSO 2020) |
| Pares de cidade com `population` idêntica | 7 (14 cidades) | **6 (12 cidades)**, todos explicados |
| Soma mundial (`population`) | 8.203.501.062 | 8.203.501.062 (inalterada) |
| Soma por nação | exata nas 179 | exata nas 179 |
| Cairo — população inflada? | — | **Não**: 9,6 M cidade própria, 23,2 M aglomeração, 86,1 M regional (exibição) |

Toda a mudança de valor está em 4 cidades do Afeganistão (tabela §2.4). Não há outra correção
aplicada: o resto da varredura vai como lista (§4) por depender de decisão da simulação.

## 1. De onde vem cada número (pipeline, confirmado no código)

```
data/definitions/world/cities_source.csv      (GeoNames cities15000/500 2026, city proper)
        │  tools/import_world_sources.py        population ← GeoNames; initial_farm_tons = 0,0003 × population
        │  tools/build_world_cities.py          sim_population = population da fonte
        │                                        agglomeration_population ← tools/agglomerations_2026.csv (citypopulation.de 2026)
        │                                        urban_population = min(aglomeração, população da nação) ou sim_population
        │                                        population = população da nação rateada por urban_population
        ▼
data/definitions/scenario/cities.csv  →  cities.json   (tools/build_scenario_data.py)
        ├─ tools/build_urban_land.py    →  urban_land.json
        └─ tools/build_housing_city.py  →  housing_city.json
```

Consumo no núcleo (verificado, não suposto): `economy_sim.cpp:85-89` lê `sim_population` e
`economy_sim.cpp:756` grava `city.simulation_population` (fallback para `population` **só** quando o
campo não existe — não é o caso de nenhuma das 834). `consumption.cpp:95,868` usa
`city.simulation_population` para a necessidade alimentar. Ou seja: **`population` (regional) não
entra no cálculo de comida**; `sim_population` entra.

## 2. Caso 1 — AFG-KAN × AFG-MAZ com 523.300

### 2.1 Origem

Os dois registros de `cities_source.csv` (GeoNames) traziam `population = 523300` e
`population_year = 2026`. Não havia aglomeração casada para nenhum dos dois (a tabela
`agglomerations_2026.csv` só publica aglomerações ≥ 1 M; para o Afeganistão consta apenas Kabul,
5.450.000). Logo `urban_population = sim_population` nos dois, e o rateio nacional deu a mesma
`population` regional (3.244.795) às duas cidades — o sintoma visível.

Não é artefato do rateio: é a **fonte** que repete o número. Kandahar está correto; Mazar-i-Sharif
recebeu o valor de Kandahar no GeoNames.

### 2.2 Fonte da correção

| Campo | Valor |
| --- | --- |
| Fonte | Central Statistics Office (CSO) Afghanistan — projeção de 2020-09-21, publicada em `citypopulation.de/en/afghanistan/cities/` (acesso 2026-11-17) |
| Valores da fonte | Kābul 4.434.600 · Herāt 574.300 · **Kandahār 523.300** · **Mazār-e Sharīf 484.500** |
| `method` | `official_projection` (projeção oficial publicada; não é interpolação nossa) |
| `confidence` | `high` — é a mesma tabela que já sustenta o valor de Kandahar no cenário |
| Ano-base | 2020-09-21 (a fonte não tem estimativa mais nova publicada para cidade) |

Não existe linha de aglomeração 2026 para Kandahar/Mazar na tabela do projeto, e **não** foi criada:
a tabela cobre ≥ 1 M e misturar cidade própria com aglomeração foi exatamente o defeito B4/Q-015.

### 2.3 Onde a correção foi gravada (para não se perder numa regeneração)

1. `tools/world_city_curation.py` — novo `POPULATION_OVERRIDES` com população, ano, `method`,
   `confidence`, `source` e `note`.
2. `tools/import_world_sources.py` — aplica o override ao montar a linha (inclusive recalculando
   `initial_farm_tons = 0,0003 × população`, a regra já usada pelo importador).
3. `tools/build_world_cities.py` — `apply_population_overrides()` reaplica na projeção (idempotente),
   então um `cities_source.csv` antigo também gera o cenário correto.
4. `data/definitions/world/cities_source.csv` — linha do AFG-MAZ corrigida em disco (484.500, ano
   2020, `source`/`note` citando a fonte; usina 157,0 → 145,4).

### 2.4 Antes → depois (valores efetivos nos arquivos)

| Cidade / campo | Antes | Depois | Δ |
| --- | ---: | ---: | ---: |
| AFG-MAZ `sim_population` | 523.300 | 484.500 | −7,4% |
| AFG-MAZ `urban_population` | 523.300 | 484.500 | −7,4% |
| AFG-MAZ `population` (regional) | 3.244.795 | 3.020.786 | −6,9% |
| AFG-MAZ usina de trigo (`output_tons_per_day`) | 157,0 | 145,4 | −7,4% |
| AFG-MAZ `housing_stock_total` | 65.087 | 60.261 | −7,4% |
| AFG-MAZ `built_up_km2` | 34,9 | 32,3 | −7,4% |
| AFG-MAZ reserva expansível (km²) | 10,5 | 9,7 | −7,6% |
| AFG-KAN `population` | 3.244.795 | 3.262.699 | +0,55% |
| AFG-KAB `population` | 33.793.493 | 33.979.950 | +0,55% |
| AFG-HER `population` | 3.561.028 | 3.580.676 | +0,55% |
| **Soma AFG** | 43.844.111 | **43.844.111** | 0 |
| **Soma mundial** | 8.203.501.062 | **8.203.501.062** | 0 |

O peso urbano do Afeganistão cai de 7.070.900 para 7.032.100; o rateio interno muda, o total
nacional não. `sim_population` das outras três cidades não mudou; `initial_farm_tons` de AFG-MAZ
segue a regra do importador (0,0003 × população), por isso muda junto — declarado aqui para não
parecer efeito colateral escondido.

## 3. Caso 2 — Cairo (EGY-CAI): os três números conferem

| Grandeza | Cenário | Fonte | Veredito |
| --- | ---: | ---: | --- |
| `sim_population` | 9.606.916 | CAPMAS 2023-07-01: **9.801.536** (cidade, inclui Helwan), via citypopulation.de/en/egypt/cities | −2,0% — coerente (GeoNames 2026) |
| `agglomeration_population` | 23.200.000 | citypopulation.de, aglomeração 2026-01-01, rank 11 (inclui Giza e Shubra al-Khaymah) | exato |
| `urban_population` | 23.200.000 | regra `min(aglomeração, nação)`; nação 118.365.995 | não cortado (correto) |
| `population` (regional) | 86.138.706 | 118.365.995 × 23,2/31,88 = 72,77% do Egito | por construção (fator EGY 3,713) |
| Nação EGY | 118.365.995 | World Bank WDI SP.POP.TOTL 2025 (`nations_source.csv`) | coerente |

**Não há município × aglomeração trocados e não há população inflada no que o núcleo consome.**
`sim_population` é a cidade própria (9,6 M), `urban_population` é a aglomeração (23,2 M) e os 86,1 M
são a região usada só para exibição — o núcleo lê `sim_population` (§1).

Por que então a comida de Cairo chega a 0,237? Não é demanda: a necessidade é
`kDemoNeedPerCapita = 0,0003`/dia (`sim_constants.h`) e a usina de Cairo produz
**2.882,1 t/d = 0,0003 × 9.606.916**, isto é, exatamente a necessidade da população de simulação com
a usina a plena carga. Qualquer queda de produção (insumo, mão de obra, importação) aparece
integralmente como razão de comida — não há folga. Isso é consistente com o "transiente de
importação de trigo" apontado no parecer; é assunto de `@gerente-simulacao` (instrumentar a cadeia
do trigo e a frota), **não** de dado de população. Nenhuma alteração foi feita em EGY.

## 4. Varredura geral

### 4.1 Cidades com `population` de cenário idêntica (depois da correção)

6 grupos / 12 cidades, todas da mesma nação. A causa é comum e **não é erro de dado**: as duas
cidades têm o **mesmo peso urbano** na tabela de aglomerações (que arredonda em 10 mil/50 mil), e o
rateio determinístico produz o mesmo inteiro.

| `population` | Cidades | Peso urbano igual | Explicação |
| ---: | --- | ---: | --- |
| 30.570.543 | CHN-JIN Jinan × CHN-KUN Kunming | 6.950.000 | tabela 2026 (rank 81, ambos 6,95 M) |
| 18.989.662 | IND-NAS Nashik × IND-VAD Vadodara | 2.500.000 | tabela 2026 (rank 244, ambos 2,5 M) |
| 8.666.945 | DEU-MUC Munique × DEU-STR Stuttgart | 2.275.000 | tabela 2026 (rank 272, ambos 2,275 M) |
| 3.916.826 | RUS-KRA Krasnodar × RUS-ROS Rostov-on-Don | 1.390.000 | tabela 2026 (rank 465, ambos 1,39 M) |
| 3.812.347 | FRA-BOR Bordéus × FRA-TLS Toulouse | 1.170.000 | tabela 2026 (rank 562, ambos 1,17 M) |
| 3.015.111 | RUS-PER Perm × RUS-VOL Volgogrado | 1.070.000 | tabela 2026 (rank 613, ambos 1,07 M) |

O sétimo grupo (AFG-KAN × AFG-MAZ, 3.244.795) era o único com causa na fonte e foi corrigido (§2).
Sobra, em `sim_population`, um conjunto de coincidências **entre nações diferentes** (1.200.000 para
Rotterdam/Santo Domingo/Harare/Shymkent etc.) — são valores redondos da tabela, não duplicatas de
fonte.

### 4.2 As 20 maiores razões `urban_population / sim_population` fora de [0,5; 2]

96 cidades acima de 2×, 2 abaixo de 0,5×, 736 dentro da faixa. As 20 maiores:

| # | Cidade | Nação | `sim_population` | `urban_population` | razão |
| ---: | --- | --- | ---: | ---: | ---: |
| 1 | IND-DEL New Delhi | IND | 317.797 | 36.900.000 | 116,11 |
| 2 | KWT-KUW Kuwait City | KWT | 60.064 | 4.825.000 | 80,33 |
| 3 | USA-POR Portland | USA | 66.881 | 2.475.000 | 37,01 |
| 4 | GMB-BAN Banjul | GMB | 37.274 | 1.210.000 | 32,46 |
| 5 | PHL-MAN Manila | PHL | 1.600.000 | 27.100.000 | 16,94 |
| 6 | USA-WAS Washington | USA | 689.545 | 8.800.000 | 12,76 |
| 7 | BHR-MAN Manama | BHR | 147.074 | 1.580.000 | 10,74 |
| 8 | USA-MIN Minneapolis | USA | 410.939 | 3.275.000 | 7,97 |
| 9 | QAT-DOH Doha | QAT | 344.939 | 2.700.000 | 7,83 |
| 10 | LKA-COL Colombo | LKA | 648.034 | 5.000.000 | 7,72 |
| 11 | THA-CHO Chon Buri | THA | 219.164 | 1.620.000 | 7,39 |
| 12 | CRI-SAN San José | CRI | 335.007 | 2.475.000 | 7,39 |
| 13 | MYS-KUA Kuala Lumpur | MYS | 1.453.975 | 9.750.000 | 6,71 |
| 14 | PRT-POR Porto | PRT | 252.687 | 1.660.000 | 6,57 |
| 15 | JPN-OSA Osaka | JPN | 2.753.862 | 17.700.000 | 6,43 |
| 16 | USA-DAL Dallas | USA | 1.326.087 | 8.100.000 | 6,11 |
| 17 | USA-SEA Seattle | USA | 780.995 | 4.625.000 | 5,92 |
| 18 | ARG-BUE Buenos Aires | ARG | 2.891.082 | 17.000.000 | 5,88 |
| 19 | MEX-MON Monterrey | MEX | 1.135.512 | 6.300.000 | 5,55 |
| 20 | USA-DEN Denver | USA | 729.019 | 3.950.000 | 5,42 |

**Leitura:** são municípios (cidade própria) comparados com aglomerações metropolitanas. A razão é o
multiplicador metropolitano do nó, não um erro de unidade: o valor de cima está certo para cidade
própria e o de baixo está certo para a aglomeração. A exceção de método é o item 1 (Delhi: o
registro do GeoNames é o município NDMC de 317.797), já documentada na auditoria Q-015 §5.
**Nada corrigido aqui:** trocar `sim_population` para aglomeração muda comida/caixa/empregos em massa;
é a decisão nº 1 da auditoria Q-015 (dona: `@gerente-simulacao` + `@gerente-qualidade`).

### 4.3 As 2 cidades abaixo de 0,5× (unidade trocada na fonte) — proposta, não aplicada

| Cidade | `sim_population` | Aglomeração | Razão | Evidência |
| --- | ---: | ---: | ---: | --- |
| CHN-WEI Weifang | 9.386.705 | 3.375.000 | 0,36 | 9.386.705 é a **população da prefeitura** de Weifang (censo 2020), não a cidade |
| PAK-PES Peshawar | 4.758.762 | 2.175.000 | 0,46 | 4.758.762 é a **população do distrito** de Peshawar (censo 2023), não a cidade |

Têm fonte clara (censo/prefeitura), mas **não foram alteradas**: mudam a unidade de simulação de duas
cidades grandes (−64% e −54%) e, portanto, o balanço de comida/caixa/emprego de CHN e PAK. Pela regra
da auditoria Q-015 §5, isso exige decisão de `@gerente-simulacao` e revisão do `@gerente-qualidade`.
Proposta para quando houver autorização: substituir por cidade própria (Weifang ~2,0 M; Peshawar
~2,2 M conforme a aglomeração da tabela) e recalcular a usina, como feito no AFG-MAZ.

## 5. Regeneração e validação (Python apenas)

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"
& $py tools/build_world_cities.py --write-scenario   # 0 AVISO; "população corrigida -> AFG-MAZ"
& $py tools/build_scenario_data.py
& $py tools/build_urban_land.py                      # com tools/data/worldclim/wc2.1_10m_elev.tif
& $py tools/build_housing_city.py
& $py tools/validate_scenario_data.py                # 0 erros, 0 avisos, 11 arquivos
& $py tools/validate_urban_land.py                   # exit 0
& $py tools/validate_housing_city.py                 # exit 0
& $py tools/audit_city_population.py                 # exit 0
```

Resultados: `validate_scenario_data` "tudo ok — 0 erros, 0 aviso(s) em 11 arquivo(s)";
`validate_urban_land` "OK: 834 cidades, 117 com área observada, 717 com área derivada";
`validate_housing_city` "OK: 834 cidades, 808 com aluguel/preço, 26 sem preço";
`audit_city_population` exit 0 — somas nacionais exatas (8.203.501.062), spread < 0,01 em todas as
nações, `urban_population` = aglomeração em 380/380 (1 cortada, SGP), ocupação RES > 1 só NPL-POK.

`tools/build_world_cities.py --check` → exit 0, sem avisos.

**Godot: não rodado** (havia medição de performance na máquina; e a mudança é só de dado/derivados).
Validar com Godot depois: `economy_integration_test.gd` e o sweep 365 d, quando a máquina estiver
livre — nenhum contrato de campo mudou, só valores de 4 cidades.

## 6. Arquivos alterados

- Geradores: `tools/world_city_curation.py` (`POPULATION_OVERRIDES`),
  `tools/import_world_sources.py` (aplica no import), `tools/build_world_cities.py`
  (`apply_population_overrides`, log no build).
- Fonte: `data/definitions/world/cities_source.csv` (linha AFG-MAZ).
- Derivados: `world/out/{cities,buildings}.csv`, `world/out/_meta.json`,
  `scenario/{cities,buildings}.{csv,json}`, `economy/{urban_land,housing_city}.json`.
  - `_meta.json` traz também duas atualizações de **metadado atrasado**, não de dado:
    a regra `city_population` (já existia no código desde o Q-015) e `scenario_ports` 138 → 144
    (contagem real do cenário).
- **Não commitado de propósito:** `data/definitions/i18n/i18n.csv` e `i18n.json`. A regeneração só
  reordena chaves (o arquivo de base tem 47 chaves `chain.*`/`trade.*` no fim, fora da ordem) e
  reescreve `i18n.json` com limpeza de entradas antigas — nada disso muda texto ou chave de cidade
  (o nome de AFG-MAZ não mudou). Fica como tarefa separada para não poluir este diff.
- `GAME_STATE.md` não foi tocado: branch de dado isolada, sem merge.

## 7. Pendências e decisões que não são minhas

1. **Weifang / Peshawar** (§4.3): correção proposta com fonte, aguardando `@gerente-simulacao` +
   `@gerente-qualidade`.
2. **`sim_population` fora de [0,5; 2]× da aglomeração em 98 das 380 casadas** (96 com
   `sim_population` abaixo de 0,5× da aglomeração e 2 acima de 2× — a direção `sim/aglomeração`,
   inversa do top-20 de §4.2): continua a decisão nº 1 da auditoria Q-015.
3. **Cairo**: instrumentar a cadeia do trigo/frota (o parecer R4 atribuía o vale à população; a
   medição aqui mostra que a usina cobre exatamente a necessidade de `sim_population`, sem folga).
4. **Relevo**: o worktree não tinha `tools/data/worldclim/wc2.1_10m_elev.tif` (insumo ignorado pelo
   git). Sem ele o `build_urban_land` grava `relief_source = no_data` em todas as 834 cidades —
   armadilha para quem regenerar num clone limpo; o arquivo foi copiado do checkout principal e o
   resultado saiu idêntico ao HEAD exceto nas 4 cidades do AFG.
