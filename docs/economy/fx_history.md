# Histórico de câmbio — base de calibração (`data/definitions/economy/fx_history/`)

> **Escopo:** dados. Este diretório só descreve o passado observado; nenhuma regra de jogo é
> alterada por ele. O núcleo C++ continua sem passo cambial (F5 roda com taxa fixa porque
> `currencies.json` declara `regime: null`). Quando a fase de câmbio flutuante existir, estes
> perfis é que limitam o passo por país (inércia) e a probabilidade de crise.

## 1. Para que serve

O dono pediu o histórico de câmbio de **cada moeda do jogo** para calibrar como uma moeda reage:
volatilidade típica, inércia (meia-vida de reversão), frequência de crises e regime predominante.
Sem isso, um passo cambial arbitrário produziria valorizações/desvalorizações irreais.

Saída:

| Arquivo | Conteúdo |
|---|---|
| `data/definitions/economy/fx_history/<CODE>.json` | série anual `ano → taxa (LCU por USD)`, fonte por ano e regime de fato quando classificado |
| `data/definitions/economy/fx_profiles.json` | um perfil derivado por moeda (volatilidade, p95, crise, meia-vida, regime, confiança) + resumo de cobertura |

**9 321 pontos anuais** em 136 moedas (8 721 do IFS, 215 do JST, 92 do World Bank, 73 derivados do
encadeamento do euro e **220 da reconstrução histórica documentada**). Fora da camada de reconstrução
nada é interpolado: ano sem dado não existe no JSON. A reconstrução só preenche anos ausentes, marca
cada linha com `source: historical_reconstruction` + `method` + `ref` e **nunca** sobrescreve fonte
estatística (ver §7).

## 2. Fontes

| Chave em `source` | Fonte | Indicador | Cobertura | Link |
|---|---|---|---|---|
| `imf_ifs` | IMF *International Financial Statistics* (via DBnomics) | `ENDA_XDC_USD_RATE` — moeda local por USD, média do período, anual | 1950–2024 (varia por país) | <https://data.imf.org/> · API <https://api.db.nomics.world/v22/series/IMF/IFS> |
| `worldbank` | World Bank Open Data | `PA.NUS.FCRF` — taxa oficial de câmbio (LCU por USD, média do período) | 1960–2025 | <https://data.worldbank.org/indicator/PA.NUS.FCRF> |
| `jst_r6` | Jordà–Schularick–Taylor *Macrohistory Database*, release 6 | `xrusd` — moeda nacional por USD, anual | 1870–2020, 18 países | <https://www.macrohistory.net/database/> |
| `irr` | Ilzetzki, Reinhart & Rogoff — *Exchange Rate Arrangements* (de facto) | classificação fina/agregada por país e ano | 1940–2016 | <https://carmenreinhart.com/exchange-rate/> |
| `derived:dem` | encadeamento próprio do euro | DEM/USD ÷ 1,95583 (paridade irrevogável) | 1925–1998 | paridade oficial do euro (1999-01-01) |
| `historical_reconstruction` | Reconstrução histórica documentada (curadoria do projeto) | fato por ano: paridade, desvalorização, reforma monetária, taxa oficial ou de mercado — arquivo `tools/data/fx_historical_facts.json` | 1925–2025, só nas moedas com lacuna + regime de fato 2017–2025 | `reconstruction.refs` com título, URL e ano em cada `<CODE>.json` |

Notas de acesso (2026-11-15):

- O banco da PWT (Penn World Table) não entra como fonte primária (o site responde com certificado
  inválido e o `xr` é redundante com IFS/World Bank na maior parte dos países). Ele é usado **apenas
  como fato documentado da Somália em 1994–2005**, via a série `FXRATESOA618NUPN` do FRED (§7), onde
  IFS e World Bank não publicam.
- A página `carmenreinhart.com/exchange-rate/` publica a classificação (`236_data.xlsx`,
  `238_data.xlsx`) e **não** publica níveis de taxa; os níveis vêm do IFS.
- FRED/ALFRED só duplicaria IFS para os países grandes; a exceção declarada é a Somália
  (`FXRATESOA618NUPN`, PWT 7.1), cuja lacuna 1994–2005 nenhuma fonte primária cobre.

## 3. Cobertura

Resumo (`fx_profiles.json:coverage`): **113 moedas com ≥50 anos, 84 com ≥75 anos, 11 com ≥100
anos** (AUD, CAD, CHF, CUP, DKK, EUR, GBP, JPY, NOK, SEK, USD). Confiança: **91 `complete`, 22
`partial`, 23 `estimated`**, das quais **4 foram rebaixadas pela fração reconstruída** (CUP, KPW, SOS
e AFN).

Regra de confiança (sem interpolação fora da reconstrução):

