# E1 — Dados de energia: parque, matriz nacional, potencial e acesso

Escopo: **só dado**. Nada foi ligado à simulação, ao núcleo C++, a `core/`, `game/`,
`map/` ou UI. Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE E, E1 (e insumos de
E5/E7). Branch `data/e1-energy`, worktree `G:/projetos/opengs-wt-data-e1`, base atual
`balance/global-resources` @ `9568996d` (merge `0edecf64`).

O aceite do E1 é: **por nação, capacidade instalada (MW) e matriz por fonte na ordem do
dado real 2024–2026; usinas individuais atribuídas à província/cidade dona; potencial
solar/eólico e hídrico derivado por província, normalizado por potencial técnico nacional
público; perdas T&D e acesso por país; licença conferida e registrada; lacuna declarada
onde não há dado.**

## 1. Artefatos

| Arquivo | Conteúdo | Registros | SHA-256 |
|---|---|---|---|
| `data/definitions/energy/national_capacity.json` | capacidade instalada e matriz por nação (2024–2025) + desvio do parque por nação/fonte | 171 nações, 158 comparações | `C215AA99…E373651D` |
| `data/definitions/energy/power_plants_2026.json` | usinas individuais (nome, fonte, MW, lat/lon) atribuídas a cidade | 34.936 usinas / 5.707 GW / 744 cidades | `D8A30AA6…AB0519DC` |
| `data/definitions/energy/renewable_potential.json` | solar e eólico por província/cidade (mensal) + proxy hídrico bruto | 15.547 províncias / 849 cidades | `28314216…D796F62` |
| `data/definitions/energy/hydro_potential.json` | potencial hídrico em MW por província/cidade, normalizado pela âncora nacional | 179 nações (135 com âncora) | `389F9716…071D5B9` |
| `data/definitions/energy/access_losses.json` | acesso à eletricidade (total/urbano/rural), perdas T&D, água segura | 217 países (173 jogáveis) | `DEDF622E…791AA07` |
| `data/definitions/energy/land_borders.json` | pares de nações com fronteira terrestre (E5) | 299 pares / 152 nações | `5465B7E5…290F8451` |
| `data/definitions/energy/_sources.json` | registro único de fonte, URL, acesso, licença, evidência e transformação | 9 fontes (6 usadas, 1 restrita, 2 recusadas) | `101A375C…0CB8116` |

Scripts reprodutíveis (todos determinísticos — 2+ execuções com SHA-256 idêntico):

```text
tools/data/energy/fetch_ember_capacity.py     cache Ember (CSV ~16 MB, não versionado)
tools/data/energy/fetch_wb_hydro_anchor.py    cache do indicador do Banco Mundial
tools/data/energy/build_national_capacity.py  -> national_capacity.json
tools/data/energy/build_hydro_national.py     -> hydro_potential.json
tools/data/energy/build_sources_registry.py   -> _sources.json
tools/build_energy_plants.py                  -> power_plants_2026.json     (GPPD)
tools/build_energy_renewables.py              -> renewable_potential.json   (WorldClim)
tools/build_energy_access.py                  -> access_losses.json         (WDI)
tools/build_energy_borders.py                 -> land_borders.json          (atlas)
tools/build_energy_common.py                  geografia/atribuição comum
tools/validate_energy_data.py                 validador (55.200 checagens)
```

Caches baixados ficam em `tools/data/` (gitignored, com manifest de URL + SHA-256 + data de
acesso). Nada acima de 20 MB é versionado.

## 2. Fontes e licenças (verificado em 2026-11-21)

