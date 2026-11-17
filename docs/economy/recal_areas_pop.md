# Recalibração de dados — área de província (raster) + população de Mazar-i-Sharif

**Data:** 2026-11-17 · **Branch:** `recal/data-areas-pop` (worktree `G:/projetos/opengs-wt-recal`,
base `balance/global-resources` @ `0a27150`) · **Escopo:** integrar dois commits de dado
(`6d253b0` Q-016 e `56a5797` áreas de província) e medir o efeito econômico no mundo de 365 d
com IA e camada urbana (S19). **Nenhuma regra de simulação, constante ou subsídio foi criada.**

> **Rodada 2 (recal2, mesmo dia).** O head decidiu **não reverter** a correção de área e tratar
> Singapura como **erro de dado**: a cidade-estado não tinha porto no cenário. O porto `SGP-SIN`
> entrou pelo pipeline de curadoria (`FORCED_PORTS`) e o mundo foi remedido (§9–§13). O veredito
> anterior (§4–§6) fica como histórico do ANTES; os números correntes são os da rodada 2.

## 1. O que foi integrado

| Merge | Origem | Conteúdo |
|---|---|---|
| `5e70f6c` | `data/q016-city-pop` @ `6d253b0` | `AFG-MAZ` (Mazar-i-Sharif) `sim_population` 523.300 → **484.500** (CSO 2020); derivados `population` regional, `housing_city.json`, `urban_land.json`, `buildings.json/csv` regerados no próprio commit. [doc](../economy/q016_city_pop_fix.md) |
| `6b2662e` | `data/province-area-fix` @ `56a5797` | `data/definitions/natural/province_areas.json` regerado pelo **raster do atlas** (`world_province.png` ∩ terra do `world_sdf.png` ∩ `nav_mask.png`) em vez do Voronoi de centróides em graus. A Antártida deixa de ser rateada para Tasmânia/Western Australia/Magallanes/Southland/Anosy: **806 províncias** fora da faixa [0,5×–2×], **9,65 M km² fantasma**; área cultivável somada das 834 cidades **−15,3%**. [doc](../economy/province_area_fix.md) |

Build debug próprio da worktree:

```powershell
cmake -S gdextension -B gdextension/build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DGODOTCPP_DIR=G:/projetos/war-economy/godot-cpp
cmake --build gdextension/build-debug
```

DLL entregue: `gdextension/bin/economy_gdextension.windows.template_debug.x86_64.dll`,
SHA-256 `22D800917FE84E2AF6E6083D01B3E27C9E4EE4C68EC9AEED97542B1149678C98` (27.341.312 B).
Nenhuma DLL de outra frente foi usada; a pasta principal (`G:/projetos/opengs-game`) não foi tocada.

## 2. Derivados que dependem de Godot (diga quais)

**Nenhum arquivo derivado precisou ser regerado por Godot.** Justificativa medida no código:

- A **fazenda inicial não está assada** em `buildings.json`: o CSV traz
  `output_tons_per_day = população_sim × 0,0003` (`tools/import_world_sources.py`, regra
  `POP_PER_TON`), sem nenhuma dependência de área de província. O dimensionamento por área é
  feito **em runtime** por `GeographyService.resolve_initial_farms()` — aptidão e
  `city_land_capacity_t_day()` vêm da área cultivável lida de `province_areas.json`, e a
  capacidade é limitada por `min(land_cap)` (`capacity > land_cap → capacity = land_cap`).
  Ou seja, o dado novo entra sozinho no próximo boot; regerar o CSV sobrescreveria o valor por
  população por um valor derivado de área, mudando a regra.
- `housing_city.json` / `urban_land.json` / `cities.json` / `buildings.csv` foram regerados no
  próprio commit Q-016 (pipeline Python, `validate_urban_land` → `OK: 834 cidades`).
- `province_nature.json` (aptidão/clima por província) **não** depende de área: é fração
  herdada do território natural.

## 3. Medição

