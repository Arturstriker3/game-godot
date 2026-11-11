# Crop Suitability Check — Diagnóstico (não aplica correção)

Data: 2026-11-10 · Escopo: aptidão agrícola (`data/definitions/natural/agriculture.json`)
vs. `core/economy/geography_service.gd` · Fonte real: FAOSTAT (produção ~2021, aproximada).

## Conclusão central

**A premissa está incorreta: a Alemanha TEM batata no jogo.**

- `crop_suitability_nation("DEU","potato")` = **0.939** (melhor território) / **0.812** (média dos 9 territórios).
- Batata alemã supera o trigo (0.733) e o milho (0.823) da própria DEU.
- Passa por todos os limiares de acesso: mapa `WHEAT_MIN=0.40` e economia `MIN_SUITABILITY=0.30`.
- `resource_points.json` contém **11 marcadores de batata** na caixa da Alemanha (valores 0.62–0.94).

Nenhum termo da fórmula zera a batata na DEU. O único redutor é o relevo (`alt_factor`)
nos territórios do sul (Alpes), que a derruba para ~0.62–0.66 — ainda muito acima de 0.

## 1. Medição — batata por território alemão e comparáveis

`crop_suitability_nation` retorna o MÁXIMO entre os territórios da nação
(`_nation_territories` filtra `nature.json` por `adm0_a3`).

| País | best batata | best trigo | best milho | veredito (batata) |
|---|---|---|---|---|
| DEU | **0.939** | 0.733 | 0.823 | ✅ presente |
| POL | 0.812 | 0.594 | 0.726 | ✅ |
| NLD | 0.814 | 0.816 | 0.681 | ✅ |
| FRA | 0.987 | 0.985 | 0.985 | ✅ |
| BEL | 0.722 | 0.757 | 0.680 | ✅ |
| GBR | 0.827 | 0.772 | 0.586 | ✅ |
| RUS | 0.999 | 0.998 | 0.998 | ✅ |
| CHN | 0.964 | 0.973 | 0.977 | ✅ |
| IND | 0.605 | 0.808 | 0.542 | ✅ |
| UKR | 0.954 | 0.901 | 0.834 | ✅ |
| USA | 0.960 | 0.909 | 0.989 | ✅ |

Territórios DEU (temp=°C anual, precip=mm/ano): batata 0.656–0.939 em todos.

## 2. Fórmula (trace completo)

`CROPS["potato"] = (2, 8, 16, 22, 350, 500, 800, 1100, 3600)` — temp_min/opt_min/opt_max/max, precip_min/opt_min/opt_max/max, alt_max.

`crop_suitability = temp_factor · water_factor · (0.6 + 0.4·soil_factor) · alt_factor`, cada fator um trapézio.

`growing_season_temp`: batata **não** está em `WARM_SEASON_CROPS`; como
`annual_temp (DEU ≈ 8.4–9.3°C) < 22`, retorna `annual_temp` (não o trimestre).

Para o território DEU mediano (TRT000647, lat 49.8):

| termo | valor | fator |
|---|---|---|
| temperatura anual 8.8°C | dentro de 8–16 (ótimo) | `temp_factor = 1.0` |
| precipitação 721 mm | dentro de 500–800 (ótimo) | `water_factor = 1.0` |
| fertilidade 0.88 (fluvisol) | `(0.88-0.15)/0.65 = 1.12 → 1.0` | `soil_factor = 1.0` |
| altitude 309 m | `1 − 309/(3600·1.4) = 0.939` | `alt_factor = 0.939` |
| **produto** | | **0.939** ✅ (bate com o JSON) |

Nada zera a batata. A hipótese de "trimestre errado" **não se manifesta na DEU**;
manifesta-se em climas continentais frios (ver §4-A).

## 3. Limiares de acesso e legenda

- Mapa (`tools/build_resource_map.py`): marcador renovável só com `value ≥ WHEAT_MIN = 0.40`.
  Batata DEU = 0.62–0.94 → **11 marcadores confirmados**.
- Economia (`geography_service.gd`): fazenda inicial exige `wheat ≥ 0.30`; a batata entra
  via `apply_to_sim` com `demo_set_suitability(city, "potato", 0.939)` — aptidão plena.
- Investidor privado (`private_investor.gd`): `MIN_SUITABILITY = 0.30` → batata elegível.