| Fonte | Uso | Licença | Situação |
|---|---|---|---|
| **GPPD v1.3.0** (WRI, 2021) | usinas individuais | **CC BY 4.0** | usada |
| **Ember — Yearly Electricity Data** (release global, 2026) | capacidade e matriz nacional 2024–2025 | **CC BY 4.0** | usada |
| **Banco Mundial Data360 — `WB_CLEAR_HP_TECHNFEASIBLE_PT`** (2021) | âncora do potencial hídrico | **CC BY 4.0** | usada |
| **Banco Mundial WDI** via Data360 (acesso, perdas T&D; água = JMP) | acesso e perdas | **CC BY 4.0** | usada |
| **Natural Earth 10m** (rios, admin) | rios do proxy hídrico, divisas | **domínio público** | usada |
| **WorldClim 2.1** (`srad`, `wind`, `elev`; precip em `province_nature.json`) | solar, eólico e desnível | **uso acadêmico/não comercial; redistribuição e uso comercial proibidos sem permissão** | ⚠️ **usada com bloqueio** |
| **IRENA ELECSTAT** | comparação de capacidade | termos IRENA art. 4.1 (**não comercial**) | **recusada** — removida das saídas |
| **Global Energy Monitor (trackers)** | usinas | CC BY 4.0, mas o download exige atestar “não uso comercial” | **recusada** |
| **IJHD — Hydropower Essentials** | número global de potencial (16.400 TWh/ano) | citação de fato, com atribuição | só citação |

**Bloqueio de licença (decisão do dono necessária).** O WorldClim declara, na página
`worldclim.org/about.html`: *“The data are freely available for academic use and other
non-commercial use. Redistribution or commercial use is not allowed without prior
permission.”* Isso **não** é CC BY. A camada solar/eólica e o desnível do proxy hídrico
dependem dela — e o clima do próprio jogo (`province_nature.json`) já usa a mesma fonte,
então é dívida do projeto, não do E1. Antes de release comercial: (a) pedir permissão
escrita a `info@worldclim.org`, ou (b) trocar por **Global Solar Atlas / Global Wind Atlas**
(CC BY 4.0, ESMAP) + MDE público (SRTM/GMTED, domínio público). O `_sources.json` carrega
`used_restricted` + `blocker`; `validate_energy_data.py --require-commercial` falha de
propósito nesse estado.

**IRENA e GEM ficaram fora.** A comparação nacional que antes usava a IRENA passou a usar a
Ember (CC BY 4.0); as chaves `national_comparison` com números da IRENA saíram de
`power_plants_2026.json` (a tabela da coleta anterior continua no histórico do git).

## 3. Matriz nacional 2024–2025 (Ember)

Regra: para cada país, a linha `Capacity (GW)` do **último ano com capacidade positiva entre
2024 e 2025** (nunca interpola, nunca extrapola). Ano com só zeros não conta (ex.: KGZ 2025
trazia apenas `Other renewables 0.0`; vale 2024). `year` fica explícito por país: **79
países em 2025, 92 em 2024**.

Mapeamento para as 9 fontes do jogo: `Hydro→hydro`, `Solar→solar`, `Wind→wind`,
`Coal→coal`, `Gas→gas`, `Nuclear→nuclear`, `Bioenergy→biomass`,
`Other fossil + Other renewables → other`. **Óleo não é separado pela Ember** (vem dentro de
`Other fossil`), então `oil: null` em todos os países, com `other_breakdown.other_fossil_mw`
e o motivo em `meta.gaps.oil` — a IRENA separa óleo, mas não pode ser usada comercialmente.
**Nenhum número foi inventado.**

Mundo (soma dos 171 países com dado): **10.033,4 GW**, 0 divergência entre a soma das fontes e
o total da Ember (tolerância 0,5 MW; `check.sum_matches_total` verdadeiro em 171/171).

| Fonte | Mundo (GW) |
|---|---|
| solar | 2.348,7 |
| carvão | 2.214,5 |
| gás | 2.058,5 |
| hídrica | 1.286,1 |
| eólica | 1.283,6 |
| nuclear | 393,1 |
| outras | 319,5 |
| biomassa | 129,4 |
| óleo | **lacuna** (dentro de “outras”) |

### 10 maiores por MW (cap = 2024–2025)

