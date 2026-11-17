# Dados de solo urbano, habitação e finanças municipais (§19 do plano de cadeias)

> Só dados — nenhum arquivo de núcleo/UI foi tocado. O núcleo ainda **não consome** estes
> arquivos; a integração (prefeitura por cidade, zoneamento, imóveis) é fase posterior.
> Regra aplicada: **fonte estatística primeiro**; lacuna → proxy regional/renda documentado
> e marcado (`estimated` + `method` + `confidence`); **nunca** um número fabricado como medição.

Esta revisão (Q-013 → fechamento das ressalvas e das lacunas §19) acrescenta:

- **`housing_city.json`** (novo, por cidade): aluguel/preço do m², estoque habitacional
  absoluto e posse por estrato de renda — itens (a), (b) e (c) das lacunas §19.
- **Relevo na reserva expansível** (`urban_land.json`): `relief_non_buildable_fraction`
  (item d), estimada da declividade do raster WorldClim/SRTM.
- **Ressalva 1 do Q-013** fechada: `height_class` agora usa a população **real da
  aglomeração** (campo `height_class_population`).
- **Ressalva 2 do Q-013** fechada: `municipal_finance.json` ganhou `method` por entrada.

## 1. Arquivos

| Arquivo | Granularidade | Entidades | Validador |
|---|---|---|---|
| `data/definitions/economy/urban_land.json` | cidade | 834/834 | `tools/validate_urban_land.py` |
| `data/definitions/economy/housing.json` | nação | 179/179 | `tools/validate_housing.py` |
| `data/definitions/economy/housing_city.json` | cidade | 834/834 | `tools/validate_housing_city.py` |
| `data/definitions/economy/municipal_finance.json` | nação | 179/179 | `tools/validate_municipal_finance.py` |

Geradores: `tools/build_urban_land.py`, `tools/build_housing.py`,
`tools/build_housing_city.py`, `tools/build_municipal_finance.py` (determinísticos;
`--check` reporta sem escrever). Os quatro validadores retornam **0** (verificado nesta
rodada).

## 2. urban_land.json (solo urbano por cidade)

Campos principais: `built_up_km2` (área urbana construída), `density_hab_km2`,
`expandable_reserve_km2` (reserva expansível), `relief_non_buildable_fraction`,
`zoning_split` (residencial/comercial/industrial), `height_class` (gabarito low/mid/high),
`height_class_population`, `coastal`, `agglomeration_population` (referência).

- **Área construída** — **117 cidades observadas** (Atlas of Urban Expansion 2016, Tabela 1
  "Areas and Densities", época T3 2009–2015) + **717 derivadas** (`built_up = população /
  densidade_proxy por faixa de renda`, `estimated=true`).
- **Densidade** — nas 117 observadas é a densidade do próprio Atlas; nas 717 derivadas é o
  proxy da faixa. Teto de plausibilidade 60.000 hab/km² (máximo real: Daca 55.200).
- **População urbana** — `urban_population` (cities.csv): `agglomeration_population` de
  `tools/agglomerations_2026.csv` (citypopulation.de 2026) quando casada — **380/834**, com a
  aglomeração limitada à população da nação (`agglomeration_capped`; único caso: Singapura, cuja
  aglomeração inclui Johor Bahru/Malásia) — senão a cidade própria da fonte (`sim_population`,
  **454/834**). `urban_population_source` registra qual foi usada. É ela que dimensiona a área
  derivada e o gabarito; `population` é a **população regional** (rateio nacional) e não entra.
- **Gabarito (`height_class`)** — regra de limiar populacional **sobre a população urbana**
  (`height_class_population` registra qual foi usada): `≥ 5 M` → high; `≥ 1 M` ou capital → mid;
  senão low. Fecha a ressalva 1 do Q-013 (Washington 8,8 M → high; Portland 2,5 M → mid; Berlim
  4,7 M → mid).
- **Reserva expansível** — sempre derivada: `built_up × 0,60` × 0,55 se costeira × 0,40 se
  ilha × `(1 − relief_non_buildable_fraction)`, teto 3×. `reserve_method` =
  `derived:coast+landmass+relief` quando o relevo é aplicado.
- **Relevo (item d do §19)** — `relief_non_buildable_fraction` (0..0,5) é a fração de
  células do raster de elevação WorldClim 2.1 10 arc-min (derivado de SRTM) com declividade
  regional > 3% num buffer 5×5 ao redor da cidade. **Limitação honesta:** 10 arc-min
  (~18,5 km/célula) não resolve encostas urbanas — captura só o relevo *regional*
  (ondulado/montanhoso vs plano); por isso `relief_confidence="low"` e o teto 0,5 evita
  zerar a reserva de cidades de vale. **151/834** cidades têm fração > 0. Se o raster
  estiver ausente no ambiente, `relief_source="no_data"` e o relevo não entra na reserva.
- **Zoneamento** — shares por faixa de renda (residencial 0,55–0,68 / comercial 0,17–0,32 /
  industrial 0,13–0,15), `estimated=true`.

## 3. housing.json (habitação por nação)

- **Casa própria** — 43 observadas (OECD HM1.3 + JPN censo nacional) + 136 proxy por faixa.
- **Favela (ODS 11.1.1)** — 169 observadas (World Bank) + 10 sem dado (`null`).
- **Tamanho do domicílio** — 155 observadas (UN DESA) + 24 proxy.
- **Aluguel/renda** — 41 observadas (OECD) + 138 proxy por faixa.
- **Estoque por padrão** — derivado do Gini (`inequality.json`), lognormal coerente com
  `income_distribution.json`.