- `complete` — ≥60 anos **e** ≥95% dos anos de 1950–2024 presentes;
- `partial` — ≥20 anos **e** ≥50% de 1950–2024;
- `estimated` — fora disso (séries curtas: países novos ou que pararam de reportar). Não é
  "número estimado": é **cobertura insuficiente** para o perfil ser representativo;
- **teto por reconstrução** (camada de fatos, §7): fração reconstruída ≥ 50% → `estimated`; ≥ 10%
  → no máximo `partial`. A confiança **nunca sobe** por reconstrução; `confidence_base` guarda a
  classificação antes do teto.

| Moeda | Anos | Primeiro | Último | Vol. | Máx. dep. | Regime (2007-16) | Confiança |
|---|---|---|---|---|---|---|---|
| AED | 59 | 1966 | 2024 | 0.016 | 0.00 | peg | partial |
| AFN | 75 | 1950 | 2024 | 0.102 | 0.71 | peg | partial |
| ALL | 33 | 1992 | 2024 | 0.116 | 0.35 | peg | estimated |
| AMD | 32 | 1993 | 2024 | 0.628 | 3.46 | peg | estimated |
| AOA | 75 | 1950 | 2024 | 0.811 | 3.84 | peg | complete |
| ARS | 62 | 1963 | 2024 | 0.758 | 3.88 | peg | partial |
| AUD | 100 | 1925 | 2024 | 0.086 | 0.36 | float | complete |
| AZN | 33 | 1992 | 2024 | 0.525 | 2.75 | peg | estimated |
| BAM | 28 | 1997 | 2024 | 0.076 | 0.18 | peg | estimated |
| BBD | 75 | 1950 | 2024 | 0.018 | 0.14 | peg | complete |
| BDT | 54 | 1971 | 2024 | 0.072 | 0.39 | peg | partial |
| BGN | 56 | 1953 | 2024 | 0.348 | 2.25 | peg | partial |
| BHD | 59 | 1966 | 2024 | 0.016 | 0.00 | peg | partial |
| BIF | 74 | 1950 | 2023 | 0.094 | 0.52 | peg | complete |
| BND | 75 | 1950 | 2024 | 0.037 | 0.12 | band | complete |
| BOB | 75 | 1950 | 2024 | 0.801 | 4.94 | peg | complete |
| BRL | 76 | 1950 | 2025 | 0.742 | 3.18 | band | complete |
| BSD | 85 | 1940 | 2024 | 0.002 | 0.00 | peg | complete |
| BTN | 75 | 1950 | 2024 | 0.066 | 0.29 | peg | complete |
| BWP | 75 | 1950 | 2024 | 0.085 | 0.38 | peg | complete |
| BYN | 31 | 1994 | 2024 | 0.416 | 1.69 | peg | estimated |
| BZD | 75 | 1950 | 2024 | 0.033 | 0.21 | peg | complete |
| CAD | 100 | 1925 | 2024 | 0.049 | 0.19 | band | complete |
| CDF | 74 | 1950 | 2023 | 0.999 | 6.16 | peg | complete |
| CHF | 100 | 1925 | 2024 | 0.089 | 0.27 | band | complete |
| CLP | 75 | 1950 | 2024 | 0.393 | 2.11 | band | complete |
| CNY | 68 | 1957 | 2024 | 0.082 | 0.40 | peg | partial |
| COP | 75 | 1950 | 2024 | 0.122 | 0.52 | band | complete |
| CRC | 75 | 1950 | 2024 | 0.135 | 0.93 | peg | complete |
| CUP | 72 | 1925 | 2025 | 0.021 | 0.08 | — | estimated |
| CZK | 33 | 1993 | 2025 | 0.088 | 0.17 | peg | estimated |
| DJF | 76 | 1950 | 2025 | 0.014 | 0.00 | peg | complete |
| DKK | 101 | 1925 | 2025 | 0.103 | 0.53 | peg | complete |
| DOP | 76 | 1950 | 2025 | 0.171 | 1.14 | peg | complete |
| DZD | 76 | 1950 | 2025 | 0.118 | 0.72 | peg | complete |
| EGP | 76 | 1950 | 2025 | 0.160 | 0.71 | peg | complete |
| ERN | 68 | 1958 | 2025 | 0.091 | 0.58 | peg | partial |
| ETB | 75 | 1950 | 2024 | 0.102 | 0.58 | peg | complete |
| EUR | 100 | 1925 | 2025 | 0.156 | 0.29 | union | complete |
| FJD | 76 | 1950 | 2025 | 0.062 | 0.32 | peg | complete |
| GBP | 101 | 1925 | 2025 | 0.088 | 0.30 | float | complete |
| GEL | 30 | 1996 | 2025 | 0.104 | 0.38 | peg | estimated |
| GHS | 76 | 1950 | 2025 | 1.111 | 1.40 | band | complete |
| GMD | 76 | 1950 | 2025 | 0.108 | 0.58 | band | complete |
| GNF | 71 | 1950 | 2020 | 0.323 | 2.62 | peg | partial |
| GTQ | 76 | 1950 | 2025 | 0.098 | 0.63 | peg | complete |
| GYD | 76 | 1950 | 2025 | 0.193 | 1.04 | peg | complete |
| HNL | 66 | 1950 | 2025 | 0.017 | 0.06 | peg | partial |
| HTG | 76 | 1950 | 2025 | 0.101 | 0.49 | peg | complete |
| HUF | 58 | 1968 | 2025 | 0.097 | 0.21 | peg | partial |
| IDR | 59 | 1967 | 2025 | 0.199 | 1.24 | peg | partial |
| ILS | 76 | 1950 | 2025 | 0.327 | 1.65 | band | complete |
| INR | 76 | 1950 | 2025 | 0.065 | 0.29 | peg | complete |
| IQD | 75 | 1950 | 2025 | 0.926 | 7.87 | peg | complete |
| IRR | 74 | 1950 | 2023 | 0.390 | 2.96 | peg | complete |
| ISK | 76 | 1950 | 2025 | 0.181 | 0.80 | band | complete |
| JMD | 76 | 1950 | 2025 | 0.143 | 0.71 | peg | complete |
| JOD | 76 | 1950 | 2025 | 0.059 | 0.43 | peg | complete |
| JPY | 101 | 1925 | 2025 | 0.355 | 3.26 | float | complete |
| KES | 76 | 1950 | 2025 | 0.093 | 0.59 | peg | complete |
| KGS | 32 | 1994 | 2025 | 0.137 | 0.63 | peg | estimated |
| KHR | 56 | 1954 | 2025 | 0.188 | 0.76 | peg | partial |
| KPW | 37 | 1978 | 2024 | 0.127 | 0.43 | — | estimated |
| KRW | 76 | 1950 | 2025 | 0.227 | 1.10 | band | complete |
| KWD | 76 | 1950 | 2025 | 0.025 | 0.07 | peg | complete |
| KZT | 32 | 1994 | 2025 | 0.147 | 0.54 | peg | estimated |
| LAK | 69 | 1957 | 2025 | 0.549 | 1.10 | peg | partial |
| LBP | 76 | 1950 | 2025 | 0.420 | 2.22 | peg | complete |
| LKR | 76 | 1950 | 2025 | 0.094 | 0.56 | peg | complete |
| LRD | 75 | 1950 | 2024 | 0.379 | 3.17 | peg | complete |
| LSL | 76 | 1950 | 2025 | 0.107 | 0.41 | peg | complete |
| LYD | 76 | 1950 | 2025 | 0.164 | 1.18 | peg | complete |
| MAD | 76 | 1950 | 2025 | 0.075 | 0.27 | peg | complete |
| MDL | 31 | 1995 | 2025 | 0.149 | 0.67 | peg | estimated |
| MGA | 76 | 1950 | 2025 | 0.126 | 0.47 | band | complete |
| MKD | 32 | 1994 | 2025 | 0.086 | 0.22 | peg | estimated |
| MMK | 71 | 1950 | 2020 | 0.573 | 4.77 | peg | partial |
| MNT | 35 | 1991 | 2025 | 0.430 | 2.11 | peg | estimated |
| MRU | 73 | 1950 | 2023 | 0.070 | 0.33 | peg | complete |
| MWK | 74 | 1950 | 2023 | 0.155 | 0.69 | peg | complete |
| MXN | 76 | 1950 | 2025 | 0.215 | 0.87 | band | complete |
| MYR | 76 | 1950 | 2025 | 0.059 | 0.33 | band | complete |
| MZN | 75 | 1950 | 2024 | 0.270 | 1.97 | band | complete |
| NAD | 64 | 1962 | 2025 | 0.115 | 0.41 | peg | partial |
| NGN | 76 | 1950 | 2025 | 0.241 | 1.44 | peg | complete |
| NIO | 76 | 1950 | 2025 | 1.158 | 7.87 | peg | complete |
| NOK | 101 | 1925 | 2025 | 0.101 | 0.39 | band | complete |
| NPR | 74 | 1952 | 2025 | 0.070 | 0.24 | peg | complete |
| NZD | 76 | 1950 | 2025 | 0.087 | 0.21 | band | complete |
| OMR | 76 | 1950 | 2025 | 0.025 | 0.14 | peg | complete |
| PAB | 86 | 1940 | 2025 | 0.000 | 0.00 | union | complete |
| PEN | 76 | 1950 | 2025 | 0.670 | 4.26 | band | complete |
| PGK | 76 | 1950 | 2025 | 0.084 | 0.37 | peg | complete |
| PHP | 76 | 1950 | 2025 | 0.112 | 0.61 | band | complete |
| PKR | 76 | 1950 | 2025 | 0.095 | 0.60 | peg | complete |
| PLN | 76 | 1950 | 2025 | 0.343 | 1.89 | band | complete |
| PYG | 76 | 1950 | 2025 | 0.180 | 0.88 | band | complete |
| QAR | 60 | 1966 | 2025 | 0.017 | 0.01 | peg | partial |
| RON | 69 | 1957 | 2025 | 0.327 | 1.39 | peg | partial |
| RSD | 29 | 1997 | 2025 | 0.338 | 1.69 | peg | estimated |
| RUB | 31 | 1993 | 2025 | 0.219 | 0.93 | band | estimated |
| RWF | 76 | 1950 | 2025 | 0.115 | 0.62 | peg | complete |
| SAR | 74 | 1952 | 2025 | 0.028 | 0.18 | peg | complete |
| SBD | 76 | 1950 | 2025 | 0.065 | 0.26 | peg | complete |
| SDG | 73 | 1950 | 2022 | 0.713 | 2.64 | peg | complete |
| SEK | 101 | 1925 | 2025 | 0.099 | 0.36 | band | complete |
| SGD | 76 | 1950 | 2025 | 0.037 | 0.12 | band | complete |
| SLE | 74 | 1950 | 2023 | 0.239 | 1.15 | peg | complete |
| SLS | 26 | 2000 | 2025 | 0.047 | 0.09 | float | estimated |
| SOS | 64 | 1950 | 2025 | 0.243 | 1.06 | free_fall | partial |
| SRD | 76 | 1950 | 2025 | 0.578 | 4.83 | peg | complete |
| SSP | 14 | 2011 | 2024 | 0.697 | 2.56 | — | estimated |
| SYP | 73 | 1950 | 2022 | 0.266 | 1.46 | band | complete |
| SZL | 76 | 1950 | 2025 | 0.107 | 0.41 | peg | complete |
| THB | 76 | 1950 | 2025 | 0.060 | 0.28 | band | complete |
| TJS | 34 | 1992 | 2025 | 0.408 | 1.61 | peg | estimated |
| TMT | 32 | 1994 | 2025 | 0.687 | 3.38 | peg | estimated |
| TND | 76 | 1950 | 2025 | 0.069 | 0.20 | peg | complete |
| TRY | 76 | 1950 | 2025 | 1.311 | 0.99 | band | complete |
| TTD | 76 | 1950 | 2025 | 0.058 | 0.38 | peg | complete |
| TWD | 75 | 1950 | 2024 | 0.094 | 0.54 | — | complete |
| TZS | 75 | 1950 | 2024 | 0.139 | 0.68 | peg | complete |
| UAH | 33 | 1993 | 2025 | 0.438 | 1.98 | peg | estimated |
| UGX | 76 | 1950 | 2025 | 0.332 | 1.91 | band | complete |
| USD | 101 | 1925 | 2025 | 0.000 | 0.00 | union | complete |
| UYU | 76 | 1950 | 2025 | 0.819 | 7.00 | band | complete |
| UZS | 19 | 1995 | 2025 | 0.202 | 0.64 | peg | estimated |
| VES | 68 | 1950 | 2017 | 0.206 | 0.87 | peg | partial |
| VND | 40 | 1983 | 2024 | 0.488 | 2.05 | peg | partial |
| XAF | 76 | 1950 | 2025 | 0.116 | 0.67 | union | complete |
| XCD | 76 | 1950 | 2025 | 0.028 | 0.19 | union | complete |
| XOF | 76 | 1950 | 2025 | 0.116 | 0.67 | union | complete |
| YER | 36 | 1990 | 2025 | 0.287 | 1.22 | — | estimated |
| ZAR | 76 | 1950 | 2025 | 0.107 | 0.41 | band | complete |
| ZMW | 76 | 1950 | 2025 | 0.852 | 0.98 | band | complete |
| ZWG | 65 | 1950 | 2025 | 1.937 | 13.45 | free_fall | partial |