| # | País | Ano | Total GW | hídrica | solar | eólica | carvão | gás | nuclear | biomassa | outras |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | CHN | 2025 | 3.726,5 | 382 | 1.202 | 641 | 1.243 | 161 | 62 | 19 | 17 |
| 2 | USA | 2025 | 1.333,2 | 84 | 212 | 159 | 189 | 550 | 97 | 10 | 33 |
| 3 | IND | 2025 | 555,5 | 49 | 136 | 55 | 251 | 27 | 9 | 11 | 19 |
| 4 | JPN | 2025 | 322,9 | 28 | 92 | 6 | 55 | 82 | 33 | 6 | 20 |
| 5 | BRA | 2025 | 260,6 | 110 | 65 | 35 | 3 | 19 | 2 | 18 | 8 |
| 6 | DEU | 2024 | 248,2 | 6 | 91 | 73 | 32 | 32 | 0 | 9 | 5 |
| 7 | RUS | 2025 | 237,5 | 52 | 2 | 3 | 37 | 113 | 29 | 0 | 1 |
| 8 | FRA | 2025 | 167,6 | 24 | 31 | 26 | 2 | 18 | 61 | 2 | 4 |
| 9 | KOR | 2025 | 158,9 | 2 | 30 | 2 | 42 | 50 | 26 | 3 | 3 |
| 10 | CAN | 2025 | 156,0 | 84 | 6 | 18 | 3 | 27 | 13 | 3 | 2 |

### Cobertura e lacunas (179 nações do jogo)

- **171 com matriz 2024–2025**; **8 sem dado** na janela: `CAF, DMA, LKA, PSX, SAH, SOL, TLS,
  UKR`. Em CAF/DMA/LKA/UKR a Ember publica o país mas **sem capacidade** em 2024/2025 (a
  Ucrânia tem série interrompida); PSX/SAH/SOL/TLS não existem como país na Ember. O último
  recurso (nada de valor inventado) é a lacuna declarada em `meta.gaps.nations_without_ember`.
- Ano-base 2024 em 92 países é **dado real**, não estimativa: a Ember ainda não publicou 2025.
- Ember tem 209 áreas; 38 não são nação do jogo (agregados, territórios).

## 4. Parque instalado individual (GPPD, ~2021)

- 34.936 usinas / **5.707 GW**; 34.934 com cidade, 34.899 com nação; **744 cidades** em 161
  nações. Cada registro é `method: gppd_2021_unadjusted` — **nada extrapolado para 2026**.
- Atribuição: lat/lon → pixel do mapa → província no raster `world_province.png` → cidade dona
  pela réplica de `GeographyService.assign_provinces` (partição por área). 2 usinas sem
  província (ponto fora do raster) ficam com `note: no_province_at_point`.
- **Conflito de nação declarado**: em **277 usinas** a nação da província do atlas difere do
  `country_iso3` do GPPD; cada uma leva `location_conflict: "<nação do raster>"` e o
  `meta.location_conflicts.top_pairs` registra os pares:

  | Par (GPPD → atlas) | Usinas |
  |---|---|
  | PRT → ESP | 36 |
  | FRA → MDG | 25 |
  | UKR → RUS | 21 (19,6 GW — Crimeia/território ocupado) |
  | FRA → DMA | 20 |
  | CAN → USA | 10 |
  | USA → CAN | 8 |
  | FRA → LCA | 8 |
  | DEU → CHE | 6 |
  | ESP → MAR | 6 |
  | SGP → MYS | 5 |

  A usina **fica** na cidade dona da província (a regra do jogo); o conflito é registro, não
  correção — corrigir exige fonte. O validador falha se aparecer conflito **não** declarado.

## 5. Capacidade nacional derivada das usinas × agregado nacional

Comparação por nação e por fonte em `national_capacity.json › plant_comparison` (158 nações
com parque e matriz). O desvio esperado é **negativo**: o GPPD é de ~2021 e cobre menos usinas
que a Ember de 2024–2025.

- **Total: mediana −43,8 %**; só 6 nações ficam acima do agregado.
- Desvio mediano por fonte: carvão −0,6 %, nuclear −1,7 %, hídrica −22,2 %, gás −41,4 %,
  solar −97,3 %, eólica −95,7 %, biomassa/outras −100 %.
- Piores negativos: SUR −99,3 %, LCA −96,7 %, AGO −83,5 %, NPL −83,3 %, BEN −82,2 %.
- Positivos: LUX +48,8 %, ISL +12,4 %, XKX +11,1 %, PRK +8,2 %, TTO +4,1 %.

