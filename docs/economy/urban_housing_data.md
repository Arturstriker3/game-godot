# Dados de solo urbano, habitação e finanças municipais (§19 do plano de cadeias)

> Só dados — nenhum arquivo de núcleo/UI foi tocado. O núcleo ainda **não consome** estes
> arquivos; a integração (prefeitura por cidade, zoneamento, imóveis) é fase posterior.
> Regra aplicada: **fonte estatística primeiro**; lacuna → proxy regional/renda documentado
> e marcado (`estimated` + `method` + `confidence`); **nunca** um número fabricado como medição.

## 1. Arquivos

| Arquivo | Granularidade | Entidades | Validador |
|---|---|---|---|
| `data/definitions/economy/urban_land.json` | cidade | 834/834 | `tools/validate_urban_land.py` |
| `data/definitions/economy/housing.json` | nação | 179/179 | `tools/validate_housing.py` |
| `data/definitions/economy/municipal_finance.json` | nação | 179/179 | `tools/validate_municipal_finance.py` |

Geradores: `tools/build_urban_land.py`, `tools/build_housing.py`,
`tools/build_municipal_finance.py` (determinísticos; `--check` reporta sem escrever).
Os três validadores retornam **0** (verificado nesta rodada e pelo verificador-flash).

## 2. urban_land.json (solo urbano por cidade)

Campos principais: `built_up_km2` (área urbana construída), `density_hab_km2`,
`expandable_reserve_km2` (reserva expansível), `zoning_split` (residencial/comercial/industrial),
`height_class` (gabarito low/mid/high), `coastal`, `agglomeration_population` (referência).

- **Área construída** — **117 cidades observadas** (Atlas of Urban Expansion 2016, Tabela 1
  "Areas and Densities", época T3 2009–2015; cada valor com 2 refs e conferido 117/117 contra a
  fonte viva pelo verificador) + **717 derivadas** (`built_up = população / densidade_proxy por
  faixa de renda`, `estimated=true`). Faixas de densidade usadas no proxy (estimativa, faixa
  GHSL UCDB/Demographia, não medição): low 15.000 · lower_mid 11.000 · upper_mid 8.500 ·
  high 6.000 hab/km².
- **Densidade** — para as 117 observadas, `density_hab_km2` é a **densidade do próprio Atlas**
  (coluna "Built-up Area Density (persons/ha)" ×100, mesmo ano/perímetro — correção pós-verificação:
  não se mistura população de aglomeração 2026 com área do Atlas 2014). Para as 717 derivadas, a
  densidade é a faixa do proxy. Teto de plausibilidade 60.000 hab/km² é **erro** no validador
  (máximo real observado: Daca 55.200).
- **População da aglomeração** — `agglomeration_population` vem de `tools/agglomerations_2026.csv`
  (citypopulation.de, Thomas Brinkhoff, 2026): **379/834 cidades casadas** por nome/ISO3 (o campo
  `population` de `cities.json` é a população nacional rateada, não a aglomeração — não serve para
  densidade). 455 cidades fora da tabela (aglomerações < 1 M) seguem com o valor de cenário,
  sinalizado.
- **Reserva expansível** — sempre derivada (`reserve_method="derived:coast+landmass"`):
  `built_up × 0,60`, ×0,55 se costeira (tem porto em `ports.csv`), ×0,40 se ilha, teto 3×.
  **Relevo NÃO é modelado** (o heightmap do projeto é placeholder plano) — limitação declarada.
- **Zoneamento** — shares por faixa de renda (referência: Atlas of Urban Expansion / OECD),
  `estimated=true` (residencial 0,55–0,68 / comercial 0,17–0,32 / industrial 0,13–0,15).
- **Gabarito** — regra por limiar: `population ≥ 5 M` → high; `≥ 1 M` ou capital → mid; senão low.

## 3. housing.json (habitação por nação)

- **Casa própria** — **43 observadas** (OECD HM1.3, 36 casadas a 0,00 de desvio + JPN censo
  nacional 2023 + HUN + ISR) + **136 proxy** por faixa de renda (0,62–0,78), `estimated=true`.
  Nota: Japão diverge da OECD (OECD 84% vs. censo 60,9%) — optou-se pelo censo nacional, registrado.