## 4. Formato

`fx_history/<CODE>.json`:

```json
{
 "version": 1, "code": "BRL", "nations": ["BRA"],
 "unit": "LCU per USD (annual average, unidade monetaria corrente da fonte)",
 "first_year": 1950, "last_year": 2024,
 "sources": ["imf_ifs"], "source_refs": { "...": "citação de cada fonte" },
 "regime_enum": ["peg","band","float","free_fall","union","dual_market"],
 "breaks": [{"year": 1986, "log_change": 1.2, "note": "salto >3x ..."}],
 "notes": ["..."], "missing_source_area": ["..."], "no_data": false,
 "series": [
   {"year": 1950, "rate": 8.0997e-14, "source": "imf_ifs", "regime": "peg", "regime_fine": 4},
   {"year": 2024, "rate": 5.38893490571, "source": "imf_ifs"}
 ]
}
```

- `rate` = **unidades de moeda local por 1 USD** (média anual). É o inverso de `rate_per_anchor`
  de `currencies.json` (que é XGS por unidade local); a conversão é feita no consumidor.
- `series` é ordenada por ano e sem lacunas declaradas: ano ausente = sem dado na fonte.
- `regime` só aparece quando o IRR classifica aquele país/ano (até 2016).
- `no_data: true` + `missing_source_area` = a moeda não tem série anual em nenhuma fonte
  (hoje: `KPW`, `SLS`). O jogo deve mostrar estado vazio, não um número.