Sweep paralelo (5 variantes de aceite + sonda de câmbio), igual ao padrão Q-016:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1 -OutPrefix recal_
```

- 365 d, `--ai=1 --urban=1 --threads=1`, 6 processos (5 variantes + `--fx=float`).
- Comparação **por nação** contra `docs/economy/perf/q16_world_sweep.json` (baseline do Q-016,
  mesmo pipeline e mesmo binário/source: a mudança é **só de dado**).
- Portões: falências de construção ≤ 28; drift de dinheiro ≤ 0,01; comida ≤ 2,5 p.p. de queda,
  exceto importadores aceitos (SOL, DJI, SAH, JOR, YEM); sonda `--fx=float` tem de diferir do
  peg (hash de trajetória).

## 4. Resultado do sweep ANTES × DEPOIS

Baseline ANTES: `docs/economy/perf/q16_world_sweep.json` (HEAD `0a27150`, mesmo binário/fonte,
365 d, `--ai=1 --urban=1 --threads=1`, pior caso das 5 variantes). DEPOIS:
`docs/economy/perf/recal_world_sweep.json` (este commit), mesma metodologia.

### 4.1 Portões

| Portão | Limite | Medido | Veredito |
|---|---|---|---|
| Falências de construção (pior variante) | ≤ 28 | **9** (9 nas 5 variantes) | OK |
| Dinheiro conservado (drift) | ≤ 0,01 | **−0,0005** em todas as 5 | OK |
| Determinismo peg ×5 | hashes iguais | `7255b0d0…` nas 5 | OK |
| Sonda `--fx=float` difere | hash diferente | `1840929b…` | OK |
| Comida por nação (queda) | ≤ 2,5 p.p., exceto SOL/DJI/SAH/JOR/YEM | **SGP −75,26; LBY −14,78; EGY −10,11** | **REPROVA** |

A sonda `--fx=float` fica fora do portão de comida (é sensibilidade do câmbio); o pior delta dela
é SGP −75,16 p.p.

### 4.2 Nações que pioram > 1 p.p. (todas)

Todas as 5 variantes de aceite fecham o mesmo `trajectory_hash`, então o valor é único:

| Nação | Q-016 (pior) | Recal (pior) | Δ | Status |
|---|---:|---:|---:|---|
| SGP | 0,930 | 0,177 | **−75,26 p.p.** | REPROVA (não é importador aceito) |
| LBY | 1,000 | 0,852 | **−14,78 p.p.** | REPROVA |
| EGY | 1,000 | 0,899 | **−10,11 p.p.** | REPROVA |
| ESP | 0,984 | 0,969 | −1,56 p.p. | piora > 1 p.p., dentro da tolerância |

**As outras 171 nações ficam dentro de ±1 p.p.** — inclusive AUS, CHL, NZL, ARG e ZAF, todas em
**1,000** antes e depois. Melhoras > 1 p.p. no mesmo sweep: SOL 0,556 → 0,663 (+10,77),
PSX 0,224 → 0,290 (+6,58), DJI 0,303 → 0,338 (+3,53), MRT 0,954 → 0,965 (+1,08).

**Leitura curta:** a área cultivável mundial caiu 15,3%, mas a **comida nacional não caiu** em
nenhuma das nações com terra fantasma (AUS/CHL/NZL/ARG/ZAF seguem 1,000). A fazenda inicial
encolheu e a produção efetiva da maioria das cidades não caiu (a aptidão, não a área, era o
limite). O que quebrou foi a nação **sem agricultura nenhuma** (SGP/LBY/EGY), não a agricultura.

## 5. Cidades de AUS / CHL / NZL / ARG / ZAF — antes × depois

Definições (todas medidas, não estimadas):

- **Área cultivável** = `city_cultivable_area_km2()` da bacia de províncias (área × fração
  cultivável).
- **Teto de terra** = `city_land_capacity_t_day()` (área cultivável × produtividade FAO).
- **Fazenda inicial** = `output_tons_per_day` **resolvido** por `resolve_initial_farms()` (inclui
  aptidão e o clamp pelo teto de terra).
- **Produção efetiva** = fazenda inicial × aptidão da cultura escolhida (t/dia).
- **Comida** = produção efetiva ÷ necessidade alimentar da cidade (t/dia); 1,000 = a cidade se
  alimenta só da própria fazenda inicial. **Não** é a comida da nação no sweep (essa está na §4).

Fonte: `tools/_tmp_recal_city_dump.gd` (diagnóstico temporário, não commitado) rodado nas duas
worktrees (HEAD `0a27150` e a recalibrada). Os totais por nação mostram que a perda de área é
concentrada: **AUS −70,2%, CHL −80,8%, NZL −70,5%**; ARG −0,8% e ZAF +1,7%.


#### AUS — Austrália

| Cidade | Área cultivável antes (km²) | Área cultivável depois (km²) | Δ | Teto de terra antes (t/dia) | Teto de terra depois (t/dia) | Fazenda inicial antes (t/dia) | Fazenda inicial depois (t/dia) | Produção efetiva antes (t/dia) | Produção efetiva depois (t/dia) | Comida antes | Comida depois |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| AUS-GEE | 1.292.940 | 55.628 | -95,7% | 1.239.805,4 | 53.342,2 | 4.854,4 | 256,8 | 101,6 | 124,4 | 1,197 | 1,466 |
| AUS-PER | 1.173.091 | 46.851 | -96,0% | 1.124.881,4 | 44.925,9 | 2.178,2 | 1.471,8 | 1.074,2 | 749,3 | 1,502 | 1,048 |
| AUS-DAR | 153.477 | 144.137 | -6,1% | 117.736,1 | 110.571,2 | 180,7 | 276,1 | 72,6 | 108,8 | 1,729 | 2,592 |
| AUS-TOW | 125.391 | 123.757 | -1,3% | 120.238,0 | 118.670,8 | 219,4 | 321,9 | 93,6 | 138,9 | 1,549 | 2,300 |
| AUS-ALI | 95.020 | 95.094 | +0,1% | 91.114,6 | 91.186,5 | 85,9 | 166,9 | 24,8 | 48,0 | 3,192 | 6,177 |
| AUS-MOR | 87.315 | 87.203 | -0,1% | 83.726,9 | 83.619,4 | 60,3 | 134,4 | 29,1 | 64,9 | 9,914 | 22,110 |
| AUS-WEI | 82.618 | 74.287 | -10,1% | 63.378,2 | 56.987,5 | 42,7 | 89,1 | 21,7 | 45,0 | 25,587 | 52,983 |
| AUS-BRO | 76.109 | 70.623 | -7,2% | 58.384,9 | 54.176,7 | 58,7 | 105,9 | 12,4 | 20,5 | 2,809 | 4,650 |
| AUS-CAN | 59.925 | 60.849 | +1,5% | 57.461,9 | 58.348,8 | 203,7 | 257,5 | 134,9 | 168,9 | 1,223 | 1,531 |
| AUS-MEL | 59.503 | 58.665 | -1,4% | 57.057,8 | 56.253,9 | 2.520,2 | 2.543,6 | 1.653,9 | 1.685,4 | 1,014 | 1,034 |
| AUS-ADE | 50.830 | 47.106 | -7,3% | 48.741,3 | 45.170,4 | 1.131,6 | 1.273,4 | 453,1 | 465,4 | 1,028 | 1,056 |
| AUS-SUN | 47.196 | 45.253 | -4,1% | 45.256,3 | 43.393,0 | 283,8 | 316,4 | 133,4 | 151,3 | 1,114 | 1,265 |
| AUS-KAL | 33.418 | 33.648 | +0,7% | 32.044,5 | 32.265,6 | 81,4 | 110,3 | 11,7 | 15,8 | 1,341 | 1,812 |
| AUS-NEW | 19.142 | 19.629 | +2,5% | 18.355,5 | 18.822,6 | 267,5 | 285,6 | 159,5 | 169,4 | 1,046 | 1,111 |
| AUS-KAR | 17.555 | 16.081 | -8,4% | 16.833,8 | 15.419,9 | 45,9 | 61,5 | 6,7 | 8,3 | 1,311 | 1,623 |
| AUS-GOL | 16.672 | 16.759 | +0,5% | 12.789,3 | 12.856,0 | 259,8 | 273,8 | 198,4 | 206,8 | 1,032 | 1,076 |
| AUS-BRI | 13.249 | 12.900 | -2,6% | 12.704,1 | 12.370,3 | 1.447,3 | 1.430,6 | 838,2 | 843,9 | 1,005 | 1,012 |
| AUS-SYD | 9.788 | 9.221 | -5,8% | 9.385,5 | 8.842,1 | 2.393,7 | 2.206,4 | 1.694,9 | 1.699,5 | 1,002 | 1,005 |
| **total (18 cidades)** | **3.413.238** | **1.017.694** | **-70,2%** | — | — | **16.315,3** | **11.581,9** | **6.714,5** | **6.714,5** | — | — |

#### CHL — Chile

| Cidade | Área cultivável antes (km²) | Área cultivável depois (km²) | Δ | Teto de terra antes (t/dia) | Teto de terra depois (t/dia) | Fazenda inicial antes (t/dia) | Fazenda inicial depois (t/dia) | Produção efetiva antes (t/dia) | Produção efetiva depois (t/dia) | Comida antes | Comida depois |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| CHL-PUN | 868.072 | 54.287 | -93,7% | 832.398,0 | 52.055,8 | 19.839,3 | 2.381,5 | 53,6 | 44,2 | 1,520 | 1,254 |
| CHL-PUE | 126.758 | 108.982 | -14,0% | 201.423,1 | 173.176,7 | 2.474,5 | 2.430,0 | 224,8 | 231,5 | 3,047 | 3,138 |
| CHL-SAN | 35.952 | 33.030 | -8,1% | 34.474,4 | 31.673,0 | 3.423,5 | 3.526,9 | 1.570,2 | 1.572,9 | 1,082 | 1,084 |
| CHL-ANT | 2.125 | 2.067 | -2,7% | 1.629,9 | 1.585,5 | 120,3 | 120,3 | 0,0 | 0,0 | 0,000 | 0,000 |
| **total (4 cidades)** | **1.032.906** | **198.366** | **-80,8%** | — | — | **25.857,6** | **8.458,7** | **1.848,6** | **1.848,6** | — | — |

#### NZL — Nova Zelândia

| Cidade | Área cultivável antes (km²) | Área cultivável depois (km²) | Δ | Teto de terra antes (t/dia) | Teto de terra depois (t/dia) | Fazenda inicial antes (t/dia) | Fazenda inicial depois (t/dia) | Produção efetiva antes (t/dia) | Produção efetiva depois (t/dia) | Comida antes | Comida depois |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| NZL-CHR | 430.642 | 74.808 | -82,6% | 412.944,3 | 71.733,4 | 417,4 | 600,2 | 185,2 | 153,2 | 1,473 | 1,218 |
| NZL-AUC | 51.224 | 44.265 | -13,6% | 39.295,3 | 33.956,8 | 808,2 | 914,3 | 471,5 | 491,0 | 1,016 | 1,058 |
| NZL-WEL | 39.167 | 34.684 | -11,4% | 37.557,7 | 33.258,3 | 385,3 | 400,9 | 118,3 | 130,8 | 1,032 | 1,141 |
| **total (3 cidades)** | **521.033** | **153.757** | **-70,5%** | — | — | **1.610,9** | **1.915,4** | **774,9** | **774,9** | — | — |

#### ARG — Argentina

| Cidade | Área cultivável antes (km²) | Área cultivável depois (km²) | Δ | Teto de terra antes (t/dia) | Teto de terra depois (t/dia) | Fazenda inicial antes (t/dia) | Fazenda inicial depois (t/dia) | Produção efetiva antes (t/dia) | Produção efetiva depois (t/dia) | Comida antes | Comida depois |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ARG-MBU | 136.281 | 136.380 | +0,1% | 104.544,1 | 104.619,9 | 80,1 | 80,1 | 63,7 | 63,5 | 23,577 | 23,487 |
| ARG-ROS | 95.968 | 95.929 | -0,0% | 92.023,7 | 91.986,7 | 440,6 | 438,3 | 335,7 | 336,0 | 1,180 | 1,181 |
| ARG-COR | 72.667 | 72.378 | -0,4% | 69.680,6 | 69.403,3 | 899,7 | 892,8 | 669,6 | 669,7 | 1,059 | 1,060 |
| ARG-SAN | 70.468 | 71.273 | +1,1% | 67.572,4 | 68.343,6 | 367,6 | 365,5 | 190,2 | 190,7 | 1,155 | 1,158 |
| ARG-NEU | 63.786 | 62.977 | -1,3% | 61.164,5 | 60.388,7 | 240,7 | 238,6 | 85,2 | 85,1 | 1,228 | 1,227 |
| ARG-BUE | 55.405 | 54.592 | -1,5% | 53.127,8 | 52.348,7 | 1.075,3 | 1.065,3 | 899,3 | 899,1 | 1,037 | 1,037 |
| ARG-MAR | 52.122 | 51.914 | -0,4% | 49.980,5 | 49.780,1 | 222,4 | 222,3 | 213,0 | 212,8 | 1,197 | 1,195 |
| ARG-COM | 22.325 | 21.761 | -2,5% | 21.407,8 | 20.866,3 | 1.149,2 | 1.038,8 | 42,8 | 42,9 | 1,013 | 1,014 |
| ARG-MEN | 17.186 | 16.901 | -1,7% | 16.480,0 | 16.206,9 | 1.899,6 | 1.903,7 | 359,0 | 359,0 | 1,006 | 1,006 |
| ARG-RIO | 12.234 | 9.504 | -22,3% | 11.731,1 | 9.113,0 | 224,1 | 211,7 | 16,4 | 16,3 | 1,039 | 1,032 |
| **total (10 cidades)** | **598.442** | **593.607** | **-0,8%** | — | — | **6.599,3** | **6.456,9** | **2.875,0** | **2.875,0** | — | — |

#### ZAF — África do Sul

| Cidade | Área cultivável antes (km²) | Área cultivável depois (km²) | Δ | Teto de terra antes (t/dia) | Teto de terra depois (t/dia) | Fazenda inicial antes (t/dia) | Fazenda inicial depois (t/dia) | Produção efetiva antes (t/dia) | Produção efetiva depois (t/dia) | Comida antes | Comida depois |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ZAF-DUR | 33.697 | 35.405 | +5,1% | 32.312,1 | 33.950,4 | 2.130,5 | 2.156,0 | 1.165,8 | 1.169,9 | 1,164 | 1,168 |
| ZAF-PRE | 33.534 | 33.524 | -0,0% | 32.155,8 | 32.146,6 | 1.632,7 | 1.613,9 | 778,8 | 777,5 | 1,229 | 1,227 |
| ZAF-GQE | 31.327 | 32.249 | +2,9% | 30.039,3 | 30.923,9 | 876,8 | 874,4 | 471,0 | 474,2 | 1,495 | 1,505 |
| ZAF-JOH | 24.696 | 24.935 | +1,0% | 23.681,3 | 23.910,1 | 5.729,5 | 5.752,9 | 2.916,4 | 2.915,3 | 1,032 | 1,032 |
| ZAF-UPI | 14.295 | 14.467 | +1,2% | 13.707,7 | 13.872,7 | 253,0 | 253,7 | 45,5 | 45,0 | 2,125 | 2,102 |
| ZAF-CAP | 12.465 | 11.929 | -4,3% | 11.952,6 | 11.439,0 | 3.068,6 | 3.134,5 | 1.474,4 | 1.469,9 | 1,030 | 1,027 |
| **total (6 cidades)** | **150.014** | **152.510** | **+1,7%** | — | — | **13.691,2** | **13.785,4** | **6.851,9** | **6.851,9** | — | — |

## 6. Diagnóstico das três nações que reprovam

### 6.1 SGP — consequência DIRETA da área (e um isolamento de comércio)

| | antes (`0a27150`) | depois |
|---|---:|---:|
| Área cultivável de SGP-SIN | 1.596 km² | **125 km²** (−92,2%) |
| Teto de terra | 2.010,9 t/dia | **157,3 t/dia** |
| Fazenda inicial resolvida | 2.010,9 t/dia | **157,3 t/dia** |
| Produção efetiva (arroz, aptidão 0,76) | 1.532,6 t/dia | **119,9 t/dia** |
| Comida da cidade (oferta local ÷ necessidade) | 0,91 | **0,07** |
| Comida da nação no sweep | 0,930 | 0,177 |

Causa: **96% da "terra cultivável" de Singapura era célula de Voronoi**, não território. A fazenda
inicial já é **redimensionada sozinha** pelo `resolve_initial_farms()` com a área nova (o valor
resolvido coincide exatamente com o teto novo, 157,3 t/dia); **nenhum dado derivado ficou velho**.

O que impede Singapura de importar o que perdeu: `ports.json` não tem porto de SGP (144 portos no
mundo, nenhum `nation = SGP`) e a `landmass` de SGP-SIN é `singapore` (ela é a única cidade dessa
massa). A regra do porto (`sim/trade.cpp`, §port_design) só permite vender entre massas de terra
quando **as duas pontas** têm porto; sozinha na própria massa e sem porto, SGP não tem rota
comercial. Com 20,5 M de poupança no fim e `imp = 0` no diagnóstico de comida, ela **tem dinheiro
e não tem acesso** — é a mesma lacuna estrutural dos importadores aceitos (SOL/DJI/SAH/JOR/YEM),
agravada pelo isolamento.

**Correção limpa dentro do escopo "consequência da área": nenhuma.** A fazenda já usa a área nova;
adicionar um porto a Singapura é um dado novo (fora da correção de área) e mudaria rotas do mundo
inteiro — não foi feito.

### 6.2 LBY e EGY — divergência de trajetória, não efeito de dado

Nas duas, **todas as cidades têm aptidão 0,0** para a cultura escolhida nas duas versões, então a
fazenda inicial não produz nada e a nação vive de importação em ambos os lados:

| | LBY | EGY |
|---|---:|---:|
| Área cultivável somada antes → depois | 17.309,6 → 16.907,5 km² | 11.985,0 → 11.723,7 km² |
| Fazenda inicial somada (t/dia) | 783,4 → 783,4 | 2.175,2 → 2.121,1 |
| Produção efetiva somada (t/dia) | **0,0 → 0,0** | **0,0 → 0,0** |
| Aptidão | 0,0 em todas | 0,0 em todas |

O mundo também não mudou de escala: no diagnóstico de comida (mesmo pipeline, sem a camada
urbana) a produção mundial 374.959 → 371.539 t/dia (−0,9%) e o preço 44,21 → 44,25 XGS/t.
A fazenda inicial só muda onde a área mudou (EGY-TAN 86,7 → 53,2 t/dia, EGY-CAI 903,9 → 873,4,
EGY-ALE 658,6 → 668,5; soma EGY 2.175,2 → 2.121,1), e a aptidão 0,0 zera a produção efetiva nos
dois lados — nenhuma dessas variações cria ou tira comida.

A queda é **divergência de trajetória** (o mundo é sensível a qualquer mudança de dado), provada
pelo A/B isolado (mesma variante `real`, 365 d, IA, `--urban=1`, artefatos
`docs/economy/perf/recal_iso_q016.json` e `recal_iso_area.json`):

| Cenário | SGP | LBY | EGY | ESP |
|---|---:|---:|---:|---:|
| ANTES (`0a27150`) | 0,930 | 1,000 | 1,000 | 0,984 |
| **só Q-016** (mudança confinada a `AFG-MAZ`) | 0,943 | **0,936** | 1,000 | 0,984 |
| **só área** | **0,167** | 1,000 | 1,000 | 0,969 |
| **as duas** (entrega) | 0,177 | 0,852 | 0,899 | 0,969 |

Ou seja: o Q-016 sozinho, sem tocar em nenhum dado de LBY/EGY, já derruba LBY 6,4 p.p.; a área
sozinha derruba SGP 76,3 p.p. e deixa LBY/EGY intactos; só a **combinação** leva LBY a 0,852 e
EGY a 0,899. O portão de 2,5 p.p. está dentro da sensibilidade de trajetória de um importador sem
agricultura — a nação não tem variável de área que a explique.

**Correção limpa: nenhuma.** Um "conserto" aqui só poderia ser calibração/subsídio para estabilizar
importador sem agricultura — exatamente a muleta que o briefing proíbe. Fica registrado como dívida
de modelo (§19.5/renda de importador), não como defeito deste dado.

### 6.3 ESP — direto e pequeno

ESP-LAS (Las Palmas) 568 → 236 km² (−58,5%), teto 545,0 → 225,9 t/dia, produção efetiva
71,7 → 28,3 t/dia, comida 0,62 → 0,25. As outras 8 cidades espanholas praticamente não mudam
(MAD/SEV/BAR/MLG ±1%), e o efeito nacional é **−1,56 p.p.**, dentro da tolerância. É uma ilha
pequena que herdava célula grande do Voronoi; o valor novo é o correto.

### 6.4 Conclusão do diagnóstico

Não há correção de dado que salve SGP/LBY/EGY sem inventar terra, porto ou subsídio. Pelo
briefing ("se não houver correção limpa, relate e pare"), **nada foi calibrado**; a decisão de
aceitar SGP na lista de importadores e de tratar LBY/EGY como dívida de modelo é do head.

## 7. Goldens e testes

**Golden A0 de equivalência** (`docs/economy/refactor/baseline/A0_60_urban.json`, 60 d, IA,
`--urban=1`, seed 0): regenerado e validado com **2 rodadas campo a campo → 0 diferenças** e
1 rodada contra o golden → shas iguais.
`save_sha256 = 3dba93b313a39b481ae3be16e0d8274715a76226f929cc1f64f7943554cf147e`,
`status_sha256 = 1cfcaf3d6025edc9cb385cf9126a3c75d856562b820e224c3e68d390d9ffd7aa`.

**Goldens S0** (`tools/economy_refactor_equiv.gd`, sem urbano) — regenerados e revalidados
(2ª rodada contra o golden → 0 diferenças):

| Golden | save_sha256 | status_sha256 |
|---|---|---|
| `real_60_ai` | `0cea757195d516973cf0aff411d8ddc842cd1288edcc87211d78ab6d9815c2ca` | `49dd230f8887a063d699e3d7d9e051513b6c880dd5d8b56fef692d473f99fe69` |
| `real_60_noai` | `30445923b2f9314633fa56d664ea1c52dc1d03db8ae19f4f864a0f38672e8a05` | `c1b8ba3b3e6250e252588a9e9074412ecea9bc5b7ce0a1efd170b732a23d0f81` |
| `demo_365_noai` | `652b5f8b2f8e5cc7315c37d1ebce3249edbf4957b702dff45fc00e148074e2ab` | `65a414f0de49014f6dccce39bd89d5a99bc7cf792c0dbc26fe6407fd02b46268` |

Nota: em `demo_365_noai` o `status_sha256` **não mudou** (cenário sintético AGRIA/PORTUS, alheio
ao dado do mundo) e o `save_sha256` mudou — o golden anterior estava defasado em relação ao formato
de save pós-S6 (D-041/D-042), não por causa desta recalibração.

**Testes** (todos headless, na DLL debug desta worktree):

| Teste | Resultado |
|---|---|
| `economy_integration_test.gd` | **OK** |
| `economy_ai_test.gd` | **OK** |
| `population_demo_test.gd` | **OK** |
| `economy_save_load_roundtrip_test.gd` | **OK — 0 diffs** (save no dia 30 + 30 d = 60 d direto) |
| `tools/validate_scenario_data.py` | **0 erros, 0 avisos** em 11 arquivos |
| `tools/validate_urban_land.py` / `validate_housing_city.py` | OK (834 cidades) |

**Baseline novo:** `docs/economy/perf/recal_world_sweep.json` (agregado das 6 rodadas), e
`tools/s19_world_sweep_parallel.ps1` passa a apontar para ele. **Defeito de ferramenta corrigido:**
o `$baseline` padrão apontava para `q016_world_sweep.json`, arquivo que **não existe** (o nome real
é `q16_world_sweep.json`); com o baseline anterior o `economy_world_test` falhava em toda rodada e
o sweep só funcionava se cada variante recebesse o caminho certo por fora.

## 8. Decisões e pendências

- **Decisão (D-071, GAME_STATE §14):** o dado real passa a ser área de província por raster do
  atlas (sem Antártida rateada) + população corrigida de Mazar-i-Sharif; baseline do sweep
  promovido para `recal_world_sweep.json`.
- **Pendência 1 (head) — RESOLVIDA na rodada 2:** o head decidiu **não reverter** e tratar SGP
  como erro de dado. O porto `SGP-SIN` entrou em `FORCED_PORTS` (§9) e o sweep `recal2` fechou
  SGP em **1,000** (§11).
- **Pendência 2 (dados) — RESOLVIDA para SGP, ABERTA para o resto:** `ports.json` passou a ter o
  porto de Singapura. A mesma lacuna existe em outras ilhas/importadores (§13) e **não** foi
  tocada nesta rodada.
- **Pendência 3 (dados):** EGY continua com aptidão agrícola 0,0 em todas as cidades (importador
  integral) — o vale de EGY já vinha registrado em D-067/R4. A trajetória mensal da rodada 2
  (§12) mostra que o número de 365 d é uma amostra de um dia em ciclo, não um colapso.
- **Pendência 4 (ferramenta, nova):** `build_scenario_data.py` **regenera** `i18n.json` a partir
  de `i18n.csv` e, com isso, **apaga** as chaves que só existem no JSON (137 hoje: `map.region.*`,
  `map.legend.routes.*`, `city.services.*`, `dock.services`, `map.icon.industry`). Na rodada 2 o
  JSON foi mesclado à mão (HEAD + `port.name.SGP-SIN`) em vez de regenerado; o conserto do
  merge é do dono da ferramenta/i18n.
- **Escopo respeitado:** nenhuma constante, subsídio, tarifa ou terra nova; nenhum arquivo de
  `core/` ou `gdextension/src/` alterado (a DLL desta worktree é o build `0a27150`, só dados
  mudaram).

## 9. Decisão do head: Singapura é erro de dado — porto `SGP-SIN` (r9)

O cenário tinha `SGP-SIN` e **nenhum porto com `nation = SGP`** (144 portos, 0 de SGP). No
`sim/trade.cpp` a travessia entre massas de terra exige um porto da nação **em cada ponta**, e
`SGP-SIN` é a única cidade da landmass `singapore`: sem porto, a nação não tinha rota marítima
nenhuma. A "fazenda de 1.691 t/dia" era o teto por população (`output_tons_per_day = sim_pop ×
0,0003`), **clampado em runtime** para 157,3 t/dia pela área real (~125 km², §6.1) — ou seja, a
produção que sustentava SGP vinha da terra fictícia do Voronoi. Singapura é um dos maiores portos
de contêineres do mundo (PSA): a ausência é erro de dado, não consequência da correção de área.

Entrada em `tools/world_city_curation.py` (`FORCED_PORTS`, seção r9), no cais de **Tanjong Pagar**,
costa sul voltada para o Estreito de Singapura:

| Campo | Valor | Como foi conferido |
|---|---|---|
| `id` / `port_of` | `SGP-SIN` | mesmo id da cidade (convenção de Heraklion/Gdańsk) |
| `lat` / `lon` | 1,2626 N / 103,8544 E | PSA Tanjong Pagar |
| `landmass` | `singapore` | igual à cidade (o porto só serve a cidade da mesma massa) |
| célula no `nav_mask` | (1971, 616), componente principal | água (`nav=0`), alcança o mar aberto pelo estreito |
| distância à terra no atlas | 4,89 km | dentro dos 10 km de `COAST_TOLERANCE_KM` |
| `source` | PSA Singapore, Tanjong Pagar Terminal | gravado no registro e no `i18n` |

Efeito direto: a capacidade do cais de nível 1 é `1000 + 0,002 × sim_população` =
**12.277 t/dia**, contra uma necessidade de 1.692 t/dia — folga suficiente para importar o que a
fazenda real (157,3 t/dia) não cobre. No sweep, SGP fecha **1,000** (§11).

## 10. Regeneração pelo pipeline e validação

```powershell
$py = "G:/tools/cpp-tools/Scripts/python.exe"
& $py tools/build_world_cities.py --write-scenario   # curadoria + promoção (0 AVISO)
& $py tools/build_scenario_data.py                   # scenario/*.json
& $py tools/validate_scenario_data.py                # 0 erros, 0 avisos
& $godot --headless --path . res://tools/nav_strait_test.tscn
```

- **Portos: 144 → 145**; `port_of` preenchido em **145/145**; cidades/construções/nações
  inalteradas (834/834/179). `i18n.csv` ganhou só `port.name.SGP-SIN` (as demais linhas mudaram
  apenas de posição: `merge_i18n` ordena as chaves; conferido chave a chave — 0 valor alterado,
  0 chave removida).
- `validate_scenario_data.py`: **0 erros, 0 avisos** em 11 arquivos (`ports.csv/json` com 145).
- `build_world_cities.py --check` depois do build: **0 AVISO**, 145 portos.
- `nav_strait_test.gd`: **PASS** com `SGP-SIN` na lista de portos — `comp=0` (= mar aberto),
  `cais->agua 0,0 cel`. O teste ganhou a entrada para não regredir.
- **Nenhum C++ mudou**; a DLL continua `22D800917FE84E2AF6E6083D01B3E27C9E4EE4C68EC9AEED97542B1149678C98`.

### 10.1 Correção manual do `i18n.json`

`build_scenario_data.py` reconstrói `i18n.json` **só** a partir de `i18n.csv`; o JSON versionado
tem 137 chaves que não estão no CSV (`map.region.*`, `map.legend.routes.*`, `city.services.*`,
`dock.services`, `map.icon.industry`). Regenerar apagaria essas chaves (UI cairia para a chave
crua). O JSON entregue é o do HEAD **mais** `port.name.SGP-SIN` nos três idiomas — uma chave
nova por locale. Fica registrado como pendência de ferramenta (§8, pendência 4).

## 11. Sweep `recal2` — 365 d ×5 + sonda de câmbio

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1 -OutPrefix recal2_
$py tools/aggregate_s19_sweep.py --baseline docs/economy/perf/q16_world_sweep.json `
  --dir "<user>/world_sweep" --prefix recal2_ --suffix _365.json `
  --out docs/economy/perf/recal2_world_sweep.json
```