- **Favela (ODS 11.1.1)** — **169 observadas** (World Bank `EN.POP.SLUM.UR.ZS`, conferidas na API
  viva) + **10 sem dado** (`null` + `no_data`, países de alta renda sem medição).
- **Tamanho do domicílio** — **155 observadas** (UN DESA Household Size and Composition 2022) +
  **24 proxy**.
- **Aluguel/renda** — **41 observadas** (OECD) + **138 proxy** por faixa de renda (0,25–0,35).
- **Estoque por padrão (popular/médio/alto)** — derivado do **Gini** (`inequality.json`), coerente
  com `income_distribution.json`: renda lognormal (σ = 2·erfinv(Gini), média = 2,0 cestas) com
  limiares 1× (popular) e 7× (alto). As 179 somam 1,0; `stock_split_source="derived:lognormal_from_gini"`.

## 4. municipal_finance.json (finanças municipais por nação)

Campos: `subnational_revenue_pct_gdp`, `property_tax_pct_total_tax`,
`central_transfers_pct_subnational`, cada um com `*_source`, `*_estimated`, `*_year`; confiança da
entrada `complete`/`partial`/`estimated`/`no_data` pela mistura.

- **Receita subnacional % PIB** — **37 observadas** (OECD Subnational / CFE SNG) · 133 proxy · 9 null.
- **Imposto sobre imóveis % da tributação** — **98 observadas** (OECD Revenue Statistics série 4100
  ÷ tributação total) · 72 proxy · 9 null. Conferidas na SDMX viva (USA 10,43 · DEU 0,98 · CAN 7,56 ·
  BRA 1,92 · CHN 2,71).
- **Transferências do governo central** — **37 observadas** (OECD) · 133 proxy · 9 null.
- Proxy por faixa de renda + flag de federação (documentado). **9 nações sem `gdp_ppp_bn`**
  (CUB, ERI, PRK, SAH, SOL, SSD, TWN, VEN, YEM) ficam `no_data` (não se inventa PIB/faixa).

## 5. Fontes

Atlas of Urban Expansion 2016 · citypopulation.de (Major Agglomerations 2026) · OECD (Housing
tenure HM1.3, Housing costs, Revenue Statistics 4100, Subnational Finance CFE SNG) · World Bank
(SDG 11.1.1 `EN.POP.SLUM.UR.ZS`) · UN DESA (Household Size and Composition).

## 6. Lacunas conhecidas (honestas)

1. **Densidade/área observada** cobre as 117 maiores do Atlas; as outras 717 são proxy de renda.
2. **Relevo** não entra na reserva expansível (heightmap placeholder).
3. **Municipal**: receita subnacional e transferências observadas só nos 37 membros OECD; IMF GFS
   não fechado nesta rodada (mapeamento de códigos) — proxy para o resto.
4. **Casa própria/aluguel** fora da OECD é proxy por renda.
5. **Japão**: casa própria usa censo nacional (60,9%), divergindo da OECD (84%) — registrado.
6. **9 nações** sem PIB PPP ficam `no_data` em municipal_finance (proxy de renda indisponível).

## 7. Verificação

Verificador-flash (independente) conferiu amostra contra as fontes vivas: **APROVADO** após 2
correções — (a) densidade urbana agora vem da coluna do próprio Atlas (era população 2026 ÷ área
2014, inflada); (b) `municipal_finance` ganhou flags `*_estimated` por campo (61 nações mistas
estavam rotuladas `complete`). Áreas do Atlas 117/117 exatas; homeownership OECD 36/36 a 0,00 de
desvio; favela/tamanho de domicílio/estoque por padrão conferidos; imposto predial OECD 7/7 exato.

## 8. Regenerar

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"
& $py tools\build_urban_land.py ; & $py tools\validate_urban_land.py
& $py tools\build_housing.py   ; & $py tools\validate_housing.py
& $py tools\build_municipal_finance.py ; & $py tools\validate_municipal_finance.py
```