`fx_profiles.json:currencies.<CODE>`:

| Campo | Método |
|---|---|
| `years_with_data`, `first_year`, `last_year`, `coverage_1950_2024` | contagem direta sobre a série |
| `volatility_annual_log` | desvio-padrão amostral de `ln(taxa_t / taxa_{t-1})` |
| `median_annual_log_change` | mediana de `ln(taxa_t / taxa_{t-1})` |
| `p95_abs_annual_log_change` | p95 de \|variação anual\| |
| `max_1y_depreciation_log` | maior variação anual positiva (depreciação) |
| `crisis_frequency` | fração dos anos com depreciação > 25% (`crisis_threshold_log`) |
| `mean_reversion_half_life_years` | AR(1) dos desvios do log do nível contra a média móvel centrada de 10 anos; `null` se φ∉(0,1) |
| `regime_recent`, `regime_window`, `regime_distribution` | moda e contagem do regime IRR (janela recente 2007–2016) |
| `confidence`, `flags` | regra acima; `flags` explica encadeamento/extensão/falhas |

## 5. Redenominações e encadeamento

O ponto crítico: uma moeda que trocou de unidade não pode gerar um salto artificial de 10³/10⁶×.

- **IFS e World Bank já entregam a série encadeada na unidade monetária corrente.** Conferido
  nos casos pedidos: `BRL` 1950 = 8,1e-14 BRL/USD (cruzeiro de 1950 ÷ cadeia de 1967/1986/1989/
  1993/1994), `ARS` 1970 = 3,8e-11, `TRY` 1970 = 1,1e-5, `ZMW` 1950 = 7,1e-4 (libra da Rodésia
  do Norte × 2 ÷ 10³ de 2013). Por isso o perfil usa **log-variações**, imunes à escala de nível.