Artefato: `docs/economy/perf/recal2_world_sweep.json`. 365 d, `--ai=1 --urban=1 --threads=1`,
6 processos (5 variantes de aceite + `--fx=float`).

### 11.1 Portões

| Portão | Limite | Medido | Veredito |
|---|---|---|---|
| Falências de construção (pior variante) | ≤ 28 | **7** (7 nas 5) | OK |
| Dinheiro conservado (drift) | ≤ 0,01 | **−0,0010** (sonda −0,0005) | OK |
| Determinismo peg ×5 | hashes iguais | `e13f92a5…` nas 5 | OK |
| Sonda `--fx=float` difere | hash diferente | `f37205f1…` | OK |
| Pior nação (comida) | — | **SAH 0,183** (importador aceito) | informativo |

### 11.2 Comida por nação — q16 (baseline) × recal (rodada 1) × recal2

| Nação | q16 | recal | **recal2** | Δ vs q16 | Δ vs recal | Status |
|---|---:|---:|---:|---:|---:|---|
| **SGP** | 0,930 | 0,177 | **1,000** | **+7,04 p.p.** | +82,30 p.p. | resolvido |
| **LBY** | 1,000 | 0,852 | **1,000** | 0,00 | +14,78 p.p. | resolvido |
| **EGY** | 1,000 | 0,899 | **0,640** | **−35,96 p.p.** | −25,85 p.p. | REPROVA (§12) |
| PSX | 0,224 | 0,290 | 0,190 | **−3,44 p.p.** | −10,01 p.p. | REPROVA (não é isento) |
| YEM | 1,000 | 1,000 | 0,942 | −5,83 p.p. | −5,83 p.p. | isento |
| SAH | 0,223 | 0,215 | 0,183 | −4,00 p.p. | −3,16 p.p. | isento |
| ESP | 0,984 | 0,969 | 0,969 | −1,56 p.p. | 0,00 | dentro da tolerância |
| SOM | 1,000 | 1,000 | 0,989 | −1,11 p.p. | −1,11 p.p. | dentro da tolerância |
| DJI | 0,303 | 0,338 | 0,351 | +4,78 p.p. | +1,25 p.p. | melhora |
| SOL | 0,556 | 0,663 | 0,614 | +5,81 p.p. | −4,96 p.p. | isento |

