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

**9 101 pontos anuais** em 136 moedas (8 721 do IFS, 215 do JST, 92 do World Bank, 73 derivados do
encadeamento do euro). Nada é interpolado: ano sem dado não existe no JSON.

## 2. Fontes

| Chave em `source` | Fonte | Indicador | Cobertura | Link |
|---|---|---|---|---|
| `imf_ifs` | IMF *International Financial Statistics* (via DBnomics) | `ENDA_XDC_USD_RATE` — moeda local por USD, média do período, anual | 1950–2024 (varia por país) | <https://data.imf.org/> · API <https://api.db.nomics.world/v22/series/IMF/IFS> |
| `worldbank` | World Bank Open Data | `PA.NUS.FCRF` — taxa oficial de câmbio (LCU por USD, média do período) | 1960–2025 | <https://data.worldbank.org/indicator/PA.NUS.FCRF> |
| `jst_r6` | Jordà–Schularick–Taylor *Macrohistory Database*, release 6 | `xrusd` — moeda nacional por USD, anual | 1870–2020, 18 países | <https://www.macrohistory.net/database/> |
| `irr` | Ilzetzki, Reinhart & Rogoff — *Exchange Rate Arrangements* (de facto) | classificação fina/agregada por país e ano | 1940–2016 | <https://carmenreinhart.com/exchange-rate/> |
| `derived:dem` | encadeamento próprio do euro | DEM/USD ÷ 1,95583 (paridade irrevogável) | 1925–1998 | paridade oficial do euro (1999-01-01) |

Notas de acesso (2026-11-15):

- O banco da PWT (Penn World Table) **não foi usado**: o site responde com certificado inválido
  para o cliente HTTP local e o `xr` é redundante com IFS/World Bank (mesma definição, 1950+).
  Lacuna registrada, sem prejuízo da série.
- A página `carmenreinhart.com/exchange-rate/` publica a classificação (`236_data.xlsx`,
  `238_data.xlsx`) e **não** publica níveis de taxa; os níveis vêm do IFS.
- FRED/ALFRED só duplicaria IFS para os países grandes (e o acesso por CSV expirou no teste);
  não foi incluído para não criar uma quarta fonte concorrente no mesmo ano.

## 3. Cobertura

Resumo (`fx_profiles.json:coverage`): **111 moedas com ≥50 anos, 81 com ≥75 anos, 10 com ≥100
anos** (AUD, CAD, CHF, DKK, EUR, GBP, JPY, NOK, SEK, USD). Confiança: **91 `complete`, 22
`partial`, 23 `estimated`**.

Regra de confiança (sem interpolação):

- `complete` — ≥60 anos **e** ≥95% dos anos de 1950–2024 presentes;
- `partial` — ≥20 anos **e** ≥50% de 1950–2024;
- `estimated` — fora disso (séries curtas: países novos ou que pararam de reportar). Não é
  "número estimado": é **cobertura insuficiente** para o perfil ser representativo.

| Moeda | Anos | Primeiro | Último | Vol. | Máx. dep. | Regime (2007-16) | Confiança |
|---|---|---|---|---|---|---|---|
| AED | 59 | 1966 | 2024 | 0.016 | 0.00 | peg | partial |
| AFN | 66 | 1955 | 2020 | 0.101 | 0.71 | peg | partial |
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
| CUP | 18 | 1957 | 1974 | 0.030 | 0.00 | — | estimated |
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
| KPW | 0 | — | — | — | — | — | estimated |
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
| SLS | 0 | — | — | — | — | — | estimated |
| SOS | 49 | 1950 | 2017 | 0.238 | 1.06 | free_fall | partial |
| SRD | 76 | 1950 | 2025 | 0.578 | 4.83 | peg | complete |
| SSP | 14 | 2011 | 2024 | 0.697 | 2.56 | — | estimated |
| SYP | 73 | 1950 | 2022 | 0.266 | 1.46 | band | complete |
| SZL | 76 | 1950 | 2025 | 0.107 | 0.41 | peg | complete |
| THB | 76 | 1950 | 2025 | 0.060 | 0.28 | band | complete |
| TJS | 34 | 1992 | 2025 | 0.408 | 1.61 | peg | estimated |
| TMT | 8 | 1994 | 2001 | 1.298 | 3.38 | — | estimated |
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
| YER | 34 | 1990 | 2023 | 0.295 | 1.22 | — | estimated |
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
  verificados. O que fica registrado, de forma reprodutível, é `breaks` (saltos > 3× entre anos
  consecutivos da mesma fonte) e `flags: structural_breaks`.

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