Nenhum limiar esconde a batata alemã.

## 4. Causa-raiz real (onde a fórmula ERRA de fato)

Dois defeitos estruturais, independentes da DEU:

**A) Estação de cultivo de trigo/batata usa temperatura ANUAL, não o trimestre quente.**
`growing_season_temp` retorna `annual_temp` para trigo/batata quando `annual_temp < 22°C`.
Mas em clima continental frio o cultivo ocorre no VERÃO (trimestre quente ≈ bio10).
Usar a média anual (~2–8°C) zera/derruba trigo/batata em grandes produtores frios:
Canada (trigo 0.066, real ~34 Mt), Cazaquistão (trigo 0.0, real ~14 Mt), estepes do norte da Rússia.

**B) Sem modelo de irrigação: `water_factor` usa precipitação ANUAL (bio12).**
Regiões áridas/semiáridas irrigadas (vale do Nilo, Indus, Amu Darya) têm precip ~0–200 mm,
abaixo do `precip_min` de algodão/trigo, e são zeradas embora a lavoura real seja irrigada:
Egito (algodão 0.0), Uzbequistão (algodão 0.0), Turcomenistão (algodão 0.0),
Paquistão (algodão 0.099, trigo 0.216).

## 5. Tabela de incoerências

Aptidão = `crop_suitability_nation` (best). Produção FAOSTAT aproximada (Mt/ano).

| País | Cultura | Aptidão atual | Produção real ≈ | Veredito |
|---|---|---|---|---|
| DEU | batata | 0.939 | 10.6 | ✅ OK (premissa falsa) |
| POL / NLD / FRA / BEL / GBR / RUS / CHN / IND / UKR / USA | batata | 0.61–1.00 | 4.5–94 | ✅ OK |
| CHN / IND / BGD / IDN / VNM / THA | arroz | 0.59–0.89 | 33–213 | ✅ OK |
| USA / CHN / BRA / ARG | milho | 0.75–0.99 | 55–384 | ✅ OK |
| USA / BRA / ARG | soja | 0.76–0.99 | 46–155 | ✅ OK |
| IND / CHN / USA / BRA | algodão | 0.73–0.98 | 2.6–5.9 | ✅ OK |
| CHN / IND / RUS / USA / FRA | trigo | 0.81–1.00 | 36–138 | ✅ OK |
| **PAK** | algodão | **0.099** | 1.5 (top 5) | ❌ irrigação (Indus) |
| **EGY** | algodão | **0.0** | 0.15 (fibra longa) | ❌ irrigação (Nilo) |
| **UZB** | algodão | **0.0** | 0.8 (top 8) | ❌ irrigação (Amu Darya) |
| **TKM** | algodão | **0.0** | 0.25 | ❌ irrigação |
| **PAK** | trigo | **0.216** | 27 (top 8) | ⚠️ irrigação |
| **KAZ** | trigo | **0.0** | 14 (top 10) | ❌ temp anual + precip |
| **CAN** | trigo | **0.066** | 34 (top 5) | ❌ temp anual (inverno longo) |

## 6. Correção proposta (não aplicada)

1. **Temperatura de cultivo de trigo/batata**: usar o trimestre QUENTE (bio10) quando
   `annual_temp < 22°C` — a estação real de cultivo em clima temperado/continental —
   em vez da média anual. Manter o trimestre frio (bio11) apenas no caso tropical
   (`annual_temp ≥ 22`). Isto resgata CAN/KAZ/RUS-norte.
2. **Irrigação**: permitir um termo de irrigação (ex. `water_factor = max(precip, irrig_min)`
   para culturas com tradição irrigada — algodão, trigo em aridez `arid`/`semi_arid`),
   ou um piso de `water_factor` em bacias fluviais curadas (Nilo/Indus/Amu Darya/São Francisco).
   Sem inventar dado: irrigação seria `data_quality` própria (estimativa de modelo), como o solo.
3. **Recalcular** `agriculture.json` via `tools/apply_real_climate.py` e regenerar
   `resource_points.json` via `tools/build_resource_map.py`.

Mudança de contrato (dados de agricultura + fórmula em `build_natural_geography.py`)
exige rodar `economy_integration_test.gd` / `economy_live_path_test.gd` e revisão de `qa-docs`.