**As outras 169 nações ficam dentro de ±1 p.p.** contra o q16. Aplicando o portão do head
(> 2,5 p.p. de queda vs q16, exceto SOL/DJI/SAH/JOR/YEM), **reprovam EGY (−35,96) e PSX
(−3,44)**; SAH e YEM caem mas são isentos. Contra a rodada 1, a lista é EGY, PSX, SOL, SAH e YEM.

**Leitura:** o porto de SGP devolveu **+82,30 p.p.** a Singapura e **+14,78 p.p.** a LBY (que
voltou a 1,000). O custo aparece em quem disputa o mesmo alimento: SGP passa a comprar
~1.534 t/dia (necessidade 1.692 − fazenda 157), e EGY — importador integral, sem aptidão —
perde 36 p.p. **na foto do dia 365**.

## 12. Trajetória mensal de LBY e EGY (o que diverge)

Séries medidas com o mesmo dado/binário (`--ai=1 --urban=1 --threads=1 --fx=real`), rodando
`economy_world_test.gd` até 30, 60, …, 360 d (o sim é determinístico: o dia N de uma corrida de
N dias é o mesmo dia N da corrida de 365 d).

| Dia | SGP | LBY | **EGY** | PSX | SAH | YEM |
|---:|---:|---:|---:|---:|---:|---:|
| 30 | 1,000 | 1,000 | 0,718 | 1,000 | 1,000 | 1,000 |
| 60 | 1,000 | 1,000 | 0,710 | 1,000 | 1,000 | 1,000 |
| 90 | 1,000 | 1,000 | 1,000 | 1,000 | 1,000 | 1,000 |
| 120 | 1,000 | 1,000 | 0,844 | 1,000 | 1,000 | 1,000 |
| 150 | 1,000 | 1,000 | 0,810 | 1,000 | 1,000 | 1,000 |
| 180 | 1,000 | 1,000 | 0,943 | 1,000 | 0,810 | 1,000 |
| 210 | 1,000 | 1,000 | 0,895 | 0,757 | 0,334 | 0,936 |
| 240 | 1,000 | 1,000 | 0,956 | 0,419 | 0,256 | 0,872 |
| 270 | 1,000 | 1,000 | 1,000 | 0,419 | 0,203 | 0,869 |
| 300 | 1,000 | 1,000 | 1,000 | 0,396 | 0,192 | 0,925 |
| 330 | 1,000 | 1,000 | 1,000 | 0,275 | 0,244 | 1,000 |
| 360 | 1,000 | 1,000 | 1,000 | 0,275 | 0,239 | 1,000 |
| **365** | **1,000** | **1,000** | **0,640** | **0,190** | **0,183** | **0,942** |