## 4. housing_city.json (habitação por cidade — itens a/b/c do §19)

Complemento **por cidade** do `housing.json` (por nação). Tudo é proxy/derivado
(`estimated=true`); nenhum valor é medição direta de mercado por cidade.

- **(a) Aluguel e preço do m²** — `rent_per_m2_usd` (mensal) e `price_per_m2_usd`, por
  **proxy de renda documentado**: `rent = rent_to_income_ratio × PIB PPP per capita ÷
  (12 × floor_m2_per_person)`; `price = rent × 12 × 20 anos` (price-to-rent).
  `rent_to_income_ratio` vem de `housing.json` (OECD AHD HC1.2); `floor_m2_per_person` por
  faixa (low 12 / lower_mid 16 / upper_mid 24 / high 38 m²/pessoa, referência OECD AHD HC2.1
  Living Space / UN-Habitat). Não há fonte gratuita estável de preço por m² por cidade
  (Numbeo é scraping/ToS; Global Property Guide é parcialmente pago), por isso é proxy.
  A renda é a da **nação** (a mesma para todas as cidades — sem dado subnacional de renda
  no projeto). **808/834** cidades têm aluguel/preço; as 26 das 9 nações sem PIB PPP ficam
  `null` (`price_source="no_data"`).
- **(b) Estoque habitacional absoluto** — `housing_stock_total = urban_population ÷ tamanho do
  domicílio` (UN DESA), dividido em `housing_stock_by_tier` (popular/médio/alto) pela
  `housing_stock_split` da nação (derivada do Gini de `inequality.json`). População = a
  **urbana** (aglomeração, senão cidade própria); a **regional** (`population` de cities.csv)
  fica no JSON só para exibição. Antes de D-004 o estoque usava a regional, o que inflava a
  ocupação do solo (Mumbai: 1,96 → 0,24; ver `docs/economy/city_population_audit.md`).
- **(c) Posse por estrato de renda** — `tenure_by_tier` = {popular, medium, high} ×
  {own, rent, informal} (frações, soma 1 por estrato), derivado de: taxa nacional de casa
  própria (OECD HM1.3) com gradiente de renda (±0,15, spread documentado via HM1.3.A3) e
  informal = população em favela (SDG 11.1.1), alocada do estrato mais pobre para o mais
  rico. Países de alta renda sem medição de favela têm informal = 0.

## 5. municipal_finance.json (finanças municipais por nação)

- **Receita subnacional % PIB** — 37 observadas (OECD SNG/CFE) · 133 proxy · 9 null.
- **Imposto sobre imóveis % da tributação** — 98 observadas (OECD RS 4100) · 72 proxy · 9 null.
- **Transferências do governo central** — 37 observadas · 133 proxy · 9 null.
- Flags `*_estimated` por campo + confiança por entrada. **Agora também `method` por
  entrada** sempre que há ao menos um campo proxy (fecha a ressalva 2 do Q-013). As 9
  nações sem `gdp_ppp_bn` ficam `no_data`.

## 6. Fontes

Atlas of Urban Expansion 2016 · citypopulation.de (Major Agglomerations 2026) · WorldClim
2.1 elevation (10 arc-min, SRTM) · OECD (Housing tenure HM1.3, Housing costs HC1.2, Living
space HC2.1, Revenue Statistics 4100, Subnational Finance CFE SNG) · World Bank (SDG 11.1.1
`EN.POP.SLUM.UR.ZS`, Gini `SI.POV.GINI`) · UN DESA (Household Size and Composition).

## 7. Lacunas conhecidas (honestas)

1. **Densidade/área observada** cobre as 117 maiores do Atlas; as outras 717 são proxy de renda.
2. **Relevo** entra na reserva como proxy *regional* (10 arc-min, confiança `low`) — encostas
   urbanas reais não são resolvidas pelo raster disponível.
3. **Aluguel/preço** é proxy de renda (não há fonte livre estável por cidade); a renda é a da
   nação, não da cidade.
4. **Posse por estrato** é derivação da taxa nacional + favela (OECD HM1.3.A3 dá quintis por
   país, mas não foi curada por país — usa-se o gradiente documentado ±0,15).
5. **Municipal**: receita subnacional e transferências observadas só nos 37 membros OECD;
   imposto sobre imóveis cobre mais países (séries RS LAC/África/Ásia/Global).
6. **Casa própria/aluguel** fora da OECD é proxy por renda.
7. **9 nações** sem PIB PPP ficam `no_data` em municipal_finance e `null` em aluguel/preço.
8. **Japão**: casa própria usa censo nacional (60,9%), divergindo da OECD (84%) — registrado.
9. **Preço de terreno por zona** (§19 item 6) e **finanças municipais por cidade** (§19)
   continuam fora do escopo de dados (modelo de mercado / alocação nacional→cidade).

## 8. Verificação

Verificador-flash (independente) confere amostra contra as fontes. Nesta rodada: validadores
**4/4 exit 0**; amostra de 15 entidades conferida contra fontes (Atlas, citypopulation.de,
WorldClim/SRTM, OECD, World Bank) — ver parecer da revisão.

## 9. Regenerar

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"
& $py tools\build_urban_land.py ; & $py tools\validate_urban_land.py
& $py tools\build_housing.py   ; & $py tools\validate_housing.py
& $py tools\build_housing_city.py ; & $py tools\validate_housing_city.py
& $py tools\build_municipal_finance.py ; & $py tools\validate_municipal_finance.py
```