- **Euro (`EUR`)** não existia antes de 1999: a série é `DEM/USD ÷ 1,95583` até 1998
  (fonte `derived:dem`, 73 pontos) e a **área euro do IFS (`U2`)** de 1999 em diante. Cada troca
  está em `notes` e em `sources`.
- **Pré-1950 (`jst_r6`)**: a extensão 1925–1949 só é aceita se a razão com o primeiro ano do IFS
  ficar em **[0,5; 2,0]** — proteção contra mudança de unidade não encadeada. Rejeições vão para
  `notes` e o ano simplesmente não entra.
- **Eventos individuais de redenominação não são tabelados à mão.** As fontes já aplicam a cadeia
  e fatores oficiais variam de país para país; repeti-los aqui seria afirmar números não
  verificados. A exceção é a camada de reconstrução (§7): quando uma lacuna só pode ser preenchida
  atravessando uma reforma monetária, o fator é declarado (`unit_note`) e o ano de mudança de base
  entra em `stat_breaks`. O que continua registrado automaticamente é `breaks` (saltos > 3× entre
  anos consecutivos da mesma fonte) e `flags: structural_breaks`.

## 6. Regime de fato (Ilzetzki–Reinhart–Rogoff)

`238_data.xlsx`, worksheet `Fine` (1940–2016), lido por país/ano e agregado pelo mapa oficial
`fine → coarse` da worksheet `Classification`:

| `regime` do JSON | Códigos finos | Significado no jogo |
|---|---|---|
| `union` | 1 | sem moeda própria / união monetária |
| `peg` | 2–8 (coarse 1–2) | peg, currency board, crawling peg |
| `band` | 9–12 (coarse 3) | banda / flutuação administrada |
| `float` | 13 (coarse 4) | flutuação livre |
| `free_fall` | 14 (coarse 5) | queda livre (> ~40%/ano) |
| `dual_market` | 15 (coarse 6) | mercado duplo sem dado de paralelo |

Distribuição dos **7 855** anos classificados: `peg` 4 266, `band` 1 646, `union` 797, `free_fall`
563, `dual_market` 398, `float` 185 — sendo **7 724 do IRR** (1940–2016) e **131 da camada de fatos**
(2017–2025, §7). O IRR não publica classificação depois de 2016; para 2017–2025 o `regime` vem de
fato documentado e a linha carrega `regime_source: historical_reconstruction` + `regime_note`.
Ano sem classificação continua sem `regime` (o perfil usa a moda de 2007–2016 em `regime_recent` e
o último ano classificado em `regime_latest`).