**LBY:** 1,000 em todos os meses — o valor 0,852 da rodada 1 era trajetória, não estrutura; com o
porto de SGP o mundo reflui e LBY importa normalmente.

**EGY:** a série **oscila entre 0,71 e 1,00** e passa os meses 270–360 em **1,000**; o 0,640 é a
amostra do **dia 365**, não um patamar. A oscilação é diária, não mensal — medido no último
trecho: dia 360 = 1,000, **361 = 0,720, 362 = 1,000, 363 = 0,763, 364 = 1,000, 365 = 0,640**.
A métrica do sweep é `consumido ÷ necessidade` **do último dia**: para um importador que recebe
comida em levas, ela bate no vale do ciclo. As cidades egípcias continuam com **aptidão 0,0**
(nenhuma mudança de dado nesta rodada) e vivem de importação nos dois lados.

**O Q-016 já mostrava a mesma fragilidade? Sim, e está medido no próprio baseline.** A sonda
`--fx=float` do `q16_world_sweep.json` fecha **EGY 0,768 (Δ −23,2 p.p.)** — um único botão
(regime de câmbio) mexia 23 p.p. na foto de EGY. Na rodada 2 a sonda dá **EGY 0,858**: o valor
com câmbio flutuante é **21,8 p.p. acima** do peg (0,640), ou seja, o 0,640 é o fundo de um vale
que o regime cambial atravessa. O comentário de `economy_world_test.gd` (FOOD_TOLERANCE) já
registrava o mesmo transiente no Q-016: **EGY 0,857 em 60 d e 0,546 em 120 d, fechando 1,000 em
365 d**. Nada disso é balanço: é limiar de importador sem agricultura.

