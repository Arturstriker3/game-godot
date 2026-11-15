# Dados iniciais do mundo — desigualdade e propriedade (só dados)

> Fonte de verdade dos dados reais de **desigualdade de renda** e **propriedade inicial**
> (estoque de IDE + setores estatais) por nação. Estes arquivos **ainda não são consumidos
> pelo núcleo** — a integração acontece depois do refactor do contrato de dados (ver
> `GAME_STATE.md` §6). Nada de regra econômica muda aqui.

Arquivos:

- `data/definitions/economy/inequality.json` — Gini por nação (0..100).
- `data/definitions/economy/ownership_baseline.json` — estoque de IDE (in/out) e setores estatais.
- `tools/build_inequality.py` / `tools/validate_inequality.py`
- `tools/build_ownership_baseline.py` / `tools/validate_ownership_baseline.py`

Reprodução (cache em `tools/data/`, gitignored):

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"
& $py tools\build_inequality.py
& $py tools\build_ownership_baseline.py
& $py tools\validate_inequality.py          # exit 0
& $py tools\validate_ownership_baseline.py  # exit 0
```

---

## 1. Desigualdade (`inequality.json`)

**Fonte primária:** World Bank, indicador **SI.POV.GINI** (escala 0..100), ano mais recente
com valor não nulo entre 2000 e 2025.
- URL: <https://data.worldbank.org/indicator/SI.POV.GINI>
- API: `https://api.worldbank.org/v2/country/all/indicator/SI.POV.GINI?format=json&per_page=30000&date=2000:2025`

**Cobertura:** **179/179 nações** (chave = `id` de `data/definitions/scenario/nations.csv`).

- **154 observadas** (Gini do World Bank, valor + ano reais): `confidence` `complete`
  (ano ≥ 2015) ou `partial` (ano < 2015).
- **25 estimadas** (`estimated: true`, `source: "regional_estimate"`, `confidence: "estimated"`):
  nações sem Gini observado no World Bank.
- **0 no_data.**

**Método das 25 estimativas regionais:** valor regional fixo (faixa típica World Bank PIP /
OECD IDD), **nunca apresentado como medição da nação**. Valores (0..1, convertidos para 0..100):
africa 43, asia 38, europe 33, middle_east 39, north_america 42, oceania 36, south_america 47.
A região vem da coluna `region` de `nations.csv`.

**Resolução de aliases ISO:** `NED` → `NLD` (25,7 em 2021) e `PSX` → `PSE` (36,4 em 2023)
usam o código real do World Bank; o campo `iso_worldbank` registra o código.

**Lacunas de fonte alternativa:** nesta versão **nenhuma fonte alternativa** (OECD IDD via
SDMX retornou HTTP 403; SWIID e institutos nacionais sem valor verificável) foi usada — as
chaves `oecd_idd`/`swiid`/`national_statistics` existem no `sources` mas estão sem uso.
Exemplos dos 10 países-chave (Gini / ano):

| País | Gini | Ano | Fonte |
|---|---|---|---|
| USA | 41,8 | 2024 | WB SI.POV.GINI |
| DEU | 33,7 | 2022 | WB SI.POV.GINI |
| BRA | 50,3 | 2024 | WB SI.POV.GINI |
| CHN | 36,0 | 2022 | WB SI.POV.GINI |
| IND | 23,0 | 2023 | WB SI.POV.GINI |
| NGA | 33,9 | 2022 | WB SI.POV.GINI |
| ZAF | 54,1 | 2022 | WB SI.POV.GINI |
| NOR | 26,5 | 2023 | WB SI.POV.GINI |
| SAU | 39,0 | — | estimativa regional (middle_east) |
| RUS | 33,0 | 2023 | WB SI.POV.GINI |

> O `income_distribution.json` (Gini 0..1 por 7 regiões, usado hoje pelo núcleo via
> `demo_set_income_gini`) **não foi alterado**. O `inequality.json` é a fonte por nação que o
> refactor vai consumir.

---

## 2. Propriedade inicial (`ownership_baseline.json`)

### 2.1 Estoque de IDE

**Fonte:** FMI **DIP** (Direct Investment Positions by Counterpart Economy, ex-CDIS), ano mais
recente disponível por país (**2024** na quase totalidade).
- URL: <https://data.imf.org/?sk=40313609-F037-48C1-84B1-E1F1CE54D6D5>
- API: `https://api.imf.org/external/sdmx/2.1/data/DIP/{ISO3}....?startPeriod=YYYY&endPeriod=YYYY&format=sdmx-csv` (header `Accept: text/csv`).