## 7. Reconstrução histórica (último recurso, marcado)

Em 2026-11-15 o dono autorizou completar lacunas de fonte **apenas** com fatos históricos
documentados, sempre marcados e citados. A camada vive em `tools/data/fx_historical_facts.json`
(versionado, legível) e é aplicada por `tools/build_fx_history.py` **depois** do merge estatístico.

Regras aplicadas (todas conferidas por `tools/validate_fx_history.py`):

1. **Nunca sobrescreve dado estatístico** — a camada só preenche anos ausentes nas fontes primárias.
2. Todo ano reconstruído traz `source: historical_reconstruction`, `method`, `basis` e `ref`, e a
   `ref` resolve em `reconstruction.refs` com `title`, `url` e `year` (o validador exige os três).
3. **Interpolação só entre dois fatos documentados**: `method: interpolated_between_documented_facts`,
   `interpolated: true`, `anchor: [ano_inicial, ano_final]`; lacuna de mais de 10 anos **não** é
   interpolada (fica ausente e o motivo vai para `notes`).
4. `stat_breaks` marca mudança de base (oficial→mercado, ou reforma monetária): a variação daquele
   ano **fica fora** da volatilidade e da frequência de crise; `volatility_documented_only_log`
   repete o cálculo só com anos não interpolados.
5. A confiança é limitada pela fração reconstruída (§3) e `confidence_base` guarda a classificação
   anterior ao teto.
6. Nenhuma moeda é estendida a período em que não existia (YER antes de 1990, TMT antes de 1993) e
   pré-1950 só entra com paridade/fato documentado (CUP, paridade com o dólar de 1915 a 1959-60).

### 7.1 O que foi preenchido

| Moeda | Anos na série | Reconstruídos | Interpolados | O que a série passa a representar | Referências |
|---|---|---|---|---|---|
| CUP | 101 (1925–2025) | 83 | 0 | paridade 1 CUP/USD (1925–1956 e 1975–1989), câmbio extraoficial anual da CEPAL (1990–2003: 7,0 → 95,0 em 1994 → 26,0 em 2003) e caixa ao público (2004+: 25 → 23,15 em 2005–2011 → 24 desde 2021) | Cuban peso (Wikipedia), Ruiz Cruz 2009, CEPAL 1999/2005, Mesa-Lago 2001, González-Corzo 2006 |
| KPW | 48 (1978–2025) | 48 | 9 | peg de 2,16 won (1978–2001) e won de **mercado** 2002–2025 (4,5 em 2002 → 2.050 em 2010 → ~8.000 em 2013–2020 → 31.300 em 2025) | CIA Factbook archive, NK Economy Watch, Liganda, Ministério da Unificação (Yonhap), BTI 2022, Daily NK, Asia Press, KDI |
| SLS | 26 (2000–2025) | 26 | 20 | taxa oficial do Banco da Somalilândia e taxa de mercado (2000–2025) | Somaliland shilling (Wikipedia) |
| SOS | 76 (1950–2025) | 27 | 7 | mercado livre do xelim somali (1990–1993, 1994–2005 pelo Penn World Table 7.1, 2006–2015, 2018–2025) | Somali shilling (Wikipedia), World Bank, Penn World Table 7.1/FRED, UNDP Somalia 2001 |
| TMT | 32 (1994–2025) | 24 | 0 | paridade 5.200 TMM/USD e desvalorização para 14.250 (2008); 2,85 e 3,50 TMT × 5.000 (2009–2025) | RFE/RL 2008, IMF F&D 2013, export.gov 2015, CIA Factbook archive |
| AFN | 76 (1950–2025) | 10 | 4 | oficial de 1950, IFS 1955–2020 e médias anuais do DAB/Banco Mundial/IRS em 2021–2025 (84,5 → 89,9 → 82,6 → 70,6 → 69,6) | Afghan afghani (Wikipedia), World Bank AFM (jan/2023 e dez/2025), DAB, IRS |
| YER | 36 (1990–2025) | 2 | 1 | mercado de Aden em jan/2025 (2.150 YER/USD) | Yemeni rial (Wikipedia) |
| ARS, TRY, EGP, NGN, LBP, RUB | inalterados | 0 | 0 | só o **regime de fato 2017–2025** (nenhum nível de taxa) | Reuters/BCRA 2025, IMF, bancos centrais, Wikipedia |

Total: **220 anos reconstruídos** (41 deles interpolados entre fatos) em 7 moedas; `KPW` e `SLS`
deixam de ser `no_data`. Interpolações declaradas nesta rodada (2026-11-15) respeitam o limite de
**5 anos entre dois fatos documentados**; as lacunas antigas do arquivo seguem o limite de 10 anos.