Leitura: **o GPPD não serve como total de MW de 2026**; serve como nome, fonte, coordenada e
“quem existe”. A matriz de 2024–2025 é a da Ember (§3). Para o E2 montar o parque inicial há
duas rotas explícitas (decisão do dono): (a) usar as usinas do GPPD e **escalonar a capacidade
por fonte até o total nacional da Ember** (transformação declarada, `method` novo); ou
(b) obter uma base de usinas 2024–2026 com licença comercial (GEM, com isenção pleiteada) —
nada disso deve ser feito por extrapolação silenciosa.

## 6. Potencial solar e eólico por província

- WorldClim 2.1 `srad` e `wind` (10 arc-min, mensal). Amostra no centróide da província;
  centróide no mar usa o pixel de terra mais próximo (`sample_offset_px`). **0 províncias sem
  amostra.**
- Por província: média anual (kWh/m²/dia e kWh/m²/ano; m/s) e mês mínimo/máximo. Por cidade:
  média ponderada pela área × fração da província, com a **série de 12 meses** (fator de
  capacidade do E2). 849 cidades.
- Sanidade (nação → solar kWh/m²/dia · vento m/s): NGA 4,80 · 2,08; AUS 5,72 · 3,18;
  BRA 4,31 · 1,50; DEU 2,85 · 3,33; USA 3,96 · 3,55; NOR 2,32 · 3,67.
- Limites declarados: 10 arc-min é grosso para sítio e o `wind` do WorldClim suaviza relevo
  (subestima vento). E a licença é o bloqueio do §2.

## 7. Potencial hídrico normalizado pela âncora nacional

O proxy bruto por província continua sendo
`raw = precip/1000 × área × desnível/1000 × runoff × river_factor` (Budyko simples para
runoff; `river_factor` = cobertura a ≤ 50 km de rio Natural Earth, piso 0,02), com
`confidence: low`. O que o E1 acrescenta é a **normalização nacional**:

```text
potencial_nacional_mw = hidro_instalada_2021_mw / (developed_pct_2021 / 100)
potencial_província_mw = potencial_nacional_mw × raw_província / Σ raw_nação
```

- `developed_pct_2021`: Banco Mundial Data360 `WB_CLEAR_HP_TECHNFEASIBLE_PT` (CC BY 4.0);
  `hidro_instalada_2021`: Ember (CC BY 4.0). O denominador não é a mesma publicação do Banco
  Mundial (Atlas Hidrelétrico 2021), então o valor **carrega esse desvio** — por isso a
  confiança é `low` em toda nação, nunca `high`.
- A soma das províncias de cada país é **igual** à âncora nacional (resto de arredondamento na
  província de maior `raw`); as frações somam 1. O teto de MW por cidade/fonte do E2 fecha por
  construção com a fonte pública.
- **135 das 179 nações** com âncora; **44 sem** (`national_anchor_missing`): o indicador não
  publica potencial desenvolvido para elas (desertos, ilhas, estados minúsculos) — lacuna
  declarada, não zero inventado. EGY e NAM têm âncora mas o proxy é 0 em todas as províncias
  (sem rio/desnível amostrado): `distribution_missing`, províncias nulas.
- Mundo declarado: **4.642 GW** (soma das 171 âncoras disponíveis). Controle de consistência:
  4.642 GW × 8.760 h × fator de capacidade 0,40 ≈ **16,3 PWh/ano**, coerente com a âncora
  global independente de 16.400 TWh/ano (IJHD) usada no proxy antigo.
- Top do potencial (MW · % já desenvolvido em 2021): CHN 711.572 · 49,8 %; RUS 412.426 ·
  12,4 %; USA 387.666 · 21,7 %; BRA 345.360 · 31,7 %; IND 187.120 · 25,0 %; CAN 178.764 ·
  46,4 %; VEN 169.307 · 10,0 %; COD 163.046 · 1,6 %. Sanidade: NOR 72.207 · 47,2 %;
  NPL 98.303 · 2,0 %; PRT 14.561 · 49,8 %; CHE é o mais desenvolvido (99,1 %).