**PSX:** aqui a queda é estrutural e **anterior** — 1,000 até 180 d e depois 0,76 → 0,42 → 0,28,
com o q16 já em 0,224. A rodada 2 só empurrou o mesmo declive 3,4 p.p. para baixo. Fica relatado
como importador em colapso de longo prazo, não como efeito novo do porto.

**Nenhuma muleta foi criada** para LBY/EGY/PSX: sem subsídio, sem tarifa, sem constante, sem
terra nova, sem aptidão inventada.

## 13. Auditoria da mesma lacuna (relatada, sem mudança)

Porto só serve a cidade da **mesma nação e mesma landmass** (`sim/trade.cpp`); sem porto, uma
nação sozinha na sua massa de terra não compra nem vende fora dela. Varredura do cenário:

**Ilhas/nações sozinhas na landmass e SEM porto (mesma classe do SGP, 11):**

| Nação | sim_pop | cidades | landmass | Observação |
|---|---:|---:|---|---|
| PHL | 7.448.460 | 6 | `philippines` | maior caso restante; arquipélago, nenhum porto |
| CUB | 2.719.689 | 2 | `cuba` | nenhum porto |
| NZL | 2.348.300 | 3 | `new_zealand` | exportador (comida 1,000), perde a exportação |
| MDG | 1.897.713 | 3 | `madagascar` | nenhum porto |
| JAM | 1.020.567 | 2 | `jamaica` | nenhum porto |
| LKA | 817.136 | 2 | `sri_lanka` | nenhum porto |
| BHS | 274.465 | 2 | `bahamas` | nenhum porto |
| FJI | 129.866 | 2 | `fiji` | nenhum porto |
| TTO | 105.411 | 2 | `trinidad` | nenhum porto |
| BRB | 98.511 | 1 | `barbados` | nenhum porto |
| SLB | 63.502 | 2 | `solomon` | nenhum porto |