### 7.2 Notas por moeda

- **KPW (Coreia do Norte).** Peg oficial de ₩2,16/USD de 1978 a 2001 (a data é o aniversário de Kim
  Jong Il) e abandono do peg em 2001. A reforma de nov/2009 (100 won antigos = 1 novo) obriga a
  encadear: 2,16 ÷ 100 = 0,0216 na unidade corrente. De 2002 a 2025 a série usa o **won de mercado**
  ("untied"), não o "tied" oficial (~104/USD em 2012 e ~100/USD em 2025): 4,5 em 2002 (300–600 won
  antigos), 22,5 em 2005, 27,5 em 2006, 32,0 em 2008, 35 em dez/2009, 2.050 em 2010, 3.800 em 2011,
  8.000 em 2012–2013 e ~8.000 estável até 2020 (8.136 em fev/2019). `stat_breaks: [2002]` marca a
  passagem do peg oficial para o mercado. Em 2024 o won desaba (8.300 em jan → 18.000 em nov) e em
  2025 a média das cotações reportadas é ~31.300 (21.000 em jan–mai, 43.400 em ago, 35.300 em nov);
  os valores 8.900 (taxa oficial fixada em abr/2024) e 18.000 (nov/2024) são níveis pontuais e não a
  média do ano. 2014–2018 e 2022 são interpolações curtas (≤5 anos) entre fatos documentados.
- **SLS (Somalilândia).** Moeda criada em 18/10/1994 a 1 SLS = 100 xelins somalis, mas sem nível em
  USD documentado antes de nov/2000. Âncoras: 4.550 (2000), 7.500 (2008), 6.000 (2015), 8.500
  (2019), 8.530 (2022), 10.000 (2025). O "peg" de 580/12 citado na fonte não tem referência e foi
  ignorado.
- **TMT (Turcomenistão).** A série estatística termina em 2001 a 5.200 TMM/USD, então a
  reconstrução é expressa em **manat antigo** para não criar salto artificial: 2002–2007 a 5.200;
  2008 a 14.250; 2009–2014 a 14.250 (= 2,85 TMT × 5.000); 2015–2025 a 17.500 (= 3,50 × 5.000). O
  mercado paralelo (40–41 TMT/USD em 2021, ~57× a taxa oficial) é ilegal e não entra.
- **CUP (Cuba).** Paridade com o dólar de 1915 a 1959-60 (1925–1956) e de novo 1 CUP/USD de 1975 a
  1989 (a paridade oficial sobreviveu até 2005; a taxa comercial do Estado era 0,74 e a de remessas
  chegou a 1,35). Em 1990 o país entra no câmbio paralelo: a série usa a **média anual extraoficial**
  da CEPAL (7,0 em 1990; 78,0 em 1993; 95,0 em 1994; 19,2 em 1996; 21,0 em 2000; 26,0 em 2001–2003),
  que é a mesma cotação adotada pela CADECA em out/1995. `stat_breaks: [1990]` marca oficial→paralelo.
  A partir de 2004 usa a taxa de **caixa ao público** (25 CUP/USD, com 23,15 em 2005–2011 quando o CUC
  valia 1,08 USD) e, depois da unificação de 01/01/2021, 24 CUP/USD. O mercado informal posterior a
  2004 (100–400 CUP/USD em 2022–2025) não entra: não há série anual, só cotações pontuais.
- **AFN (Afeganistão).** 1950 = 25,5 (taxa oficial; mercado livre 39,0), 1955–2020 vem do IFS e
  2021–2025 usa as **médias anuais** do DAB/Banco Mundial e da série do IRS: 84,5 (2021), 89,9 (2022),
  82,6 (2023), 70,6 (2024) e 69,6 (2025). O valor anterior de 65 AFN/USD para 2024 não tinha data de
  fechamento e divergia da série mensal do DAB (dez/2024: 70,3); foi corrigido. O DAB fechou
  31/12/2025 em 66,07 AFN/USD (compra/venda).
- **SOS (Somália).** Sem autoridade monetária efetiva desde 1991: a série usa mercado livre
  (3.470 no fim de 1990, 2.000 em jun/1991, 5.000 em jun/1993, 13.400 em 2006 … 23.000 em 2015,
  26.000 em 2025). `stat_breaks: [1990]` marca oficial→mercado. A lacuna de 1994–2005 é preenchida
  com a **média anual do Penn World Table 7.1** (via FRED): 4.338 (1994), 9.572 (2000), 20.025
  (2002, pico) e 15.251 (2005); as observações pontuais de 1991 e 1993 foram mantidas e a divergência
  com a média anual do PWT está declarada nas `notes`.