Campos por nação: `fdi_inward_stock_usd` (IDE recebido), `fdi_outward_stock_usd` (IDE no
exterior), `fdi_year`, `fdi_source`, `fdi_method` (`reported_total` via contraparte agregada
`G001` ou `sum_counterparties`), `fdi_basis` (`own_report` = DV_TYPE `O`; `mirror_estimate` =
DV_TYPE `SCC`), `fdi_inward_pct_gdp` / `fdi_outward_pct_gdp` (= estoque ÷ PIB nominal), e
`main_investors` (top contrapartes do inward, com `share`).

**PIB nominal (para % do PIB):** World Bank `NY.GDP.MKTP.CD`, mesmo ano do estoque de IDE.

**Cobertura:** **178/179 nações com estoque de IDE** · **1 no_data** (SOL, Somalilândia — sem
código FMI/World Bank). `main_investors` presente quando a fonte publica a contraparte.

**Casos de código ISO:** `NED→NLD`, `PSX→WBG`, `XKX→KOS`, `SAH→ESH` no FMI DIP (o World Bank
usa `PSE`/`XKX` para o PIB). 7 nações com posição líquida negativa reportada (COG, GNQ, GRD,
MOZ, MRT, SSD, TCD) têm o campo anulado com `fdi_note` explicando o valor (não se mantém número
negativo de estoque).

Exemplos (2024, USD bi):

| País | inward | outward | inward/PIB | outward/PIB | base |
|---|---|---|---|---|---|
| USA | 5.707,7 | 6.826,8 | 19,5% | 23,3% | próprio |
| CHN | 3.729,4 | 3.139,9 | 19,9% | 16,8% | próprio |
| DEU | 1.240,3 | 2.208,6 | 26,5% | 47,1% | próprio |
| IND | 803,0 | 136,3 | 21,4% | 3,6% | próprio |
| BRA | 621,2 | 313,1 | 28,4% | 14,3% | próprio |
| ZAF | 115,5 | 153,6 | 28,8% | 38,3% | próprio |
| NOR | 162,1 | 231,5 | 32,4% | 46,2% | próprio |
| RUS | 225,9 | 188,3 | 10,3% | 8,6% | espelho |
| SAU | 70,4 | 80,6 | 5,6% | 6,4% | espelho |
| NGA | 42,1 | 9,9 | 16,7% | 3,9% | próprio |

### 2.2 Peso das estatais (SOE)

**Resultado honesto: 0 nações com percentual numérico.** Não foi encontrada fonte publicada e
verificável de participação das estatais no PIB/emprego/ativos por país (OECD "Ownership and
Governance of SOEs" retornou 403; IMF Fiscal Monitor out/2020 cap. 3 é PDF sem leitor de tabela
acessível; estudos nacionais não publicam % do PIB comparável). Todas as 179 nações têm
`soe_share_gdp: null`, `soe_share_employment: null`, `soe_no_data: true` e
`soe_reason: "sem fonte publicada de peso de estatal"`. **Nenhum percentual foi inventado.**

**Setores estatais documentados (qualitativo):** `state_sectors` + `state_sectors_ref`
(preenchidos em **12 nações**) listam setores/empresas estatais **fato documentado e citado**,
independente do peso numérico: CHN (State Grid, CNPC/Sinopec, China Mobile/Telecom, big four
banks, SASAC), SAU (Aramco 81,48% governo direto, Ma'aden, SEC, stc, PIF), NOR (Equinor 67%,
Statkraft, Statnett, Telenor), RUS (Gazprom 50,23% Federação, Rosneft, Sberbank, RZD, Rostec),
ZAF (Eskom, Transnet, Telkom, Denel), BRA (Petrobras, Banco do Brasil, Caixa, BNDES), IND (SBI,
ONGC/IOCL, Coal India, Indian Railways, LIC), DEU (Deutsche Bahn, Deutsche Telekom, Deutsche
Post via KfW), FRA (EDF 100%, SNCF, Orange), MEX (Pemex, CFE), JPN (Japan Post, NTT, JR), USA
(USPS, Amtrak). Demais 167 nações: `state_sectors: null`.

---

## 3. Lacunas restantes

1. **Peso numérico das estatais** (participação no PIB/emprego/ativos) — bloqueado por falta de
   fonte publicada verificável; nenhum número foi fabricado.
2. **Gini de 25 nações** sem observação no World Bank (Golfo, pequenas ilhas, estados frágeis,
   PRK/CUB/TWN) — hoje estimativa regional marcada; uma rodada futura pode fechar com OECD IDD
   (NED/NZL já resolvidos), SWIID ou institutos nacionais quando a fonte for acessível.
3. **Deciles/quintis** — o `inequality.json` traz deciles (WB `SI.DST.*`) quando disponíveis,
   mas a cobertura é esparsa; WID (World Inequality Database) não foi integrado nesta versão.
4. **Taiwan (TWN)** tem estoque de IDE (espelho) mas sem PIB no World Bank → `fdi_*_pct_gdp`
   null; o caminho é PIB do IMF WEO (fonte nova, não autorizada nesta rodada).