**Cidades-estado/importadoras do pedido:** `HKG`, `MDV` e `MUS` **não existem como nação** no
cenário (MUS foi removida na r2; HKG e MDV nunca entraram no roster). `BHR` (Manama), `QAT`
(Doha, Madinat ash Shamal) e `BRN` (Bandar Seri Begawan, Kuala Belait) têm cidade costeira e
**nenhum porto**, mas estão na landmass `asia` com dezenas de nações — compram por terra da
vizinhança, então não são o mesmo caso de isolamento. `MLT` já tem porto (`MLT-VAL`).

**Achado extra — porto com landmass que não casa com a cidade (15 portos inutilizáveis):**
`IDN` tem cidades em `indonesia` e **9 dos 10 portos** em `java` (só `IDN-MEDA` serve); `TUR`
tem cidades em `asia` e os portos `TUR-IST`, `TUR-IZM`, `TUR-ADA` em `continent`; `AZE-BAK`,
`AZE-SUM` e `GEO-BAT` também estão em `continent` com cidades em `asia`. É dado legado (a
landmass "java" do Medan antigo já aparecia no docstring da r5) e **não foi tocado** aqui.

## 14. Goldens e testes da rodada 2

Goldens regenerados porque o dado mudou (porto), e revalidados com 2 rodadas → **0 diferenças**:

| Golden | save_sha256 | status_sha256 |
|---|---|---|
| `A0_60_urban.json` (60 d, IA, `--urban=1`) | `4a84ea05c82eb521d7e9bd3b2d7746f3dc9eeefa513ded73b311cf1b6b2d3847` | `a15ecf7f8fd6827956d79a48bfca6548d8465b04edb1efc1ef59c067fbba5d43` |
| `real_60_ai.json` | `7abd5095954eb28c863dbec3c63060ae6ddf89ce3b91338489aa22045302bc72` | `ce19827c036af02eb3a4f364e2fc093d04fc98000bdc897910c8ff54f10981d6` |
| `real_60_noai.json` | `058abb029cc548407d306d6843c2286805673b37716c3c976fa2b142eac8f681` | `b87a498d26e5cdf4731021768088a5dd2a22b2f24386d5de804edfc6819a98af` |
| `demo_365_noai.json` | `652b5f8b2f8e5cc7315c37d1ebce3249edbf4957b702dff45fc00e148074e2ab` | `65a414f0de49014f6dccce39bd89d5a99bc7cf792c0dbc26fe6407fd02b46268` |

`demo_365_noai` **não mudou** (cenário sintético AGRIA/PORTUS, alheio ao dado do mundo).

| Teste | Resultado |
|---|---|
| `economy_integration_test.gd` | **OK** |
| `economy_ai_test.gd` | **OK** |
| `population_demo_test.gd` | **OK** |
| `economy_save_load_roundtrip_test.gd` | **OK — 0 diffs** |
| `economy_refactor_equiv.gd` (3 goldens S0, contra o golden) | **0 diferenças** em cada |
| `world_equivalence.ps1 -Baseline A0_60_urban.json -Rounds 2` | **PASS — 0 diferenças** |
| `validate_scenario_data.py` | **0 erros, 0 avisos** |
| `nav_strait_test.tscn` | **PASS** (com `SGP-SIN`) |

**Baseline novo:** `docs/economy/perf/recal2_world_sweep.json`; `tools/s19_world_sweep_parallel.ps1`
passa a apontar para ele.