Distribuição dos 7 724 anos classificados: `peg` 4 244, `band` 1 639, `union` 797, `free_fall`
563, `dual_market` 362, `float` 119. **Não há classificação publicada depois de 2016** — 2017–2025
ficam sem `regime` no JSON e o perfil usa a moda de 2007–2016.

## 7. Como reproduzir e validar

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"   # requer openpyxl
& $py tools/build_fx_history.py          # baixa o que falta (cache em tools/data/fx_cache/)
& $py tools/build_fx_history.py --check  # só reporta cobertura, não escreve
& $py tools/validate_fx_history.py       # falha (exit != 0) se algo estiver inconsistente
```

`tools/data/fx_cache/` é **gitignored** (o `.gitignore` já ignora `tools/data/`), então o
repositório guarda só o resultado derivado. O validador confere: toda moeda de
`currencies.json` tem perfil e arquivo; anos crescentes e únicos; taxa finita > 0; fonte
conhecida; regime no enum; estatísticas do perfil recalculadas a partir da série; resumo de
cobertura; UTF-8 sem BOM e LF.

## 8. Limitações e lacunas (declaradas, não preenchidas)

| Situação | Moedas | O que o dado diz |
|---|---|---|
| Sem série anual em nenhuma fonte | `KPW`, `SLS` | `no_data: true`; o jogo deve exibir ausência |
| Série interrompida pela fonte | `TMT` (1994–2001), `CUP` (1957–1974), `AFN` (1955–2020), `SOS` (1950–2017), `YER` (1990–2023) | últimos anos ausentes; nada extrapolado |
| Fim de série recente | `ERG`, `SYR`, `PRK` etc. | último ano publicado aparece em `last_year` |
| Sem classificação IRR | `TWD`, `XKX`, `SSP`, `TMT`… | `regime_recent: null` |
| Unidade do último ponto | `ZWG` | o IFS encerra `ZW` em 2024 ainda em unidade ZWL; a introdução do ZiG (2024-04) não está encadeada na fonte — usar o último ano com cautela |
| Pré-1950 | todas exceto 18 países do JST | começa em 1950; alvo de 1925 só é atingido por AUD, CAD, CHF, DKK, EUR, GBP, JPY, NOK, SEK, USD |
| Câmbio real | todas | não há CPI por país/ano na base; a meia-vida é calculada no **log do nível nominal** contra tendência móvel de 10 anos, como proxy declarada |

## 9. Uso previsto na fase de política cambial

1. **Inércia do passo diário**: `volatility_annual_log / sqrt(365)` dá a ordem de grandeza do
   passo diário típico; o limite por país pode ser `k ×` esse valor.
2. **Probabilidade de crise**: `crisis_frequency` (BRL 0,45; ARS 0,46; NGN 0,14; CHF 0,01)
   alimenta a chance anual de um evento extremo, com `max_1y_depreciation_log` como teto.
3. **Reversão à média**: `mean_reversion_half_life_years` define a velocidade do retorno após um
   choque; `null` significa "não estimável" (não usar 0).
4. **Regime**: `regime_recent` decide se o país nasce com passo cambial (`float`/`band`) ou fixo
   (`peg`/`union`), substituindo o `regime: null` atual de `currencies.json` quando a F5 evoluir.
5. **Confiança**: `estimated` não deve receber o mesmo peso de calibração que `complete`.