- 254 províncias (1,6 %) ficam `no_hydro_data` (sem precipitação/área/desnível) e **não**
  recebem zero: ficam nulas.

## 8. Acesso, perdas e água (E5/E7)

- Ano mais recente não vazio por país/indicador; ausente = `null`, sem interpolação.
- Cobertura entre 217 países: acesso à eletricidade 216, urbano/rural 214, **perdas T&D 150**,
  água segura 151 (urbano 104, rural 85). 173 das 179 nações jogáveis têm algum dado; 6 não
  (`PSX, SAH, SOL, SSD, TWN, XKX`).
- Amostra (acesso total/urb/rural · perdas · água): BRA 99,8/99,9/99,2 · 14,9 % · 88,6;
  NGA 62,5/85,5/23,5 · 14,1 % · 29,9; NOR 100/100/100 · 4,6 % · 99,1; CHN 100/100/100 ·
  3,1 % · (só urbana 96,4).
- Perdas T&D são a referência do piso técnico do E5 (o piso nunca chega a zero).

## 9. Fronteiras terrestres (E5)

Derivadas do raster de donos com máscara de terra, 4-vizinhança, comprimento com cosseno da
latitude, pares < 30 km descartados: 299 pares entre 152 nações. Controles negativos passaram
(FRA-GBR, DNK-SWE, IDN-SGP, RUS-USA, CHN-TWN, ESP-MAR, ITA-TUN ausentes); positivos coerentes
(CHN 14 vizinhos, BRA 10, DEU 9, FRA 8, NOR 3, USA 2). A **lista de pares** é o dado; o km é
±20 % (CAN-USA 9.841 km vs ~8.900 oficial).

## 10. Validação

```text
python tools/validate_energy_data.py
55200 checagens | 0 falhas | 2 avisos
```

Checagens: licença completa (nome, `commercial_ok`, data e evidência) para toda fonte e todo
arquivo consumidor existente; matriz com as 9 fontes, ano na janela, soma das fontes = total
(≤ 0,5 MW) e óleo como lacuna; cobertura 171 + 8 = 179; usinas com coordenada no mundo,
província do **mesmo** país (conflito só se declarado), cidade da mesma nação, soma por nação
= soma das usinas; solar/eólico em faixa física e série mensal de 12 meses; hidro com soma das
províncias = âncora nacional (≤ 0,01 MW) e frações = 1; percentuais de acesso em 0..100;
fronteiras sem par repetido/consigo mesma.

Avisos esperados: (1) WorldClim restrito (§2); (2) 277 usinas com conflito de nação declarado
(§4). Saídas byte-idênticas em execuções repetidas.

## 11. Lacunas e o que fica para E2 (decisão declarada, não implementada)

1. **Licença do WorldClim** (§2) — bloqueia release comercial de solar/eólico/relevo.
2. **Óleo separado** — a Ember não separa; a IRENA pode, mas é não comercial. Lacuna
   registrada (`oil: null`).
3. **8 nações sem matriz 2024–2025** e **44 sem âncora hídrica** — lacunas por fonte ausente,
   não por falha de coleta.
4. **Parque 2021 vs 2026** — o GPPD serve de inventário de usinas, não de total MW. E2 precisa
   da decisão (a) escalonar por fonte até a Ember ou (b) base de usinas recente com licença
   comercial (GEM com isenção). Nada foi extrapolado.
5. **`location_conflict`** (277 usinas) precisa de regra no E2/E5: a usina fica na cidade da
   província do atlas; o país do GPPD é registro.
6. **Geração por fonte** (TWh) está disponível na mesma base da Ember e ainda não foi usada —
   candidata natural ao fator de capacidade do E2.
7. **Água** é só distribuição (E7); não há hidrologia nesta fase.

## 12. Achado de qualidade fora do escopo

`data/definitions/natural/province_areas.json` tem células degeneradas (ex.: província 14087
“Tasmania 1” com 1.781.428 km²; AUS soma 12,1 M km² contra 7,7 M reais). Os agregados de
energia **não** usam esse arquivo: a área vem do raster do atlas (total 145,9 M km², coerente
com a terra emersa). Vale corrigir `tools/compute_province_areas.py` na frente do mapa.