- **YER (Iémen).** Moeda só existe desde a unificação de 1990 — nada é reconstruído antes disso. O
  nível de 2025 (2.150 em Aden) e o de 2024 (interpolado) representam o mercado do governo
  reconhecido; o norte houthi negocia a ~530 YER/USD (câmbio duplo, documentado no regime).
- **ARS, TRY, EGP, NGN, LBP, RUB.** Só regime de fato, sem inventar taxa: ARS (flutuação 2017, banda
  2018, `dual_market` 2019–2023, crawling peg 2024, banda móvel 1.000–1.400 desde 11/04/2025);
  TRY (flutuação, banda em 2021, `peg` de fato em 2022 com depósitos protegidos, flutuação desde
  jun/2023); EGP (`dual_market` em 2023, unificação em 06/03/2024); NGN (múltiplas janelas até 2022,
  flutuação desde 12/06/2023); LBP (`peg` 1.507,5 até 2019, `dual_market` 2020–2023, flutuação com
  oficial ~89.500 desde 2024); RUB (`dual_market` em 2022 com controles de capital, flutuação
  administrada desde 2023).

## 8. Como reproduzir e validar

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"   # requer openpyxl
& $py tools/build_fx_history.py          # baixa o que falta (cache em tools/data/fx_cache/)
& $py tools/build_fx_history.py --check  # só reporta cobertura, não escreve
& $py tools/validate_fx_history.py       # falha (exit != 0) se algo estiver inconsistente
```

`tools/data/fx_cache/` é **gitignored** — só `tools/data/fx_historical_facts.json` está versionado
(por exceção explícita no `.gitignore`). O validador confere: toda moeda de `currencies.json` tem
perfil e arquivo; anos crescentes e únicos; taxa finita > 0; fonte conhecida; regime no enum; refs
com título/URL/ano em toda linha reconstruída; consistência `interpolated` ↔ `method`; estatísticas
do perfil recalculadas a partir da série (com `stat_breaks` fora das variações); confiança = base
limitada pela fração reconstruída; resumo de cobertura; o próprio arquivo de fatos; UTF-8 sem BOM e LF.

## 9. Limitações e lacunas (declaradas, não preenchidas)

| Situação | Moedas | O que o dado diz |
|---|---|---|
| Sem série anual em nenhuma fonte | `SSP` (2011–2024 apenas), `ZWG` (buracos 2009–2019) | série curta; o jogo deve exibir ausência fora do intervalo |
| Lacunas que a reconstrução **não** cobriu | `KPW` 1950–1977; `SLS` 1950–1999; `TMT` 1950–1993; `YER` 1950–1989 | não há fato documentado (moeda/peg ainda não existia ou não há nível em USD): o ano fica ausente |
| Fim de série recente | `ERG`, `SYR`, `PRK` etc. | último ano publicado aparece em `last_year` |
| Sem classificação de regime | `TWD`, `XKX`, `SSP`… | `regime_recent: null`; 2017–2025 só existe para as 13 moedas do arquivo de fatos |
| Unidade do último ponto | `ZWG` | o IFS encerra `ZW` em 2024 ainda em unidade ZWL; a introdução do ZiG (2024-04) não está encadeada na fonte — usar o último ano com cautela |
| Pré-1950 | todas exceto 19 séries (18 países do JST + `CUP`) | começa em 1950; alvo de 1925 é atingido por AUD, CAD, CHF, DKK, EUR, GBP, JPY, NOK, SEK, USD e CUP (paridade documentada) |
| Câmbio real | todas | não há CPI por país/ano na base; a meia-vida é calculada no **log do nível nominal** contra tendência móvel de 10 anos, como proxy declarada |
| Estimativas de guerra civil | `AFN` 1992–2001, `SOS` 1990–1993 | as fontes divergem por ordens de grandeza; os pontos escolhidos estão citados e a interpolação é explícita |

## 10. Uso previsto na fase de política cambial

1. **Inércia do passo diário**: `volatility_annual_log / sqrt(365)` dá a ordem de grandeza do
   passo diário típico; o limite por país pode ser `k ×` esse valor. Para séries com reconstrução,
   preferir `volatility_documented_only_log` quando ele existir (isola o efeito da interpolação).
2. **Probabilidade de crise**: `crisis_frequency` (BRL 0,45; ARS 0,46; NGN 0,14; CHF 0,01)
   alimenta a chance anual de um evento extremo, com `max_1y_depreciation_log` como teto.
3. **Reversão à média**: `mean_reversion_half_life_years` define a velocidade do retorno após um
   choque; `null` significa "não estimável" (não usar 0).
4. **Regime**: `regime_recent` decide se o país nasce com passo cambial (`float`/`band`) ou fixo
   (`peg`/`union`), substituindo o `regime: null` atual de `currencies.json` quando a F5 evoluir.
5. **Confiança**: `estimated` não deve receber o mesmo peso de calibração que `complete`.
